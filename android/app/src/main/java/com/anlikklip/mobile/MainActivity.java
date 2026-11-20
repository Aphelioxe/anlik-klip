package com.anlikklip.mobile;

import android.Manifest;
import android.content.Intent;
import android.content.SharedPreferences;
import android.content.pm.PackageManager;
import android.net.Uri;
import android.os.Build;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.text.Editable;
import android.text.TextWatcher;
import android.view.View;
import android.widget.Button;
import android.widget.EditText;
import android.widget.LinearLayout;
import android.widget.TextView;
import android.widget.Toast;

import androidx.activity.result.ActivityResultLauncher;
import androidx.activity.result.contract.ActivityResultContracts;
import androidx.appcompat.app.AlertDialog;
import androidx.appcompat.app.AppCompatActivity;
import androidx.appcompat.widget.SwitchCompat;
import androidx.core.content.ContextCompat;
import androidx.recyclerview.widget.LinearLayoutManager;
import androidx.recyclerview.widget.RecyclerView;
import androidx.swiperefreshlayout.widget.SwipeRefreshLayout;

import com.journeyapps.barcodescanner.ScanContract;
import com.journeyapps.barcodescanner.ScanOptions;

import java.util.ArrayList;
import java.util.HashSet;
import java.util.List;
import java.util.Locale;
import java.util.Set;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;

public class MainActivity extends AppCompatActivity implements ClipAdapter.Listener {
    private static final String PREFS = "anlikklip_prefs";
    private static final String KEY_HOST = "host";
    private static final String KEY_PORT = "port";
    private static final String KEY_PIN = "pin";
    private static final String KEY_AUTO_DL = "auto_dl";
    private static final String KEY_DOWNLOADED = "downloaded_files";

    private final ExecutorService netExecutor = Executors.newFixedThreadPool(4);
    private final ExecutorService dlExecutor = Executors.newSingleThreadExecutor();
    private final Handler mainHandler = new Handler(Looper.getMainLooper());

    private TextView txtStatus;
    private LinearLayout panelConnect;
    private LinearLayout panelControls;
    private EditText edtIp;
    private EditText edtPort;
    private EditText edtPin;
    private EditText edtSearch;
    private Button btnFilterAll;
    private Button btnFilterFav;
    private SwitchCompat swAutoDownload;
    private SwipeRefreshLayout swipeRefresh;

    private ClipAdapter adapter;
    private final List<ClipItem> allClips = new ArrayList<>();
    private final Set<String> downloadedFiles = new HashSet<>();

    private boolean connected = false;
    private boolean onlyFavorites = false;
    private String currentHost = "";
    private int currentPort = 47820;
    private String currentPin = "";
    private ClipItem pendingDownloadClip = null;

    private final Runnable autoSyncRunnable = new Runnable() {
        @Override
        public void run() {
            if (connected && swAutoDownload.isChecked()) {
                refreshClips(true);
                mainHandler.postDelayed(this, 10000);
            }
        }
    };

    private final ActivityResultLauncher<ScanOptions> qrLauncher =
            registerForActivityResult(new ScanContract(), result -> {
                if (result.getContents() != null) {
                    handleQrPayload(result.getContents());
                }
            });

    private final ActivityResultLauncher<String> storagePermLauncher =
            registerForActivityResult(new ActivityResultContracts.RequestPermission(), granted -> {
                if (granted && pendingDownloadClip != null) {
                    ClipItem c = pendingDownloadClip;
                    pendingDownloadClip = null;
                    startDownload(c);
                } else if (!granted) {
                    pendingDownloadClip = null;
                    Toast.makeText(this, "Galeriye kaydetmek için depolama izni gerekiyor", Toast.LENGTH_LONG).show();
                }
            });

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        setContentView(R.layout.activity_main);

        txtStatus = findViewById(R.id.txtStatus);
        panelConnect = findViewById(R.id.panelConnect);
        panelControls = findViewById(R.id.panelControls);
        edtIp = findViewById(R.id.edtIp);
        edtPort = findViewById(R.id.edtPort);
        edtPin = findViewById(R.id.edtPin);
        edtSearch = findViewById(R.id.edtSearch);
        btnFilterAll = findViewById(R.id.btnFilterAll);
        btnFilterFav = findViewById(R.id.btnFilterFav);
        swAutoDownload = findViewById(R.id.swAutoDownload);
        swipeRefresh = findViewById(R.id.swipeRefresh);

        RecyclerView recycler = findViewById(R.id.recyclerClips);
        recycler.setLayoutManager(new LinearLayoutManager(this));
        adapter = new ClipAdapter(netExecutor, this);
        recycler.setAdapter(adapter);

        SharedPreferences sp = getSharedPreferences(PREFS, MODE_PRIVATE);
        edtIp.setText(sp.getString(KEY_HOST, ""));
        edtPort.setText(String.valueOf(sp.getInt(KEY_PORT, 47820)));
        edtPin.setText(sp.getString(KEY_PIN, ""));
        swAutoDownload.setChecked(sp.getBoolean(KEY_AUTO_DL, false));
        downloadedFiles.addAll(sp.getStringSet(KEY_DOWNLOADED, new HashSet<>()));

        findViewById(R.id.btnScanQr).setOnClickListener(v -> launchQrScanner());
        findViewById(R.id.btnToggleConnect).setOnClickListener(v ->
                panelConnect.setVisibility(panelConnect.getVisibility() == View.VISIBLE ? View.GONE : View.VISIBLE));
        findViewById(R.id.btnDiscover).setOnClickListener(v -> discoverOnLan());
        findViewById(R.id.btnConnect).setOnClickListener(v -> connectFromInputs());

        btnFilterAll.setOnClickListener(v -> {
            onlyFavorites = false;
            updateFilterButtons();
            applyFilter();
        });
        btnFilterFav.setOnClickListener(v -> {
            onlyFavorites = true;
            updateFilterButtons();
            applyFilter();
        });

        edtSearch.addTextChangedListener(new TextWatcher() {
            @Override public void beforeTextChanged(CharSequence s, int start, int count, int after) {}
            @Override public void onTextChanged(CharSequence s, int start, int before, int count) { applyFilter(); }
            @Override public void afterTextChanged(Editable s) {}
        });

        swAutoDownload.setOnCheckedChangeListener((buttonView, isChecked) -> {
            getSharedPreferences(PREFS, MODE_PRIVATE).edit().putBoolean(KEY_AUTO_DL, isChecked).apply();
            mainHandler.removeCallbacks(autoSyncRunnable);
            if (isChecked && connected) {
                mainHandler.postDelayed(autoSyncRunnable, 2000);
            }
        });

        swipeRefresh.setOnRefreshListener(() -> refreshClips(false));

        if (!edtIp.getText().toString().trim().isEmpty() && !edtPin.getText().toString().trim().isEmpty()) {
            connectFromInputs();
        }
    }

    private void launchQrScanner() {
        ScanOptions opts = new ScanOptions();
        opts.setPrompt("PC'deki Anlık Klip QR kodunu okutun");
        opts.setBeepEnabled(false);
        opts.setOrientationLocked(false);
        qrLauncher.launch(opts);
    }

    private void handleQrPayload(String raw) {
        try {
            Uri uri = Uri.parse(raw.trim());
            String host = uri.getHost();
            int port = uri.getPort() > 0 ? uri.getPort() : 47820;
            String pin = uri.getQueryParameter("pin");
            if (host != null && !host.isEmpty()) {
                edtIp.setText(host);
                edtPort.setText(String.valueOf(port));
                if (pin != null && !pin.isEmpty()) {
                    edtPin.setText(pin);
                }
                connectFromInputs();
                return;
            }
        } catch (Exception ignored) {
        }
        Toast.makeText(this, "Geçersiz QR kod", Toast.LENGTH_SHORT).show();
    }

    private void discoverOnLan() {
        txtStatus.setText("Ağda Anlık Klip aranıyor...");
        netExecutor.execute(() -> {
            List<ServerDiscovery.DiscoveredPc> pcs = ServerDiscovery.discover(1800);
            mainHandler.post(() -> {
                if (pcs.isEmpty()) {
                    txtStatus.setText(connected ? txtStatus.getText() : getString(R.string.status_disconnected));
                    Toast.makeText(this, "Ağda PC bulunamadı. PC'de 'Telefon Aktarımı' açık mı?", Toast.LENGTH_LONG).show();
                    return;
                }
                if (pcs.size() == 1) {
                    ServerDiscovery.DiscoveredPc pc = pcs.get(0);
                    edtIp.setText(pc.ip);
                    edtPort.setText(String.valueOf(pc.port));
                    txtStatus.setText("Bulundu: " + pc.name + " (PIN girip Bağlan'a basın)");
                } else {
                    String[] labels = new String[pcs.size()];
                    for (int i = 0; i < pcs.size(); i++) labels[i] = pcs.get(i).toString();
                    new AlertDialog.Builder(this)
                            .setTitle("PC Seçin")
                            .setItems(labels, (d, which) -> {
                                ServerDiscovery.DiscoveredPc pc = pcs.get(which);
                                edtIp.setText(pc.ip);
                                edtPort.setText(String.valueOf(pc.port));
                            })
                            .show();
                }
            });
        });
    }

    private void connectFromInputs() {
        String host = edtIp.getText().toString().trim();
        String portStr = edtPort.getText().toString().trim();
        String pin = edtPin.getText().toString().trim();
        if (host.isEmpty() || pin.isEmpty()) {
            Toast.makeText(this, "IP ve PIN girin veya QR kod tarayın", Toast.LENGTH_SHORT).show();
            return;
        }
        int port = 47820;
        try { port = Integer.parseInt(portStr); } catch (Exception ignored) {}

        currentHost = host;
        currentPort = port;
        currentPin = pin;
        txtStatus.setText("Bağlanılıyor...");

        netExecutor.execute(() -> {
            try {
                ApiClient.ServerInfo info = ApiClient.fetchInfo(currentHost, currentPort, currentPin);
                getSharedPreferences(PREFS, MODE_PRIVATE).edit()
                        .putString(KEY_HOST, currentHost)
                        .putInt(KEY_PORT, currentPort)
                        .putString(KEY_PIN, currentPin)
                        .apply();
                mainHandler.post(() -> {
                    connected = true;
                    adapter.setConnection(currentHost, currentPort, currentPin);
                    txtStatus.setText("Bağlı: " + info.pcName + " (" + info.clipCount + " klip)");
                    panelConnect.setVisibility(View.GONE);
                    panelControls.setVisibility(View.VISIBLE);
                    refreshClips(false);
                    mainHandler.removeCallbacks(autoSyncRunnable);
                    if (swAutoDownload.isChecked()) {
                        mainHandler.postDelayed(autoSyncRunnable, 10000);
                    }
                });
            } catch (Exception e) {
                mainHandler.post(() -> {
                    connected = false;
                    txtStatus.setText("Bağlantı hatası: " + e.getMessage());
                    Toast.makeText(this, "Bağlanılamadı: " + e.getMessage(), Toast.LENGTH_LONG).show();
                });
            }
        });
    }

    private void refreshClips(boolean fromAutoSync) {
        if (!connected) {
            swipeRefresh.setRefreshing(false);
            return;
        }
        netExecutor.execute(() -> {
            try {
                List<ClipItem> fetched = ApiClient.fetchClips(currentHost, currentPort, currentPin);
                for (ClipItem c : fetched) {
                    c.downloaded = downloadedFiles.contains(c.fileName);
                }
                mainHandler.post(() -> {
                    allClips.clear();
                    allClips.addAll(fetched);
                    applyFilter();
                    swipeRefresh.setRefreshing(false);
                    if (fromAutoSync && swAutoDownload.isChecked()) {
                        for (ClipItem c : allClips) {
                            if (!c.downloaded && !c.downloading) {
                                startDownload(c);
                            }
                        }
                    }
                });
            } catch (Exception e) {
                mainHandler.post(() -> {
                    swipeRefresh.setRefreshing(false);
                    if (!fromAutoSync) {
                        Toast.makeText(this, "Liste alınamadı: " + e.getMessage(), Toast.LENGTH_SHORT).show();
                    }
                });
            }
        });
    }

    private void applyFilter() {
        String q = edtSearch.getText().toString().trim().toLowerCase(Locale.ROOT);
        List<ClipItem> filtered = new ArrayList<>();
        for (ClipItem c : allClips) {
            if (onlyFavorites && !c.isFavorite) continue;
            if (!q.isEmpty() && !c.fileName.toLowerCase(Locale.ROOT).contains(q)) continue;
            filtered.add(c);
        }
        adapter.setItems(filtered);
    }

    private void updateFilterButtons() {
        btnFilterAll.setBackgroundTintList(ContextCompat.getColorStateList(this, onlyFavorites ? R.color.bg_card : R.color.accent_blue));
        btnFilterFav.setBackgroundTintList(ContextCompat.getColorStateList(this, onlyFavorites ? R.color.accent_blue : R.color.bg_card));
    }

    @Override
    public void onPlayClip(ClipItem item) {
        Intent it = new Intent(this, PlayerActivity.class);
        it.putExtra(PlayerActivity.EXTRA_HOST, currentHost);
        it.putExtra(PlayerActivity.EXTRA_PORT, currentPort);
        it.putExtra(PlayerActivity.EXTRA_PIN, currentPin);
        it.putExtra(PlayerActivity.EXTRA_FILE, item.fileName);
        it.putExtra(PlayerActivity.EXTRA_SIZE, item.sizeBytes);
        startActivity(it);
    }

    @Override
    public void onDownloadClip(ClipItem item) {
        if (Build.VERSION.SDK_INT <= Build.VERSION_CODES.P &&
                ContextCompat.checkSelfPermission(this, Manifest.permission.WRITE_EXTERNAL_STORAGE)
                        != PackageManager.PERMISSION_GRANTED) {
            pendingDownloadClip = item;
            storagePermLauncher.launch(Manifest.permission.WRITE_EXTERNAL_STORAGE);
            return;
        }
        startDownload(item);
    }

    private void startDownload(ClipItem item) {
        if (item.downloading) return;
        item.downloading = true;
        item.downloadProgress = 0;
        adapter.notifyClipUpdated(item.fileName);

        dlExecutor.execute(() -> {
            try {
                ApiClient.downloadClipToGallery(this, currentHost, currentPort, currentPin, item, pct ->
                        mainHandler.post(() -> {
                            item.downloadProgress = pct;
                            adapter.notifyClipUpdated(item.fileName);
                        }));
                mainHandler.post(() -> {
                    item.downloading = false;
                    item.downloaded = true;
                    item.downloadProgress = 100;
                    downloadedFiles.add(item.fileName);
                    getSharedPreferences(PREFS, MODE_PRIVATE).edit()
                            .putStringSet(KEY_DOWNLOADED, new HashSet<>(downloadedFiles))
                            .apply();
                    adapter.notifyClipUpdated(item.fileName);
                    Toast.makeText(this, item.fileName + " galeriye kaydedildi", Toast.LENGTH_SHORT).show();
                });
            } catch (Exception e) {
                mainHandler.post(() -> {
                    item.downloading = false;
                    adapter.notifyClipUpdated(item.fileName);
                    Toast.makeText(this, "İndirme hatası: " + e.getMessage(), Toast.LENGTH_LONG).show();
                });
            }
        });
    }

    @Override
    protected void onDestroy() {
        super.onDestroy();
        mainHandler.removeCallbacks(autoSyncRunnable);
        netExecutor.shutdownNow();
        dlExecutor.shutdownNow();
    }
}

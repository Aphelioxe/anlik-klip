package com.anlikklip.mobile;

import android.net.Uri;
import android.os.Bundle;
import android.view.View;
import android.widget.Button;
import android.widget.MediaController;
import android.widget.ProgressBar;
import android.widget.TextView;
import android.widget.Toast;
import android.widget.VideoView;

import androidx.appcompat.app.AppCompatActivity;

import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;

public class PlayerActivity extends AppCompatActivity {
    public static final String EXTRA_HOST = "host";
    public static final String EXTRA_PORT = "port";
    public static final String EXTRA_PIN = "pin";
    public static final String EXTRA_FILE = "file";
    public static final String EXTRA_SIZE = "size";

    private final ExecutorService executor = Executors.newSingleThreadExecutor();
    private VideoView videoView;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        setContentView(R.layout.activity_player);

        String host = getIntent().getStringExtra(EXTRA_HOST);
        int port = getIntent().getIntExtra(EXTRA_PORT, 47820);
        String pin = getIntent().getStringExtra(EXTRA_PIN);
        String file = getIntent().getStringExtra(EXTRA_FILE);
        long size = getIntent().getLongExtra(EXTRA_SIZE, 0);

        Button btnBack = findViewById(R.id.btnBack);
        TextView txtTitle = findViewById(R.id.txtPlayerTitle);
        Button btnDownload = findViewById(R.id.btnPlayerDownload);
        videoView = findViewById(R.id.videoView);
        ProgressBar progressVideo = findViewById(R.id.progressVideo);

        txtTitle.setText(file);
        btnBack.setOnClickListener(v -> finish());

        MediaController controller = new MediaController(this);
        controller.setAnchorView(videoView);
        videoView.setMediaController(controller);

        String streamUrl = ApiClient.buildClipStreamUrl(host, port, pin, file);
        videoView.setVideoURI(Uri.parse(streamUrl));
        videoView.setOnPreparedListener(mp -> {
            progressVideo.setVisibility(View.GONE);
            mp.start();
        });
        videoView.setOnErrorListener((mp, what, extra) -> {
            progressVideo.setVisibility(View.GONE);
            Toast.makeText(this, "Video oynatılamadı", Toast.LENGTH_SHORT).show();
            return true;
        });

        btnDownload.setOnClickListener(v -> {
            btnDownload.setEnabled(false);
            btnDownload.setText("İndiriliyor...");
            ClipItem item = new ClipItem();
            item.fileName = file;
            item.sizeBytes = size;
            executor.execute(() -> {
                try {
                    ApiClient.downloadClipToGallery(this, host, port, pin, item,
                            pct -> runOnUiThread(() -> btnDownload.setText(pct + "%")));
                    runOnUiThread(() -> {
                        btnDownload.setText(R.string.downloaded);
                        Toast.makeText(this, "Galeriye kaydedildi: Movies/AnlikKlip", Toast.LENGTH_SHORT).show();
                    });
                } catch (Exception e) {
                    runOnUiThread(() -> {
                        btnDownload.setEnabled(true);
                        btnDownload.setText(R.string.download_to_gallery);
                        Toast.makeText(this, "İndirme hatası: " + e.getMessage(), Toast.LENGTH_LONG).show();
                    });
                }
            });
        });
    }

    @Override
    protected void onDestroy() {
        super.onDestroy();
        if (videoView != null) videoView.stopPlayback();
        executor.shutdownNow();
    }
}

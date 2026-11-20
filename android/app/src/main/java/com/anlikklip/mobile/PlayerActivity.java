package com.anlikklip.mobile;

import android.content.pm.ActivityInfo;
import android.media.MediaPlayer;
import android.media.PlaybackParams;
import android.net.Uri;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.view.GestureDetector;
import android.view.MotionEvent;
import android.view.View;
import android.view.WindowManager;
import android.widget.Button;
import android.widget.FrameLayout;
import android.widget.LinearLayout;
import android.widget.ProgressBar;
import android.widget.SeekBar;
import android.widget.TextView;
import android.widget.Toast;
import android.widget.VideoView;

import androidx.appcompat.app.AppCompatActivity;

import com.google.android.material.slider.RangeSlider;

import java.util.List;
import java.util.Locale;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;

public class PlayerActivity extends AppCompatActivity {
    public static final String EXTRA_HOST = "host";
    public static final String EXTRA_PORT = "port";
    public static final String EXTRA_PIN = "pin";
    public static final String EXTRA_FILE = "file";
    public static final String EXTRA_SIZE = "size";

    private final ExecutorService executor = Executors.newSingleThreadExecutor();
    private final Handler mainHandler = new Handler(Looper.getMainLooper());

    private VideoView videoView;
    private MediaPlayer activeMediaPlayer;
    private LinearLayout panelTopBar;
    private LinearLayout panelVideoControls;
    private LinearLayout panelTrim;
    private SeekBar seekVideo;
    private TextView txtVideoTime;
    private TextView txtSeekFeedback;
    private TextView txtTrimRange;
    private RangeSlider sliderTrim;
    private Button btnPlayPause;
    private Button btnSpeed;
    private Button btnFullscreen;
    private Button btnTrimSave;

    private boolean isFullscreen = false;
    private boolean trimOpen = false;
    private boolean userSeeking = false;
    private float currentSpeed = 1.0f;
    private int durationMs = 0;

    private final Runnable progressRunnable = new Runnable() {
        @Override
        public void run() {
            if (videoView != null && durationMs > 0 && !userSeeking) {
                int pos = videoView.getCurrentPosition();
                seekVideo.setProgress(pos);
                txtVideoTime.setText(fmtMs(pos) + " / " + fmtMs(durationMs));
            }
            mainHandler.postDelayed(this, 250);
        }
    };

    private final Runnable hideFeedbackRunnable = () -> {
        if (txtSeekFeedback != null) txtSeekFeedback.setVisibility(View.GONE);
    };

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        setContentView(R.layout.activity_player);

        String host = getIntent().getStringExtra(EXTRA_HOST);
        int port = getIntent().getIntExtra(EXTRA_PORT, 47820);
        String pin = getIntent().getStringExtra(EXTRA_PIN);
        String file = getIntent().getStringExtra(EXTRA_FILE);
        long size = getIntent().getLongExtra(EXTRA_SIZE, 0);

        panelTopBar = findViewById(R.id.panelTopBar);
        panelVideoControls = findViewById(R.id.panelVideoControls);
        panelTrim = findViewById(R.id.panelTrim);
        seekVideo = findViewById(R.id.seekVideo);
        txtVideoTime = findViewById(R.id.txtVideoTime);
        txtSeekFeedback = findViewById(R.id.txtSeekFeedback);
        txtTrimRange = findViewById(R.id.txtTrimRange);
        sliderTrim = findViewById(R.id.sliderTrim);
        btnPlayPause = findViewById(R.id.btnPlayPause);
        btnSpeed = findViewById(R.id.btnSpeed);
        btnFullscreen = findViewById(R.id.btnFullscreen);
        btnTrimSave = findViewById(R.id.btnTrimSave);

        Button btnBack = findViewById(R.id.btnBack);
        TextView txtTitle = findViewById(R.id.txtPlayerTitle);
        Button btnToggleTrim = findViewById(R.id.btnToggleTrim);
        Button btnDownload = findViewById(R.id.btnPlayerDownload);
        FrameLayout videoContainer = findViewById(R.id.videoContainer);
        videoView = findViewById(R.id.videoView);
        ProgressBar progressVideo = findViewById(R.id.progressVideo);

        txtTitle.setText(file);
        btnBack.setOnClickListener(v -> finish());

        String streamUrl = ApiClient.buildClipStreamUrl(host, port, pin, file);
        videoView.setVideoURI(Uri.parse(streamUrl));
        videoView.setOnPreparedListener(mp -> {
            activeMediaPlayer = mp;
            progressVideo.setVisibility(View.GONE);
            durationMs = Math.max(1000, mp.getDuration());
            seekVideo.setMax(durationMs);
            float durSec = durationMs / 1000.0f;
            sliderTrim.setValueFrom(0.0f);
            sliderTrim.setValueTo(Math.max(1.0f, durSec));
            sliderTrim.setValues(0.0f, Math.max(1.0f, durSec));
            updateTrimLabel();
            mp.start();
            btnPlayPause.setText("⏸");
            mainHandler.post(progressRunnable);
        });
        videoView.setOnCompletionListener(mp -> btnPlayPause.setText("▶"));
        videoView.setOnErrorListener((mp, what, extra) -> {
            progressVideo.setVisibility(View.GONE);
            Toast.makeText(this, "Video oynatılamadı", Toast.LENGTH_SHORT).show();
            return true;
        });

        GestureDetector gd = new GestureDetector(this, new GestureDetector.SimpleOnGestureListener() {
            @Override
            public boolean onSingleTapConfirmed(MotionEvent e) {
                int vis = panelVideoControls.getVisibility() == View.VISIBLE ? View.GONE : View.VISIBLE;
                panelVideoControls.setVisibility(vis);
                return true;
            }

            @Override
            public boolean onDoubleTap(MotionEvent e) {
                if (durationMs <= 0) return true;
                boolean forward = e.getX() >= videoContainer.getWidth() * 0.5f;
                int delta = forward ? 5000 : -5000;
                int target = Math.max(0, Math.min(durationMs, videoView.getCurrentPosition() + delta));
                videoView.seekTo(target);
                seekVideo.setProgress(target);
                txtSeekFeedback.setText(forward ? "+5 sn ↻" : "↺ -5 sn");
                txtSeekFeedback.setVisibility(View.VISIBLE);
                mainHandler.removeCallbacks(hideFeedbackRunnable);
                mainHandler.postDelayed(hideFeedbackRunnable, 750);
                return true;
            }
        });
        videoContainer.setOnTouchListener((v, event) -> {
            gd.onTouchEvent(event);
            return true;
        });

        seekVideo.setOnSeekBarChangeListener(new SeekBar.OnSeekBarChangeListener() {
            @Override public void onProgressChanged(SeekBar sb, int progress, boolean fromUser) {
                if (fromUser) txtVideoTime.setText(fmtMs(progress) + " / " + fmtMs(durationMs));
            }
            @Override public void onStartTrackingTouch(SeekBar sb) { userSeeking = true; }
            @Override public void onStopTrackingTouch(SeekBar sb) {
                userSeeking = false;
                videoView.seekTo(sb.getProgress());
            }
        });

        btnPlayPause.setOnClickListener(v -> {
            if (videoView.isPlaying()) {
                videoView.pause();
                btnPlayPause.setText("▶");
            } else {
                videoView.start();
                btnPlayPause.setText("⏸");
            }
        });

        btnSpeed.setOnClickListener(v -> {
            if (currentSpeed < 0.75f) currentSpeed = 1.0f;
            else if (currentSpeed < 1.25f) currentSpeed = 1.5f;
            else if (currentSpeed < 1.75f) currentSpeed = 2.0f;
            else currentSpeed = 0.5f;
            btnSpeed.setText(String.format(Locale.US, "%.1fx", currentSpeed));
            if (activeMediaPlayer != null) {
                try {
                    PlaybackParams pp = activeMediaPlayer.getPlaybackParams();
                    pp.setSpeed(currentSpeed);
                    activeMediaPlayer.setPlaybackParams(pp);
                } catch (Exception ignored) {
                }
            }
        });

        btnFullscreen.setOnClickListener(v -> toggleFullscreen());

        btnToggleTrim.setOnClickListener(v -> {
            if (isFullscreen) toggleFullscreen();
            trimOpen = !trimOpen;
            panelTrim.setVisibility(trimOpen ? View.VISIBLE : View.GONE);
        });

        sliderTrim.addOnChangeListener((slider, value, fromUser) -> {
            updateTrimLabel();
            if (fromUser && videoView != null) {
                videoView.seekTo((int) (value * 1000f));
            }
        });

        btnTrimSave.setOnClickListener(v -> {
            List<Float> vals = sliderTrim.getValues();
            if (vals.size() < 2) return;
            long startMs = (long) (vals.get(0) * 1000f);
            long endMs = (long) (vals.get(1) * 1000f);
            if (endMs - startMs < 500) {
                Toast.makeText(this, "Kırpma aralığı en az 0.5 saniye olmalı", Toast.LENGTH_SHORT).show();
                return;
            }
            btnTrimSave.setEnabled(false);
            btnTrimSave.setText("Kırpılıyor...");
            executor.execute(() -> {
                try {
                    VideoTrimmer.trimUrlToGallery(this, streamUrl, pin, file, startMs, endMs);
                    runOnUiThread(() -> {
                        btnTrimSave.setEnabled(true);
                        btnTrimSave.setText("✂ Kırp & Galeriye Kaydet");
                        Toast.makeText(this, "Kırpılan klip galeriye kaydedildi: Movies/AnlikKlip", Toast.LENGTH_LONG).show();
                    });
                } catch (Exception e) {
                    runOnUiThread(() -> {
                        btnTrimSave.setEnabled(true);
                        btnTrimSave.setText("✂ Kırp & Galeriye Kaydet");
                        Toast.makeText(this, "Kırpma hatası: " + e.getMessage(), Toast.LENGTH_LONG).show();
                    });
                }
            });
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

    private void toggleFullscreen() {
        isFullscreen = !isFullscreen;
        if (isFullscreen) {
            panelTopBar.setVisibility(View.GONE);
            panelTrim.setVisibility(View.GONE);
            trimOpen = false;
            getWindow().addFlags(WindowManager.LayoutParams.FLAG_FULLSCREEN);
            setRequestedOrientation(ActivityInfo.SCREEN_ORIENTATION_SENSOR_LANDSCAPE);
            btnFullscreen.setText("✕");
        } else {
            panelTopBar.setVisibility(View.VISIBLE);
            getWindow().clearFlags(WindowManager.LayoutParams.FLAG_FULLSCREEN);
            setRequestedOrientation(ActivityInfo.SCREEN_ORIENTATION_UNSPECIFIED);
            btnFullscreen.setText("⛶");
        }
    }

    private void updateTrimLabel() {
        List<Float> vals = sliderTrim.getValues();
        if (vals.size() >= 2) {
            txtTrimRange.setText("Kırpma: " + fmtMs((long) (vals.get(0) * 1000f)) +
                    " → " + fmtMs((long) (vals.get(1) * 1000f)));
        }
    }

    private static String fmtMs(long ms) {
        long sec = Math.max(0, ms / 1000L);
        return String.format(Locale.US, "%02d:%02d", sec / 60, sec % 60);
    }

    @Override
    protected void onDestroy() {
        super.onDestroy();
        mainHandler.removeCallbacks(progressRunnable);
        mainHandler.removeCallbacks(hideFeedbackRunnable);
        if (videoView != null) videoView.stopPlayback();
        executor.shutdownNow();
    }
}

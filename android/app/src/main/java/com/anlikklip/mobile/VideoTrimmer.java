package com.anlikklip.mobile;

import android.content.ContentResolver;
import android.content.ContentValues;
import android.content.Context;
import android.media.MediaCodec;
import android.media.MediaExtractor;
import android.media.MediaFormat;
import android.media.MediaMuxer;
import android.media.MediaScannerConnection;
import android.net.Uri;
import android.os.Build;
import android.os.Environment;
import android.provider.MediaStore;

import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.InputStream;
import java.io.OutputStream;
import java.net.HttpURLConnection;
import java.net.URL;
import java.nio.ByteBuffer;

public class VideoTrimmer {

    public static void trimUrlToGallery(
            Context context,
            String streamUrl,
            String pin,
            String baseFileName,
            long startMs,
            long endMs
    ) throws Exception {
        File cacheDir = new File(context.getCacheDir(), "trim_tmp");
        if (!cacheDir.exists()) cacheDir.mkdirs();

        File srcTmp = new File(cacheDir, "src.mp4");
        File outTmp = new File(cacheDir, "out.mp4");
        if (srcTmp.exists()) srcTmp.delete();
        if (outTmp.exists()) outTmp.delete();

        try {
            // 1. Download source MP4 to app cache
            HttpURLConnection conn = null;
            try {
                URL url = new URL(streamUrl);
                conn = (HttpURLConnection) url.openConnection();
                conn.setConnectTimeout(8000);
                conn.setReadTimeout(30000);
                conn.setRequestProperty("X-Pin", pin);
                int code = conn.getResponseCode();
                if (code != 200) throw new IllegalStateException("Sunucu hatası: HTTP " + code);
                try (InputStream is = conn.getInputStream();
                     OutputStream os = new FileOutputStream(srcTmp)) {
                    byte[] buf = new byte[64 * 1024];
                    int r;
                    while ((r = is.read(buf)) != -1) os.write(buf, 0, r);
                }
            } finally {
                if (conn != null) conn.disconnect();
            }

            // 2. Fast keyframe-aligned MediaExtractor -> MediaMuxer trim
            trimLocalMp4(srcTmp.getAbsolutePath(), outTmp.getAbsolutePath(), startMs * 1000L, endMs * 1000L);

            // 3. Save trimmed MP4 to Gallery (Movies/AnlikKlip)
            String stem = baseFileName.endsWith(".mp4")
                    ? baseFileName.substring(0, baseFileName.length() - 4)
                    : baseFileName;
            String outName = stem + "_kesit.mp4";
            saveFileToGallery(context, outTmp, outName);
        } finally {
            if (srcTmp.exists()) srcTmp.delete();
            if (outTmp.exists()) outTmp.delete();
        }
    }

    private static void trimLocalMp4(String srcPath, String dstPath, long startUs, long endUs) throws Exception {
        MediaExtractor extractor = new MediaExtractor();
        MediaMuxer muxer = null;
        try {
            extractor.setDataSource(srcPath);
            int trackCount = extractor.getTrackCount();
            int videoTrack = -1;
            int audioTrack = -1;

            muxer = new MediaMuxer(dstPath, MediaMuxer.OutputFormat.MUXER_OUTPUT_MPEG_4);
            int muxVideo = -1;
            int muxAudio = -1;
            int maxBufSize = 512 * 1024;

            for (int i = 0; i < trackCount; i++) {
                MediaFormat fmt = extractor.getTrackFormat(i);
                String mime = fmt.getString(MediaFormat.KEY_MIME);
                if (mime == null) continue;
                if (mime.startsWith("video/") && videoTrack < 0) {
                    videoTrack = i;
                    muxVideo = muxer.addTrack(fmt);
                    if (fmt.containsKey(MediaFormat.KEY_MAX_INPUT_SIZE)) {
                        maxBufSize = Math.max(maxBufSize, fmt.getInteger(MediaFormat.KEY_MAX_INPUT_SIZE));
                    }
                } else if (mime.startsWith("audio/") && audioTrack < 0) {
                    // Use Track 0 (Mix audio)
                    audioTrack = i;
                    muxAudio = muxer.addTrack(fmt);
                }
            }

            if (videoTrack < 0) throw new IllegalStateException("Video izi bulunamadı");

            extractor.selectTrack(videoTrack);
            if (audioTrack >= 0) extractor.selectTrack(audioTrack);
            extractor.seekTo(startUs, MediaExtractor.SEEK_TO_PREVIOUS_SYNC);

            muxer.start();
            ByteBuffer buffer = ByteBuffer.allocate(maxBufSize);
            MediaCodec.BufferInfo info = new MediaCodec.BufferInfo();
            long basePtsUs = -1;

            while (true) {
                int trackIdx = extractor.getSampleTrackIndex();
                if (trackIdx < 0) break;

                long ptsUs = extractor.getSampleTime();
                if (ptsUs > endUs) break;

                if (trackIdx == videoTrack && basePtsUs < 0) {
                    basePtsUs = ptsUs;
                }
                if (basePtsUs < 0 || ptsUs < basePtsUs) {
                    extractor.advance();
                    continue;
                }

                int size = extractor.readSampleData(buffer, 0);
                if (size < 0) break;

                info.offset = 0;
                info.size = size;
                info.presentationTimeUs = ptsUs - basePtsUs;
                int flags = extractor.getSampleFlags();
                info.flags = (flags & MediaExtractor.SAMPLE_FLAG_SYNC) != 0
                        ? MediaCodec.BUFFER_FLAG_KEY_FRAME
                        : 0;

                if (trackIdx == videoTrack) {
                    muxer.writeSampleData(muxVideo, buffer, info);
                } else if (trackIdx == audioTrack && muxAudio >= 0) {
                    muxer.writeSampleData(muxAudio, buffer, info);
                }
                extractor.advance();
            }
        } finally {
            try { extractor.release(); } catch (Exception ignored) {}
            if (muxer != null) {
                try { muxer.stop(); } catch (Exception ignored) {}
                try { muxer.release(); } catch (Exception ignored) {}
            }
        }
    }

    private static void saveFileToGallery(Context context, File file, String displayName) throws Exception {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q) {
            ContentResolver resolver = context.getContentResolver();
            ContentValues values = new ContentValues();
            values.put(MediaStore.Video.Media.DISPLAY_NAME, displayName);
            values.put(MediaStore.Video.Media.MIME_TYPE, "video/mp4");
            values.put(MediaStore.Video.Media.RELATIVE_PATH, Environment.DIRECTORY_MOVIES + "/AnlikKlip");
            values.put(MediaStore.Video.Media.IS_PENDING, 1);

            Uri uri = resolver.insert(MediaStore.Video.Media.EXTERNAL_CONTENT_URI, values);
            if (uri == null) throw new IllegalStateException("Galeriye dosya oluşturulamadı");

            try (InputStream is = new FileInputStream(file);
                 OutputStream os = resolver.openOutputStream(uri)) {
                if (os == null) throw new IllegalStateException("Çıkış akışı açılamadı");
                byte[] buf = new byte[64 * 1024];
                int r;
                while ((r = is.read(buf)) != -1) os.write(buf, 0, r);
                os.flush();
            } catch (Exception e) {
                resolver.delete(uri, null, null);
                throw e;
            }

            ContentValues done = new ContentValues();
            done.put(MediaStore.Video.Media.IS_PENDING, 0);
            resolver.update(uri, done, null, null);
        } else {
            File dir = new File(Environment.getExternalStoragePublicDirectory(Environment.DIRECTORY_MOVIES), "AnlikKlip");
            if (!dir.exists() && !dir.mkdirs()) throw new IllegalStateException("Klasör oluşturulamadı");
            File dst = new File(dir, displayName);
            try (InputStream is = new FileInputStream(file);
                 OutputStream os = new FileOutputStream(dst)) {
                byte[] buf = new byte[64 * 1024];
                int r;
                while ((r = is.read(buf)) != -1) os.write(buf, 0, r);
                os.flush();
            }
            MediaScannerConnection.scanFile(context, new String[]{dst.getAbsolutePath()}, new String[]{"video/mp4"}, null);
        }
    }
}

package com.anlikklip.mobile;

import android.content.ContentResolver;
import android.content.ContentValues;
import android.content.Context;
import android.graphics.Bitmap;
import android.graphics.BitmapFactory;
import android.media.MediaScannerConnection;
import android.net.Uri;
import android.os.Build;
import android.os.Environment;
import android.provider.MediaStore;
import android.util.LruCache;

import org.json.JSONArray;
import org.json.JSONObject;

import java.io.BufferedReader;
import java.io.File;
import java.io.FileOutputStream;
import java.io.InputStream;
import java.io.InputStreamReader;
import java.io.OutputStream;
import java.net.HttpURLConnection;
import java.net.URL;
import java.net.URLEncoder;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.List;

public class ApiClient {
    public static class ServerInfo {
        public String pcName;
        public int port;
        public int clipCount;
        public String version;
    }

    public interface ProgressCallback {
        void onProgress(int percent);
    }

    private static final LruCache<String, Bitmap> thumbCache = new LruCache<>(64);

    public static void clearThumbCache() {
        thumbCache.evictAll();
    }

    private static String baseUrl(String host, int port) {
        return "http://" + host.trim() + ":" + port;
    }

    private static String encode(String s) {
        try {
            return URLEncoder.encode(s, "UTF-8").replace("+", "%20");
        } catch (Exception e) {
            return s;
        }
    }

    public static String buildClipStreamUrl(String host, int port, String pin, String fileName) {
        return baseUrl(host, port) + "/api/clip?file=" + encode(fileName) + "&pin=" + encode(pin);
    }

    public static ServerInfo fetchInfo(String host, int port, String pin) throws Exception {
        String json = httpGetString(baseUrl(host, port) + "/api/info", pin);
        JSONObject obj = new JSONObject(json);
        ServerInfo info = new ServerInfo();
        info.pcName = obj.optString("pcName", "PC");
        info.port = obj.optInt("port", port);
        info.clipCount = obj.optInt("clipCount", 0);
        info.version = obj.optString("version", "1.2.0");
        return info;
    }

    public static List<ClipItem> fetchClips(String host, int port, String pin) throws Exception {
        String json = httpGetString(baseUrl(host, port) + "/api/clips", pin);
        JSONArray arr = new JSONArray(json);
        List<ClipItem> list = new ArrayList<>(arr.length());
        for (int i = 0; i < arr.length(); i++) {
            JSONObject o = arr.getJSONObject(i);
            ClipItem item = new ClipItem();
            item.fileName = o.optString("fileName");
            item.sizeBytes = o.optLong("sizeBytes", 0);
            item.durationSec = o.optDouble("durationSec", 0.0);
            item.width = o.optInt("width", 0);
            item.height = o.optInt("height", 0);
            item.date = o.optString("date", "");
            item.isFavorite = o.optBoolean("isFavorite", false);
            list.add(item);
        }
        return list;
    }

    public static Bitmap fetchThumbnail(String host, int port, String pin, String fileName) {
        String key = host + ":" + port + "/" + fileName;
        Bitmap cached = thumbCache.get(key);
        if (cached != null) return cached;

        HttpURLConnection conn = null;
        try {
            URL url = new URL(baseUrl(host, port) + "/api/thumb?file=" + encode(fileName));
            conn = (HttpURLConnection) url.openConnection();
            conn.setConnectTimeout(4000);
            conn.setReadTimeout(6000);
            conn.setRequestProperty("X-Pin", pin);
            if (conn.getResponseCode() == 200) {
                try (InputStream is = conn.getInputStream()) {
                    Bitmap bmp = BitmapFactory.decodeStream(is);
                    if (bmp != null) {
                        thumbCache.put(key, bmp);
                    }
                    return bmp;
                }
            }
        } catch (Exception ignored) {
        } finally {
            if (conn != null) conn.disconnect();
        }
        return null;
    }

    public static void downloadClipToGallery(
            Context context,
            String host,
            int port,
            String pin,
            ClipItem clip,
            ProgressCallback progressCallback
    ) throws Exception {
        HttpURLConnection conn = null;
        try {
            URL url = new URL(buildClipStreamUrl(host, port, pin, clip.fileName));
            conn = (HttpURLConnection) url.openConnection();
            conn.setConnectTimeout(6000);
            conn.setReadTimeout(30000);
            conn.setRequestProperty("X-Pin", pin);

            int code = conn.getResponseCode();
            if (code == 401) throw new SecurityException("PIN hatalı");
            if (code != 200) throw new IllegalStateException("Sunucu hatası: HTTP " + code);

            long totalBytes = conn.getContentLengthLong();
            if (totalBytes <= 0) totalBytes = clip.sizeBytes;

            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q) {
                ContentResolver resolver = context.getContentResolver();
                ContentValues values = new ContentValues();
                values.put(MediaStore.Video.Media.DISPLAY_NAME, clip.fileName);
                values.put(MediaStore.Video.Media.MIME_TYPE, "video/mp4");
                values.put(MediaStore.Video.Media.RELATIVE_PATH, Environment.DIRECTORY_MOVIES + "/AnlikKlip");
                values.put(MediaStore.Video.Media.IS_PENDING, 1);

                Uri uri = resolver.insert(MediaStore.Video.Media.EXTERNAL_CONTENT_URI, values);
                if (uri == null) throw new IllegalStateException("Galeriye dosya oluşturulamadı");

                try (InputStream is = conn.getInputStream();
                     OutputStream os = resolver.openOutputStream(uri)) {
                    if (os == null) throw new IllegalStateException("Çıkış akışı açılamadı");
                    copyStreamWithProgress(is, os, totalBytes, progressCallback);
                } catch (Exception e) {
                    resolver.delete(uri, null, null);
                    throw e;
                }

                ContentValues doneValues = new ContentValues();
                doneValues.put(MediaStore.Video.Media.IS_PENDING, 0);
                resolver.update(uri, doneValues, null, null);
            } else {
                File dir = new File(Environment.getExternalStoragePublicDirectory(Environment.DIRECTORY_MOVIES), "AnlikKlip");
                if (!dir.exists() && !dir.mkdirs()) {
                    throw new IllegalStateException("Klasör oluşturulamadı");
                }
                File outFile = new File(dir, clip.fileName);
                try (InputStream is = conn.getInputStream();
                     OutputStream os = new FileOutputStream(outFile)) {
                    copyStreamWithProgress(is, os, totalBytes, progressCallback);
                }
                MediaScannerConnection.scanFile(context, new String[]{outFile.getAbsolutePath()}, new String[]{"video/mp4"}, null);
            }
        } finally {
            if (conn != null) conn.disconnect();
        }
    }

    private static void copyStreamWithProgress(InputStream is, OutputStream os, long totalBytes, ProgressCallback cb) throws Exception {
        byte[] buf = new byte[64 * 1024];
        long copied = 0;
        int lastPct = -1;
        int r;
        while ((r = is.read(buf)) != -1) {
            os.write(buf, 0, r);
            copied += r;
            if (cb != null && totalBytes > 0) {
                int pct = (int) Math.min(100, (copied * 100L) / totalBytes);
                if (pct != lastPct) {
                    lastPct = pct;
                    cb.onProgress(pct);
                }
            }
        }
        os.flush();
    }

    private static String httpGetString(String urlStr, String pin) throws Exception {
        HttpURLConnection conn = null;
        try {
            URL url = new URL(urlStr);
            conn = (HttpURLConnection) url.openConnection();
            conn.setConnectTimeout(4000);
            conn.setReadTimeout(8000);
            conn.setRequestProperty("X-Pin", pin);
            int code = conn.getResponseCode();
            if (code == 401) throw new SecurityException("PIN hatalı");
            if (code != 200) throw new IllegalStateException("HTTP " + code);

            StringBuilder sb = new StringBuilder();
            try (BufferedReader br = new BufferedReader(new InputStreamReader(conn.getInputStream(), StandardCharsets.UTF_8))) {
                String line;
                while ((line = br.readLine()) != null) sb.append(line);
            }
            return sb.toString();
        } finally {
            if (conn != null) conn.disconnect();
        }
    }
}

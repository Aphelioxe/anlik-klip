package com.anlikklip.mobile;

public class ClipItem {
    public String fileName = "";
    public long sizeBytes = 0;
    public double durationSec = 0.0;
    public int width = 0;
    public int height = 0;
    public String date = "";
    public boolean isFavorite = false;

    public boolean downloaded = false;
    public boolean downloading = false;
    public int downloadProgress = 0;

    public String getResolutionLabel() {
        if (height >= 1080) return "1080p";
        if (height >= 720) return "720p";
        if (height > 0) return height + "p";
        return "MP4";
    }

    public String getFormattedDuration() {
        int total = (int) Math.round(durationSec);
        int m = total / 60;
        int s = total % 60;
        return String.format("%02d:%02d", m, s);
    }

    public String getFormattedSize() {
        double mb = sizeBytes / (1024.0 * 1024.0);
        if (mb >= 1000.0) {
            return String.format("%.2f GB", mb / 1024.0);
        }
        return String.format("%.1f MB", mb);
    }
}

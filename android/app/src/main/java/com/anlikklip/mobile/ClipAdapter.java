package com.anlikklip.mobile;

import android.graphics.Bitmap;
import android.os.Handler;
import android.os.Looper;
import android.view.LayoutInflater;
import android.view.View;
import android.view.ViewGroup;
import android.widget.Button;
import android.widget.ImageView;
import android.widget.LinearLayout;
import android.widget.ProgressBar;
import android.widget.TextView;

import androidx.annotation.NonNull;
import androidx.core.content.ContextCompat;
import androidx.recyclerview.widget.RecyclerView;

import java.util.ArrayList;
import java.util.HashSet;
import java.util.List;
import java.util.Set;
import java.util.concurrent.ExecutorService;

public class ClipAdapter extends RecyclerView.Adapter<ClipAdapter.Holder> {
    public interface Listener {
        void onPlayClip(ClipItem item);
        void onDownloadClip(ClipItem item);
        void onSelectionChanged(int selectedCount, boolean selectionMode);
    }

    private final List<ClipItem> items = new ArrayList<>();
    private final Set<String> selectedFiles = new HashSet<>();
    private final ExecutorService executor;
    private final Handler mainHandler = new Handler(Looper.getMainLooper());
    private final Listener listener;

    private boolean selectionMode = false;
    private String host = "";
    private int port = 47820;
    private String pin = "";

    public ClipAdapter(ExecutorService executor, Listener listener) {
        this.executor = executor;
        this.listener = listener;
    }

    public void setConnection(String host, int port, String pin) {
        this.host = host;
        this.port = port;
        this.pin = pin;
    }

    public void setItems(List<ClipItem> newItems) {
        items.clear();
        items.addAll(newItems);
        notifyDataSetChanged();
    }

    public boolean isSelectionMode() {
        return selectionMode;
    }

    public List<ClipItem> getSelectedItems() {
        List<ClipItem> out = new ArrayList<>();
        for (ClipItem c : items) {
            if (selectedFiles.contains(c.fileName)) out.add(c);
        }
        return out;
    }

    public void selectAll() {
        selectionMode = true;
        selectedFiles.clear();
        for (ClipItem c : items) selectedFiles.add(c.fileName);
        notifyDataSetChanged();
        listener.onSelectionChanged(selectedFiles.size(), true);
    }

    public void clearSelection() {
        selectionMode = false;
        selectedFiles.clear();
        notifyDataSetChanged();
        listener.onSelectionChanged(0, false);
    }

    public void notifyClipUpdated(String fileName) {
        for (int i = 0; i < items.size(); i++) {
            if (items.get(i).fileName.equals(fileName)) {
                notifyItemChanged(i);
                return;
            }
        }
    }

    @NonNull
    @Override
    public Holder onCreateViewHolder(@NonNull ViewGroup parent, int viewType) {
        View v = LayoutInflater.from(parent.getContext()).inflate(R.layout.item_clip, parent, false);
        return new Holder(v);
    }

    @Override
    public void onBindViewHolder(@NonNull Holder holder, int position) {
        ClipItem item = items.get(position);
        holder.boundFile = item.fileName;

        boolean isSelected = selectionMode && selectedFiles.contains(item.fileName);
        holder.cardRoot.setBackgroundResource(isSelected ? R.drawable.bg_card_selected : R.drawable.bg_card);
        holder.txtSelectedBadge.setVisibility(isSelected ? View.VISIBLE : View.GONE);

        holder.txtTitle.setText(item.fileName);
        holder.txtMeta.setText(item.date + " • " + item.getFormattedSize() + " • " + item.getFormattedDuration());
        holder.txtDuration.setText(item.getFormattedDuration());
        String tag = extractGameTag(item.fileName);
        holder.txtResBadge.setText(tag.isEmpty() ? item.getResolutionLabel() : (tag + " • " + item.getResolutionLabel()));
        holder.txtFavBadge.setVisibility(item.isFavorite ? View.VISIBLE : View.GONE);

        if (item.downloading) {
            holder.btnDownload.setEnabled(false);
            holder.btnDownload.setText(item.downloadProgress + "%");
            holder.btnDownload.setBackgroundTintList(ContextCompat.getColorStateList(holder.itemView.getContext(), R.color.bg_input));
            holder.progressDownload.setVisibility(View.VISIBLE);
            holder.progressDownload.setProgress(item.downloadProgress);
        } else if (item.downloaded) {
            holder.btnDownload.setEnabled(true);
            holder.btnDownload.setText(R.string.downloaded);
            holder.btnDownload.setBackgroundTintList(ContextCompat.getColorStateList(holder.itemView.getContext(), R.color.accent_green));
            holder.progressDownload.setVisibility(View.GONE);
        } else {
            holder.btnDownload.setEnabled(true);
            holder.btnDownload.setText(R.string.download_to_gallery);
            holder.btnDownload.setBackgroundTintList(ContextCompat.getColorStateList(holder.itemView.getContext(), R.color.accent_blue));
            holder.progressDownload.setVisibility(View.GONE);
        }

        holder.imgThumb.setImageBitmap(null);
        holder.imgThumb.setClipToOutline(true);
        final String targetFile = item.fileName;
        executor.execute(() -> {
            Bitmap bmp = ApiClient.fetchThumbnail(host, port, pin, targetFile);
            if (bmp != null) {
                mainHandler.post(() -> {
                    if (targetFile.equals(holder.boundFile)) {
                        holder.imgThumb.setImageBitmap(bmp);
                    }
                });
            }
        });

        holder.itemView.setOnClickListener(v -> {
            if (selectionMode) {
                toggleSelection(item.fileName);
            } else {
                listener.onPlayClip(item);
            }
        });

        holder.itemView.setOnLongClickListener(v -> {
            if (!selectionMode) {
                selectionMode = true;
            }
            toggleSelection(item.fileName);
            return true;
        });

        holder.btnDownload.setOnClickListener(v -> listener.onDownloadClip(item));
    }

    private void toggleSelection(String fileName) {
        if (selectedFiles.contains(fileName)) {
            selectedFiles.remove(fileName);
            if (selectedFiles.isEmpty()) selectionMode = false;
        } else {
            selectedFiles.add(fileName);
        }
        notifyDataSetChanged();
        listener.onSelectionChanged(selectedFiles.size(), selectionMode);
    }

    private static String extractGameTag(String fileName) {
        if (fileName == null) return "";
        int us = fileName.indexOf('_');
        if (us <= 1 || us > 20) return "";
        String prefix = fileName.substring(0, us);
        if ("Klip".equalsIgnoreCase(prefix)) return "";
        return prefix;
    }

    @Override
    public int getItemCount() {
        return items.size();
    }

    static class Holder extends RecyclerView.ViewHolder {
        String boundFile = "";
        final LinearLayout cardRoot;
        final ImageView imgThumb;
        final TextView txtResBadge;
        final TextView txtSelectedBadge;
        final TextView txtFavBadge;
        final TextView txtDuration;
        final TextView txtTitle;
        final TextView txtMeta;
        final Button btnDownload;
        final ProgressBar progressDownload;

        Holder(@NonNull View itemView) {
            super(itemView);
            cardRoot = itemView.findViewById(R.id.cardRoot);
            imgThumb = itemView.findViewById(R.id.imgThumb);
            txtResBadge = itemView.findViewById(R.id.txtResBadge);
            txtSelectedBadge = itemView.findViewById(R.id.txtSelectedBadge);
            txtFavBadge = itemView.findViewById(R.id.txtFavBadge);
            txtDuration = itemView.findViewById(R.id.txtDuration);
            txtTitle = itemView.findViewById(R.id.txtTitle);
            txtMeta = itemView.findViewById(R.id.txtMeta);
            btnDownload = itemView.findViewById(R.id.btnDownload);
            progressDownload = itemView.findViewById(R.id.progressDownload);
        }
    }
}

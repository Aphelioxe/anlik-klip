package com.anlikklip.mobile;

import android.graphics.Bitmap;
import android.os.Handler;
import android.os.Looper;
import android.view.LayoutInflater;
import android.view.View;
import android.view.ViewGroup;
import android.widget.Button;
import android.widget.ImageView;
import android.widget.ProgressBar;
import android.widget.TextView;

import androidx.annotation.NonNull;
import androidx.core.content.ContextCompat;
import androidx.recyclerview.widget.RecyclerView;

import java.util.ArrayList;
import java.util.List;
import java.util.concurrent.ExecutorService;

public class ClipAdapter extends RecyclerView.Adapter<ClipAdapter.Holder> {
    public interface Listener {
        void onPlayClip(ClipItem item);
        void onDownloadClip(ClipItem item);
    }

    private final List<ClipItem> items = new ArrayList<>();
    private final ExecutorService executor;
    private final Handler mainHandler = new Handler(Looper.getMainLooper());
    private final Listener listener;

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

        holder.txtTitle.setText(item.fileName);
        holder.txtMeta.setText(item.date + " • " + item.getFormattedSize());
        holder.txtDuration.setText(item.getFormattedDuration());
        holder.txtResBadge.setText(item.getResolutionLabel());
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

        holder.itemView.setOnClickListener(v -> listener.onPlayClip(item));
        holder.btnDownload.setOnClickListener(v -> listener.onDownloadClip(item));
    }

    @Override
    public int getItemCount() {
        return items.size();
    }

    static class Holder extends RecyclerView.ViewHolder {
        String boundFile = "";
        final ImageView imgThumb;
        final TextView txtResBadge;
        final TextView txtFavBadge;
        final TextView txtDuration;
        final TextView txtTitle;
        final TextView txtMeta;
        final Button btnDownload;
        final ProgressBar progressDownload;

        Holder(@NonNull View itemView) {
            super(itemView);
            imgThumb = itemView.findViewById(R.id.imgThumb);
            txtResBadge = itemView.findViewById(R.id.txtResBadge);
            txtFavBadge = itemView.findViewById(R.id.txtFavBadge);
            txtDuration = itemView.findViewById(R.id.txtDuration);
            txtTitle = itemView.findViewById(R.id.txtTitle);
            txtMeta = itemView.findViewById(R.id.txtMeta);
            btnDownload = itemView.findViewById(R.id.btnDownload);
            progressDownload = itemView.findViewById(R.id.progressDownload);
        }
    }
}

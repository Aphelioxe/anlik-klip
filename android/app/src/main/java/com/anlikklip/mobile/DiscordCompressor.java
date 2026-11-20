package com.anlikklip.mobile;

import android.content.Context;
import android.media.MediaCodec;
import android.media.MediaCodecInfo;
import android.media.MediaExtractor;
import android.media.MediaFormat;
import android.media.MediaMuxer;
import android.opengl.EGL14;
import android.opengl.EGLConfig;
import android.opengl.EGLContext;
import android.opengl.EGLDisplay;
import android.opengl.EGLExt;
import android.opengl.EGLSurface;
import android.opengl.GLES11Ext;
import android.opengl.GLES20;
import android.opengl.Matrix;
import android.graphics.SurfaceTexture;
import android.view.Surface;

import java.io.File;
import java.io.FileOutputStream;
import java.io.InputStream;
import java.io.OutputStream;
import java.net.HttpURLConnection;
import java.net.URL;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.nio.FloatBuffer;

/**
 * Compresses an MP4 clip to <= 9.5 MB (Discord-ready) using native Android hardware MediaCodec
 * (Surface-to-Surface transcode) + passthrough AAC audio, with zero external dependencies.
 */
public class DiscordCompressor {

    public interface ProgressListener {
        void onProgress(String statusText);
    }

    public static File compressUrlTo10Mb(
            Context context,
            String streamUrl,
            String pin,
            String fileName,
            ProgressListener listener
    ) throws Exception {
        File cacheDir = new File(context.getCacheDir(), "share_tmp");
        if (!cacheDir.exists()) cacheDir.mkdirs();

        File srcTmp = new File(cacheDir, "dl_src.mp4");
        File outFile = new File(cacheDir, "discord_" + (fileName.endsWith(".mp4") ? fileName : (fileName + ".mp4")));
        if (srcTmp.exists()) srcTmp.delete();
        if (outFile.exists()) outFile.delete();

        try {
            if (listener != null) listener.onProgress("İndiriliyor...");
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

            if (srcTmp.length() <= 9_500_000L) {
                if (!srcTmp.renameTo(outFile)) {
                    copyFile(srcTmp, outFile);
                }
                return outFile;
            }

            if (listener != null) listener.onProgress("10 MB'a sıkıştırılıyor...");
            transcodeToTargetSize(srcTmp.getAbsolutePath(), outFile.getAbsolutePath(), 9_400_000L);
            return outFile;
        } finally {
            if (srcTmp.exists()) srcTmp.delete();
        }
    }

    private static void copyFile(File src, File dst) throws Exception {
        try (InputStream is = new java.io.FileInputStream(src);
             OutputStream os = new FileOutputStream(dst)) {
            byte[] b = new byte[64 * 1024];
            int r;
            while ((r = is.read(b)) != -1) os.write(b, 0, r);
        }
    }

    private static void transcodeToTargetSize(String srcPath, String dstPath, long targetBytes) throws Exception {
        MediaExtractor extractor = new MediaExtractor();
        MediaMuxer muxer = null;
        MediaCodec decoder = null;
        MediaCodec encoder = null;
        CodecSurfacePipeline pipeline = null;

        try {
            extractor.setDataSource(srcPath);
            int videoTrack = -1, audioTrack = -1;
            MediaFormat srcV = null, srcA = null;
            for (int i = 0; i < extractor.getTrackCount(); i++) {
                MediaFormat f = extractor.getTrackFormat(i);
                String mime = f.getString(MediaFormat.KEY_MIME);
                if (mime == null) continue;
                if (mime.startsWith("video/") && videoTrack < 0) { videoTrack = i; srcV = f; }
                else if (mime.startsWith("audio/") && audioTrack < 0) { audioTrack = i; srcA = f; }
            }
            if (videoTrack < 0 || srcV == null) throw new IllegalStateException("Video izi bulunamadı");

            long durUs = srcV.containsKey(MediaFormat.KEY_DURATION) ? srcV.getLong(MediaFormat.KEY_DURATION) : 30_000_000L;
            double durSec = Math.max(1.0, durUs / 1_000_000.0);
            int srcW = srcV.getInteger(MediaFormat.KEY_WIDTH);
            int srcH = srcV.getInteger(MediaFormat.KEY_HEIGHT);

            double totalBits = targetBytes * 8.0 * 0.88;
            double audioBits = (audioTrack >= 0) ? (160_000.0 * durSec) : 0.0;
            int targetBitrate = (int) Math.max(350_000.0, Math.min(8_000_000.0, (totalBits - audioBits) / durSec));

            int outW = srcW, outH = srcH, outFps = 30;
            if (targetBitrate < 2_200_000 && srcH > 720) {
                outW = 1280;
                outH = 720;
            }
            outW &= ~15;
            outH &= ~15;

            MediaFormat encFmt = MediaFormat.createVideoFormat(MediaFormat.MIMETYPE_VIDEO_AVC, outW, outH);
            encFmt.setInteger(MediaFormat.KEY_COLOR_FORMAT, MediaCodecInfo.CodecCapabilities.COLOR_FormatSurface);
            encFmt.setInteger(MediaFormat.KEY_BIT_RATE, targetBitrate);
            encFmt.setInteger(MediaFormat.KEY_FRAME_RATE, outFps);
            encFmt.setInteger(MediaFormat.KEY_I_FRAME_INTERVAL, 1);

            encoder = MediaCodec.createEncoderByType(MediaFormat.MIMETYPE_VIDEO_AVC);
            encoder.configure(encFmt, null, null, MediaCodec.CONFIGURE_FLAG_ENCODE);
            Surface encSurface = encoder.createInputSurface();
            pipeline = new CodecSurfacePipeline(encSurface);
            encoder.start();

            decoder = MediaCodec.createDecoderByType(srcV.getString(MediaFormat.KEY_MIME));
            decoder.configure(srcV, pipeline.getDecoderSurface(), null, 0);
            decoder.start();

            muxer = new MediaMuxer(dstPath, MediaMuxer.OutputFormat.MUXER_OUTPUT_MPEG_4);
            int muxVideo = -1;
            int muxAudio = (audioTrack >= 0) ? muxer.addTrack(srcA) : -1;
            boolean muxerStarted = false;

            extractor.selectTrack(videoTrack);

            MediaCodec.BufferInfo decInfo = new MediaCodec.BufferInfo();
            MediaCodec.BufferInfo encInfo = new MediaCodec.BufferInfo();
            boolean inDone = false;
            boolean decDone = false;
            boolean encDone = false;

            while (!encDone) {
                if (!inDone) {
                    int inIdx = decoder.dequeueInputBuffer(10_000);
                    if (inIdx >= 0) {
                        ByteBuffer ib = decoder.getInputBuffer(inIdx);
                        int sz = extractor.readSampleData(ib, 0);
                        if (sz < 0) {
                            decoder.queueInputBuffer(inIdx, 0, 0, 0, MediaCodec.BUFFER_FLAG_END_OF_STREAM);
                            inDone = true;
                        } else {
                            decoder.queueInputBuffer(inIdx, 0, sz, extractor.getSampleTime(), 0);
                            extractor.advance();
                        }
                    }
                }

                if (!decDone) {
                    int outIdx = decoder.dequeueOutputBuffer(decInfo, 10_000);
                    if (outIdx >= 0) {
                        boolean render = (decInfo.size > 0);
                        decoder.releaseOutputBuffer(outIdx, render);
                        if (render) {
                            pipeline.awaitNewImage();
                            pipeline.drawImage(decInfo.presentationTimeUs * 1000L);
                        }
                        if ((decInfo.flags & MediaCodec.BUFFER_FLAG_END_OF_STREAM) != 0) {
                            encoder.signalEndOfInputStream();
                            decDone = true;
                        }
                    }
                }

                while (true) {
                    int eOut = encoder.dequeueOutputBuffer(encInfo, 5_000);
                    if (eOut == MediaCodec.INFO_TRY_AGAIN_LATER) break;
                    if (eOut == MediaCodec.INFO_OUTPUT_FORMAT_CHANGED) {
                        if (!muxerStarted) {
                            muxVideo = muxer.addTrack(encoder.getOutputFormat());
                            muxer.start();
                            muxerStarted = true;
                        }
                    } else if (eOut >= 0) {
                        ByteBuffer ob = encoder.getOutputBuffer(eOut);
                        if (ob != null && encInfo.size > 0 && muxerStarted &&
                                (encInfo.flags & MediaCodec.BUFFER_FLAG_CODEC_CONFIG) == 0) {
                            ob.position(encInfo.offset);
                            ob.limit(encInfo.offset + encInfo.size);
                            muxer.writeSampleData(muxVideo, ob, encInfo);
                        }
                        encoder.releaseOutputBuffer(eOut, false);
                        if ((encInfo.flags & MediaCodec.BUFFER_FLAG_END_OF_STREAM) != 0) {
                            encDone = true;
                            break;
                        }
                    }
                }
            }

            // Copy audio track directly (passthrough AAC)
            if (audioTrack >= 0 && muxAudio >= 0 && muxerStarted) {
                extractor.unselectTrack(videoTrack);
                extractor.selectTrack(audioTrack);
                extractor.seekTo(0, MediaExtractor.SEEK_TO_CLOSEST_SYNC);
                ByteBuffer abuf = ByteBuffer.allocate(256 * 1024);
                MediaCodec.BufferInfo ainfo = new MediaCodec.BufferInfo();
                while (true) {
                    int sz = extractor.readSampleData(abuf, 0);
                    if (sz < 0) break;
                    ainfo.offset = 0;
                    ainfo.size = sz;
                    ainfo.presentationTimeUs = extractor.getSampleTime();
                    ainfo.flags = (extractor.getSampleFlags() & MediaExtractor.SAMPLE_FLAG_SYNC) != 0
                            ? MediaCodec.BUFFER_FLAG_KEY_FRAME : 0;
                    muxer.writeSampleData(muxAudio, abuf, ainfo);
                    extractor.advance();
                }
            }
        } finally {
            try { if (decoder != null) { decoder.stop(); decoder.release(); } } catch (Exception ignored) {}
            try { if (encoder != null) { encoder.stop(); encoder.release(); } } catch (Exception ignored) {}
            try { if (pipeline != null) pipeline.release(); } catch (Exception ignored) {}
            try { extractor.release(); } catch (Exception ignored) {}
            try { if (muxer != null) { muxer.stop(); muxer.release(); } } catch (Exception ignored) {}
        }
    }

    private static class CodecSurfacePipeline implements SurfaceTexture.OnFrameAvailableListener {
        private EGLDisplay eglDisplay = EGL14.EGL_NO_DISPLAY;
        private EGLContext eglContext = EGL14.EGL_NO_CONTEXT;
        private EGLSurface eglSurface = EGL14.EGL_NO_SURFACE;
        private SurfaceTexture surfaceTexture;
        private Surface decoderSurface;
        private final Object frameSync = new Object();
        private boolean frameAvailable = false;

        private int program;
        private int texId;
        private int aPos, aTex, uMvp, uSt;
        private final float[] stMatrix = new float[16];
        private final float[] mvpMatrix = new float[16];
        private final FloatBuffer quadBuf;

        CodecSurfacePipeline(Surface encoderSurface) {
            float[] quad = {
                -1f, -1f, 0f, 0f,
                 1f, -1f, 1f, 0f,
                -1f,  1f, 0f, 1f,
                 1f,  1f, 1f, 1f
            };
            quadBuf = ByteBuffer.allocateDirect(quad.length * 4).order(ByteOrder.nativeOrder()).asFloatBuffer();
            quadBuf.put(quad).position(0);

            eglDisplay = EGL14.eglGetDisplay(EGL14.EGL_DEFAULT_DISPLAY);
            int[] ver = new int[2];
            EGL14.eglInitialize(eglDisplay, ver, 0, ver, 1);

            int[] cfgAttribs = {
                EGL14.EGL_RED_SIZE, 8, EGL14.EGL_GREEN_SIZE, 8, EGL14.EGL_BLUE_SIZE, 8,
                EGL14.EGL_RENDERABLE_TYPE, EGL14.EGL_OPENGL_ES2_BIT,
                0x3142, 1, EGL14.EGL_NONE
            };
            EGLConfig[] cfgs = new EGLConfig[1];
            int[] num = new int[1];
            EGL14.eglChooseConfig(eglDisplay, cfgAttribs, 0, cfgs, 0, 1, num, 0);

            int[] ctxAttribs = { EGL14.EGL_CONTEXT_CLIENT_VERSION, 2, EGL14.EGL_NONE };
            eglContext = EGL14.eglCreateContext(eglDisplay, cfgs[0], EGL14.EGL_NO_CONTEXT, ctxAttribs, 0);
            int[] surfAttribs = { EGL14.EGL_NONE };
            eglSurface = EGL14.eglCreateWindowSurface(eglDisplay, cfgs[0], encoderSurface, surfAttribs, 0);
            EGL14.eglMakeCurrent(eglDisplay, eglSurface, eglSurface, eglContext);

            int[] textures = new int[1];
            GLES20.glGenTextures(1, textures, 0);
            texId = textures[0];
            GLES20.glBindTexture(GLES11Ext.GL_TEXTURE_EXTERNAL_OES, texId);
            GLES20.glTexParameteri(GLES11Ext.GL_TEXTURE_EXTERNAL_OES, GLES20.GL_TEXTURE_MIN_FILTER, GLES20.GL_LINEAR);
            GLES20.glTexParameteri(GLES11Ext.GL_TEXTURE_EXTERNAL_OES, GLES20.GL_TEXTURE_MAG_FILTER, GLES20.GL_LINEAR);

            String vs = "uniform mat4 uMvp; uniform mat4 uSt; attribute vec2 aPos; attribute vec2 aTex; varying vec2 vTex; " +
                    "void main(){ gl_Position = uMvp * vec4(aPos,0.0,1.0); vTex = (uSt * vec4(aTex,0.0,1.0)).xy; }";
            String fs = "#extension GL_OES_EGL_image_external : require\n precision mediump float; varying vec2 vTex; " +
                    "uniform samplerExternalOES sTex; void main(){ gl_FragColor = texture2D(sTex, vTex); }";
            int vsh = compileShader(GLES20.GL_VERTEX_SHADER, vs);
            int fsh = compileShader(GLES20.GL_FRAGMENT_SHADER, fs);
            program = GLES20.glCreateProgram();
            GLES20.glAttachShader(program, vsh);
            GLES20.glAttachShader(program, fsh);
            GLES20.glLinkProgram(program);
            aPos = GLES20.glGetAttribLocation(program, "aPos");
            aTex = GLES20.glGetAttribLocation(program, "aTex");
            uMvp = GLES20.glGetUniformLocation(program, "uMvp");
            uSt = GLES20.glGetUniformLocation(program, "uSt");
            Matrix.setIdentityM(mvpMatrix, 0);

            surfaceTexture = new SurfaceTexture(texId);
            surfaceTexture.setOnFrameAvailableListener(this);
            decoderSurface = new Surface(surfaceTexture);
        }

        Surface getDecoderSurface() { return decoderSurface; }

        @Override
        public void onFrameAvailable(SurfaceTexture st) {
            synchronized (frameSync) {
                frameAvailable = true;
                frameSync.notifyAll();
            }
        }

        void awaitNewImage() {
            synchronized (frameSync) {
                while (!frameAvailable) {
                    try { frameSync.wait(1000); } catch (InterruptedException ignored) {}
                    if (!frameAvailable) break;
                }
                frameAvailable = false;
            }
            surfaceTexture.updateTexImage();
            surfaceTexture.getTransformMatrix(stMatrix);
        }

        void drawImage(long ptsNs) {
            GLES20.glUseProgram(program);
            GLES20.glActiveTexture(GLES20.GL_TEXTURE0);
            GLES20.glBindTexture(GLES11Ext.GL_TEXTURE_EXTERNAL_OES, texId);
            quadBuf.position(0);
            GLES20.glVertexAttribPointer(aPos, 2, GLES20.GL_FLOAT, false, 16, quadBuf);
            GLES20.glEnableVertexAttribArray(aPos);
            quadBuf.position(2);
            GLES20.glVertexAttribPointer(aTex, 2, GLES20.GL_FLOAT, false, 16, quadBuf);
            GLES20.glEnableVertexAttribArray(aTex);
            GLES20.glUniformMatrix4fv(uMvp, 1, false, mvpMatrix, 0);
            GLES20.glUniformMatrix4fv(uSt, 1, false, stMatrix, 0);
            GLES20.glDrawArrays(GLES20.GL_TRIANGLE_STRIP, 0, 4);
            EGLExt.eglPresentationTimeANDROID(eglDisplay, eglSurface, ptsNs);
            EGL14.eglSwapBuffers(eglDisplay, eglSurface);
        }

        private static int compileShader(int type, String src) {
            int s = GLES20.glCreateShader(type);
            GLES20.glShaderSource(s, src);
            GLES20.glCompileShader(s);
            return s;
        }

        void release() {
            if (decoderSurface != null) decoderSurface.release();
            if (surfaceTexture != null) surfaceTexture.release();
            if (eglDisplay != EGL14.EGL_NO_DISPLAY) {
                EGL14.eglMakeCurrent(eglDisplay, EGL14.EGL_NO_SURFACE, EGL14.EGL_NO_SURFACE, EGL14.EGL_NO_CONTEXT);
                EGL14.eglDestroySurface(eglDisplay, eglSurface);
                EGL14.eglDestroyContext(eglDisplay, eglContext);
                EGL14.eglTerminate(eglDisplay);
            }
        }
    }
}

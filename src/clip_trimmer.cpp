#include "clip_trimmer.h"
#include "com_ptr.h"

#include <d3d11.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <mftransform.h>
#include <mferror.h>
#include <codecapi.h>
#include <icodecapi.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

namespace {

bool EndsWithMp4(const std::wstring& s) {
    return s.size() > 4 && _wcsicmp(s.c_str() + s.size() - 4, L".mp4") == 0;
}

inline int16_t FloatToPcm16(float v) {
    if (v > 1.0f) v = 1.0f;
    if (v < -1.0f) v = -1.0f;
    int32_t iv = static_cast<int32_t>(std::lrintf(v * 32767.0f));
    return static_cast<int16_t>(std::max(-32768, std::min(32767, iv)));
}

bool FlushPcmChunks(IMFSinkWriter* sw, DWORD audioStream, std::vector<int16_t>& pcmAccum,
                    int64_t& writtenPairs, bool flushTail) {
    while (pcmAccum.size() >= 2048 || (flushTail && pcmAccum.size() >= 2)) {
        const size_t availPairs = pcmAccum.size() / 2;
        const size_t takePairs = std::min<size_t>(1024, availPairs);
        const DWORD cb = static_cast<DWORD>(takePairs * 4);

        ComPtr<IMFMediaBuffer> buf;
        if (FAILED(MFCreateMemoryBuffer(cb, buf.Put())) || !buf) return false;
        BYTE* dst = nullptr;
        if (SUCCEEDED(buf->Lock(&dst, nullptr, nullptr))) {
            memcpy(dst, pcmAccum.data(), cb);
            buf->Unlock();
            buf->SetCurrentLength(cb);
        }

        ComPtr<IMFSample> smp;
        if (FAILED(MFCreateSample(smp.Put())) || !smp) return false;
        smp->AddBuffer(buf.Get());
        smp->SetSampleTime((writtenPairs * 10000000LL) / 48000LL);
        smp->SetSampleDuration((static_cast<int64_t>(takePairs) * 10000000LL) / 48000LL);

        if (FAILED(sw->WriteSample(audioStream, smp.Get()))) return false;
        writtenPairs += static_cast<int64_t>(takePairs);
        pcmAccum.erase(pcmAccum.begin(), pcmAccum.begin() + static_cast<ptrdiff_t>(takePairs * 2));
    }
    return true;
}

bool AddAacOutputStream(IMFSinkWriter* sw, DWORD& outStreamIdx) {
    ComPtr<IMFMediaType> outA;
    MFCreateMediaType(outA.Put());
    outA->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
    outA->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_AAC);
    outA->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
    outA->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, 48000);
    outA->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, 2);
    outA->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, 24000);
    if (FAILED(sw->AddStream(outA.Get(), &outStreamIdx))) return false;

    ComPtr<IMFMediaType> inA;
    MFCreateMediaType(inA.Put());
    inA->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
    inA->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_PCM);
    inA->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
    inA->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, 48000);
    inA->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, 2);
    inA->SetUINT32(MF_MT_AUDIO_BLOCK_ALIGNMENT, 4);
    inA->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, 48000 * 4);
    return SUCCEEDED(sw->SetInputMediaType(outStreamIdx, inA.Get(), nullptr));
}

bool FastPassthroughTrimMp4(const std::wstring& srcPath, const std::wstring& dstPath,
                            double startSec, double endSec,
                            std::atomic<bool>* cancelFlag) {
    ClipAudioTrackLayout audioLayout;
    DetectClipAudioLayout(srcPath, audioLayout);

    ComPtr<IMFAttributes> rAttr;
    MFCreateAttributes(rAttr.Put(), 1);
    if (rAttr) rAttr->SetUINT32(MF_READWRITE_DISABLE_CONVERTERS, TRUE);

    ComPtr<IMFSourceReader> sr;
    if (FAILED(MFCreateSourceReaderFromURL(srcPath.c_str(), rAttr.Get(), sr.Put())) || !sr) {
        return false;
    }

    std::wstring tmpPath = dstPath + L".tmp.mp4";
    DeleteFileW(tmpPath.c_str());

    ComPtr<IMFAttributes> wAttr;
    MFCreateAttributes(wAttr.Put(), 2);
    if (wAttr) {
        wAttr->SetUINT32(MF_READWRITE_DISABLE_CONVERTERS, TRUE);
        wAttr->SetUINT32(MF_SINK_WRITER_DISABLE_THROTTLING, TRUE);
    }
    ComPtr<IMFSinkWriter> sw;
    if (FAILED(MFCreateSinkWriterFromURL(tmpPath.c_str(), nullptr, wAttr.Get(), sw.Put())) || !sw) {
        return false;
    }

    sr->SetStreamSelection(static_cast<DWORD>(MF_SOURCE_READER_ALL_STREAMS), FALSE);

    DWORD vIn = DWORD(-1);
    DWORD targetAudioIn = DWORD(-1);
    if (!audioLayout.streamIndices.empty()) {
        int pos = (audioLayout.mixStreamPos >= 0 &&
                   static_cast<size_t>(audioLayout.mixStreamPos) < audioLayout.streamIndices.size())
            ? audioLayout.mixStreamPos : 0;
        targetAudioIn = static_cast<DWORD>(audioLayout.streamIndices[static_cast<size_t>(pos)]);
    }

    bool addedA = false;
    std::vector<DWORD> outMap(16, DWORD(-1));
    std::vector<bool> isStreamDone(16, true);
    DWORD streamCount = 0;

    for (DWORD si = 0; si < 16; ++si) {
        ComPtr<IMFMediaType> mt;
        if (FAILED(sr->GetNativeMediaType(si, 0, mt.Put())) || !mt) break;
        GUID maj{};
        mt->GetGUID(MF_MT_MAJOR_TYPE, &maj);
        const bool takeVideo = (maj == MFMediaType_Video && vIn == DWORD(-1));
        const bool wantThisAudio = (maj == MFMediaType_Audio && !addedA &&
                                    (targetAudioIn == DWORD(-1) || si == targetAudioIn));
        if (takeVideo || wantThisAudio) {
            DWORD outIdx = 0;
            if (FAILED(sw->AddStream(mt.Get(), &outIdx)) ||
                FAILED(sw->SetInputMediaType(outIdx, mt.Get(), nullptr))) {
                sw.Reset();
                DeleteFileW(tmpPath.c_str());
                return false;
            }
            sr->SetStreamSelection(si, TRUE);
            outMap[si] = outIdx;
            isStreamDone[si] = false;
            if (takeVideo) vIn = si;
            if (wantThisAudio) addedA = true;
            streamCount = std::max(streamCount, si + 1);
        }
    }
    if (vIn == DWORD(-1) || streamCount == 0) {
        sw.Reset();
        DeleteFileW(tmpPath.c_str());
        return false;
    }

    if (FAILED(sw->BeginWriting())) {
        sw.Reset();
        DeleteFileW(tmpPath.c_str());
        return false;
    }

    const double seekSec = std::max(0.0, startSec - 1.2);
    PROPVARIANT pv;
    PropVariantInit(&pv);
    pv.vt = VT_I8;
    pv.hVal.QuadPart = static_cast<LONGLONG>(seekSec * 10000000.0);
    sr->SetCurrentPosition(GUID_NULL, pv);
    PropVariantClear(&pv);

    const LONGLONG keyThresh100 = static_cast<LONGLONG>(std::max(0.0, startSec - 0.45) * 10000000.0);
    const LONGLONG end100 = static_cast<LONGLONG>(endSec * 10000000.0);
    bool gotFirstKey = false;
    LONGLONG basePts = 0;
    int vWritten = 0;
    int aWritten = 0;

    while (true) {
        if (cancelFlag && cancelFlag->load()) {
            sw.Reset();
            DeleteFileW(tmpPath.c_str());
            return false;
        }
        bool allDone = true;
        for (DWORD i = 0; i < streamCount; ++i) {
            if (!isStreamDone[i]) { allDone = false; break; }
        }
        if (allDone) break;

        DWORD si = 0, flags = 0;
        LONGLONG ts = 0;
        ComPtr<IMFSample> smp;
        if (FAILED(sr->ReadSample(static_cast<DWORD>(MF_SOURCE_READER_ANY_STREAM), 0, &si, &flags, &ts, smp.Put()))) {
            break;
        }
        if (flags & MF_SOURCE_READERF_ENDOFSTREAM) {
            if (si < streamCount) {
                isStreamDone[si] = true;
                sr->SetStreamSelection(si, FALSE);
            }
            continue;
        }
        if (!smp || si >= streamCount || outMap[si] == DWORD(-1)) continue;

        if (!gotFirstKey) {
            if (si != vIn) continue;
            UINT32 cp = 0;
            smp->GetUINT32(MFSampleExtension_CleanPoint, &cp);
            if (!cp) {
                ComPtr<IMFMediaBuffer> kb;
                if (SUCCEEDED(smp->ConvertToContiguousBuffer(kb.Put())) && kb) {
                    BYTE* p = nullptr; DWORD bl = 0;
                    if (SUCCEEDED(kb->Lock(&p, nullptr, &bl))) {
                        for (DWORD k = 0; k + 4 < bl && k < 256; ++k) {
                            if (p[k] == 0 && p[k + 1] == 0 && p[k + 2] == 1 && ((p[k + 3] & 0x1F) == 5)) { cp = 1; break; }
                            if (k + 5 < bl && p[k] == 0 && p[k + 1] == 0 && p[k + 2] == 0 && p[k + 3] == 1 && ((p[k + 4] & 0x1F) == 5)) { cp = 1; break; }
                        }
                        kb->Unlock();
                    }
                }
            }
            if (!cp || ts < keyThresh100) continue;
            gotFirstKey = true;
            basePts = ts;
        }

        if (ts > end100) {
            isStreamDone[si] = true;
            sr->SetStreamSelection(si, FALSE);
            continue;
        }
        if (ts < basePts) continue;

        smp->SetSampleTime(ts - basePts);
        smp->DeleteItem(MFSampleExtension_DecodeTimestamp);
        if (SUCCEEDED(sw->WriteSample(outMap[si], smp.Get()))) {
            if (si == vIn) ++vWritten;
            else ++aWritten;
        }
    }

    if (!gotFirstKey || vWritten < 2 || (addedA && aWritten == 0) || FAILED(sw->Finalize())) {
        sw.Reset();
        DeleteFileW(tmpPath.c_str());
        return false;
    }
    sw.Reset();
    FixMp4AudioTrackFlags(tmpPath);
    if (!MoveFileExW(tmpPath.c_str(), dstPath.c_str(), MOVEFILE_REPLACE_EXISTING)) {
        DeleteFileW(tmpPath.c_str());
        return false;
    }
    return true;
}

bool CopyVideoSampleBuffer(ID3D11DeviceContext* ctx, ID3D11Texture2D* stgTex,
                           IMFSample* srcSmp, uint32_t w, uint32_t h, IMFSample* dstSmp) {
    ComPtr<IMFMediaBuffer> buf;
    if (FAILED(srcSmp->GetBufferByIndex(0, buf.Put())) || !buf) return false;

    ComPtr<IMFDXGIBuffer> db;
    ComPtr<ID3D11Texture2D> dtex;
    UINT sub = 0;
    if (ctx && stgTex && SUCCEEDED(buf->QueryInterface(__uuidof(IMFDXGIBuffer), db.PutVoid())) &&
        SUCCEEDED(db->GetResource(__uuidof(ID3D11Texture2D), dtex.PutVoid()))) {
        db->GetSubresourceIndex(&sub);
        D3D11_BOX box{ 0, 0, 0, w, h, 1 };
        ctx->CopySubresourceRegion(stgTex, 0, 0, 0, 0, dtex.Get(), sub, &box);

        D3D11_MAPPED_SUBRESOURCE mapped{};
        if (FAILED(ctx->Map(stgTex, 0, D3D11_MAP_READ, 0, &mapped))) return false;

        const DWORD nvLen = (w * h * 3) / 2;
        ComPtr<IMFMediaBuffer> outBuf;
        bool ok = false;
        if (SUCCEEDED(MFCreateMemoryBuffer(nvLen, outBuf.Put())) && outBuf) {
            BYTE* dst = nullptr;
            if (SUCCEEDED(outBuf->Lock(&dst, nullptr, nullptr))) {
                const BYTE* srcY = reinterpret_cast<const BYTE*>(mapped.pData);
                for (uint32_t y = 0; y < h; ++y) {
                    memcpy(dst + y * w, srcY + y * mapped.RowPitch, w);
                }
                const BYTE* srcUV = srcY + h * mapped.RowPitch;
                BYTE* dstUV = dst + w * h;
                for (uint32_t y = 0; y < h / 2; ++y) {
                    memcpy(dstUV + y * w, srcUV + y * mapped.RowPitch, w);
                }
                outBuf->Unlock();
                outBuf->SetCurrentLength(nvLen);
                dstSmp->AddBuffer(outBuf.Get());
                ok = true;
            }
        }
        ctx->Unmap(stgTex, 0);
        return ok;
    }

    ComPtr<IMFMediaBuffer> cbuf;
    if (SUCCEEDED(srcSmp->ConvertToContiguousBuffer(cbuf.Put())) && cbuf) {
        BYTE* srcPtr = nullptr;
        DWORD srcLen = 0;
        if (SUCCEEDED(cbuf->Lock(&srcPtr, nullptr, &srcLen)) && srcLen > 0) {
            ComPtr<IMFMediaBuffer> copyBuf;
            if (SUCCEEDED(MFCreateMemoryBuffer(srcLen, copyBuf.Put())) && copyBuf) {
                BYTE* dstPtr = nullptr;
                if (SUCCEEDED(copyBuf->Lock(&dstPtr, nullptr, nullptr))) {
                    memcpy(dstPtr, srcPtr, srcLen);
                    copyBuf->Unlock();
                    copyBuf->SetCurrentLength(srcLen);
                    dstSmp->AddBuffer(copyBuf.Get());
                    cbuf->Unlock();
                    return true;
                }
            }
            cbuf->Unlock();
        }
    }
    return false;
}

void AppendResampledPcm(const std::vector<int16_t>& srcPcm, double speed, float gain,
                        size_t targetOutPairs, std::vector<int16_t>& dstPcm) {
    const size_t srcPairs = srcPcm.size() / 2;
    if (targetOutPairs == 0) return;
    if (srcPairs == 0) {
        dstPcm.insert(dstPcm.end(), targetOutPairs * 2, 0);
        return;
    }
    dstPcm.reserve(dstPcm.size() + targetOutPairs * 2);
    for (size_t i = 0; i < targetOutPairs; ++i) {
        double srcPos = static_cast<double>(i) * speed;
        if (srcPos >= static_cast<double>(srcPairs - 1)) {
            srcPos = static_cast<double>(srcPairs - 1);
        }
        size_t i0 = static_cast<size_t>(srcPos);
        size_t i1 = std::min(i0 + 1, srcPairs - 1);
        double frac = srcPos - static_cast<double>(i0);
        float l0 = static_cast<float>(srcPcm[i0 * 2]) / 32767.0f;
        float r0 = static_cast<float>(srcPcm[i0 * 2 + 1]) / 32767.0f;
        float l1 = static_cast<float>(srcPcm[i1 * 2]) / 32767.0f;
        float r1 = static_cast<float>(srcPcm[i1 * 2 + 1]) / 32767.0f;
        float l = static_cast<float>((l0 * (1.0 - frac) + l1 * frac) * gain);
        float r = static_cast<float>((r0 * (1.0 - frac) + r1 * frac) * gain);
        dstPcm.push_back(FloatToPcm16(l));
        dstPcm.push_back(FloatToPcm16(r));
    }
}

struct DecodedMusicTrack {
    size_t startPair = 0;
    std::vector<int16_t> pcm; // stereo 48kHz interleaved (L, R)
};

bool FlushMixPcmChunks(IMFSinkWriter* sw, DWORD audioStream, std::vector<int16_t>& pcmAccum,
                       int64_t& writtenPairs, const std::vector<DecodedMusicTrack>& music, bool flushTail) {
    while (pcmAccum.size() >= 2048 || (flushTail && pcmAccum.size() >= 2)) {
        const size_t availPairs = pcmAccum.size() / 2;
        const size_t takePairs = std::min<size_t>(1024, availPairs);
        const DWORD cb = static_cast<DWORD>(takePairs * 4);

        ComPtr<IMFMediaBuffer> buf;
        if (FAILED(MFCreateMemoryBuffer(cb, buf.Put())) || !buf) return false;
        BYTE* dst = nullptr;
        if (SUCCEEDED(buf->Lock(&dst, nullptr, nullptr))) {
            int16_t* out16 = reinterpret_cast<int16_t*>(dst);
            if (music.empty()) {
                memcpy(out16, pcmAccum.data(), cb);
            } else {
                const size_t basePair = static_cast<size_t>(writtenPairs);
                for (size_t k = 0; k < takePairs; ++k) {
                    const size_t gPair = basePair + k;
                    float l = static_cast<float>(pcmAccum[k * 2]) / 32767.0f;
                    float r = static_cast<float>(pcmAccum[k * 2 + 1]) / 32767.0f;
                    for (const auto& m : music) {
                        if (gPair >= m.startPair) {
                            const size_t mp = gPair - m.startPair;
                            if (mp * 2 + 1 < m.pcm.size()) {
                                l += static_cast<float>(m.pcm[mp * 2]) / 32767.0f;
                                r += static_cast<float>(m.pcm[mp * 2 + 1]) / 32767.0f;
                            }
                        }
                    }
                    out16[k * 2] = FloatToPcm16(l);
                    out16[k * 2 + 1] = FloatToPcm16(r);
                }
            }
            buf->Unlock();
            buf->SetCurrentLength(cb);
        }

        ComPtr<IMFSample> smp;
        if (FAILED(MFCreateSample(smp.Put())) || !smp) return false;
        smp->AddBuffer(buf.Get());
        smp->SetSampleTime((writtenPairs * 10000000LL) / 48000LL);
        smp->SetSampleDuration((static_cast<int64_t>(takePairs) * 10000000LL) / 48000LL);

        if (FAILED(sw->WriteSample(audioStream, smp.Get()))) return false;
        writtenPairs += static_cast<int64_t>(takePairs);
        pcmAccum.erase(pcmAccum.begin(), pcmAccum.begin() + static_cast<ptrdiff_t>(takePairs * 2));
    }
    return true;
}

bool DecodeAudioToPcm48k(const std::wstring& path, double trimStartSec, double maxDurSec,
                          float gain, bool fadeIn, bool fadeOut, std::vector<int16_t>& outPcm) {
    outPcm.clear();
    ComPtr<IMFSourceReader> sr;
    if (FAILED(MFCreateSourceReaderFromURL(path.c_str(), nullptr, sr.Put())) || !sr) return false;

    DWORD aStream = DWORD(-1);
    sr->SetStreamSelection(static_cast<DWORD>(MF_SOURCE_READER_ALL_STREAMS), FALSE);
    for (DWORD i = 0; i < 8; ++i) {
        ComPtr<IMFMediaType> mt;
        if (FAILED(sr->GetNativeMediaType(i, 0, mt.Put())) || !mt) break;
        GUID maj{};
        mt->GetGUID(MF_MT_MAJOR_TYPE, &maj);
        if (maj == MFMediaType_Audio) { aStream = i; break; }
    }
    if (aStream == DWORD(-1)) return false;
    sr->SetStreamSelection(aStream, TRUE);

    ComPtr<IMFMediaType> decA;
    MFCreateMediaType(decA.Put());
    decA->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
    decA->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_PCM);
    decA->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
    decA->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, 48000);
    decA->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, 2);
    if (FAILED(sr->SetCurrentMediaType(aStream, nullptr, decA.Get()))) return false;

    const LONGLONG start100 = static_cast<LONGLONG>(std::max(0.0, trimStartSec) * 1e7);
    const size_t maxPairs = (maxDurSec > 0.05) ? static_cast<size_t>(maxDurSec * 48000.0) : (48000ULL * 300ULL);
    if (start100 > 0) {
        PROPVARIANT pv;
        PropVariantInit(&pv);
        pv.vt = VT_I8;
        pv.hVal.QuadPart = std::max(0LL, start100 - 5000000LL);
        sr->SetCurrentPosition(GUID_NULL, pv);
        PropVariantClear(&pv);
    }

    while ((outPcm.size() / 2) < maxPairs) {
        DWORD si = 0, fl = 0; LONGLONG ts = 0;
        ComPtr<IMFSample> smp;
        if (FAILED(sr->ReadSample(aStream, 0, &si, &fl, &ts, smp.Put())) || (fl & MF_SOURCE_READERF_ENDOFSTREAM)) break;
        if (!smp) continue;
        LONGLONG dur = 0; smp->GetSampleDuration(&dur);
        if (dur <= 0) dur = 213333LL;
        if (ts + dur <= start100) continue;

        ComPtr<IMFMediaBuffer> buf;
        if (SUCCEEDED(smp->ConvertToContiguousBuffer(buf.Put())) && buf) {
            BYTE* p = nullptr; DWORD len = 0;
            if (SUCCEEDED(buf->Lock(&p, nullptr, &len)) && len >= 4) {
                const size_t nPairs = len / 4;
                size_t skip = (ts < start100) ? static_cast<size_t>(((start100 - ts) * 48000LL) / 10000000LL) : 0;
                const size_t curPairs = outPcm.size() / 2;
                if (skip < nPairs && curPairs < maxPairs) {
                    const size_t take = std::min(nPairs - skip, maxPairs - curPairs);
                    const int16_t* s = reinterpret_cast<const int16_t*>(p) + skip * 2;
                    outPcm.reserve(outPcm.size() + take * 2);
                    for (size_t k = 0; k < take * 2; ++k) {
                        outPcm.push_back(FloatToPcm16((static_cast<float>(s[k]) / 32767.0f) * gain));
                    }
                }
                buf->Unlock();
            }
        }
    }

    const size_t totalPairs = outPcm.size() / 2;
    if (totalPairs == 0) return false;
    const size_t fadePairs = std::min<size_t>(19200, totalPairs / 4); // up to 0.4s
    if (fadeIn && fadePairs > 0) {
        for (size_t i = 0; i < fadePairs; ++i) {
            float f = static_cast<float>(i) / static_cast<float>(fadePairs);
            outPcm[i * 2] = FloatToPcm16((static_cast<float>(outPcm[i * 2]) / 32767.0f) * f);
            outPcm[i * 2 + 1] = FloatToPcm16((static_cast<float>(outPcm[i * 2 + 1]) / 32767.0f) * f);
        }
    }
    if (fadeOut && fadePairs > 0) {
        for (size_t i = 0; i < fadePairs; ++i) {
            size_t idx = totalPairs - 1 - i;
            float f = static_cast<float>(i) / static_cast<float>(fadePairs);
            outPcm[idx * 2] = FloatToPcm16((static_cast<float>(outPcm[idx * 2]) / 32767.0f) * f);
            outPcm[idx * 2 + 1] = FloatToPcm16((static_cast<float>(outPcm[idx * 2 + 1]) / 32767.0f) * f);
        }
    }
    return true;
}

void ApplyNv12FadeInSample(IMFSample* smp, uint32_t w, uint32_t h, float alpha) {
    if (!smp || alpha >= 0.999f) return;
    alpha = std::max(0.0f, std::min(1.0f, alpha));
    ComPtr<IMFMediaBuffer> buf;
    if (FAILED(smp->ConvertToContiguousBuffer(buf.Put())) || !buf) return;
    BYTE* p = nullptr; DWORD len = 0;
    const size_t yBytes = static_cast<size_t>(w) * h;
    const size_t uvBytes = yBytes / 2;
    if (SUCCEEDED(buf->Lock(&p, nullptr, &len)) && len >= yBytes + uvBytes) {
        const int32_t aFixed = static_cast<int32_t>(std::lround(alpha * 256.0f));
        for (size_t i = 0; i < yBytes; ++i) {
            int32_t v = 16 + (((static_cast<int32_t>(p[i]) - 16) * aFixed) >> 8);
            p[i] = static_cast<BYTE>(std::max(16, std::min(235, v)));
        }
        for (size_t i = yBytes; i < yBytes + uvBytes; ++i) {
            int32_t v = 128 + (((static_cast<int32_t>(p[i]) - 128) * aFixed) >> 8);
            p[i] = static_cast<BYTE>(std::max(16, std::min(240, v)));
        }
        buf->Unlock();
    }
}

bool HasCustomTransitionNear(double sec, const std::vector<MontageTransitionSpec>& transitions) {
    for (const auto& tr : transitions) {
        double halfW = std::max(0.10, tr.durationSec) * 0.65 + 0.15;
        if (std::abs(tr.timelineSec - sec) <= halfW) return true;
    }
    return false;
}

bool ApplyNv12TransitionEffects(IMFSample* smp, uint32_t w, uint32_t h, BYTE blackY,
                                double tSec, const std::vector<MontageTransitionSpec>& transitions,
                                std::vector<BYTE>& scratch) {
    if (!smp || transitions.empty() || w < 4 || h < 4) return false;

    const BYTE whiteY = (blackY == 0) ? 255 : 235;
    const BYTE maxChroma = (blackY == 0) ? 255 : 240;
    const BYTE minChroma = (blackY == 0) ? 0 : 16;
    const uint32_t hw = w / 2;
    const uint32_t hh = h / 2;
    const size_t yBytes = static_cast<size_t>(w) * h;
    const size_t uvBytes = static_cast<size_t>(hw) * hh * 2;
    const size_t totalBytes = yBytes + uvBytes;

    bool anyApplied = false;
    BYTE* p = nullptr;
    DWORD len = 0;
    ComPtr<IMFMediaBuffer> buf;

    auto ensureLocked = [&]() -> bool {
        if (p) return true;
        if (FAILED(smp->ConvertToContiguousBuffer(buf.Put())) || !buf) return false;
        if (FAILED(buf->Lock(&p, nullptr, &len)) || len < totalBytes) {
            p = nullptr;
            buf.Reset();
            return false;
        }
        return true;
    };

    for (const auto& tr : transitions) {
        double dur = std::max(0.15, std::min(4.0, tr.durationSec));
        double halfDur = dur * 0.5;
        double startT = tr.timelineSec - halfDur;
        double endT = tr.timelineSec + halfDur;
        if (tSec < startT || tSec > endT) continue;
        if (!ensureLocked()) break;

        double u = std::max(0.0, std::min(1.0, (tSec - startT) / dur));
        double bell = std::sin(u * 3.14159265358979323846);
        anyApplied = true;

        switch (tr.type) {
            case MontageTransitionType::FadeBlack: {
                double edgeDist = std::abs(u - 0.5) * 2.0; // 1 -> 0 -> 1
                float alpha = static_cast<float>(std::pow(edgeDist, 1.35));
                const int32_t aFixed = static_cast<int32_t>(std::lround(alpha * 256.0f));
                const int32_t by = static_cast<int32_t>(blackY);
                for (size_t i = 0; i < yBytes; ++i) {
                    int32_t v = by + (((static_cast<int32_t>(p[i]) - by) * aFixed) >> 8);
                    p[i] = static_cast<BYTE>(std::max<int32_t>(blackY, std::min<int32_t>(whiteY, v)));
                }
                for (size_t i = yBytes; i < totalBytes; ++i) {
                    int32_t v = 128 + (((static_cast<int32_t>(p[i]) - 128) * aFixed) >> 8);
                    p[i] = static_cast<BYTE>(std::max<int32_t>(minChroma, std::min<int32_t>(maxChroma, v)));
                }
                break;
            }
            case MontageTransitionType::FlashWhite: {
                float flash = static_cast<float>(std::min(0.96, std::pow(std::max(0.0, bell), 2.1)));
                const int32_t fFixed = static_cast<int32_t>(std::lround(flash * 256.0f));
                const int32_t wy = static_cast<int32_t>(whiteY);
                for (size_t i = 0; i < yBytes; ++i) {
                    int32_t cur = static_cast<int32_t>(p[i]);
                    int32_t v = cur + (((wy - cur) * fFixed) >> 8);
                    p[i] = static_cast<BYTE>(std::max<int32_t>(blackY, std::min<int32_t>(whiteY, v)));
                }
                for (size_t i = yBytes; i < totalBytes; ++i) {
                    int32_t cur = static_cast<int32_t>(p[i]);
                    int32_t v = cur + (((128 - cur) * fFixed) >> 8);
                    p[i] = static_cast<BYTE>(std::max<int32_t>(minChroma, std::min<int32_t>(maxChroma, v)));
                }
                break;
            }
            case MontageTransitionType::ZoomPunch: {
                double punch = bell * bell;
                double scale = 1.0 + 0.36 * punch;
                double invScale = 1.0 / scale;
                scratch.resize(totalBytes);
                memcpy(scratch.data(), p, totalBytes);

                const BYTE* srcY = scratch.data();
                const BYTE* srcUV = scratch.data() + yBytes;
                BYTE* dstUV = p + yBytes;

                const double cx = static_cast<double>(w) * 0.5;
                const double cy = static_cast<double>(h) * 0.5;
                std::vector<uint32_t> lutX(w);
                for (uint32_t x = 0; x < w; ++x) {
                    int sx = static_cast<int>(cx + (static_cast<double>(x) - cx) * invScale);
                    lutX[x] = static_cast<uint32_t>(std::max(0, std::min<int>(static_cast<int>(w) - 1, sx)));
                }
                const int32_t popFixed = static_cast<int32_t>(std::lround(std::pow(punch, 1.5) * 0.22 * 256.0));
                for (uint32_t y = 0; y < h; ++y) {
                    int sy = static_cast<int>(cy + (static_cast<double>(y) - cy) * invScale);
                    uint32_t csy = static_cast<uint32_t>(std::max(0, std::min<int>(static_cast<int>(h) - 1, sy)));
                    const BYTE* sRow = srcY + static_cast<size_t>(csy) * w;
                    BYTE* dRow = p + static_cast<size_t>(y) * w;
                    if (popFixed <= 2) {
                        for (uint32_t x = 0; x < w; ++x) dRow[x] = sRow[lutX[x]];
                    } else {
                        for (uint32_t x = 0; x < w; ++x) {
                            int32_t v = static_cast<int32_t>(sRow[lutX[x]]);
                            v = v + (((static_cast<int32_t>(whiteY) - v) * popFixed) >> 8);
                            dRow[x] = static_cast<BYTE>(std::min<int32_t>(whiteY, v));
                        }
                    }
                }
                const double hcx = static_cast<double>(hw) * 0.5;
                const double hcy = static_cast<double>(hh) * 0.5;
                std::vector<uint32_t> lutUvX(hw);
                for (uint32_t ux = 0; ux < hw; ++ux) {
                    int sx = static_cast<int>(hcx + (static_cast<double>(ux) - hcx) * invScale);
                    lutUvX[ux] = static_cast<uint32_t>(std::max(0, std::min<int>(static_cast<int>(hw) - 1, sx)));
                }
                for (uint32_t uy = 0; uy < hh; ++uy) {
                    int sy = static_cast<int>(hcy + (static_cast<double>(uy) - hcy) * invScale);
                    uint32_t csy = static_cast<uint32_t>(std::max(0, std::min<int>(static_cast<int>(hh) - 1, sy)));
                    const BYTE* sUvRow = srcUV + static_cast<size_t>(csy) * hw * 2;
                    BYTE* dUvRow = dstUV + static_cast<size_t>(uy) * hw * 2;
                    for (uint32_t ux = 0; ux < hw; ++ux) {
                        const uint32_t sux = lutUvX[ux] * 2;
                        dUvRow[ux * 2] = sUvRow[sux];
                        dUvRow[ux * 2 + 1] = sUvRow[sux + 1];
                    }
                }
                break;
            }
            case MontageTransitionType::GlitchShake: {
                double amp = bell * bell;
                scratch.resize(totalBytes);
                memcpy(scratch.data(), p, totalBytes);

                const BYTE* srcY = scratch.data();
                const BYTE* srcUV = scratch.data() + yBytes;
                BYTE* dstUV = p + yBytes;

                uint32_t frameHash = static_cast<uint32_t>(std::llround(u * 240.0)) * 2654435761u + 131u;
                const uint32_t stripH = std::max<uint32_t>(16u, (h / 12u) & ~1u);
                const int maxShift = std::max(4, static_cast<int>(w * 0.055 * amp)) & ~1;
                const int chromaSplit = std::max(2, static_cast<int>((hw * 0.028) * amp));

                for (uint32_t y = 0; y < h; ++y) {
                    uint32_t band = y / stripH;
                    uint32_t hsh = (frameHash ^ (band * 2246822519u)) * 3266489917u;
                    int32_t dx = 0;
                    if (((hsh >> 8) % 10u) < 7u) {
                        dx = (static_cast<int>(hsh % static_cast<uint32_t>(maxShift * 2 + 1)) - maxShift) & ~1;
                    }
                    const BYTE* sRow = srcY + static_cast<size_t>(y) * w;
                    BYTE* dRow = p + static_cast<size_t>(y) * w;
                    const bool flashBar = (amp > 0.55 && ((hsh >> 20) % 11u) == 0u);
                    for (uint32_t x = 0; x < w; ++x) {
                        int sx = static_cast<int>(x) + dx;
                        if (sx < 0) sx += static_cast<int>(w);
                        else if (sx >= static_cast<int>(w)) sx -= static_cast<int>(w);
                        BYTE yv = sRow[sx];
                        if (flashBar) yv = static_cast<BYTE>(std::min<int>(whiteY, static_cast<int>(yv) + 55));
                        dRow[x] = yv;
                    }
                }
                for (uint32_t uy = 0; uy < hh; ++uy) {
                    uint32_t band = (uy * 2u) / stripH;
                    uint32_t hsh = (frameHash ^ (band * 2246822519u)) * 3266489917u;
                    int32_t dxU = chromaSplit + static_cast<int>((hsh % 7u)) - 3;
                    int32_t dxV = -chromaSplit - static_cast<int>((hsh % 7u)) + 3;
                    const BYTE* sRow = srcUV + static_cast<size_t>(uy) * hw * 2;
                    BYTE* dRow = dstUV + static_cast<size_t>(uy) * hw * 2;
                    for (uint32_t ux = 0; ux < hw; ++ux) {
                        int sxU = std::max(0, std::min<int>(static_cast<int>(hw) - 1, static_cast<int>(ux) + dxU));
                        int sxV = std::max(0, std::min<int>(static_cast<int>(hw) - 1, static_cast<int>(ux) + dxV));
                        dRow[ux * 2] = sRow[sxU * 2];
                        dRow[ux * 2 + 1] = sRow[sxV * 2 + 1];
                    }
                }
                break;
            }
            case MontageTransitionType::SlideWipe: {
                uint32_t covL = 0, covR = 0;
                uint32_t edgePos = 0;
                if (u < 0.5) {
                    double frac = u * 2.0;
                    covR = static_cast<uint32_t>(std::lround(w * frac)) & ~1u;
                    edgePos = covR;
                } else {
                    double frac = (u - 0.5) * 2.0;
                    covL = static_cast<uint32_t>(std::lround(w * frac)) & ~1u;
                    covR = w;
                    edgePos = covL;
                }
                for (uint32_t y = 0; y < h; ++y) {
                    BYTE* row = p + static_cast<size_t>(y) * w;
                    if (covR > covL && covR <= w) {
                        memset(row + covL, blackY, covR - covL);
                    }
                    for (uint32_t bx = (edgePos > 4 ? edgePos - 4 : 0); bx < std::min<uint32_t>(w, edgePos + 4); ++bx) {
                        row[bx] = whiteY;
                    }
                }
                for (uint32_t uy = 0; uy < hh; ++uy) {
                    BYTE* row = p + yBytes + static_cast<size_t>(uy) * hw * 2;
                    uint32_t uL = std::min(hw, covL / 2);
                    uint32_t uR = std::min(hw, covR / 2);
                    if (uR > uL) memset(row + uL * 2, 128, (uR - uL) * 2);
                }
                break;
            }
        }
    }

    if (buf && p) buf->Unlock();
    return anyApplied;
}

bool ApplyNv12FrameTransform(IMFSample* smp, uint32_t w, uint32_t h, BYTE blackY,
                             double tSec, const std::vector<MontageFrameSpec>& frames,
                             std::vector<BYTE>& scratch) {
    if (!smp || frames.empty() || w < 16 || h < 16) return false;
    const MontageFrameSpec* active = nullptr;
    for (int i = static_cast<int>(frames.size()) - 1; i >= 0; --i) {
        const auto& f = frames[static_cast<size_t>(i)];
        if (tSec >= f.startSec - 1e-6 && tSec <= f.endSec + 1e-6) {
            active = &f;
            break;
        }
    }
    if (!active || active->scale >= 0.995) return false;

    const double sc = std::max(0.20, std::min(1.0, active->scale));
    const uint32_t dstW = std::max<uint32_t>(16u, std::min<uint32_t>(w, static_cast<uint32_t>(std::lround(w * sc)) & ~1u));
    const uint32_t dstH = std::max<uint32_t>(16u, std::min<uint32_t>(h, static_cast<uint32_t>(std::lround(h * sc)) & ~1u));
    const uint32_t dstX = static_cast<uint32_t>(std::max<int32_t>(0,
        std::min<int32_t>(static_cast<int32_t>(w - dstW),
            static_cast<int32_t>(std::lround(static_cast<double>(w) * active->centerX - static_cast<double>(dstW) * 0.5)) & ~1)));
    const uint32_t dstY = static_cast<uint32_t>(std::max<int32_t>(0,
        std::min<int32_t>(static_cast<int32_t>(h - dstH),
            static_cast<int32_t>(std::lround(static_cast<double>(h) * active->centerY - static_cast<double>(dstH) * 0.5)) & ~1)));

    const uint32_t hw = w / 2, hh = h / 2;
    const uint32_t dhw = dstW / 2, dhh = dstH / 2;
    const uint32_t dhx = dstX / 2, dhy = dstY / 2;
    const size_t yBytes = static_cast<size_t>(w) * h;
    const size_t uvBytes = static_cast<size_t>(hw) * hh * 2;
    const size_t totalBytes = yBytes + uvBytes;

    ComPtr<IMFMediaBuffer> buf;
    if (FAILED(smp->ConvertToContiguousBuffer(buf.Put())) || !buf) return false;
    BYTE* p = nullptr; DWORD len = 0;
    if (FAILED(buf->Lock(&p, nullptr, &len)) || !p || len < totalBytes) return false;

    scratch.resize(totalBytes);
    memcpy(scratch.data(), p, totalBytes);
    memset(p, blackY, yBytes);
    memset(p + yBytes, 128, uvBytes);

    const BYTE* srcY = scratch.data();
    const BYTE* srcUV = scratch.data() + yBytes;
    BYTE* dstUV = p + yBytes;

    std::vector<uint32_t> lutX(dstW);
    for (uint32_t dx = 0; dx < dstW; ++dx) {
        lutX[dx] = std::min<uint32_t>(w - 1, (dx * w) / dstW);
    }
    for (uint32_t dy = 0; dy < dstH; ++dy) {
        uint32_t sy = std::min<uint32_t>(h - 1, (dy * h) / dstH);
        const BYTE* sRow = srcY + static_cast<size_t>(sy) * w;
        BYTE* dRow = p + static_cast<size_t>(dstY + dy) * w + dstX;
        for (uint32_t dx = 0; dx < dstW; ++dx) dRow[dx] = sRow[lutX[dx]];
    }

    std::vector<uint32_t> lutUvX(dhw);
    for (uint32_t dux = 0; dux < dhw; ++dux) {
        lutUvX[dux] = std::min<uint32_t>(hw - 1, (dux * hw) / dhw);
    }
    for (uint32_t duy = 0; duy < dhh; ++duy) {
        uint32_t suy = std::min<uint32_t>(hh - 1, (duy * hh) / dhh);
        const BYTE* sUvRow = srcUV + static_cast<size_t>(suy) * hw * 2;
        BYTE* dUvRow = dstUV + (static_cast<size_t>(dhy + duy) * hw + dhx) * 2;
        for (uint32_t dux = 0; dux < dhw; ++dux) {
            const uint32_t sux = lutUvX[dux] * 2;
            dUvRow[dux * 2] = sUvRow[sux];
            dUvRow[dux * 2 + 1] = sUvRow[sux + 1];
        }
    }

    buf->Unlock();
    return true;
}

bool CreateBlackNv12Sample(const std::vector<BYTE>& blackNv12, LONGLONG ts100, LONGLONG dur100,
                           ComPtr<IMFSample>& outSmp) {
    outSmp.Reset();
    if (blackNv12.empty()) return false;
    ComPtr<IMFMediaBuffer> buf;
    const DWORD len = static_cast<DWORD>(blackNv12.size());
    if (FAILED(MFCreateMemoryBuffer(len, buf.Put())) || !buf) return false;
    BYTE* dst = nullptr;
    if (FAILED(buf->Lock(&dst, nullptr, nullptr)) || !dst) return false;
    memcpy(dst, blackNv12.data(), len);
    buf->Unlock();
    buf->SetCurrentLength(len);
    ComPtr<IMFSample> smp;
    if (FAILED(MFCreateSample(smp.Put())) || !smp) return false;
    smp->AddBuffer(buf.Get());
    smp->SetSampleTime(ts100);
    smp->SetSampleDuration(dur100);
    outSmp = smp;
    return true;
}

struct SourceSegment {
    std::wstring path;
    double startSec = 0.0;
    double endSec = 0.0;
    double speed = 1.0;
    float gameGain = 1.0f;
    float micGain = 1.0f;
    double timelineStartSec = -1.0;
};

} // namespace

std::wstring BuildTrimmedCopyPath(const std::wstring& srcPath) {
    size_t slash = srcPath.find_last_of(L"\\/");
    std::wstring dir = (slash != std::wstring::npos) ? srcPath.substr(0, slash + 1) : L"";
    std::wstring name = (slash != std::wstring::npos) ? srcPath.substr(slash + 1) : srcPath;
    std::wstring stem = EndsWithMp4(name) ? name.substr(0, name.size() - 4) : name;

    std::wstring cand = dir + stem + L"_kesit.mp4";
    if (GetFileAttributesW(cand.c_str()) == INVALID_FILE_ATTRIBUTES) {
        return cand;
    }
    for (int i = 2; i < 1000; ++i) {
        wchar_t buf[64] = {};
        swprintf(buf, 64, L"_kesit_%d.mp4", i);
        cand = dir + stem + buf;
        if (GetFileAttributesW(cand.c_str()) == INVALID_FILE_ATTRIBUTES) {
            return cand;
        }
    }
    return dir + stem + L"_kesit_x.mp4";
}

std::wstring BuildMontageOutputPath(const std::wstring& dir) {
    std::wstring base = dir;
    if (!base.empty() && base.back() != L'\\' && base.back() != L'/') base.push_back(L'\\');
    SYSTEMTIME st{};
    GetLocalTime(&st);
    wchar_t stem[64] = {};
    swprintf(stem, 64, L"Montaj_%04d-%02d-%02d_%02d-%02d-%02d",
             st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    std::wstring cand = base + stem + L".mp4";
    if (GetFileAttributesW(cand.c_str()) == INVALID_FILE_ATTRIBUTES) return cand;
    for (int i = 2; i < 1000; ++i) {
        wchar_t suf[32] = {};
        swprintf(suf, 32, L"_%d.mp4", i);
        cand = base + stem + suf;
        if (GetFileAttributesW(cand.c_str()) == INVALID_FILE_ATTRIBUTES) return cand;
    }
    return base + stem + L"_x.mp4";
}

bool FixMp4AudioTrackFlags(const std::wstring& mp4Path, int enabledAudioIndex) {
    HANDLE hF = CreateFileW(mp4Path.c_str(), GENERIC_READ | GENERIC_WRITE,
                            FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hF == INVALID_HANDLE_VALUE) return false;

    LARGE_INTEGER fileSize{};
    if (!GetFileSizeEx(hF, &fileSize) || fileSize.QuadPart < 16) {
        CloseHandle(hF);
        return false;
    }

    auto be32 = [](const uint8_t* p) -> uint32_t {
        return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | uint32_t(p[3]);
    };
    auto be64 = [](const uint8_t* p) -> uint64_t {
        return (uint64_t(p[0]) << 56) | (uint64_t(p[1]) << 48) | (uint64_t(p[2]) << 40) | (uint64_t(p[3]) << 32) |
               (uint64_t(p[4]) << 24) | (uint64_t(p[5]) << 16) | (uint64_t(p[6]) << 8) | uint64_t(p[7]);
    };

    int64_t pos = 0;
    int64_t moovOffset = -1;
    uint32_t moovSize = 0;
    while (pos + 8 <= fileSize.QuadPart) {
        LARGE_INTEGER li{}; li.QuadPart = pos;
        if (!SetFilePointerEx(hF, li, nullptr, FILE_BEGIN)) break;
        uint8_t hdr[16]{};
        DWORD rd = 0;
        if (!ReadFile(hF, hdr, 8, &rd, nullptr) || rd < 8) break;
        uint64_t sz = be32(hdr);
        if (sz == 1) {
            if (!ReadFile(hF, hdr + 8, 8, &rd, nullptr) || rd < 8) break;
            sz = be64(hdr + 8);
        } else if (sz == 0) {
            sz = static_cast<uint64_t>(fileSize.QuadPart - pos);
        }
        if (sz < 8 || pos + static_cast<int64_t>(sz) > fileSize.QuadPart) break;
        if (memcmp(hdr + 4, "moov", 4) == 0 && sz <= 16ULL * 1024ULL * 1024ULL) {
            moovOffset = pos;
            moovSize = static_cast<uint32_t>(sz);
            break;
        }
        pos += static_cast<int64_t>(sz);
    }

    if (moovOffset < 0 || moovSize < 16) {
        CloseHandle(hF);
        return false;
    }

    std::vector<uint8_t> moov(moovSize);
    LARGE_INTEGER li{}; li.QuadPart = moovOffset;
    DWORD rd = 0;
    if (!SetFilePointerEx(hF, li, nullptr, FILE_BEGIN) ||
        !ReadFile(hF, moov.data(), moovSize, &rd, nullptr) || rd != moovSize) {
        CloseHandle(hF);
        return false;
    }

    int audioTrackCount = 0;
    bool modified = false;
    size_t mp = 8;
    while (mp + 8 <= moov.size()) {
        uint32_t bsz = be32(&moov[mp]);
        if (bsz < 8 || mp + bsz > moov.size()) break;
        if (memcmp(&moov[mp + 4], "trak", 4) == 0) {
            size_t tkhdPos = 0, tkhdEnd = 0;
            bool isSound = false;
            size_t tp = mp + 8;
            while (tp + 8 <= mp + bsz) {
                uint32_t tsz = be32(&moov[tp]);
                if (tsz < 8 || tp + tsz > mp + bsz) break;
                if (memcmp(&moov[tp + 4], "tkhd", 4) == 0) {
                    tkhdPos = tp;
                    tkhdEnd = tp + tsz;
                } else if (memcmp(&moov[tp + 4], "mdia", 4) == 0) {
                    size_t mdp = tp + 8;
                    while (mdp + 8 <= tp + tsz) {
                        uint32_t msz = be32(&moov[mdp]);
                        if (msz < 8 || mdp + msz > tp + tsz) break;
                        if (memcmp(&moov[mdp + 4], "hdlr", 4) == 0 && mdp + 20 <= tp + tsz) {
                            if (memcmp(&moov[mdp + 16], "soun", 4) == 0) isSound = true;
                        }
                        mdp += msz;
                    }
                }
                tp += tsz;
            }
            if (isSound && tkhdPos > 0) {
                uint8_t ver = moov[tkhdPos + 8];
                size_t altOff = (ver == 0) ? (tkhdPos + 40) : (tkhdPos + 52);
                if (altOff + 2 <= tkhdEnd) {
                    const int targetTrack = (enabledAudioIndex >= 0) ? enabledAudioIndex : 0;
                    uint8_t wantFlag = (audioTrackCount == targetTrack) ? 0x01 : 0x00;
                    if (moov[tkhdPos + 9] != 0 || moov[tkhdPos + 10] != 0 || moov[tkhdPos + 11] != wantFlag) {
                        moov[tkhdPos + 9] = 0;
                        moov[tkhdPos + 10] = 0;
                        moov[tkhdPos + 11] = wantFlag;
                        modified = true;
                    }
                    if (moov[altOff] != 0 || moov[altOff + 1] != 1) {
                        moov[altOff] = 0;
                        moov[altOff + 1] = 1;
                        modified = true;
                    }
                }
                ++audioTrackCount;
            }
        }
        mp += bsz;
    }

    if (modified && audioTrackCount > 1) {
        DWORD wr = 0;
        SetFilePointerEx(hF, li, nullptr, FILE_BEGIN);
        WriteFile(hF, moov.data(), moovSize, &wr, nullptr);
    }
    CloseHandle(hF);
    return true;
}

bool ProbeClipAudioTracks(const std::wstring& path, bool& outHasSeparateGameMic, double* outDurationSec) {
    outHasSeparateGameMic = false;
    if (outDurationSec) *outDurationSec = 0.0;

    ComPtr<IMFSourceReader> sr;
    if (FAILED(MFCreateSourceReaderFromURL(path.c_str(), nullptr, sr.Put())) || !sr) {
        return false;
    }
    int audioCount = 0;
    for (DWORD si = 0; si < 8; ++si) {
        ComPtr<IMFMediaType> mt;
        if (FAILED(sr->GetNativeMediaType(si, 0, mt.Put())) || !mt) break;
        GUID maj{};
        mt->GetGUID(MF_MT_MAJOR_TYPE, &maj);
        if (maj == MFMediaType_Audio) ++audioCount;
    }
    outHasSeparateGameMic = (audioCount >= 3);

    if (outDurationSec) {
        PROPVARIANT pv;
        PropVariantInit(&pv);
        if (SUCCEEDED(sr->GetPresentationAttribute(MF_SOURCE_READER_MEDIASOURCE, MF_PD_DURATION, &pv))) {
            if (pv.vt == VT_UI8) *outDurationSec = static_cast<double>(pv.uhVal.QuadPart) / 1e7;
            else if (pv.vt == VT_I8) *outDurationSec = static_cast<double>(pv.hVal.QuadPart) / 1e7;
        }
        PropVariantClear(&pv);
    }
    return true;
}

bool ProbeAudioFileDuration(const std::wstring& path, double& outDurSec) {
    outDurSec = 0.0;
    ComPtr<IMFSourceReader> sr;
    if (FAILED(MFCreateSourceReaderFromURL(path.c_str(), nullptr, sr.Put())) || !sr) return false;
    PROPVARIANT pv;
    PropVariantInit(&pv);
    if (SUCCEEDED(sr->GetPresentationAttribute(MF_SOURCE_READER_MEDIASOURCE, MF_PD_DURATION, &pv))) {
        if (pv.vt == VT_UI8) outDurSec = static_cast<double>(pv.uhVal.QuadPart) / 1e7;
        else if (pv.vt == VT_I8) outDurSec = static_cast<double>(pv.hVal.QuadPart) / 1e7;
    }
    PropVariantClear(&pv);
    return outDurSec > 0.05;
}

bool DetectClipAudioLayout(const std::wstring& path, ClipAudioTrackLayout& outLayout) {
    outLayout = {};
    if (path.empty()) return false;

    ComPtr<IMFSourceReader> sr;
    if (FAILED(MFCreateSourceReaderFromURL(path.c_str(), nullptr, sr.Put())) || !sr) {
        return false;
    }

    sr->SetStreamSelection(static_cast<DWORD>(MF_SOURCE_READER_ALL_STREAMS), FALSE);
    for (DWORD i = 0; i < 16; ++i) {
        ComPtr<IMFMediaType> mt;
        if (FAILED(sr->GetNativeMediaType(i, 0, mt.Put())) || !mt) break;
        GUID maj{};
        mt->GetGUID(MF_MT_MAJOR_TYPE, &maj);
        if (maj == MFMediaType_Audio) {
            outLayout.streamIndices.push_back(static_cast<unsigned long>(i));
        }
    }
    if (outLayout.streamIndices.empty()) return false;

    const size_t nA = outLayout.streamIndices.size();
    outLayout.mixStreamPos = 0;
    outLayout.hasSeparateGameMic = (nA >= 3);
    if (nA == 1) {
        return true;
    }

    double durSec = 0.0;
    PROPVARIANT pv;
    PropVariantInit(&pv);
    if (SUCCEEDED(sr->GetPresentationAttribute(MF_SOURCE_READER_MEDIASOURCE, MF_PD_DURATION, &pv))) {
        if (pv.vt == VT_UI8) durSec = static_cast<double>(pv.uhVal.QuadPart) / 1e7;
        else if (pv.vt == VT_I8) durSec = static_cast<double>(pv.hVal.QuadPart) / 1e7;
    }
    PropVariantClear(&pv);

    const size_t analyzeCount = std::min<size_t>(nA, 3);
    for (size_t k = 0; k < analyzeCount; ++k) {
        DWORD si = static_cast<DWORD>(outLayout.streamIndices[k]);
        sr->SetStreamSelection(si, TRUE);
        ComPtr<IMFMediaType> pcm;
        MFCreateMediaType(pcm.Put());
        pcm->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
        pcm->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_PCM);
        pcm->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
        pcm->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, 48000);
        pcm->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, 2);
        sr->SetCurrentMediaType(si, nullptr, pcm.Get());
    }

    struct PcmChunk {
        LONGLONG ts = 0;
        std::vector<float> s;
    };

    double sumSq[3] = { 0.0, 0.0, 0.0 };
    uint64_t sampleCnt[3] = { 0, 0, 0 };
    double sumErr[3] = { 0.0, 0.0, 0.0 };
    double refAbs = 0.0;
    uint64_t matchedSamples = 0;

    std::vector<double> probeTimes;
    if (durSec > 1.5) {
        probeTimes = { durSec * 0.08, durSec * 0.35, durSec * 0.65, durSec * 0.88 };
    } else {
        probeTimes = { 0.0 };
    }

    for (double tSec : probeTimes) {
        PROPVARIANT pos;
        PropVariantInit(&pos);
        pos.vt = VT_I8;
        pos.hVal.QuadPart = static_cast<LONGLONG>(std::max(0.0, tSec) * 1e7);
        sr->SetCurrentPosition(GUID_NULL, pos);
        PropVariantClear(&pos);

        std::vector<PcmChunk> chunks[3];
        size_t readsPerTrack[3] = { 0, 0, 0 };
        bool ended[3] = { false, false, false };
        constexpr int kMaxPackets = 60;
        for (int pIdx = 0; pIdx < kMaxPackets; ++pIdx) {
            bool allDone = true;
            for (size_t k = 0; k < analyzeCount; ++k) {
                if (readsPerTrack[k] < 10 && !ended[k]) { allDone = false; break; }
            }
            if (allDone) break;

            DWORD si = 0, fl = 0;
            LONGLONG ts = 0;
            ComPtr<IMFSample> smp;
            if (FAILED(sr->ReadSample(static_cast<DWORD>(MF_SOURCE_READER_ANY_STREAM), 0, &si, &fl, &ts, smp.Put()))) break;

            int kMatch = -1;
            for (size_t k = 0; k < analyzeCount; ++k) {
                if (outLayout.streamIndices[k] == si) { kMatch = static_cast<int>(k); break; }
            }
            if (fl & MF_SOURCE_READERF_ENDOFSTREAM) {
                if (kMatch >= 0) ended[kMatch] = true;
                if (!smp) continue;
            }
            if (!smp || kMatch < 0 || readsPerTrack[kMatch] >= 12) continue;

            ComPtr<IMFMediaBuffer> buf;
            if (SUCCEEDED(smp->ConvertToContiguousBuffer(buf.Put())) && buf) {
                BYTE* p = nullptr;
                DWORD len = 0;
                if (SUCCEEDED(buf->Lock(&p, nullptr, &len)) && len >= 4) {
                    const int16_t* s16 = reinterpret_cast<const int16_t*>(p);
                    const size_t nPairs = len / 4;
                    PcmChunk ch;
                    ch.ts = ts;
                    ch.s.resize(nPairs);
                    for (size_t i = 0; i < nPairs; ++i) {
                        float v = (static_cast<float>(s16[i * 2]) + static_cast<float>(s16[i * 2 + 1])) * (0.5f / 32768.0f);
                        ch.s[i] = v;
                        sumSq[kMatch] += static_cast<double>(v) * static_cast<double>(v);
                    }
                    sampleCnt[kMatch] += nPairs;
                    chunks[kMatch].push_back(std::move(ch));
                    ++readsPerTrack[kMatch];
                    buf->Unlock();
                }
            }
        }

        if (analyzeCount >= 3) {
            for (const auto& c0 : chunks[0]) {
                const PcmChunk* c1 = nullptr;
                const PcmChunk* c2 = nullptr;
                LONGLONG bestD1 = 120000;
                LONGLONG bestD2 = 120000;
                for (const auto& cand : chunks[1]) {
                    LONGLONG d = (cand.ts >= c0.ts) ? (cand.ts - c0.ts) : (c0.ts - cand.ts);
                    if (d <= bestD1) { bestD1 = d; c1 = &cand; }
                }
                for (const auto& cand : chunks[2]) {
                    LONGLONG d = (cand.ts >= c0.ts) ? (cand.ts - c0.ts) : (c0.ts - cand.ts);
                    if (d <= bestD2) { bestD2 = d; c2 = &cand; }
                }
                if (!c1 || !c2) continue;
                const size_t n = std::min(c0.s.size(), std::min(c1->s.size(), c2->s.size()));
                for (size_t i = 0; i < n; i += 2) {
                    float a = c0.s[i];
                    float b = c1->s[i];
                    float c = c2->s[i];
                    float ab = std::clamp(a + b, -1.0f, 1.0f);
                    float ac = std::clamp(a + c, -1.0f, 1.0f);
                    float bc = std::clamp(b + c, -1.0f, 1.0f);
                    sumErr[0] += std::fabs(a - bc);
                    sumErr[1] += std::fabs(b - ac);
                    sumErr[2] += std::fabs(c - ab);
                    refAbs += std::fabs(a) + std::fabs(b) + std::fabs(c);
                    ++matchedSamples;
                }
            }
        }
    }

    double rms[3] = { 0.0, 0.0, 0.0 };
    for (size_t k = 0; k < analyzeCount; ++k) {
        rms[k] = (sampleCnt[k] > 0) ? std::sqrt(sumSq[k] / static_cast<double>(sampleCnt[k])) : 0.0;
    }

    if (analyzeCount == 2) {
        outLayout.mixStreamPos = (rms[1] > rms[0] * 1.05) ? 1 : 0;
        outLayout.gameStreamPos = -1;
        outLayout.micStreamPos = -1;
        outLayout.hasSeparateGameMic = false;
        return true;
    }

    int mixPos = 0;
    if (matchedSamples > 500 && refAbs > 1e-3) {
        const double tieTol = 0.025 * refAbs;
        double minErr = std::min(sumErr[0], std::min(sumErr[1], sumErr[2]));
        const bool ok0 = (sumErr[0] <= minErr + tieTol);
        const bool ok1 = (sumErr[1] <= minErr + tieTol);
        const bool ok2 = (sumErr[2] <= minErr + tieTol);
        if (ok0 && !ok2) {
            mixPos = 0;
        } else if (ok2 && !ok0) {
            mixPos = 2;
        } else {
            int bestIdx = 0;
            for (int k = 1; k < 3; ++k) {
                if (sumErr[k] < sumErr[bestIdx]) bestIdx = k;
            }
            if (ok0 && ok2 && !ok1) {
                mixPos = (rms[2] > rms[0] * 1.08) ? 2 : 0;
            } else {
                mixPos = bestIdx;
            }
        }
    } else {
        mixPos = (rms[2] > rms[0] * 1.08 && rms[2] >= rms[1]) ? 2
               : (rms[1] > rms[0] * 1.08 && rms[1] > rms[2]) ? 1
               : 0;
    }

    outLayout.mixStreamPos = mixPos;
    if (mixPos == 0) {
        outLayout.gameStreamPos = 1;
        outLayout.micStreamPos = 2;
    } else if (mixPos == 2) {
        outLayout.gameStreamPos = 1;
        outLayout.micStreamPos = 0;
    } else {
        outLayout.gameStreamPos = (rms[0] >= rms[2]) ? 0 : 2;
        outLayout.micStreamPos = (outLayout.gameStreamPos == 0) ? 2 : 0;
    }
    return true;
}

bool ExtractAudioWaveform(const std::wstring& path, int trackChoice, int numBuckets, std::vector<float>& outPeaks) {
    outPeaks.clear();
    if (numBuckets <= 0) return false;
    ClipAudioTrackLayout audioLayout;
    DetectClipAudioLayout(path, audioLayout);

    ComPtr<IMFSourceReader> sr;
    if (FAILED(MFCreateSourceReaderFromURL(path.c_str(), nullptr, sr.Put())) || !sr) return false;

    std::vector<DWORD> aStreams;
    sr->SetStreamSelection(static_cast<DWORD>(MF_SOURCE_READER_ALL_STREAMS), FALSE);
    for (DWORD i = 0; i < 8; ++i) {
        ComPtr<IMFMediaType> mt;
        if (FAILED(sr->GetNativeMediaType(i, 0, mt.Put())) || !mt) break;
        GUID maj{}; mt->GetGUID(MF_MT_MAJOR_TYPE, &maj);
        if (maj == MFMediaType_Audio) aStreams.push_back(i);
    }
    if (aStreams.empty()) return false;

    double durSec = 0.0;
    PROPVARIANT pv;
    PropVariantInit(&pv);
    if (SUCCEEDED(sr->GetPresentationAttribute(MF_SOURCE_READER_MEDIASOURCE, MF_PD_DURATION, &pv))) {
        if (pv.vt == VT_UI8) durSec = static_cast<double>(pv.uhVal.QuadPart) / 1e7;
        else if (pv.vt == VT_I8) durSec = static_cast<double>(pv.hVal.QuadPart) / 1e7;
    }
    PropVariantClear(&pv);
    if (durSec <= 0.05) return false;

    int mappedPos = trackChoice;
    if (audioLayout.streamIndices.size() == aStreams.size()) {
        if (trackChoice <= 0) {
            mappedPos = audioLayout.mixStreamPos;
        } else if (audioLayout.hasSeparateGameMic) {
            if (trackChoice == 1 && audioLayout.gameStreamPos >= 0) mappedPos = audioLayout.gameStreamPos;
            else if (trackChoice == 2 && audioLayout.micStreamPos >= 0) mappedPos = audioLayout.micStreamPos;
        } else {
            mappedPos = audioLayout.mixStreamPos;
        }
    }
    size_t idx = (mappedPos >= 0 && static_cast<size_t>(mappedPos) < aStreams.size()) ? static_cast<size_t>(mappedPos) : 0;
    DWORD si = aStreams[idx];
    sr->SetStreamSelection(si, TRUE);
    ComPtr<IMFMediaType> pcm;
    MFCreateMediaType(pcm.Put());
    pcm->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
    pcm->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_PCM);
    pcm->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
    pcm->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, 48000);
    pcm->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, 2);
    if (FAILED(sr->SetCurrentMediaType(si, nullptr, pcm.Get()))) return false;

    outPeaks.assign(static_cast<size_t>(numBuckets), 0.0f);
    for (int b = 0; b < numBuckets; ++b) {
        double t = (static_cast<double>(b) + 0.5) * durSec / static_cast<double>(numBuckets);
        PROPVARIANT pos;
        PropVariantInit(&pos);
        pos.vt = VT_I8;
        pos.hVal.QuadPart = static_cast<LONGLONG>(t * 1e7);
        sr->SetCurrentPosition(GUID_NULL, pos);
        PropVariantClear(&pos);

        for (int tries = 0; tries < 4; ++tries) {
            DWORD rSi = 0, fl = 0; LONGLONG ts = 0;
            ComPtr<IMFSample> smp;
            if (FAILED(sr->ReadSample(si, 0, &rSi, &fl, &ts, smp.Put())) || (fl & MF_SOURCE_READERF_ENDOFSTREAM)) break;
            if (!smp) continue;
            ComPtr<IMFMediaBuffer> buf;
            if (SUCCEEDED(smp->ConvertToContiguousBuffer(buf.Put())) && buf) {
                BYTE* p = nullptr; DWORD len = 0;
                if (SUCCEEDED(buf->Lock(&p, nullptr, &len)) && len >= 2) {
                    const int16_t* s = reinterpret_cast<const int16_t*>(p);
                    DWORD n = len / 2;
                    double sumSq = 0.0;
                    for (DWORD k = 0; k < n; ++k) {
                        double v = static_cast<double>(s[k]) / 32768.0;
                        sumSq += v * v;
                    }
                    float rms = (n > 0) ? static_cast<float>(std::sqrt(sumSq / n)) : 0.0f;
                    outPeaks[static_cast<size_t>(b)] = std::min(1.0f, rms * 3.8f);
                    buf->Unlock();
                }
            }
            break;
        }
    }
    return true;
}

bool EditMp4Clip(const ClipEditSpec& spec, std::string& outError,
                 std::atomic<bool>* cancelFlag) {
    const bool isMontage = !spec.montageSegments.empty();
    double startSec = std::max(0.0, spec.startSec);
    double endSec = spec.endSec;
    if (!isMontage && endSec <= startSec + 0.2) {
        outError = "Kırpma aralığı en az 0.2 saniye olmalı.";
        return false;
    }

    const double defaultSpeed = std::max(0.5, std::min(2.0, spec.speed));
    const float defaultGameGain = std::max(0.0f, std::min(2.0f, static_cast<float>(spec.gameVolPct) / 100.0f));
    const float defaultMicGain = std::max(0.0f, std::min(2.0f, static_cast<float>(spec.micVolPct) / 100.0f));

    std::vector<SourceSegment> segments;
    bool hasCut = false;
    bool hasInsert = false;
    std::wstring primaryPath = spec.srcPath;

    if (isMontage) {
        for (const auto& ms : spec.montageSegments) {
            if (ms.path.empty()) continue;
            double s0 = std::max(0.0, ms.startSec);
            double s1 = ms.endSec;
            if (s1 <= s0 + 0.2) {
                bool dummyMulti = false;
                double probedDur = 0.0;
                if (!ProbeClipAudioTracks(ms.path, dummyMulti, &probedDur) || probedDur <= 0.2) continue;
                s1 = probedDur;
            }
            if (s1 > s0 + 0.2) {
                double segSpd = (ms.speed > 0.05) ? std::max(0.5, std::min(2.0, ms.speed)) : defaultSpeed;
                float segGame = (ms.gameVolPct >= 0) ? std::max(0.0f, std::min(2.0f, static_cast<float>(ms.gameVolPct) / 100.0f)) : defaultGameGain;
                float segMic = (ms.micVolPct >= 0) ? std::max(0.0f, std::min(2.0f, static_cast<float>(ms.micVolPct) / 100.0f)) : defaultMicGain;
                double tlStart = (ms.timelineStartSec >= 0.0) ? ms.timelineStartSec : -1.0;
                segments.push_back({ ms.path, s0, s1, segSpd, segGame, segMic, tlStart });
            }
        }
        if (segments.empty()) {
            outError = "Montaj için geçerli klip bulunamadı.";
            return false;
        }
        primaryPath = segments[0].path;
    } else {
        hasCut = (spec.cutEndSec > spec.cutStartSec + 0.1) &&
                 (spec.cutStartSec > startSec + 0.1) &&
                 (spec.cutEndSec < endSec - 0.1);
        double insertDur = 0.0;
        bool insertHasMulti = false;
        hasInsert = !spec.insertClipPath.empty() &&
                    ProbeClipAudioTracks(spec.insertClipPath, insertHasMulti, &insertDur) &&
                    insertDur > 0.2;

        if (hasCut) {
            segments.push_back({ spec.srcPath, startSec, spec.cutStartSec, defaultSpeed, defaultGameGain, defaultMicGain });
            if (hasInsert) segments.push_back({ spec.insertClipPath, 0.0, insertDur, defaultSpeed, defaultGameGain, defaultMicGain });
            segments.push_back({ spec.srcPath, spec.cutEndSec, endSec, defaultSpeed, defaultGameGain, defaultMicGain });
        } else {
            segments.push_back({ spec.srcPath, startSec, endSec, defaultSpeed, defaultGameGain, defaultMicGain });
            if (hasInsert) segments.push_back({ spec.insertClipPath, 0.0, insertDur, defaultSpeed, defaultGameGain, defaultMicGain });
        }
    }

    // Fast lossless passthrough trim (<0.3s, minimal RAM) when only start/end changed
    if (!isMontage && !hasCut && !hasInsert && !spec.fadeTransitions &&
        spec.musicTracks.empty() && spec.transitions.empty() && spec.frameTransforms.empty() &&
        spec.targetSizeMb == 0 && !spec.vertical916 &&
        std::abs(defaultSpeed - 1.0) < 0.01 &&
        spec.gameVolPct == 100 && spec.micVolPct == 100) {
        if (FastPassthroughTrimMp4(spec.srcPath, spec.dstPath, startSec, endSec, cancelFlag)) {
            return true;
        }
        if (cancelFlag && cancelFlag->load()) {
            outError = "İşlem iptal edildi.";
            return false;
        }
    }

    D3D_FEATURE_LEVEL fl = D3D_FEATURE_LEVEL_11_0;
    ComPtr<ID3D11Device> dev;
    ComPtr<ID3D11DeviceContext> ctx;
    D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
        D3D11_CREATE_DEVICE_VIDEO_SUPPORT | D3D11_CREATE_DEVICE_BGRA_SUPPORT,
        &fl, 1, D3D11_SDK_VERSION, dev.Put(), nullptr, ctx.Put());
    if (dev) {
        ComPtr<ID3D10Multithread> mt;
        if (SUCCEEDED(dev->QueryInterface(__uuidof(ID3D10Multithread), mt.PutVoid())) && mt) {
            mt->SetMultithreadProtected(TRUE);
        }
    }

    UINT token = 0;
    ComPtr<IMFDXGIDeviceManager> dm;
    if (dev && SUCCEEDED(MFCreateDXGIDeviceManager(&token, dm.Put())) && dm) {
        if (FAILED(dm->ResetDevice(dev.Get(), token))) dm.Reset();
    }

    // Probe primary video format & audio track layout
    bool srcMultiTrack = false;
    ProbeClipAudioTracks(primaryPath, srcMultiTrack, nullptr);

    ComPtr<IMFSourceReader> probeSr;
    if (FAILED(MFCreateSourceReaderFromURL(primaryPath.c_str(), nullptr, probeSr.Put())) || !probeSr) {
        outError = "Kaynak klip okunamadı.";
        return false;
    }

    DWORD firstV = 0;
    bool foundV = false, foundA = false;
    for (DWORD si = 0; si < 8; ++si) {
        ComPtr<IMFMediaType> mt;
        if (FAILED(probeSr->GetNativeMediaType(si, 0, mt.Put())) || !mt) break;
        GUID maj{};
        mt->GetGUID(MF_MT_MAJOR_TYPE, &maj);
        if (maj == MFMediaType_Video && !foundV) { firstV = si; foundV = true; }
        if (maj == MFMediaType_Audio) foundA = true;
    }
    if (!foundV) {
        outError = "Video akışı bulunamadı.";
        return false;
    }

    ComPtr<IMFMediaType> nativeV;
    probeSr->GetNativeMediaType(firstV, 0, nativeV.Put());
    UINT32 srcW = 1280, srcH = 720, fpsN = 60, fpsD = 1, avgBr = 6000000;
    UINT32 yuvMatrix = MFVideoTransferMatrix_BT709;
    UINT32 primaries = MFVideoPrimaries_BT709;
    UINT32 transFunc = MFVideoTransFunc_709;
    UINT32 nomRange  = MFNominalRange_16_235;
    if (nativeV) {
        MFGetAttributeSize(nativeV.Get(), MF_MT_FRAME_SIZE, &srcW, &srcH);
        MFGetAttributeRatio(nativeV.Get(), MF_MT_FRAME_RATE, &fpsN, &fpsD);
        nativeV->GetUINT32(MF_MT_AVG_BITRATE, &avgBr);
        UINT32 v = 0;
        if (SUCCEEDED(nativeV->GetUINT32(MF_MT_YUV_MATRIX, &v)) && v != 0) yuvMatrix = v;
        if (SUCCEEDED(nativeV->GetUINT32(MF_MT_VIDEO_PRIMARIES, &v)) && v != 0) primaries = v;
        if (SUCCEEDED(nativeV->GetUINT32(MF_MT_TRANSFER_FUNCTION, &v)) && v != 0) transFunc = v;
        if (SUCCEEDED(nativeV->GetUINT32(MF_MT_VIDEO_NOMINAL_RANGE, &v)) && v != 0) nomRange = v;
    }
    if (fpsN == 0 || fpsD == 0) { fpsN = 60; fpsD = 1; }
    avgBr = std::max<uint32_t>(2500000u, std::min<uint32_t>(20000000u, avgBr));
    probeSr.Reset();

    double totalOutDurSec = 0.0;
    double minSegSpeed = 2.0;
    double maxSegSpeed = 0.5;
    for (auto& sg : segments) {
        double outDur = std::max(0.0, sg.endSec - sg.startSec) / sg.speed;
        if (sg.timelineStartSec >= 0.0) {
            totalOutDurSec = std::max(totalOutDurSec, sg.timelineStartSec);
        }
        sg.timelineStartSec = totalOutDurSec;
        totalOutDurSec += outDur;
        minSegSpeed = std::min(minSegSpeed, sg.speed);
        maxSegSpeed = std::max(maxSegSpeed, sg.speed);
    }
    if (totalOutDurSec < 0.2) totalOutDurSec = 0.2;

    std::vector<DecodedMusicTrack> decodedMusic;
    for (const auto& mt : spec.musicTracks) {
        if (mt.path.empty() || mt.volPct <= 0) continue;
        const double tlStart = std::max(0.0, mt.timelineStartSec);
        if (tlStart >= totalOutDurSec - 0.05) continue;
        double availDur = totalOutDurSec - tlStart;
        double maxD = (mt.durationSec > 0.05) ? std::min(mt.durationSec, availDur) : availDur;
        if (maxD <= 0.05) continue;
        DecodedMusicTrack dmt;
        dmt.startPair = static_cast<size_t>(tlStart * 48000.0);
        float mGain = std::max(0.0f, std::min(2.0f, static_cast<float>(mt.volPct) / 100.0f));
        if (DecodeAudioToPcm48k(mt.path, std::max(0.0, mt.trimStartSec), maxD, mGain, mt.fadeIn, mt.fadeOut, dmt.pcm)) {
            decodedMusic.push_back(std::move(dmt));
        }
    }

    UINT32 vpInW = srcW, vpInH = srcH;
    for (const auto& sg : segments) {
        if (_wcsicmp(sg.path.c_str(), primaryPath.c_str()) == 0) continue;
        ComPtr<IMFSourceReader> segProbe;
        if (SUCCEEDED(MFCreateSourceReaderFromURL(sg.path.c_str(), nullptr, segProbe.Put())) && segProbe) {
            for (DWORD si = 0; si < 8; ++si) {
                ComPtr<IMFMediaType> mt;
                if (FAILED(segProbe->GetNativeMediaType(si, 0, mt.Put())) || !mt) break;
                GUID maj{};
                mt->GetGUID(MF_MT_MAJOR_TYPE, &maj);
                if (maj == MFMediaType_Video) {
                    UINT32 iw = 0, ih = 0;
                    if (SUCCEEDED(MFGetAttributeSize(mt.Get(), MF_MT_FRAME_SIZE, &iw, &ih))) {
                        vpInW = std::max(vpInW, iw);
                        vpInH = std::max(vpInH, ih);
                    }
                } else if (maj == MFMediaType_Audio) {
                    foundA = true;
                }
            }
        }
    }
    const bool anyAudio = foundA || !decodedMusic.empty();

    UINT32 outW = srcW, outH = srcH, outFpsN = fpsN, outFpsD = fpsD, outBr = avgBr;
    const bool isDiscordExport = (spec.targetSizeMb > 0);
    if (spec.vertical916) {
        outW = (srcH >= 1080) ? 1080u : 720u;
        outH = (srcH >= 1080) ? 1920u : 1280u;
    }
    if (maxSegSpeed < 0.99) {
        outFpsN = std::max<uint32_t>(15u, static_cast<uint32_t>(std::lround((static_cast<double>(fpsN) / fpsD) * maxSegSpeed)));
        outFpsD = 1;
    } else if (!isDiscordExport && maxSegSpeed > 1.01) {
        outBr = std::min<uint32_t>(24000000u, static_cast<uint32_t>(outBr * 1.25));
    }
    if (isDiscordExport) {
        const double targetBits = static_cast<double>(spec.targetSizeMb) * 8.0 * 1024.0 * 1024.0 * 0.88;
        const double audioBits = anyAudio ? (96000.0 * totalOutDurSec) : 0.0;
        double vBpsD = (targetBits - audioBits) / totalOutDurSec;
        uint32_t vBps = static_cast<uint32_t>(std::max(450000.0, std::min(12000000.0, vBpsD)));
        if (vBps < 2000000u) {
            if (!spec.vertical916 && outH > 720) {
                outW = 1280;
                outH = 720;
            } else if (spec.vertical916 && outW > 720) {
                outW = 720;
                outH = 1280;
            }
            if ((outFpsN / outFpsD) > 30) {
                outFpsN = 30;
                outFpsD = 1;
            }
        }
        outBr = vBps;
    }
    (void)minSegSpeed;
    const bool needVp = (outW != srcW || outH != srcH || spec.vertical916 || hasInsert || segments.size() > 1);
    const LONGLONG outFrameDur100 = (10000000LL * outFpsD) / std::max<uint32_t>(1u, outFpsN);

    std::wstring tmpPath = spec.dstPath + L".tmp.mp4";
    DeleteFileW(tmpPath.c_str());

    ComPtr<IMFAttributes> wAttr;
    MFCreateAttributes(wAttr.Put(), 4);
    if (wAttr) {
        wAttr->SetUINT32(MF_SINK_WRITER_DISABLE_THROTTLING, TRUE);
        wAttr->SetUINT32(MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, TRUE);
    }

    ComPtr<IMFSinkWriter> sw;
    HRESULT hr = MFCreateSinkWriterFromURL(tmpPath.c_str(), nullptr, wAttr.Get(), sw.Put());
    if (FAILED(hr) || !sw) {
        outError = "Çıkış MP4 dosyası oluşturulamadı.";
        return false;
    }

    auto setColorAttrs = [&](IMFMediaType* mt) {
        mt->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
        mt->SetUINT32(MF_MT_YUV_MATRIX, yuvMatrix);
        mt->SetUINT32(MF_MT_VIDEO_PRIMARIES, primaries);
        mt->SetUINT32(MF_MT_TRANSFER_FUNCTION, transFunc);
        mt->SetUINT32(MF_MT_VIDEO_NOMINAL_RANGE, nomRange);
    };

    ComPtr<IMFMediaType> outV;
    MFCreateMediaType(outV.Put());
    outV->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    outV->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264);
    outV->SetUINT32(MF_MT_AVG_BITRATE, outBr);
    MFSetAttributeSize(outV.Get(), MF_MT_FRAME_SIZE, outW, outH);
    MFSetAttributeRatio(outV.Get(), MF_MT_FRAME_RATE, outFpsN, outFpsD);
    outV->SetUINT32(MF_MT_MPEG2_PROFILE, eAVEncH264VProfile_High);
    setColorAttrs(outV.Get());

    DWORD vOutIdx = 0;
    hr = sw->AddStream(outV.Get(), &vOutIdx);
    if (SUCCEEDED(hr)) {
        ComPtr<IMFMediaType> inV;
        MFCreateMediaType(inV.Put());
        inV->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        inV->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12);
        MFSetAttributeSize(inV.Get(), MF_MT_FRAME_SIZE, outW, outH);
        MFSetAttributeRatio(inV.Get(), MF_MT_FRAME_RATE, outFpsN, outFpsD);
        setColorAttrs(inV.Get());
        hr = sw->SetInputMediaType(vOutIdx, inV.Get(), nullptr);
    }
    if (FAILED(hr)) {
        DeleteFileW(tmpPath.c_str());
        outError = "Çıkış video formatı ayarlanamadı.";
        return false;
    }

    // Always emit a single clean Mix AAC stream on re-encoded outputs so Discord, browsers, and WMP play full audio reliably
    DWORD aOutMix = 0, aOutGame = 0, aOutMic = 0;
    bool hasOutMix = anyAudio && AddAacOutputStream(sw.Get(), aOutMix);
    bool hasOutGame = false;
    bool hasOutMic = false;
    (void)srcMultiTrack;

    {
        ComPtr<ICodecAPI> encCodec;
        if (SUCCEEDED(sw->GetServiceForStream(vOutIdx, GUID_NULL, IID_ICodecAPI, encCodec.PutVoid())) && encCodec) {
            VARIANT v;
            VariantInit(&v);
            v.vt = VT_UI4;
            v.ulVal = std::max<uint32_t>(1u, outFpsN / outFpsD);
            encCodec->SetValue(&CODECAPI_AVEncMPVGOPSize, &v);

            // Disable B-frame reordering so hardware encoder never holds/overwrites queued DXGI frames
            // (Avoid AVEncCommonLowLatency=TRUE which can distort VUI/HRD timing in Chromium/Discord)
            v.ulVal = 0;
            encCodec->SetValue(&CODECAPI_AVEncMPVDefaultBPictureCount, &v);
        }
    }

    if (FAILED(sw->BeginWriting())) {
        DeleteFileW(tmpPath.c_str());
        outError = "MP4 yazımı başlatılamadı.";
        return false;
    }

    ComPtr<ID3D11Texture2D> stgTex;
    ComPtr<ID3D11Texture2D> gpuOutPool[16];
    bool hasGpuPool = false;
    ComPtr<ID3D11VideoDevice> vdev;
    ComPtr<ID3D11VideoContext> vctx;
    ComPtr<ID3D11VideoProcessorEnumerator> vpEnum;
    ComPtr<ID3D11VideoProcessor> vp;
    ComPtr<ID3D11Texture2D> vpInTex, vpOutTex;
    ComPtr<ID3D11VideoProcessorInputView> vpInView;
    ComPtr<ID3D11VideoProcessorOutputView> vpOutView;

    if (dev) {
        D3D11_TEXTURE2D_DESC sd{};
        sd.Width = outW;
        sd.Height = outH;
        sd.MipLevels = 1;
        sd.ArraySize = 1;
        sd.Format = DXGI_FORMAT_NV12;
        sd.SampleDesc.Count = 1;
        sd.Usage = D3D11_USAGE_STAGING;
        sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        dev->CreateTexture2D(&sd, nullptr, stgTex.Put());

        // Always use per-frame IMFMediaBuffer via staging readback rather than a shared DXGI ring pool:
        // prevents shared decoder/encoder DXGI_ERROR_DEVICE_HUNG and 16-frame ring-buffer freeze loops.
        hasGpuPool = false;

        if (needVp && SUCCEEDED(dev->QueryInterface(__uuidof(ID3D11VideoDevice), vdev.PutVoid())) &&
            ctx && SUCCEEDED(ctx->QueryInterface(__uuidof(ID3D11VideoContext), vctx.PutVoid()))) {
            D3D11_VIDEO_PROCESSOR_CONTENT_DESC cd{};
            cd.InputFrameFormat = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
            cd.InputWidth = vpInW; cd.InputHeight = vpInH;
            cd.OutputWidth = outW; cd.OutputHeight = outH;
            cd.Usage = D3D11_VIDEO_USAGE_PLAYBACK_NORMAL;
            if (SUCCEEDED(vdev->CreateVideoProcessorEnumerator(&cd, vpEnum.Put())) && vpEnum) {
                vdev->CreateVideoProcessor(vpEnum.Get(), 0, vp.Put());
            }
            D3D11_TEXTURE2D_DESC td{};
            td.Width = vpInW; td.Height = vpInH; td.MipLevels = 1; td.ArraySize = 1;
            td.Format = DXGI_FORMAT_NV12; td.SampleDesc.Count = 1;
            td.Usage = D3D11_USAGE_DEFAULT; td.BindFlags = D3D11_BIND_DECODER;
            dev->CreateTexture2D(&td, nullptr, vpInTex.Put());
            td.Width = outW; td.Height = outH; td.BindFlags = D3D11_BIND_RENDER_TARGET;
            dev->CreateTexture2D(&td, nullptr, vpOutTex.Put());
            if (vp && vpEnum && vpInTex && vpOutTex) {
                D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC ivd{};
                ivd.ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D;
                vdev->CreateVideoProcessorInputView(vpInTex.Get(), vpEnum.Get(), &ivd, vpInView.Put());
                D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC ovd{};
                ovd.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D;
                vdev->CreateVideoProcessorOutputView(vpOutTex.Get(), vpEnum.Get(), &ovd, vpOutView.Put());
            }
        }
    }

    const LONGLONG defaultVDur = outFrameDur100;
    LONGLONG outVBase100 = 0;
    int64_t outFrameIdx = 0;
    int vWritten = 0;
    bool aborted = false;

    std::vector<int16_t> outMixAccum, outGameAccum, outMicAccum;
    int64_t mixWrittenPairs = 0, gameWrittenPairs = 0, micWrittenPairs = 0;

    const BYTE blackY = (nomRange == MFNominalRange_0_255) ? 0 : 16;
    std::vector<BYTE> blackNv12(static_cast<size_t>(outW) * outH * 3 / 2, 128);
    memset(blackNv12.data(), blackY, static_cast<size_t>(outW) * outH);
    std::vector<BYTE> fxScratch;
    std::vector<BYTE> fxRawCopy;

    for (size_t segIdx = 0; segIdx < segments.size(); ++segIdx) {
        const auto& seg = segments[segIdx];
        if (cancelFlag && cancelFlag->load()) { aborted = true; break; }

        // Fill any intentional timeline gap before this segment with black NV12 video + silent/music audio
        const LONGLONG segStart100 = static_cast<LONGLONG>(std::llround(std::max(0.0, seg.timelineStartSec) * 1e7));
        bool emittedGap = false;
        while (true) {
            if (cancelFlag && cancelFlag->load()) { aborted = true; break; }
            LONGLONG gridTs = (outFrameIdx * 10000000LL * outFpsD) / outFpsN;
            if (gridTs + (outFrameDur100 / 2) >= segStart100) break;
            LONGLONG nextGridTs = ((outFrameIdx + 1) * 10000000LL * outFpsD) / outFpsN;
            LONGLONG newDur = std::max<LONGLONG>(1, nextGridTs - gridTs);
            ComPtr<IMFSample> blkSmp;
            if (!CreateBlackNv12Sample(blackNv12, gridTs, newDur, blkSmp)) {
                aborted = true;
                break;
            }
            if (!spec.transitions.empty()) {
                ApplyNv12TransitionEffects(blkSmp.Get(), outW, outH, blackY,
                    static_cast<double>(gridTs) / 1e7, spec.transitions, fxScratch);
            }
            if (FAILED(sw->WriteSample(vOutIdx, blkSmp.Get()))) {
                aborted = true;
                break;
            }
            ++outFrameIdx;
            ++vWritten;
            emittedGap = true;

            if (hasOutMix) {
                constexpr size_t kSilentChunk = 16384;
                const size_t targetGapPairs = static_cast<size_t>((nextGridTs * 48000LL) / 10000000LL);
                const size_t curPairs = static_cast<size_t>(mixWrittenPairs) + (outMixAccum.size() / 2);
                if (curPairs + kSilentChunk <= targetGapPairs) {
                    outMixAccum.insert(outMixAccum.end(), kSilentChunk * 2, 0);
                    if (hasOutGame) outGameAccum.insert(outGameAccum.end(), kSilentChunk * 2, 0);
                    if (hasOutMic)  outMicAccum.insert(outMicAccum.end(), kSilentChunk * 2, 0);
                    FlushMixPcmChunks(sw.Get(), aOutMix, outMixAccum, mixWrittenPairs, decodedMusic, false);
                    if (hasOutGame) FlushPcmChunks(sw.Get(), aOutGame, outGameAccum, gameWrittenPairs, false);
                    if (hasOutMic)  FlushPcmChunks(sw.Get(), aOutMic, outMicAccum, micWrittenPairs, false);
                }
            }
        }
        if (aborted) break;
        if (emittedGap) {
            outVBase100 = (outFrameIdx * 10000000LL * outFpsD) / outFpsN;
            if (hasOutMix) {
                const size_t targetGapPairs = static_cast<size_t>((outVBase100 * 48000LL) / 10000000LL);
                const size_t curPairs = static_cast<size_t>(mixWrittenPairs) + (outMixAccum.size() / 2);
                if (curPairs < targetGapPairs) {
                    const size_t padPairs = targetGapPairs - curPairs;
                    outMixAccum.insert(outMixAccum.end(), padPairs * 2, 0);
                    if (hasOutGame) outGameAccum.insert(outGameAccum.end(), padPairs * 2, 0);
                    if (hasOutMic)  outMicAccum.insert(outMicAccum.end(), padPairs * 2, 0);
                    FlushMixPcmChunks(sw.Get(), aOutMix, outMixAccum, mixWrittenPairs, decodedMusic, false);
                    if (hasOutGame) FlushPcmChunks(sw.Get(), aOutGame, outGameAccum, gameWrittenPairs, false);
                    if (hasOutMic)  FlushPcmChunks(sw.Get(), aOutMic, outMicAccum, micWrittenPairs, false);
                }
            }
        }

        ComPtr<IMFAttributes> rAttr;
        MFCreateAttributes(rAttr.Put(), 4);
        if (dm && rAttr) {
            rAttr->SetUINT32(MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, TRUE);
            rAttr->SetUnknown(MF_SOURCE_READER_D3D_MANAGER, dm.Get());
        }

        ComPtr<IMFSourceReader> sr;
        if (FAILED(MFCreateSourceReaderFromURL(seg.path.c_str(), rAttr.Get(), sr.Put())) || !sr) {
            outError = "Kaynak klip bölümü okunamadı.";
            sw.Reset();
            DeleteFileW(tmpPath.c_str());
            return false;
        }

        DWORD vIn = 0;
        std::vector<DWORD> aIns;
        bool segFoundV = false;
        for (DWORD si = 0; si < 8; ++si) {
            ComPtr<IMFMediaType> mt;
            if (FAILED(sr->GetNativeMediaType(si, 0, mt.Put())) || !mt) break;
            GUID maj{};
            mt->GetGUID(MF_MT_MAJOR_TYPE, &maj);
            if (maj == MFMediaType_Video && !segFoundV) { vIn = si; segFoundV = true; }
            if (maj == MFMediaType_Audio) aIns.push_back(si);
        }
        if (!segFoundV) continue;

        sr->SetStreamSelection(static_cast<DWORD>(MF_SOURCE_READER_ALL_STREAMS), FALSE);
        sr->SetStreamSelection(vIn, TRUE);

        ComPtr<IMFMediaType> decV;
        MFCreateMediaType(decV.Put());
        decV->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        decV->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12);
        MFSetAttributeSize(decV.Get(), MF_MT_FRAME_SIZE, srcW, srcH);
        if (FAILED(sr->SetCurrentMediaType(vIn, nullptr, decV.Get()))) {
            // Fallback without forced frame size
            ComPtr<IMFMediaType> decV2;
            MFCreateMediaType(decV2.Put());
            decV2->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
            decV2->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12);
            sr->SetCurrentMediaType(vIn, nullptr, decV2.Get());
        }

        auto cfgAudioIn = [&](DWORD idx) -> bool {
            sr->SetStreamSelection(idx, TRUE);
            ComPtr<IMFMediaType> decA;
            MFCreateMediaType(decA.Put());
            decA->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
            decA->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_PCM);
            decA->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
            decA->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, 48000);
            decA->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, 2);
            return SUCCEEDED(sr->SetCurrentMediaType(idx, nullptr, decA.Get()));
        };

        ClipAudioTrackLayout segLayout;
        const bool hasSegLayout = !aIns.empty() && DetectClipAudioLayout(seg.path, segLayout);
        DWORD logicalAIns[3] = { DWORD(-1), DWORD(-1), DWORD(-1) };
        if (hasSegLayout && segLayout.streamIndices.size() == aIns.size()) {
            const int mixPos = segLayout.mixStreamPos;
            const int gamePos = segLayout.gameStreamPos;
            const int micPos = segLayout.micStreamPos;
            logicalAIns[0] = (mixPos >= 0 && static_cast<size_t>(mixPos) < aIns.size()) ? aIns[static_cast<size_t>(mixPos)] : aIns[0];
            if (segLayout.hasSeparateGameMic && gamePos >= 0 && micPos >= 0 &&
                static_cast<size_t>(gamePos) < aIns.size() && static_cast<size_t>(micPos) < aIns.size()) {
                logicalAIns[1] = aIns[static_cast<size_t>(gamePos)];
                logicalAIns[2] = aIns[static_cast<size_t>(micPos)];
            }
        } else {
            for (size_t k = 0; k < aIns.size() && k < 3; ++k) logicalAIns[k] = aIns[k];
        }

        const bool segHasMulti = (logicalAIns[1] != DWORD(-1) && logicalAIns[2] != DWORD(-1));
        bool segHasAudio = false;
        bool segHasGameMic = false;
        if (segHasMulti) {
            segHasGameMic = cfgAudioIn(logicalAIns[1]) && cfgAudioIn(logicalAIns[2]);
            if (segHasGameMic) {
                segHasAudio = true;
            } else {
                sr->SetStreamSelection(logicalAIns[1], FALSE);
                sr->SetStreamSelection(logicalAIns[2], FALSE);
                segHasAudio = (logicalAIns[0] != DWORD(-1)) && cfgAudioIn(logicalAIns[0]);
            }
        } else if (logicalAIns[0] != DWORD(-1)) {
            segHasAudio = cfgAudioIn(logicalAIns[0]);
        }

        const LONGLONG start100 = static_cast<LONGLONG>(seg.startSec * 1e7);
        const LONGLONG end100 = static_cast<LONGLONG>(seg.endSec * 1e7);
        const LONGLONG span100 = std::max<LONGLONG>(1, end100 - start100);
        const LONGLONG outSpan100 = static_cast<LONGLONG>(static_cast<double>(span100) / seg.speed);

        PROPVARIANT pv;
        PropVariantInit(&pv);
        pv.vt = VT_I8;
        pv.hVal.QuadPart = std::max(0LL, start100 - 20000000LL);
        sr->SetCurrentPosition(GUID_NULL, pv);
        PropVariantClear(&pv);

        bool vDone = false;
        bool aDone[3] = { !(segHasAudio && !segHasGameMic), !segHasGameMic, !segHasGameMic };
        std::vector<int16_t> segPcm[3];
        const size_t targetOutPairs = static_cast<size_t>((outSpan100 * 48000LL) / 10000000LL);
        size_t segOutFlushed = 0;
        int64_t segOutFrames = 0;

        while ((!vDone || !aDone[0] || !aDone[1] || !aDone[2]) && !aborted) {
            if (cancelFlag && cancelFlag->load()) { aborted = true; break; }

            DWORD streamIdx = 0, flags = 0;
            LONGLONG ts = 0;
            ComPtr<IMFSample> smp;
            hr = sr->ReadSample(static_cast<DWORD>(MF_SOURCE_READER_ANY_STREAM), 0, &streamIdx, &flags, &ts, smp.Put());
            if (FAILED(hr)) break;

            auto markStreamDone = [&](DWORD si) {
                sr->SetStreamSelection(si, FALSE);
                if (si == vIn) vDone = true;
                else {
                    for (size_t k = 0; k < 3; ++k) {
                        if (logicalAIns[k] == si) aDone[k] = true;
                    }
                }
            };

            if (flags & MF_SOURCE_READERF_ENDOFSTREAM) {
                markStreamDone(streamIdx);
                continue;
            }
            if (!smp) continue;

            LONGLONG dur = 0;
            smp->GetSampleDuration(&dur);
            if (dur <= 0) dur = (streamIdx == vIn) ? defaultVDur : ((1024LL * 10000000LL) / 48000LL);
            if (ts >= end100) {
                markStreamDone(streamIdx);
                continue;
            }
            if (ts + dur <= start100) continue;

            if (streamIdx == vIn && !vDone) {
                LONGLONG relIn = std::max(0LL, ts - start100);
                LONGLONG mappedTs = outVBase100 + static_cast<LONGLONG>(static_cast<double>(relIn) / seg.speed);
                LONGLONG gridTs = (outFrameIdx * 10000000LL * outFpsD) / outFpsN;
                if (mappedTs + (outFrameDur100 / 2) < gridTs) {
                    continue; // cleanly pace to output fps without cramming or stuttering
                }
                if (gridTs >= outVBase100 + outSpan100) {
                    continue;
                }
                LONGLONG nextGridTs = ((outFrameIdx + 1) * 10000000LL * outFpsD) / outFpsN;
                LONGLONG newDur = std::max<LONGLONG>(1, nextGridTs - gridTs);

                float segFadeAlpha = 1.0f;
                if (spec.fadeTransitions && segments.size() > 1) {
                    const double segOutDurSec = static_cast<double>(outSpan100) / 1e7;
                    const double segOutStartSec = static_cast<double>(outVBase100) / 1e7;
                    const double fadeSec = std::min(0.35, segOutDurSec * 0.25);
                    const double relOutSec = static_cast<double>(std::max<LONGLONG>(0, gridTs - outVBase100)) / 1e7;
                    if (segIdx > 0 && relOutSec < fadeSec && fadeSec > 0.01 &&
                        !HasCustomTransitionNear(segOutStartSec, spec.transitions)) {
                        segFadeAlpha = std::min(segFadeAlpha, std::max(0.05f, static_cast<float>(relOutSec / fadeSec)));
                    }
                    if (segIdx + 1 < segments.size() && relOutSec > (segOutDurSec - fadeSec) && fadeSec > 0.01 &&
                        !HasCustomTransitionNear(segOutStartSec + segOutDurSec, spec.transitions)) {
                        segFadeAlpha = std::min(segFadeAlpha, std::max(0.05f, static_cast<float>((segOutDurSec - relOutSec) / fadeSec)));
                    }
                }

                ComPtr<IMFSample> outSmp;
                MFCreateSample(outSmp.Put());
                if (!outSmp) continue;
                outSmp->SetSampleTime(gridTs);
                outSmp->SetSampleDuration(newDur);

                bool copied = false;
                // Only use VideoProcessor when scaling, 9:16 vertical, or aspect-fit is actually needed for THIS frame
                bool useVpForFrame = needVp && vp && vctx && vpInTex && vpOutTex && vpInView && vpOutView && stgTex;
                if (useVpForFrame && !spec.vertical916) {
                    ComPtr<IMFMediaBuffer> chkBuf;
                    ComPtr<IMFDXGIBuffer> chkDb;
                    ComPtr<ID3D11Texture2D> chkTex;
                    if (SUCCEEDED(smp->GetBufferByIndex(0, chkBuf.Put())) && chkBuf &&
                        SUCCEEDED(chkBuf->QueryInterface(__uuidof(IMFDXGIBuffer), chkDb.PutVoid())) &&
                        SUCCEEDED(chkDb->GetResource(__uuidof(ID3D11Texture2D), chkTex.PutVoid()))) {
                        D3D11_TEXTURE2D_DESC cDesc{};
                        chkTex->GetDesc(&cDesc);
                        if (cDesc.Width == outW && cDesc.Height == outH) {
                            useVpForFrame = false; // direct GPU copy: avoids D3D11 VideoProcessor TDR hangs
                        }
                    }
                }
                if (useVpForFrame) {
                    ComPtr<IMFMediaBuffer> buf;
                    ComPtr<IMFDXGIBuffer> db;
                    ComPtr<ID3D11Texture2D> dtex;
                    UINT sub = 0;
                    if (SUCCEEDED(smp->GetBufferByIndex(0, buf.Put())) && buf &&
                        SUCCEEDED(buf->QueryInterface(__uuidof(IMFDXGIBuffer), db.PutVoid())) &&
                        SUCCEEDED(db->GetResource(__uuidof(ID3D11Texture2D), dtex.PutVoid()))) {
                        db->GetSubresourceIndex(&sub);
                        D3D11_TEXTURE2D_DESC dDesc{};
                        dtex->GetDesc(&dDesc);
                        UINT copyW = std::min(vpInW, dDesc.Width) & ~1U;
                        UINT copyH = std::min(vpInH, dDesc.Height) & ~1U;
                        D3D11_BOX box{ 0, 0, 0, copyW, copyH, 1 };
                        ctx->CopySubresourceRegion(vpInTex.Get(), 0, 0, 0, 0, dtex.Get(), sub, &box);

                        if (spec.vertical916) {
                            LONG cropW = std::min<LONG>(static_cast<LONG>(copyW),
                                std::max<LONG>(16, (static_cast<LONG>(copyH) * static_cast<LONG>(outW)) / static_cast<LONG>(outH))) & ~1L;
                            LONG cropX = std::max<LONG>(0, (static_cast<LONG>(copyW) - cropW) / 2) & ~1L;
                            RECT bgSrc{ cropX, 0, cropX + cropW, static_cast<LONG>(copyH) };
                            RECT dstFull{ 0, 0, static_cast<LONG>(outW), static_cast<LONG>(outH) };
                            LONG fgH = std::min<LONG>(static_cast<LONG>(outH),
                                (static_cast<LONG>(outW) * static_cast<LONG>(copyH)) / std::max<LONG>(2, static_cast<LONG>(copyW))) & ~1L;
                            LONG fgY = std::max<LONG>(0, (static_cast<LONG>(outH) - fgH) / 2) & ~1L;
                            RECT fgSrc{ 0, 0, static_cast<LONG>(copyW), static_cast<LONG>(copyH) };
                            RECT fgDst{ 0, fgY, static_cast<LONG>(outW), fgY + fgH };

                            vctx->VideoProcessorSetStreamSourceRect(vp.Get(), 0, TRUE, &bgSrc);
                            vctx->VideoProcessorSetStreamDestRect(vp.Get(), 0, TRUE, &dstFull);
                            vctx->VideoProcessorSetStreamSourceRect(vp.Get(), 1, TRUE, &fgSrc);
                            vctx->VideoProcessorSetStreamDestRect(vp.Get(), 1, TRUE, &fgDst);

                            D3D11_VIDEO_PROCESSOR_STREAM streams[2]{};
                            streams[0].Enable = TRUE; streams[0].pInputSurface = vpInView.Get();
                            streams[1].Enable = TRUE; streams[1].pInputSurface = vpInView.Get();
                            vctx->VideoProcessorBlt(vp.Get(), vpOutView.Get(), 0, 2, streams);
                        } else {
                            RECT srcFull{ 0, 0, static_cast<LONG>(copyW), static_cast<LONG>(copyH) };
                            RECT dstRect{ 0, 0, static_cast<LONG>(outW), static_cast<LONG>(outH) };
                            if (static_cast<uint64_t>(copyW) * outH != static_cast<uint64_t>(copyH) * outW) {
                                D3D11_VIDEO_COLOR bgCol{};
                                bgCol.RGBA = { 0.0f, 0.0f, 0.0f, 1.0f };
                                vctx->VideoProcessorSetOutputBackgroundColor(vp.Get(), FALSE, &bgCol);
                                double srcAr = static_cast<double>(copyW) / std::max(1.0, static_cast<double>(copyH));
                                double dstAr = static_cast<double>(outW) / std::max(1.0, static_cast<double>(outH));
                                if (srcAr < dstAr) {
                                    LONG fitW = std::min<LONG>(static_cast<LONG>(outW), static_cast<LONG>(std::lround(outH * srcAr))) & ~1L;
                                    LONG fitX = ((static_cast<LONG>(outW) - fitW) / 2) & ~1L;
                                    dstRect = { fitX, 0, fitX + fitW, static_cast<LONG>(outH) };
                                } else {
                                    LONG fitH = std::min<LONG>(static_cast<LONG>(outH), static_cast<LONG>(std::lround(outW / srcAr))) & ~1L;
                                    LONG fitY = ((static_cast<LONG>(outH) - fitH) / 2) & ~1L;
                                    dstRect = { 0, fitY, static_cast<LONG>(outW), fitY + fitH };
                                }
                            }
                            vctx->VideoProcessorSetStreamSourceRect(vp.Get(), 0, TRUE, &srcFull);
                            vctx->VideoProcessorSetStreamDestRect(vp.Get(), 0, TRUE, &dstRect);
                            D3D11_VIDEO_PROCESSOR_STREAM st{};
                            st.Enable = TRUE; st.pInputSurface = vpInView.Get();
                            if (FAILED(vctx->VideoProcessorBlt(vp.Get(), vpOutView.Get(), 0, 1, &st))) {
                                copied = false;
                            }
                        }

                        bool dxgiAttached = false;
                        if (hasGpuPool && vpOutTex) {
                            const int pIdx = static_cast<int>(outFrameIdx % 16);
                            ctx->CopyResource(gpuOutPool[pIdx].Get(), vpOutTex.Get());
                            ComPtr<IMFMediaBuffer> dxBuf;
                            if (SUCCEEDED(MFCreateDXGISurfaceBuffer(__uuidof(ID3D11Texture2D), gpuOutPool[pIdx].Get(), 0, FALSE, dxBuf.Put())) && dxBuf) {
                                dxBuf->SetCurrentLength((outW * outH * 3) / 2);
                                outSmp->AddBuffer(dxBuf.Get());
                                dxgiAttached = true;
                                copied = true;
                            }
                        }
                        if (!dxgiAttached) {
                            ctx->CopyResource(stgTex.Get(), vpOutTex.Get());
                            D3D11_MAPPED_SUBRESOURCE mapped{};
                            if (SUCCEEDED(ctx->Map(stgTex.Get(), 0, D3D11_MAP_READ, 0, &mapped))) {
                                const DWORD nvLen = (outW * outH * 3) / 2;
                                ComPtr<IMFMediaBuffer> ob;
                                if (SUCCEEDED(MFCreateMemoryBuffer(nvLen, ob.Put())) && ob) {
                                    BYTE* dst = nullptr;
                                    if (SUCCEEDED(ob->Lock(&dst, nullptr, nullptr))) {
                                        const BYTE* srcY = reinterpret_cast<const BYTE*>(mapped.pData);
                                        for (uint32_t y = 0; y < outH; ++y) memcpy(dst + y * outW, srcY + y * mapped.RowPitch, outW);
                                        const BYTE* srcUV = srcY + outH * mapped.RowPitch;
                                        BYTE* dstUV = dst + outW * outH;
                                        for (uint32_t y = 0; y < outH / 2; ++y) memcpy(dstUV + y * outW, srcUV + y * mapped.RowPitch, outW);
                                        ob->Unlock();
                                        ob->SetCurrentLength(nvLen);
                                        outSmp->AddBuffer(ob.Get());
                                        copied = true;
                                    }
                                }
                                ctx->Unmap(stgTex.Get(), 0);
                            }
                        }
                    }
                }
                if (!copied) {
                    if (hasGpuPool && ctx) {
                        ComPtr<IMFMediaBuffer> buf;
                        ComPtr<IMFDXGIBuffer> db;
                        ComPtr<ID3D11Texture2D> dtex;
                        UINT sub = 0;
                        if (SUCCEEDED(smp->GetBufferByIndex(0, buf.Put())) && buf &&
                            SUCCEEDED(buf->QueryInterface(__uuidof(IMFDXGIBuffer), db.PutVoid())) &&
                            SUCCEEDED(db->GetResource(__uuidof(ID3D11Texture2D), dtex.PutVoid()))) {
                            db->GetSubresourceIndex(&sub);
                            const int pIdx = static_cast<int>(outFrameIdx % 16);
                            D3D11_TEXTURE2D_DESC dDesc{};
                            dtex->GetDesc(&dDesc);
                            D3D11_BOX box{ 0, 0, 0, std::min(outW, dDesc.Width), std::min(outH, dDesc.Height), 1 };
                            ctx->CopySubresourceRegion(gpuOutPool[pIdx].Get(), 0, 0, 0, 0, dtex.Get(), sub, &box);
                            ComPtr<IMFMediaBuffer> dxBuf;
                            if (SUCCEEDED(MFCreateDXGISurfaceBuffer(__uuidof(ID3D11Texture2D), gpuOutPool[pIdx].Get(), 0, FALSE, dxBuf.Put())) && dxBuf) {
                                dxBuf->SetCurrentLength((outW * outH * 3) / 2);
                                outSmp->AddBuffer(dxBuf.Get());
                                copied = true;
                            }
                        }
                    }
                    if (!copied) {
                        copied = CopyVideoSampleBuffer(ctx.Get(), stgTex.Get(), smp.Get(), outW, outH, outSmp.Get());
                    }
                }
                if (!copied) continue;
                if (segFadeAlpha < 0.999f) {
                    ApplyNv12FadeInSample(outSmp.Get(), outW, outH, segFadeAlpha);
                }
                const bool needDupBackup = !spec.transitions.empty() || !spec.frameTransforms.empty();
                fxRawCopy.clear();
                if (needDupBackup) {
                    ComPtr<IMFMediaBuffer> rb;
                    if (SUCCEEDED(outSmp->ConvertToContiguousBuffer(rb.Put())) && rb) {
                        BYTE* rp = nullptr; DWORD rl = 0;
                        if (SUCCEEDED(rb->Lock(&rp, nullptr, &rl)) && rl > 0) {
                            fxRawCopy.assign(rp, rp + rl);
                            rb->Unlock();
                        }
                    }
                    const double frameSec = static_cast<double>(gridTs) / 1e7;
                    ApplyNv12FrameTransform(outSmp.Get(), outW, outH, blackY,
                        frameSec, spec.frameTransforms, fxScratch);
                    ApplyNv12TransitionEffects(outSmp.Get(), outW, outH, blackY,
                        frameSec, spec.transitions, fxScratch);
                }
                if (SUCCEEDED(sw->WriteSample(vOutIdx, outSmp.Get()))) {
                    ++outFrameIdx;
                    ++segOutFrames;
                    ++vWritten;

                    // Hold/duplicate frame on grid if input FPS is lower than output FPS (e.g. 30fps clip in 60fps montage, or 0.5x)
                    int dupCap = 0;
                    while (dupCap < 6) {
                        LONGLONG gTs = (outFrameIdx * 10000000LL * outFpsD) / outFpsN;
                        if (gTs >= outVBase100 + outSpan100) break;
                        if (gTs + (outFrameDur100 / 2) > mappedTs) break;
                        LONGLONG nGTs = ((outFrameIdx + 1) * 10000000LL * outFpsD) / outFpsN;
                        LONGLONG nDur = std::max<LONGLONG>(1, nGTs - gTs);
                        ComPtr<IMFSample> dupSmp;
                        MFCreateSample(dupSmp.Put());
                        if (!dupSmp) break;
                        dupSmp->SetSampleTime(gTs);
                        dupSmp->SetSampleDuration(nDur);
                        bool dupOk = false;
                        if (!fxRawCopy.empty()) {
                            const DWORD sl = static_cast<DWORD>(fxRawCopy.size());
                            ComPtr<IMFMediaBuffer> cb;
                            if (SUCCEEDED(MFCreateMemoryBuffer(sl, cb.Put())) && cb) {
                                BYTE* dp = nullptr;
                                if (SUCCEEDED(cb->Lock(&dp, nullptr, nullptr))) {
                                    memcpy(dp, fxRawCopy.data(), sl);
                                    cb->Unlock();
                                    cb->SetCurrentLength(sl);
                                    dupSmp->AddBuffer(cb.Get());
                                    dupOk = true;
                                }
                            }
                            if (dupOk) {
                                const double dupSec = static_cast<double>(gTs) / 1e7;
                                ApplyNv12FrameTransform(dupSmp.Get(), outW, outH, blackY,
                                    dupSec, spec.frameTransforms, fxScratch);
                                ApplyNv12TransitionEffects(dupSmp.Get(), outW, outH, blackY,
                                    dupSec, spec.transitions, fxScratch);
                            }
                        }
                        if (!dupOk && hasGpuPool && ctx) {
                            const int pIdx = static_cast<int>(outFrameIdx % 16);
                            if (needVp && vpOutTex) {
                                ctx->CopyResource(gpuOutPool[pIdx].Get(), vpOutTex.Get());
                                ComPtr<IMFMediaBuffer> dxBuf;
                                if (SUCCEEDED(MFCreateDXGISurfaceBuffer(__uuidof(ID3D11Texture2D), gpuOutPool[pIdx].Get(), 0, FALSE, dxBuf.Put())) && dxBuf) {
                                    dxBuf->SetCurrentLength((outW * outH * 3) / 2);
                                    dupSmp->AddBuffer(dxBuf.Get());
                                    dupOk = true;
                                }
                            }
                        }
                        if (!dupOk) {
                            ComPtr<IMFMediaBuffer> srcBuf;
                            if (SUCCEEDED(outSmp->ConvertToContiguousBuffer(srcBuf.Put())) && srcBuf) {
                                BYTE* sp = nullptr; DWORD sl = 0;
                                if (SUCCEEDED(srcBuf->Lock(&sp, nullptr, &sl)) && sl > 0) {
                                    ComPtr<IMFMediaBuffer> cb;
                                    if (SUCCEEDED(MFCreateMemoryBuffer(sl, cb.Put())) && cb) {
                                        BYTE* dp = nullptr;
                                        if (SUCCEEDED(cb->Lock(&dp, nullptr, nullptr))) {
                                            memcpy(dp, sp, sl);
                                            cb->Unlock();
                                            cb->SetCurrentLength(sl);
                                            dupSmp->AddBuffer(cb.Get());
                                            dupOk = true;
                                        }
                                    }
                                    srcBuf->Unlock();
                                }
                            }
                        }
                        if (!dupOk || FAILED(sw->WriteSample(vOutIdx, dupSmp.Get()))) break;
                        ++outFrameIdx;
                        ++segOutFrames;
                        ++vWritten;
                        ++dupCap;
                    }

                    // Keep IMFSinkWriter interleaved smoothly even when a video segment lacks an audio stream
                    if (hasOutMix && !segHasAudio) {
                        constexpr size_t kSilentChunk = 16384;
                        const size_t vProducedPairs = static_cast<size_t>((segOutFrames * outFpsD * 48000LL) / outFpsN);
                        const size_t capPairs = std::min(vProducedPairs, targetOutPairs);
                        while (segOutFlushed + kSilentChunk <= capPairs) {
                            outMixAccum.insert(outMixAccum.end(), kSilentChunk * 2, 0);
                            if (hasOutGame) outGameAccum.insert(outGameAccum.end(), kSilentChunk * 2, 0);
                            if (hasOutMic)  outMicAccum.insert(outMicAccum.end(), kSilentChunk * 2, 0);
                            FlushMixPcmChunks(sw.Get(), aOutMix, outMixAccum, mixWrittenPairs, decodedMusic, false);
                            if (hasOutGame) FlushPcmChunks(sw.Get(), aOutGame, outGameAccum, gameWrittenPairs, false);
                            if (hasOutMic)  FlushPcmChunks(sw.Get(), aOutMic, outMicAccum, micWrittenPairs, false);
                            segOutFlushed += kSilentChunk;
                        }
                    }
                }
            } else {
                int trackIdx = -1;
                for (size_t k = 0; k < 3; ++k) {
                    if (logicalAIns[k] == streamIdx && !aDone[k]) { trackIdx = static_cast<int>(k); break; }
                }
                if (trackIdx >= 0) {
                    ComPtr<IMFMediaBuffer> abuf;
                    if (SUCCEEDED(smp->ConvertToContiguousBuffer(abuf.Put())) && abuf) {
                        BYTE* p = nullptr;
                        DWORD len = 0;
                        if (SUCCEEDED(abuf->Lock(&p, nullptr, &len)) && len >= 4) {
                            const size_t nPairs = len / 4;
                            size_t skipPairs = 0;
                            if (ts < start100) {
                                skipPairs = static_cast<size_t>(((start100 - ts) * 48000LL) / 10000000LL);
                            }
                            const size_t segMaxPairs = static_cast<size_t>((span100 * 48000LL) / 10000000LL);
                            const size_t curPairs = segPcm[trackIdx].size() / 2;
                            if (skipPairs < nPairs && curPairs < segMaxPairs) {
                                const size_t take = std::min<size_t>(nPairs - skipPairs, segMaxPairs - curPairs);
                                const int16_t* s16 = reinterpret_cast<const int16_t*>(p) + skipPairs * 2;
                                segPcm[trackIdx].insert(segPcm[trackIdx].end(), s16, s16 + take * 2);
                            }
                            if ((segPcm[trackIdx].size() / 2) >= segMaxPairs) {
                                aDone[trackIdx] = true;
                                sr->SetStreamSelection(streamIdx, FALSE);
                            }
                            abuf->Unlock();
                        }
                    }
                }
                // Live-flush audio chunks (~0.34s) so IMFSinkWriter interleaver never stalls video WriteSample
                if (hasOutMix) {
                    constexpr size_t kChunkOut = 16384;
                    const size_t needIn = static_cast<size_t>(std::ceil(static_cast<double>(kChunkOut) * seg.speed)) + 4;
                    while (segOutFlushed + kChunkOut <= targetOutPairs) {
                        bool ready = segHasGameMic
                            ? (segPcm[1].size() / 2 >= needIn && segPcm[2].size() / 2 >= needIn)
                            : (segPcm[0].size() / 2 >= needIn);
                        if (!ready) break;
                        const size_t consumeIn = static_cast<size_t>(std::lround(static_cast<double>(kChunkOut) * seg.speed));
                        if (segHasGameMic) {
                            std::vector<int16_t> gIn(segPcm[1].begin(), segPcm[1].begin() + consumeIn * 2);
                            std::vector<int16_t> mIn(segPcm[2].begin(), segPcm[2].begin() + consumeIn * 2);
                            segPcm[1].erase(segPcm[1].begin(), segPcm[1].begin() + consumeIn * 2);
                            segPcm[2].erase(segPcm[2].begin(), segPcm[2].begin() + consumeIn * 2);
                            std::vector<int16_t> gRes, mRes;
                            AppendResampledPcm(gIn, seg.speed, seg.gameGain, kChunkOut, gRes);
                            AppendResampledPcm(mIn, seg.speed, seg.micGain, kChunkOut, mRes);
                            outMixAccum.reserve(outMixAccum.size() + kChunkOut * 2);
                            for (size_t i = 0; i < kChunkOut * 2; ++i) {
                                float mix = static_cast<float>(gRes[i]) / 32767.0f + static_cast<float>(mRes[i]) / 32767.0f;
                                outMixAccum.push_back(FloatToPcm16(mix));
                            }
                            if (hasOutGame) outGameAccum.insert(outGameAccum.end(), gRes.begin(), gRes.end());
                            if (hasOutMic)  outMicAccum.insert(outMicAccum.end(), mRes.begin(), mRes.end());
                        } else {
                            std::vector<int16_t> sIn(segPcm[0].begin(), segPcm[0].begin() + consumeIn * 2);
                            segPcm[0].erase(segPcm[0].begin(), segPcm[0].begin() + consumeIn * 2);
                            AppendResampledPcm(sIn, seg.speed, seg.gameGain, kChunkOut, outMixAccum);
                            if (hasOutGame) AppendResampledPcm(sIn, seg.speed, seg.gameGain, kChunkOut, outGameAccum);
                            if (hasOutMic)  outMicAccum.insert(outMicAccum.end(), kChunkOut * 2, 0);
                        }
                        FlushMixPcmChunks(sw.Get(), aOutMix, outMixAccum, mixWrittenPairs, decodedMusic, false);
                        if (hasOutGame) FlushPcmChunks(sw.Get(), aOutGame, outGameAccum, gameWrittenPairs, false);
                        if (hasOutMic)  FlushPcmChunks(sw.Get(), aOutMic, outMicAccum, micWrittenPairs, false);
                        segOutFlushed += kChunkOut;
                    }
                }
            }
        }

        if (aborted) break;

        const LONGLONG newOutVBase100 = (outFrameIdx * 10000000LL * outFpsD) / outFpsN;
        const size_t totalTargetPairsAtSegEnd = static_cast<size_t>((newOutVBase100 * 48000LL) / 10000000LL);
        const size_t curCumMixPairs = static_cast<size_t>(mixWrittenPairs) + (outMixAccum.size() / 2);

        if (hasOutMix && curCumMixPairs < totalTargetPairsAtSegEnd) {
            const size_t remOutPairs = totalTargetPairsAtSegEnd - curCumMixPairs;
            if (segHasGameMic) {
                std::vector<int16_t> gRes, mRes;
                AppendResampledPcm(segPcm[1], seg.speed, seg.gameGain, remOutPairs, gRes);
                AppendResampledPcm(segPcm[2], seg.speed, seg.micGain, remOutPairs, mRes);
                outMixAccum.reserve(outMixAccum.size() + remOutPairs * 2);
                for (size_t i = 0; i < remOutPairs * 2; ++i) {
                    float mix = static_cast<float>(gRes[i]) / 32767.0f + static_cast<float>(mRes[i]) / 32767.0f;
                    outMixAccum.push_back(FloatToPcm16(mix));
                }
                if (hasOutGame) outGameAccum.insert(outGameAccum.end(), gRes.begin(), gRes.end());
                if (hasOutMic)  outMicAccum.insert(outMicAccum.end(), mRes.begin(), mRes.end());
            } else {
                AppendResampledPcm(segPcm[0], seg.speed, seg.gameGain, remOutPairs, outMixAccum);
                if (hasOutGame) AppendResampledPcm(segPcm[0], seg.speed, seg.gameGain, remOutPairs, outGameAccum);
                if (hasOutMic)  outMicAccum.insert(outMicAccum.end(), remOutPairs * 2, 0);
            }
            FlushMixPcmChunks(sw.Get(), aOutMix, outMixAccum, mixWrittenPairs, decodedMusic, false);
            if (hasOutGame) FlushPcmChunks(sw.Get(), aOutGame, outGameAccum, gameWrittenPairs, false);
            if (hasOutMic)  FlushPcmChunks(sw.Get(), aOutMic, outMicAccum, micWrittenPairs, false);
        }

        outVBase100 = newOutVBase100;
    }

    if (aborted || vWritten == 0) {
        sw.Reset();
        DeleteFileW(tmpPath.c_str());
        outError = aborted ? "Düzenleme iptal edildi." : "Seçilen aralıkta video karesi bulunamadı.";
        return false;
    }

    if (hasOutMix)  FlushMixPcmChunks(sw.Get(), aOutMix, outMixAccum, mixWrittenPairs, decodedMusic, true);
    if (hasOutGame) FlushPcmChunks(sw.Get(), aOutGame, outGameAccum, gameWrittenPairs, true);
    if (hasOutMic)  FlushPcmChunks(sw.Get(), aOutMic, outMicAccum, micWrittenPairs, true);

    hr = sw->Finalize();
    sw.Reset();
    if (FAILED(hr)) {
        DeleteFileW(tmpPath.c_str());
        outError = "Düzenlenen MP4 dosyası sonlandırılamadı.";
        return false;
    }

    FixMp4AudioTrackFlags(tmpPath, 0);
    if (!MoveFileExW(tmpPath.c_str(), spec.dstPath.c_str(), MOVEFILE_REPLACE_EXISTING)) {
        DeleteFileW(tmpPath.c_str());
        outError = "Düzenlenen dosya hedef konuma kaydedilemedi.";
        return false;
    }
    return true;
}

bool TrimMp4Clip(const std::wstring& srcPath, const std::wstring& dstPath,
                 double startSec, double endSec, std::string& outError,
                 std::atomic<bool>* cancelFlag) {
    ClipEditSpec spec;
    spec.srcPath = srcPath;
    spec.dstPath = dstPath;
    spec.startSec = startSec;
    spec.endSec = endSec;
    return EditMp4Clip(spec, outError, cancelFlag);
}

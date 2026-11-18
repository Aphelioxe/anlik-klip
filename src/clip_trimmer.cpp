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
#include <cstdio>
#include <cstring>
#include <vector>

namespace {

bool EndsWithMp4(const std::wstring& s) {
    return s.size() > 4 && _wcsicmp(s.c_str() + s.size() - 4, L".mp4") == 0;
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

bool TrimMp4Clip(const std::wstring& srcPath, const std::wstring& dstPath,
                 double startSec, double endSec, std::string& outError,
                 std::atomic<bool>* cancelFlag) {
    if (startSec < 0.0) startSec = 0.0;
    if (endSec <= startSec + 0.2) {
        outError = "Kırpma aralığı en az 0.2 saniye olmalı.";
        return false;
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

    ComPtr<IMFAttributes> rAttr;
    MFCreateAttributes(rAttr.Put(), 4);
    if (dm && rAttr) {
        rAttr->SetUINT32(MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, TRUE);
        rAttr->SetUnknown(MF_SOURCE_READER_D3D_MANAGER, dm.Get());
    }

    ComPtr<IMFSourceReader> sr;
    HRESULT hr = MFCreateSourceReaderFromURL(srcPath.c_str(), rAttr.Get(), sr.Put());
    if (FAILED(hr) || !sr) {
        outError = "Kaynak klip okunamadı.";
        return false;
    }

    DWORD vInStream = static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM);
    DWORD aInStream = static_cast<DWORD>(MF_SOURCE_READER_FIRST_AUDIO_STREAM);
    bool foundV = false, foundA = false;
    for (DWORD si = 0; si < 8; ++si) {
        ComPtr<IMFMediaType> mt;
        if (FAILED(sr->GetNativeMediaType(si, 0, mt.Put())) || !mt) break;
        GUID maj{};
        mt->GetGUID(MF_MT_MAJOR_TYPE, &maj);
        if (maj == MFMediaType_Video && !foundV) { vInStream = si; foundV = true; }
        if (maj == MFMediaType_Audio && !foundA) { aInStream = si; foundA = true; }
    }
    if (!foundV) {
        outError = "Video akışı bulunamadı.";
        return false;
    }

    ComPtr<IMFMediaType> nativeV;
    sr->GetNativeMediaType(vInStream, 0, nativeV.Put());
    UINT32 srcW = 1280, srcH = 720, fpsN = 60, fpsD = 1, avgBr = 6000000;
    if (nativeV) {
        MFGetAttributeSize(nativeV.Get(), MF_MT_FRAME_SIZE, &srcW, &srcH);
        MFGetAttributeRatio(nativeV.Get(), MF_MT_FRAME_RATE, &fpsN, &fpsD);
        nativeV->GetUINT32(MF_MT_AVG_BITRATE, &avgBr);
    }
    if (fpsN == 0 || fpsD == 0) { fpsN = 60; fpsD = 1; }
    avgBr = std::max<uint32_t>(2500000u, std::min<uint32_t>(20000000u, avgBr));

    ComPtr<IMFMediaType> decV;
    MFCreateMediaType(decV.Put());
    decV->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    decV->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12);
    if (FAILED(sr->SetCurrentMediaType(vInStream, nullptr, decV.Get()))) {
        outError = "Video çözücü NV12 moduna alınamadı.";
        return false;
    }

    UINT32 yuvMatrix = MFVideoTransferMatrix_BT709;
    UINT32 primaries = MFVideoPrimaries_BT709;
    UINT32 transFunc = MFVideoTransFunc_709;
    UINT32 nomRange  = MFNominalRange_16_235;
    ComPtr<IMFMediaType> curDecV;
    if (SUCCEEDED(sr->GetCurrentMediaType(vInStream, curDecV.Put())) && curDecV) {
        UINT32 v = 0;
        if (SUCCEEDED(curDecV->GetUINT32(MF_MT_YUV_MATRIX, &v)) && v != 0) yuvMatrix = v;
        if (SUCCEEDED(curDecV->GetUINT32(MF_MT_VIDEO_PRIMARIES, &v)) && v != 0) primaries = v;
        if (SUCCEEDED(curDecV->GetUINT32(MF_MT_TRANSFER_FUNCTION, &v)) && v != 0) transFunc = v;
        if (SUCCEEDED(curDecV->GetUINT32(MF_MT_VIDEO_NOMINAL_RANGE, &v)) && v != 0) nomRange = v;
    }

    bool hasAudio = false;
    if (foundA) {
        ComPtr<IMFMediaType> decA;
        MFCreateMediaType(decA.Put());
        decA->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
        decA->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_PCM);
        decA->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
        decA->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, 48000);
        decA->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, 2);
        if (SUCCEEDED(sr->SetCurrentMediaType(aInStream, nullptr, decA.Get()))) {
            hasAudio = true;
        }
    }

    std::wstring tmpPath = dstPath + L".tmp.mp4";
    DeleteFileW(tmpPath.c_str());

    ComPtr<IMFAttributes> wAttr;
    MFCreateAttributes(wAttr.Put(), 4);
    if (dm && wAttr) {
        wAttr->SetUINT32(MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, TRUE);
        wAttr->SetUnknown(MF_SINK_WRITER_D3D_MANAGER, dm.Get());
    }
    if (wAttr) wAttr->SetUINT32(MF_SINK_WRITER_DISABLE_THROTTLING, TRUE);

    ComPtr<IMFSinkWriter> sw;
    hr = MFCreateSinkWriterFromURL(tmpPath.c_str(), nullptr, wAttr.Get(), sw.Put());
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
    outV->SetUINT32(MF_MT_AVG_BITRATE, avgBr);
    MFSetAttributeSize(outV.Get(), MF_MT_FRAME_SIZE, srcW, srcH);
    MFSetAttributeRatio(outV.Get(), MF_MT_FRAME_RATE, fpsN, fpsD);
    outV->SetUINT32(MF_MT_MPEG2_PROFILE, eAVEncH264VProfile_High);
    setColorAttrs(outV.Get());

    DWORD vOutIdx = 0;
    hr = sw->AddStream(outV.Get(), &vOutIdx);
    if (SUCCEEDED(hr)) {
        ComPtr<IMFMediaType> inV;
        MFCreateMediaType(inV.Put());
        inV->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        inV->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12);
        MFSetAttributeSize(inV.Get(), MF_MT_FRAME_SIZE, srcW, srcH);
        MFSetAttributeRatio(inV.Get(), MF_MT_FRAME_RATE, fpsN, fpsD);
        setColorAttrs(inV.Get());
        hr = sw->SetInputMediaType(vOutIdx, inV.Get(), nullptr);
    }
    if (FAILED(hr)) {
        DeleteFileW(tmpPath.c_str());
        outError = "Çıkış video formatı ayarlanamadı.";
        return false;
    }

    DWORD aOutIdx = 0;
    if (hasAudio) {
        ComPtr<IMFMediaType> outA;
        MFCreateMediaType(outA.Put());
        outA->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
        outA->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_AAC);
        outA->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
        outA->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, 48000);
        outA->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, 2);
        outA->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, 20000);
        if (SUCCEEDED(sw->AddStream(outA.Get(), &aOutIdx))) {
            ComPtr<IMFMediaType> inA;
            MFCreateMediaType(inA.Put());
            inA->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
            inA->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_PCM);
            inA->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
            inA->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, 48000);
            inA->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, 2);
            inA->SetUINT32(MF_MT_AUDIO_BLOCK_ALIGNMENT, 4);
            inA->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, 48000 * 4);
            if (FAILED(sw->SetInputMediaType(aOutIdx, inA.Get(), nullptr))) {
                hasAudio = false;
            }
        } else {
            hasAudio = false;
        }
    }

    {
        ComPtr<ICodecAPI> encCodec;
        if (SUCCEEDED(sw->GetServiceForStream(vOutIdx, GUID_NULL, IID_ICodecAPI, encCodec.PutVoid())) && encCodec) {
            VARIANT v;
            VariantInit(&v);
            v.vt = VT_UI4;
            v.ulVal = std::max<uint32_t>(1u, fpsN / fpsD);
            encCodec->SetValue(&CODECAPI_AVEncMPVGOPSize, &v);
        }
    }

    if (FAILED(sw->BeginWriting())) {
        DeleteFileW(tmpPath.c_str());
        outError = "MP4 yazımı başlatılamadı.";
        return false;
    }

    ComPtr<ID3D11Texture2D> stgTex;
    if (dev) {
        D3D11_TEXTURE2D_DESC sd{};
        sd.Width = srcW;
        sd.Height = srcH;
        sd.MipLevels = 1;
        sd.ArraySize = 1;
        sd.Format = DXGI_FORMAT_NV12;
        sd.SampleDesc.Count = 1;
        sd.Usage = D3D11_USAGE_STAGING;
        sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        dev->CreateTexture2D(&sd, nullptr, stgTex.Put());
    }

    const LONGLONG start100 = static_cast<LONGLONG>(startSec * 1e7);
    const LONGLONG end100   = static_cast<LONGLONG>(endSec * 1e7);
    const LONGLONG span100  = end100 - start100;
    const LONGLONG defaultVDur = (10000000LL * fpsD) / fpsN;

    // Seek ~2s before startSec so SourceReader decodes cleanly from an IDR frame
    {
        PROPVARIANT pv;
        PropVariantInit(&pv);
        pv.vt = VT_I8;
        pv.hVal.QuadPart = std::max(0LL, start100 - 20000000LL);
        sr->SetCurrentPosition(GUID_NULL, pv);
        PropVariantClear(&pv);
    }

    bool vDone = false;
    bool aDone = !hasAudio;
    int vWritten = 0;
    LONGLONG lastVTs = -1;
    std::vector<int16_t> pcmAccum;
    int64_t aWrittenPairs = 0;
    const int64_t aMaxPairs = (span100 * 48000LL) / 10000000LL;
    bool aborted = false;

    while ((!vDone || !aDone) && !aborted) {
        if (cancelFlag && cancelFlag->load()) {
            aborted = true;
            break;
        }
        DWORD targetStream = (!vDone && !aDone)
            ? static_cast<DWORD>(MF_SOURCE_READER_ANY_STREAM)
            : (!vDone ? vInStream : aInStream);
        DWORD streamIdx = 0, flags = 0;
        LONGLONG ts = 0;
        ComPtr<IMFSample> smp;
        hr = sr->ReadSample(targetStream, 0, &streamIdx, &flags, &ts, smp.Put());
        if (FAILED(hr)) break;

        if (flags & MF_SOURCE_READERF_ENDOFSTREAM) {
            if (streamIdx == vInStream) vDone = true;
            else aDone = true;
            continue;
        }
        if (!smp) continue;

        LONGLONG dur = 0;
        smp->GetSampleDuration(&dur);
        if (dur <= 0) dur = (streamIdx == vInStream) ? defaultVDur : ((1024LL * 10000000LL) / 48000LL);

        if (ts >= end100) {
            if (streamIdx == vInStream) vDone = true;
            else aDone = true;
            continue;
        }
        if (ts + dur <= start100) {
            continue;
        }

        if (streamIdx == vInStream && !vDone) {
            LONGLONG newTs = std::max(0LL, ts - start100);
            if (newTs <= lastVTs) newTs = lastVTs + 1;
            LONGLONG newDur = dur;
            if (ts < start100) newDur = (ts + dur) - start100;
            if (newTs + newDur > span100) newDur = span100 - newTs;
            if (newDur <= 0) continue;

            ComPtr<IMFSample> outSmp;
            MFCreateSample(outSmp.Put());
            if (!outSmp) continue;
            outSmp->SetSampleTime(newTs);
            outSmp->SetSampleDuration(newDur);
            if (!CopyVideoSampleBuffer(ctx.Get(), stgTex.Get(), smp.Get(), srcW, srcH, outSmp.Get())) {
                continue;
            }
            if (SUCCEEDED(sw->WriteSample(vOutIdx, outSmp.Get()))) {
                lastVTs = newTs;
                ++vWritten;
            }
        } else if (streamIdx == aInStream && !aDone) {
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
                    if (skipPairs < nPairs) {
                        const int64_t queuedPairs = static_cast<int64_t>(pcmAccum.size() / 2);
                        const int64_t remainAllowed = std::max<int64_t>(0, aMaxPairs - (aWrittenPairs + queuedPairs));
                        const size_t take = std::min<size_t>(nPairs - skipPairs, static_cast<size_t>(remainAllowed));
                        if (take > 0) {
                            const int16_t* s16 = reinterpret_cast<const int16_t*>(p) + skipPairs * 2;
                            pcmAccum.insert(pcmAccum.end(), s16, s16 + take * 2);
                            FlushPcmChunks(sw.Get(), aOutIdx, pcmAccum, aWrittenPairs, false);
                        }
                        if (aWrittenPairs + static_cast<int64_t>(pcmAccum.size() / 2) >= aMaxPairs) {
                            aDone = true;
                        }
                    }
                    abuf->Unlock();
                }
            }
        }
    }

    if (aborted || vWritten == 0) {
        sw.Reset();
        DeleteFileW(tmpPath.c_str());
        outError = aborted ? "Kırpma iptal edildi." : "Seçilen aralıkta video karesi bulunamadı.";
        return false;
    }

    if (hasAudio) {
        FlushPcmChunks(sw.Get(), aOutIdx, pcmAccum, aWrittenPairs, true);
    }

    hr = sw->Finalize();
    sw.Reset();
    if (FAILED(hr)) {
        DeleteFileW(tmpPath.c_str());
        outError = "Kırpılan MP4 dosyası sonlandırılamadı.";
        return false;
    }

    if (!MoveFileExW(tmpPath.c_str(), dstPath.c_str(), MOVEFILE_REPLACE_EXISTING)) {
        DeleteFileW(tmpPath.c_str());
        outError = "Kırpılan dosya hedef konuma kaydedilemedi.";
        return false;
    }
    return true;
}

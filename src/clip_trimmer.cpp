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

inline float SoftClipSample(float x) {
    if (x > 1.0f) return 1.0f - std::exp(-(x - 1.0f)) * 0.25f > 1.0f ? 1.0f : 1.0f;
    if (x < -1.0f) return -1.0f;
    return x;
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

    DWORD vIn = DWORD(-1);
    std::vector<DWORD> outMap(16, DWORD(-1));
    std::vector<bool> isStreamDone(16, true);
    DWORD streamCount = 0;

    for (DWORD si = 0; si < 16; ++si) {
        ComPtr<IMFMediaType> mt;
        if (FAILED(sr->GetNativeMediaType(si, 0, mt.Put())) || !mt) break;
        GUID maj{};
        mt->GetGUID(MF_MT_MAJOR_TYPE, &maj);
        if (maj == MFMediaType_Video || maj == MFMediaType_Audio) {
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
            if (maj == MFMediaType_Video && vIn == DWORD(-1)) vIn = si;
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
        if (SUCCEEDED(sw->WriteSample(outMap[si], smp.Get())) && si == vIn) {
            ++vWritten;
        }
    }

    if (!gotFirstKey || vWritten < 2 || FAILED(sw->Finalize())) {
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

struct SourceSegment {
    std::wstring path;
    double startSec = 0.0;
    double endSec = 0.0;
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

bool FixMp4AudioTrackFlags(const std::wstring& mp4Path) {
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
                    uint8_t wantFlag = (audioTrackCount == 0) ? 0x01 : 0x00;
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

bool EditMp4Clip(const ClipEditSpec& spec, std::string& outError,
                 std::atomic<bool>* cancelFlag) {
    const bool isMontage = !spec.montageSegments.empty();
    double startSec = std::max(0.0, spec.startSec);
    double endSec = spec.endSec;
    if (!isMontage && endSec <= startSec + 0.2) {
        outError = "Kırpma aralığı en az 0.2 saniye olmalı.";
        return false;
    }

    const double speed = std::max(0.5, std::min(2.0, spec.speed));
    const float gameGain = std::max(0.0f, std::min(2.0f, static_cast<float>(spec.gameVolPct) / 100.0f));
    const float micGain = std::max(0.0f, std::min(2.0f, static_cast<float>(spec.micVolPct) / 100.0f));

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
                segments.push_back({ ms.path, s0, s1 });
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
            segments.push_back({ spec.srcPath, startSec, spec.cutStartSec });
            if (hasInsert) segments.push_back({ spec.insertClipPath, 0.0, insertDur });
            segments.push_back({ spec.srcPath, spec.cutEndSec, endSec });
        } else {
            segments.push_back({ spec.srcPath, startSec, endSec });
            if (hasInsert) segments.push_back({ spec.insertClipPath, 0.0, insertDur });
        }
    }

    // Fast lossless passthrough trim (<0.3s, minimal RAM) when only start/end changed
    if (!isMontage && !hasCut && !hasInsert && !spec.fadeTransitions &&
        spec.targetSizeMb == 0 && !spec.vertical916 &&
        std::abs(speed - 1.0) < 0.01 &&
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
    for (const auto& sg : segments) {
        totalOutDurSec += std::max(0.0, sg.endSec - sg.startSec) / speed;
    }
    if (totalOutDurSec < 0.2) totalOutDurSec = 0.2;

    UINT32 outW = srcW, outH = srcH, outFpsN = fpsN, outFpsD = fpsD, outBr = avgBr;
    const bool isDiscordExport = (spec.targetSizeMb > 0);
    if (spec.vertical916) {
        outW = (srcH >= 1080) ? 1080u : 720u;
        outH = (srcH >= 1080) ? 1920u : 1280u;
    }
    if (speed < 0.99) {
        outFpsN = std::max<uint32_t>(15u, static_cast<uint32_t>(std::lround((static_cast<double>(fpsN) / fpsD) * speed)));
        outFpsD = 1;
    } else if (!isDiscordExport && speed > 1.01) {
        outBr = std::min<uint32_t>(24000000u, static_cast<uint32_t>(outBr * 1.25));
    }
    if (isDiscordExport) {
        const double targetBits = static_cast<double>(spec.targetSizeMb) * 8.0 * 1024.0 * 1024.0 * 0.88;
        const double audioBits = foundA ? (96000.0 * totalOutDurSec) : 0.0;
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
                    break;
                }
            }
        }
    }
    const bool needVp = (outW != srcW || outH != srcH || spec.vertical916 || hasInsert || segments.size() > 1 || spec.fadeTransitions);
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

    DWORD aOutMix = 0, aOutGame = 0, aOutMic = 0;
    bool hasOutMix = foundA && AddAacOutputStream(sw.Get(), aOutMix);
    bool hasOutGame = !isDiscordExport && hasOutMix && srcMultiTrack && AddAacOutputStream(sw.Get(), aOutGame);
    bool hasOutMic = !isDiscordExport && hasOutGame && AddAacOutputStream(sw.Get(), aOutMic);

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

    for (size_t segIdx = 0; segIdx < segments.size(); ++segIdx) {
        const auto& seg = segments[segIdx];
        if (cancelFlag && cancelFlag->load()) { aborted = true; break; }

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

        const bool segHasMulti = (aIns.size() >= 3);
        const bool segHasAudio = !aIns.empty() && cfgAudioIn(aIns[0]);
        const bool segHasGameMic = segHasAudio && segHasMulti && cfgAudioIn(aIns[1]) && cfgAudioIn(aIns[2]);

        const LONGLONG start100 = static_cast<LONGLONG>(seg.startSec * 1e7);
        const LONGLONG end100 = static_cast<LONGLONG>(seg.endSec * 1e7);
        const LONGLONG span100 = std::max<LONGLONG>(1, end100 - start100);
        const LONGLONG outSpan100 = static_cast<LONGLONG>(static_cast<double>(span100) / speed);

        PROPVARIANT pv;
        PropVariantInit(&pv);
        pv.vt = VT_I8;
        pv.hVal.QuadPart = std::max(0LL, start100 - 20000000LL);
        sr->SetCurrentPosition(GUID_NULL, pv);
        PropVariantClear(&pv);

        bool vDone = false;
        bool aDone[3] = { !segHasAudio, !segHasGameMic, !segHasGameMic };
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
                    for (size_t k = 0; k < aIns.size() && k < 3; ++k) {
                        if (aIns[k] == si) aDone[k] = true;
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
                LONGLONG mappedTs = outVBase100 + static_cast<LONGLONG>(static_cast<double>(relIn) / speed);
                LONGLONG gridTs = (outFrameIdx * 10000000LL * outFpsD) / outFpsN;
                if (mappedTs + (outFrameDur100 / 2) < gridTs) {
                    continue; // cleanly pace to output fps without cramming or stuttering
                }
                if (gridTs >= outVBase100 + outSpan100) {
                    continue;
                }
                LONGLONG nextGridTs = ((outFrameIdx + 1) * 10000000LL * outFpsD) / outFpsN;
                LONGLONG newDur = std::max<LONGLONG>(1, nextGridTs - gridTs);

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

                        float segAlpha = 1.0f;
                        if (spec.fadeTransitions && segments.size() > 1) {
                            const double segOutDurSec = static_cast<double>(outSpan100) / 1e7;
                            const double fadeSec = std::min(0.35, segOutDurSec * 0.25);
                            const double relOutSec = static_cast<double>(std::max<LONGLONG>(0, gridTs - outVBase100)) / 1e7;
                            if (segIdx > 0 && relOutSec < fadeSec && fadeSec > 0.01) {
                                segAlpha = std::min(segAlpha, std::max(0.05f, static_cast<float>(relOutSec / fadeSec)));
                            }
                            if (segIdx + 1 < segments.size() && relOutSec > (segOutDurSec - fadeSec) && fadeSec > 0.01) {
                                segAlpha = std::min(segAlpha, std::max(0.05f, static_cast<float>((segOutDurSec - relOutSec) / fadeSec)));
                            }
                            D3D11_VIDEO_COLOR bgCol{};
                            bgCol.RGBA = { 0.0f, 0.0f, 0.0f, 1.0f };
                            vctx->VideoProcessorSetOutputBackgroundColor(vp.Get(), FALSE, &bgCol);
                        }

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
                            vctx->VideoProcessorSetStreamAlpha(vp.Get(), 0, TRUE, segAlpha);
                            vctx->VideoProcessorSetStreamSourceRect(vp.Get(), 1, TRUE, &fgSrc);
                            vctx->VideoProcessorSetStreamDestRect(vp.Get(), 1, TRUE, &fgDst);
                            vctx->VideoProcessorSetStreamAlpha(vp.Get(), 1, TRUE, segAlpha);

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
                            vctx->VideoProcessorSetStreamAlpha(vp.Get(), 0, TRUE, segAlpha);
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
                        if (hasGpuPool && ctx) {
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
                }
            } else {
                int trackIdx = -1;
                for (size_t k = 0; k < aIns.size() && k < 3; ++k) {
                    if (aIns[k] == streamIdx && !aDone[k]) { trackIdx = static_cast<int>(k); break; }
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
                    const size_t needIn = static_cast<size_t>(std::ceil(static_cast<double>(kChunkOut) * speed)) + 4;
                    while (segOutFlushed + kChunkOut <= targetOutPairs) {
                        bool ready = segHasGameMic
                            ? (segPcm[1].size() / 2 >= needIn && segPcm[2].size() / 2 >= needIn)
                            : (segPcm[0].size() / 2 >= needIn);
                        if (!ready) break;
                        const size_t consumeIn = static_cast<size_t>(std::lround(static_cast<double>(kChunkOut) * speed));
                        if (segHasGameMic) {
                            std::vector<int16_t> gIn(segPcm[1].begin(), segPcm[1].begin() + consumeIn * 2);
                            std::vector<int16_t> mIn(segPcm[2].begin(), segPcm[2].begin() + consumeIn * 2);
                            segPcm[1].erase(segPcm[1].begin(), segPcm[1].begin() + consumeIn * 2);
                            segPcm[2].erase(segPcm[2].begin(), segPcm[2].begin() + consumeIn * 2);
                            std::vector<int16_t> gRes, mRes;
                            AppendResampledPcm(gIn, speed, gameGain, kChunkOut, gRes);
                            AppendResampledPcm(mIn, speed, micGain, kChunkOut, mRes);
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
                            AppendResampledPcm(sIn, speed, gameGain, kChunkOut, outMixAccum);
                            if (hasOutGame) AppendResampledPcm(sIn, speed, gameGain, kChunkOut, outGameAccum);
                            if (hasOutMic)  outMicAccum.insert(outMicAccum.end(), kChunkOut * 2, 0);
                        }
                        FlushPcmChunks(sw.Get(), aOutMix, outMixAccum, mixWrittenPairs, false);
                        if (hasOutGame) FlushPcmChunks(sw.Get(), aOutGame, outGameAccum, gameWrittenPairs, false);
                        if (hasOutMic)  FlushPcmChunks(sw.Get(), aOutMic, outMicAccum, micWrittenPairs, false);
                        segOutFlushed += kChunkOut;
                    }
                }
            }
        }

        if (aborted) break;

        const LONGLONG actualOutSpan100 = (segOutFrames > 0)
            ? std::min(outSpan100, segOutFrames * outFrameDur100)
            : outSpan100;
        const size_t actualTargetOutPairs = static_cast<size_t>((actualOutSpan100 * 48000LL) / 10000000LL);

        if (hasOutMix && segOutFlushed < actualTargetOutPairs) {
            const size_t remOutPairs = actualTargetOutPairs - segOutFlushed;
            if (segHasGameMic) {
                std::vector<int16_t> gRes, mRes;
                AppendResampledPcm(segPcm[1], speed, gameGain, remOutPairs, gRes);
                AppendResampledPcm(segPcm[2], speed, micGain, remOutPairs, mRes);
                outMixAccum.reserve(outMixAccum.size() + remOutPairs * 2);
                for (size_t i = 0; i < remOutPairs * 2; ++i) {
                    float mix = static_cast<float>(gRes[i]) / 32767.0f + static_cast<float>(mRes[i]) / 32767.0f;
                    outMixAccum.push_back(FloatToPcm16(mix));
                }
                if (hasOutGame) outGameAccum.insert(outGameAccum.end(), gRes.begin(), gRes.end());
                if (hasOutMic)  outMicAccum.insert(outMicAccum.end(), mRes.begin(), mRes.end());
            } else {
                AppendResampledPcm(segPcm[0], speed, gameGain, remOutPairs, outMixAccum);
                if (hasOutGame) AppendResampledPcm(segPcm[0], speed, gameGain, remOutPairs, outGameAccum);
                if (hasOutMic)  outMicAccum.insert(outMicAccum.end(), remOutPairs * 2, 0);
            }
            FlushPcmChunks(sw.Get(), aOutMix, outMixAccum, mixWrittenPairs, false);
            if (hasOutGame) FlushPcmChunks(sw.Get(), aOutGame, outGameAccum, gameWrittenPairs, false);
            if (hasOutMic)  FlushPcmChunks(sw.Get(), aOutMic, outMicAccum, micWrittenPairs, false);
        }

        outVBase100 += actualOutSpan100;
    }

    if (aborted || vWritten == 0) {
        sw.Reset();
        DeleteFileW(tmpPath.c_str());
        outError = aborted ? "Düzenleme iptal edildi." : "Seçilen aralıkta video karesi bulunamadı.";
        return false;
    }

    if (hasOutMix)  FlushPcmChunks(sw.Get(), aOutMix, outMixAccum, mixWrittenPairs, true);
    if (hasOutGame) FlushPcmChunks(sw.Get(), aOutGame, outGameAccum, gameWrittenPairs, true);
    if (hasOutMic)  FlushPcmChunks(sw.Get(), aOutMic, outMicAccum, micWrittenPairs, true);

    hr = sw->Finalize();
    sw.Reset();
    if (FAILED(hr)) {
        DeleteFileW(tmpPath.c_str());
        outError = "Düzenlenen MP4 dosyası sonlandırılamadı.";
        return false;
    }

    FixMp4AudioTrackFlags(tmpPath);
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

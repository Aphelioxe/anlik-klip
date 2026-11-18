#include "mp4_writer.h"
#include "com_ptr.h"
#include "config.h"

#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <mferror.h>
#include <codecapi.h>
#include <cstdio>
#include <cstring>

std::wstring BuildUniqueClipPath(const std::wstring& outputDir) {
    EnsureDirectoryExists(outputDir);
    SYSTEMTIME st{};
    GetLocalTime(&st);

    wchar_t base[128] = {};
    swprintf(base, 128, L"Klip_%04u-%02u-%02u_%02u-%02u-%02u",
        st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);

    std::wstring path = outputDir + L"\\" + base + L".mp4";
    if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES) {
        return path;
    }
    for (int i = 2; i < 1000; ++i) {
        wchar_t cand[160] = {};
        swprintf(cand, 160, L"%ls_%d.mp4", base, i);
        path = outputDir + L"\\" + cand;
        if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES) {
            return path;
        }
    }
    return outputDir + L"\\" + base + L"_x.mp4";
}

// Fallback: extract Annex-B SPS (NAL type 7) + PPS (NAL type 8) if MF_MT_MPEG_SEQUENCE_HEADER was absent.
static std::vector<uint8_t> ExtractSpsPpsFromKeyframe(const std::vector<uint8_t>& buf) {
    std::vector<uint8_t> out;
    const uint8_t* p = buf.data();
    size_t n = buf.size();
    std::vector<size_t> starts;
    for (size_t i = 0; i + 4 < n; ++i) {
        if (p[i] == 0 && p[i + 1] == 0) {
            if (p[i + 2] == 1) {
                starts.push_back(i);
                i += 2;
            } else if (i + 4 < n && p[i + 2] == 0 && p[i + 3] == 1) {
                starts.push_back(i);
                i += 3;
            }
        }
    }
    for (size_t k = 0; k < starts.size(); ++k) {
        size_t s = starts[k];
        size_t scLen = (p[s + 2] == 1) ? 3 : 4;
        size_t end = (k + 1 < starts.size()) ? starts[k + 1] : n;
        if (s + scLen < end) {
            uint8_t nalType = p[s + scLen] & 0x1F;
            if (nalType == 7 || nalType == 8) {
                out.insert(out.end(), p + s, p + end);
            }
        }
    }
    return out;
}

bool WriteClipToMp4(const ClipSnapshot& snap, const std::wstring& finalPath, std::string& errorOut) {
    if (snap.video.empty()) {
        errorOut = "Tamponda henüz video karesi yok.";
        return false;
    }

    std::wstring tmpPath = finalPath + L".tmp.mp4";
    DeleteFileW(tmpPath.c_str());

    ComPtr<IMFAttributes> swAttr;
    HRESULT hr = MFCreateAttributes(swAttr.Put(), 2);
    if (FAILED(hr)) {
        errorOut = "MFCreateAttributes basarisiz";
        return false;
    }
    swAttr->SetUINT32(MF_READWRITE_DISABLE_CONVERTERS, TRUE);
    swAttr->SetUINT32(MF_SINK_WRITER_DISABLE_THROTTLING, TRUE);

    ComPtr<IMFSinkWriter> sw;
    hr = MFCreateSinkWriterFromURL(tmpPath.c_str(), nullptr, swAttr.Get(), sw.Put());
    if (FAILED(hr)) {
        char msg[96];
        snprintf(msg, sizeof(msg), "MFCreateSinkWriterFromURL hr=0x%08lx", (unsigned long)hr);
        errorOut = msg;
        return false;
    }

    // Video media type (passthrough H.264)
    ComPtr<IMFMediaType> vMT;
    MFCreateMediaType(vMT.Put());
    vMT->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    vMT->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264);
    MFSetAttributeSize(vMT.Get(), MF_MT_FRAME_SIZE, snap.width, snap.height);
    MFSetAttributeRatio(vMT.Get(), MF_MT_FRAME_RATE, snap.fps, 1);
    vMT->SetUINT32(MF_MT_AVG_BITRATE, snap.videoBitrateBps);
    vMT->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    vMT->SetUINT32(MF_MT_MPEG2_PROFILE, eAVEncH264VProfile_High);
    vMT->SetUINT32(MF_MT_YUV_MATRIX, MFVideoTransferMatrix_BT709);
    vMT->SetUINT32(MF_MT_VIDEO_PRIMARIES, MFVideoPrimaries_BT709);
    vMT->SetUINT32(MF_MT_TRANSFER_FUNCTION, MFVideoTransFunc_709);
    vMT->SetUINT32(MF_MT_VIDEO_NOMINAL_RANGE, MFNominalRange_16_235);

    std::vector<uint8_t> seqHdr = snap.h264SeqHeader;
    if (seqHdr.empty()) {
        seqHdr = ExtractSpsPpsFromKeyframe(*snap.video[0].data);
    }
    if (!seqHdr.empty()) {
        vMT->SetBlob(MF_MT_MPEG_SEQUENCE_HEADER, seqHdr.data(), static_cast<UINT32>(seqHdr.size()));
    }

    DWORD vStream = 0;
    hr = sw->AddStream(vMT.Get(), &vStream);
    if (SUCCEEDED(hr)) hr = sw->SetInputMediaType(vStream, vMT.Get(), nullptr);
    if (FAILED(hr)) {
        char msg[96];
        snprintf(msg, sizeof(msg), "Video stream setup hr=0x%08lx", (unsigned long)hr);
        errorOut = msg;
        DeleteFileW(tmpPath.c_str());
        return false;
    }

    bool hasAudio = (!snap.audio.empty() && !snap.aacUserData.empty());
    DWORD aStream = 0;
    if (hasAudio) {
        ComPtr<IMFMediaType> aMT;
        MFCreateMediaType(aMT.Put());
        aMT->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
        aMT->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_AAC);
        aMT->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
        aMT->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, 48000);
        aMT->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, 2);
        aMT->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, 20000);
        aMT->SetUINT32(MF_MT_AAC_PAYLOAD_TYPE, 0);
        aMT->SetUINT32(MF_MT_AAC_AUDIO_PROFILE_LEVEL_INDICATION, 0x29);
        aMT->SetBlob(MF_MT_USER_DATA, snap.aacUserData.data(), static_cast<UINT32>(snap.aacUserData.size()));

        hr = sw->AddStream(aMT.Get(), &aStream);
        if (SUCCEEDED(hr)) hr = sw->SetInputMediaType(aStream, aMT.Get(), nullptr);
        if (FAILED(hr)) {
            Logger::Log("Mp4Writer: audio stream setup failed hr=0x%08lx, continuing video-only", (unsigned long)hr);
            hasAudio = false;
        }
    }

    hr = sw->BeginWriting();
    if (FAILED(hr)) {
        char msg[96];
        snprintf(msg, sizeof(msg), "BeginWriting hr=0x%08lx", (unsigned long)hr);
        errorOut = msg;
        DeleteFileW(tmpPath.c_str());
        return false;
    }

    const int64_t basePts = snap.basePts100ns;
    const int64_t defaultVDur = 10000000LL / (snap.fps > 0 ? snap.fps : 60);
    const int64_t defaultADur = 1024LL * 10000000LL / 48000;

    size_t vi = 0, ai = 0;
    const size_t aCount = hasAudio ? snap.audio.size() : 0;

    while (vi < snap.video.size() || ai < aCount) {
        bool writeV = (vi < snap.video.size()) &&
                      (ai >= aCount || snap.video[vi].pts100ns <= snap.audio[ai].pts100ns);
        const EncodedPacket& pkt = writeV ? snap.video[vi++] : snap.audio[ai++];
        if (!pkt.data || pkt.data->empty()) continue;

        ComPtr<IMFMediaBuffer> mb;
        hr = MFCreateMemoryBuffer(static_cast<DWORD>(pkt.data->size()), mb.Put());
        if (FAILED(hr)) continue;

        BYTE* dst = nullptr;
        if (SUCCEEDED(mb->Lock(&dst, nullptr, nullptr))) {
            memcpy(dst, pkt.data->data(), pkt.data->size());
            mb->Unlock();
            mb->SetCurrentLength(static_cast<DWORD>(pkt.data->size()));
        }

        ComPtr<IMFSample> sample;
        MFCreateSample(sample.Put());
        sample->AddBuffer(mb.Get());
        int64_t relPts = pkt.pts100ns - basePts;
        if (relPts < 0) relPts = 0;
        sample->SetSampleTime(relPts);
        sample->SetSampleDuration(pkt.dur100ns > 0 ? pkt.dur100ns : (writeV ? defaultVDur : defaultADur));
        if (writeV && pkt.keyframe) {
            sample->SetUINT32(MFSampleExtension_CleanPoint, TRUE);
        }

        hr = sw->WriteSample(writeV ? vStream : aStream, sample.Get());
        if (FAILED(hr)) {
            char msg[96];
            snprintf(msg, sizeof(msg), "WriteSample(%s) hr=0x%08lx", writeV ? "V" : "A", (unsigned long)hr);
            errorOut = msg;
            sw.Reset();
            DeleteFileW(tmpPath.c_str());
            return false;
        }
    }

    hr = sw->Finalize();
    sw.Reset();
    if (FAILED(hr)) {
        char msg[96];
        snprintf(msg, sizeof(msg), "Finalize hr=0x%08lx", (unsigned long)hr);
        errorOut = msg;
        DeleteFileW(tmpPath.c_str());
        return false;
    }

    if (!MoveFileExW(tmpPath.c_str(), finalPath.c_str(), MOVEFILE_REPLACE_EXISTING)) {
        char msg[96];
        snprintf(msg, sizeof(msg), "MoveFileExW err=%lu", GetLastError());
        errorOut = msg;
        DeleteFileW(tmpPath.c_str());
        return false;
    }

    return true;
}

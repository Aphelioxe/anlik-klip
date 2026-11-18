#include "audio_engine.h"
#include "com_ptr.h"

#include <mmdeviceapi.h>
#include <audioclient.h>
#include <functiondiscoverykeys_devpkey.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mftransform.h>
#include <mferror.h>
#include <propvarutil.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <deque>

namespace {

struct PcmFormatInfo {
    uint32_t sampleRate = 48000;
    uint16_t channels = 2;
    uint16_t bitsPerSample = 32;
    uint16_t blockAlign = 8;
    bool isFloat = true;
    bool supported = false;
};

PcmFormatInfo ParseWaveFormat(const WAVEFORMATEX* wf) {
    PcmFormatInfo info;
    if (!wf) return info;
    info.sampleRate = wf->nSamplesPerSec;
    info.channels = wf->nChannels;
    info.bitsPerSample = wf->wBitsPerSample;
    info.blockAlign = wf->nBlockAlign;

    if (wf->wFormatTag == WAVE_FORMAT_IEEE_FLOAT) {
        info.isFloat = true;
        info.supported = (wf->wBitsPerSample == 32);
    } else if (wf->wFormatTag == WAVE_FORMAT_PCM) {
        info.isFloat = false;
        info.supported = (wf->wBitsPerSample == 16 || wf->wBitsPerSample == 24 || wf->wBitsPerSample == 32);
    } else if (wf->wFormatTag == WAVE_FORMAT_EXTENSIBLE && wf->cbSize >= 22) {
        const auto* wfe = reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(wf);
        if (wfe->SubFormat.Data1 == 0x00000003u) { // KSDATAFORMAT_SUBTYPE_IEEE_FLOAT
            info.isFloat = true;
            info.supported = (wf->wBitsPerSample == 32);
        } else if (wfe->SubFormat.Data1 == 0x00000001u) { // KSDATAFORMAT_SUBTYPE_PCM
            info.isFloat = false;
            info.supported = (wf->wBitsPerSample == 16 || wf->wBitsPerSample == 24 || wf->wBitsPerSample == 32);
        }
    }
    if (info.channels == 0 || info.sampleRate == 0) info.supported = false;
    return info;
}

inline float ReadSampleAsFloat(const uint8_t* ptr, const PcmFormatInfo& fmt) {
    if (fmt.isFloat) {
        float v = 0.0f;
        memcpy(&v, ptr, sizeof(float));
        return v;
    }
    if (fmt.bitsPerSample == 16) {
        int16_t v = 0;
        memcpy(&v, ptr, sizeof(int16_t));
        return static_cast<float>(v) / 32768.0f;
    }
    if (fmt.bitsPerSample == 24) {
        int32_t v = (static_cast<int32_t>(ptr[0])) |
                    (static_cast<int32_t>(ptr[1]) << 8) |
                    (static_cast<int32_t>(static_cast<int8_t>(ptr[2])) << 16);
        return static_cast<float>(v) / 8388608.0f;
    }
    if (fmt.bitsPerSample == 32) {
        int32_t v = 0;
        memcpy(&v, ptr, sizeof(int32_t));
        return static_cast<float>(v) / 2147483648.0f;
    }
    return 0.0f;
}

void DownmixFrameToStereo(const uint8_t* framePtr, const PcmFormatInfo& fmt, float& outL, float& outR) {
    const uint32_t bps = fmt.bitsPerSample / 8;
    const uint16_t ch = fmt.channels;
    if (ch == 1) {
        float m = ReadSampleAsFloat(framePtr, fmt);
        outL = m;
        outR = m;
        return;
    }
    if (ch == 2) {
        outL = ReadSampleAsFloat(framePtr, fmt);
        outR = ReadSampleAsFloat(framePtr + bps, fmt);
        return;
    }
    if (ch == 6) {
        // 5.1: FL, FR, FC, LFE, BL, BR
        float fl = ReadSampleAsFloat(framePtr + 0 * bps, fmt);
        float fr = ReadSampleAsFloat(framePtr + 1 * bps, fmt);
        float fc = ReadSampleAsFloat(framePtr + 2 * bps, fmt);
        float bl = ReadSampleAsFloat(framePtr + 4 * bps, fmt);
        float br = ReadSampleAsFloat(framePtr + 5 * bps, fmt);
        outL = fl + 0.7071f * fc + 0.7071f * bl;
        outR = fr + 0.7071f * fc + 0.7071f * br;
        return;
    }
    if (ch == 8) {
        // 7.1: FL, FR, FC, LFE, BL, BR, SL, SR
        float fl = ReadSampleAsFloat(framePtr + 0 * bps, fmt);
        float fr = ReadSampleAsFloat(framePtr + 1 * bps, fmt);
        float fc = ReadSampleAsFloat(framePtr + 2 * bps, fmt);
        float bl = ReadSampleAsFloat(framePtr + 4 * bps, fmt);
        float br = ReadSampleAsFloat(framePtr + 5 * bps, fmt);
        float sl = ReadSampleAsFloat(framePtr + 6 * bps, fmt);
        float sr = ReadSampleAsFloat(framePtr + 7 * bps, fmt);
        outL = fl + 0.7071f * fc + 0.7071f * bl + 0.7071f * sl;
        outR = fr + 0.7071f * fc + 0.7071f * br + 0.7071f * sr;
        return;
    }
    // Generic multi-channel fallback
    float l = 0.0f, r = 0.0f;
    for (uint16_t c = 0; c < ch; ++c) {
        float s = ReadSampleAsFloat(framePtr + c * bps, fmt);
        if ((c & 1) == 0) l += s; else r += s;
    }
    float norm = 2.0f / static_cast<float>(ch);
    outL = l * norm;
    outR = r * norm;
}

struct StereoSample {
    float l;
    float r;
};

struct WasapiStream {
    ComPtr<IMMDevice> device;
    ComPtr<IAudioClient> client;
    ComPtr<IAudioCaptureClient> capture;
    ComPtr<IAudioClient> silentRenderClient;
    ComPtr<IAudioRenderClient> silentRender;
    uint32_t silentRenderBufFrames = 0;
    PcmFormatInfo fmt;
    std::wstring deviceId;
    bool active = false;
    bool isLoopback = false;

    // Resampler state for non-48kHz sources
    double resamplePos = 0.0;
    StereoSample prevSample{0.0f, 0.0f};
    std::vector<StereoSample> scratch;

    void Close() {
        if (silentRenderClient) silentRenderClient->Stop();
        silentRender.Reset();
        silentRenderClient.Reset();
        if (client) client->Stop();
        capture.Reset();
        client.Reset();
        device.Reset();
        deviceId.clear();
        active = false;
        resamplePos = 0.0;
        prevSample = {0.0f, 0.0f};
    }

    bool Open(IMMDeviceEnumerator* en, bool loopback, const std::wstring& targetId) {
        Close();
        isLoopback = loopback;
        if (!en) return false;

        HRESULT hr = E_FAIL;
        if (!loopback && !targetId.empty()) {
            hr = en->GetDevice(targetId.c_str(), device.Put());
        }
        if (FAILED(hr)) {
            hr = en->GetDefaultAudioEndpoint(loopback ? eRender : eCapture, eConsole, device.Put());
        }
        if (FAILED(hr) || !device) return false;

        LPWSTR rawId = nullptr;
        if (SUCCEEDED(device->GetId(&rawId)) && rawId) {
            deviceId = rawId;
            CoTaskMemFree(rawId);
        }

        hr = device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, client.PutVoid());
        if (FAILED(hr)) { Close(); return false; }

        WAVEFORMATEX* wf = nullptr;
        hr = client->GetMixFormat(&wf);
        if (FAILED(hr) || !wf) { Close(); return false; }

        fmt = ParseWaveFormat(wf);
        if (!fmt.supported) {
            CoTaskMemFree(wf);
            Close();
            return false;
        }

        DWORD flags = loopback ? AUDCLNT_STREAMFLAGS_LOOPBACK : 0;
        hr = client->Initialize(AUDCLNT_SHAREMODE_SHARED, flags, 1000000 /* 100ms */, 0, wf, nullptr);
        if (FAILED(hr)) {
            CoTaskMemFree(wf);
            Close();
            return false;
        }

        hr = client->GetService(__uuidof(IAudioCaptureClient), capture.PutVoid());
        if (FAILED(hr) || !capture) {
            CoTaskMemFree(wf);
            Close();
            return false;
        }

        // Best-effort silent render client on loopback endpoint so Realtek/Windows audio engine
        // keeps delivering continuous loopback frames even when no app is playing audio.
        if (loopback) {
            ComPtr<IAudioClient> srClient;
            if (SUCCEEDED(device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, srClient.PutVoid()))) {
                if (SUCCEEDED(srClient->Initialize(AUDCLNT_SHAREMODE_SHARED, 0, 1000000, 0, wf, nullptr))) {
                    UINT32 bufSize = 0;
                    srClient->GetBufferSize(&bufSize);
                    ComPtr<IAudioRenderClient> sr;
                    if (SUCCEEDED(srClient->GetService(__uuidof(IAudioRenderClient), sr.PutVoid())) && bufSize > 0) {
                        BYTE* pData = nullptr;
                        if (SUCCEEDED(sr->GetBuffer(bufSize, &pData))) {
                            sr->ReleaseBuffer(bufSize, AUDCLNT_BUFFERFLAGS_SILENT);
                        }
                        if (SUCCEEDED(srClient->Start())) {
                            silentRenderClient = srClient;
                            silentRender = sr;
                            silentRenderBufFrames = bufSize;
                        }
                    }
                }
            }
        }

        CoTaskMemFree(wf);

        hr = client->Start();
        if (FAILED(hr)) { Close(); return false; }

        active = true;
        return true;
    }

    void KeepSilentRenderAlive() {
        if (!silentRenderClient || !silentRender || silentRenderBufFrames == 0) return;
        UINT32 padding = 0;
        if (FAILED(silentRenderClient->GetCurrentPadding(&padding))) return;
        if (padding < silentRenderBufFrames) {
            UINT32 avail = silentRenderBufFrames - padding;
            BYTE* pData = nullptr;
            if (SUCCEEDED(silentRender->GetBuffer(avail, &pData))) {
                silentRender->ReleaseBuffer(avail, AUDCLNT_BUFFERFLAGS_SILENT);
            }
        }
    }

    // Returns false if device invalidated/error and needs reopen
    bool DrainTo48k(std::deque<StereoSample>& outQueue, bool keepSamples) {
        if (!active || !capture) return false;
        KeepSilentRenderAlive();

        UINT32 pktSize = 0;
        HRESULT hr = capture->GetNextPacketSize(&pktSize);
        if (FAILED(hr)) return false;

        while (pktSize > 0) {
            BYTE* data = nullptr;
            UINT32 frames = 0;
            DWORD flags = 0;
            hr = capture->GetBuffer(&data, &frames, &flags, nullptr, nullptr);
            if (FAILED(hr)) return false;

            if (keepSamples && frames > 0) {
                bool silent = (flags & AUDCLNT_BUFFERFLAGS_SILENT) != 0;
                if (fmt.sampleRate == 48000) {
                    for (UINT32 i = 0; i < frames; ++i) {
                        if (silent) {
                            outQueue.push_back({0.0f, 0.0f});
                        } else {
                            float l = 0.0f, r = 0.0f;
                            DownmixFrameToStereo(data + i * fmt.blockAlign, fmt, l, r);
                            outQueue.push_back({l, r});
                        }
                    }
                } else {
                    scratch.resize(frames);
                    for (UINT32 i = 0; i < frames; ++i) {
                        if (silent) {
                            scratch[i] = {0.0f, 0.0f};
                        } else {
                            DownmixFrameToStereo(data + i * fmt.blockAlign, fmt, scratch[i].l, scratch[i].r);
                        }
                    }
                    double step = static_cast<double>(fmt.sampleRate) / 48000.0;
                    double pos = resamplePos;
                    while (pos < static_cast<double>(frames)) {
                        int idx0 = static_cast<int>(std::floor(pos));
                        double frac = pos - idx0;
                        StereoSample s0 = (idx0 < 0) ? prevSample : scratch[static_cast<size_t>(idx0)];
                        StereoSample s1 = (idx0 + 1 < static_cast<int>(frames)) ? scratch[static_cast<size_t>(idx0 + 1)] : scratch[frames - 1];
                        outQueue.push_back({
                            static_cast<float>(s0.l + (s1.l - s0.l) * frac),
                            static_cast<float>(s0.r + (s1.r - s0.r) * frac)
                        });
                        pos += step;
                    }
                    resamplePos = pos - static_cast<double>(frames);
                    prevSample = scratch[frames - 1];
                }
            }

            capture->ReleaseBuffer(frames);
            hr = capture->GetNextPacketSize(&pktSize);
            if (FAILED(hr)) return false;
        }
        return true;
    }
};

std::wstring GetCurrentDefaultEndpointId(IMMDeviceEnumerator* en, EDataFlow flow) {
    ComPtr<IMMDevice> dev;
    if (FAILED(en->GetDefaultAudioEndpoint(flow, eConsole, dev.Put())) || !dev) return {};
    LPWSTR raw = nullptr;
    std::wstring res;
    if (SUCCEEDED(dev->GetId(&raw)) && raw) {
        res = raw;
        CoTaskMemFree(raw);
    }
    return res;
}

inline float SoftClip(float x) {
    if (x > 1.0f || x < -1.0f) {
        return std::tanh(x);
    }
    return x;
}

/**
 * Envelope noise gate with transient key-click rejection and smooth gain ramp.
 * Keeps a small high-pass sidechain for desk thumps/fan hum without altering voice EQ.
 */
struct MicNoiseGate {
    float hpPrevIn = 0.0f;
    float hpState = 0.0f;
    float envSq = 0.0f;
    float fastSq = 0.0f;
    float peakHoldSq = 0.0f;
    float gain = 0.0f;
    int aboveCount = 0;
    int holdLeft = 0;
    int meterSamples = 0;
    float meterDb = -60.0f;
    bool open = false;

    void Reset() {
        envSq = 0.0f;
        fastSq = 0.0f;
        peakHoldSq = 0.0f;
        gain = 0.0f;
        aboveCount = 0;
        holdLeft = 0;
        meterSamples = 0;
        meterDb = -60.0f;
        open = false;
    }

    float Process(float mono, bool enabled, float openSq, float closeSq, bool& meterUpdated) {
        meterUpdated = false;
        const float hpCoeff = 0.9896f; // ~80 Hz at 48 kHz
        hpState = hpCoeff * (hpState + mono - hpPrevIn);
        hpPrevIn = mono;

        const float sq = hpState * hpState;
        envSq += ((sq > envSq) ? 0.00148f : 0.00059f) * (sq - envSq);
        if (envSq > peakHoldSq) peakHoldSq = envSq;
        if (++meterSamples >= 1440) { // ~30 ms meter window
            const float rms = std::sqrt(peakHoldSq + 1e-12f);
            const float db = 20.0f * std::log10(rms + 1e-6f);
            meterDb = std::max(-60.0f, std::min(0.0f, db));
            peakHoldSq = 0.0f;
            meterSamples = 0;
            meterUpdated = true;
        }

        if (!enabled) {
            gain = 1.0f;
            open = true;
            return 1.0f;
        }

        fastSq += ((sq > fastSq) ? 0.01f : 0.04f) * (sq - fastSq);
        if (fastSq >= openSq) {
            // Require ~10 ms sustained energy so <8 ms mechanical key clicks do not trigger the gate
            if (aboveCount < 480) ++aboveCount;
            if (aboveCount >= 480) {
                open = true;
                holdLeft = 10560; // 220 ms hold
            }
        } else {
            aboveCount = std::max(0, aboveCount - 6);
            if (open) {
                if (envSq >= closeSq) {
                    holdLeft = 10560;
                } else if (holdLeft > 0) {
                    --holdLeft;
                } else {
                    open = false;
                }
            }
        }

        const float target = open ? 1.0f : 0.0f;
        gain += ((target > gain) ? 0.0045f : 0.00022f) * (target - gain); // ~5 ms attack / ~95 ms release
        return gain;
    }
};

} // namespace

std::vector<AudioDeviceInfo> EnumerateMicDevices() {
    std::vector<AudioDeviceInfo> list;
    ComPtr<IMMDeviceEnumerator> en;
    HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
        __uuidof(IMMDeviceEnumerator), en.PutVoid());
    if (FAILED(hr) || !en) return list;

    ComPtr<IMMDeviceCollection> col;
    hr = en->EnumAudioEndpoints(eCapture, DEVICE_STATE_ACTIVE, col.Put());
    if (FAILED(hr) || !col) return list;

    UINT count = 0;
    col->GetCount(&count);
    for (UINT i = 0; i < count; ++i) {
        ComPtr<IMMDevice> dev;
        if (FAILED(col->Item(i, dev.Put())) || !dev) continue;
        LPWSTR rawId = nullptr;
        std::wstring id;
        if (SUCCEEDED(dev->GetId(&rawId)) && rawId) {
            id = rawId;
            CoTaskMemFree(rawId);
        }
        std::wstring name = L"Mikrofon";
        ComPtr<IPropertyStore> ps;
        if (SUCCEEDED(dev->OpenPropertyStore(STGM_READ, ps.Put())) && ps) {
            PROPVARIANT pv;
            PropVariantInit(&pv);
            if (SUCCEEDED(ps->GetValue(PKEY_Device_FriendlyName, &pv)) && pv.vt == VT_LPWSTR && pv.pwszVal) {
                name = pv.pwszVal;
            }
            PropVariantClear(&pv);
        }
        list.push_back({std::move(id), std::move(name)});
    }
    return list;
}

AudioEngine::AudioEngine(RingBuffer& ring) : ring_(ring) {}

AudioEngine::~AudioEngine() {
    Stop();
}

bool AudioEngine::Start(int64_t masterStartQpc,
    bool captureGame, int gameVolPct,
    bool captureMic, int micVolPct, bool micMuted,
    const std::wstring& micDeviceId,
    bool micNoiseGate, int micGateThresholdDb) {
    Stop();
    {
        std::lock_guard<std::mutex> lk(errMutex_);
        lastError_.clear();
    }
    micLevelDb_.store(-60.0f, std::memory_order_relaxed);
    UpdateMixSettings(captureGame, gameVolPct, captureMic, micVolPct, micMuted,
        micNoiseGate, micGateThresholdDb);
    stop_.store(false);
    thread_ = std::thread(&AudioEngine::ThreadMain, this, masterStartQpc, micDeviceId);
    return true;
}

void AudioEngine::Stop() {
    stop_.store(true);
    if (thread_.joinable()) {
        thread_.join();
    }
    micLevelDb_.store(-60.0f, std::memory_order_relaxed);
}

void AudioEngine::UpdateMixSettings(bool captureGame, int gameVolPct,
    bool captureMic, int micVolPct, bool micMuted,
    bool micNoiseGate, int micGateThresholdDb) {
    captureGame_.store(captureGame);
    gameVolPct_.store(std::max(0, std::min(200, gameVolPct)));
    captureMic_.store(captureMic);
    micVolPct_.store(std::max(0, std::min(200, micVolPct)));
    micMuted_.store(micMuted);
    micNoiseGate_.store(micNoiseGate);
    micGateThreshDb_.store(std::max(-60, std::min(-15, micGateThresholdDb)));
}

std::wstring AudioEngine::GetLastError() const {
    std::lock_guard<std::mutex> lk(errMutex_);
    return lastError_;
}

void AudioEngine::ThreadMain(int64_t masterStartQpc, std::wstring micDeviceId) {
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);
    ComScope comScope(COINIT_MULTITHREADED);

    // Setup MF AAC Encoder
    ComPtr<IMFTransform> aenc;
    MFT_REGISTER_TYPE_INFO outInfo = { MFMediaType_Audio, MFAudioFormat_AAC };
    IMFActivate** acts = nullptr;
    UINT32 actCount = 0;
    HRESULT hr = MFTEnumEx(MFT_CATEGORY_AUDIO_ENCODER,
        MFT_ENUM_FLAG_SYNCMFT | MFT_ENUM_FLAG_LOCALMFT | MFT_ENUM_FLAG_SORTANDFILTER,
        nullptr, &outInfo, &acts, &actCount);

    if (SUCCEEDED(hr) && actCount > 0) {
        hr = acts[0]->ActivateObject(__uuidof(IMFTransform), aenc.PutVoid());
        for (UINT32 i = 0; i < actCount; ++i) acts[i]->Release();
        CoTaskMemFree(acts);
    } else {
        if (acts) CoTaskMemFree(acts);
        std::lock_guard<std::mutex> lk(errMutex_);
        lastError_ = L"AAC kodlayıcı bulunamadı.";
        Logger::Log("AudioEngine: MFTEnumEx AAC failed hr=0x%08lx count=%u", (unsigned long)hr, actCount);
        return;
    }

    if (FAILED(hr) || !aenc) {
        std::lock_guard<std::mutex> lk(errMutex_);
        lastError_ = L"AAC kodlayıcı başlatılamadı.";
        return;
    }

    ComPtr<IMFMediaType> outMT;
    MFCreateMediaType(outMT.Put());
    outMT->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
    outMT->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_AAC);
    outMT->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
    outMT->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, 48000);
    outMT->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, 2);
    outMT->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, 20000); // 160 kbps
    outMT->SetUINT32(MF_MT_AAC_PAYLOAD_TYPE, 0);
    outMT->SetUINT32(MF_MT_AAC_AUDIO_PROFILE_LEVEL_INDICATION, 0x29);
    hr = aenc->SetOutputType(0, outMT.Get(), 0);

    ComPtr<IMFMediaType> inMT;
    MFCreateMediaType(inMT.Put());
    inMT->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
    inMT->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_PCM);
    inMT->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
    inMT->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, 48000);
    inMT->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, 2);
    inMT->SetUINT32(MF_MT_AUDIO_BLOCK_ALIGNMENT, 4);
    inMT->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, 48000 * 4);
    if (SUCCEEDED(hr)) hr = aenc->SetInputType(0, inMT.Get(), 0);

    if (FAILED(hr)) {
        std::lock_guard<std::mutex> lk(errMutex_);
        lastError_ = L"AAC format ayarı başarısız.";
        Logger::Log("AudioEngine: AAC SetInput/OutputType failed hr=0x%08lx", (unsigned long)hr);
        return;
    }

    ComPtr<IMFMediaType> curOut;
    if (SUCCEEDED(aenc->GetOutputCurrentType(0, curOut.Put())) && curOut) {
        UINT32 ulen = 0;
        if (SUCCEEDED(curOut->GetBlobSize(MF_MT_USER_DATA, &ulen)) && ulen > 0) {
            std::vector<uint8_t> udata(ulen);
            if (SUCCEEDED(curOut->GetBlob(MF_MT_USER_DATA, udata.data(), ulen, nullptr))) {
                ring_.UpdateAacUserData(std::move(udata));
            }
        }
    }

    aenc->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
    aenc->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);

    ComPtr<IMMDeviceEnumerator> en;
    CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
        __uuidof(IMMDeviceEnumerator), en.PutVoid());

    WasapiStream gameStream;
    WasapiStream micStream;

    if (en) {
        if (captureGame_.load()) {
            if (!gameStream.Open(en.Get(), true, L"")) {
                Logger::Log("AudioEngine: initial loopback open failed");
            } else {
                Logger::Log("AudioEngine: loopback opened rate=%u ch=%u float=%d",
                    gameStream.fmt.sampleRate, gameStream.fmt.channels, gameStream.fmt.isFloat ? 1 : 0);
            }
        }
        if (captureMic_.load()) {
            if (!micStream.Open(en.Get(), false, micDeviceId)) {
                Logger::Log("AudioEngine: initial mic open failed");
            } else {
                Logger::Log("AudioEngine: mic opened rate=%u ch=%u float=%d",
                    micStream.fmt.sampleRate, micStream.fmt.channels, micStream.fmt.isFloat ? 1 : 0);
            }
        }
    }

    std::deque<StereoSample> gameQ;
    std::deque<StereoSample> micQ;
    std::vector<int16_t> pcmAccum;
    pcmAccum.reserve(4096);
    MicNoiseGate noiseGate;

    int64_t totalEmitted = 0;
    int64_t aacSamplesFed = 0;
    int64_t lastReopenTryQpc = GetQpcNow();
    int64_t lastDefCheckQpc = lastReopenTryQpc;

    HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    if (!timer) {
        timer = CreateWaitableTimerW(nullptr, FALSE, nullptr);
    }

    MFT_OUTPUT_STREAM_INFO osi{};
    aenc->GetOutputStreamInfo(0, &osi);
    const DWORD outBufSize = osi.cbSize > 0 ? osi.cbSize : 8192;

    while (!stop_.load()) {
        if (timer) {
            LARGE_INTEGER due{};
            due.QuadPart = -100000LL; // 10ms relative
            SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE);
            WaitForSingleObject(timer, 20);
        } else {
            Sleep(10);
        }
        if (stop_.load()) break;

        const int64_t nowQpc = GetQpcNow();
        const bool wantGame = captureGame_.load();
        const bool wantMic = captureMic_.load();

        // Periodically check for endpoint switch / reconnect every ~1.5s
        if (en && QpcTo100Ns(nowQpc - lastDefCheckQpc) >= 15000000LL) {
            lastDefCheckQpc = nowQpc;
            if (wantGame && gameStream.active) {
                std::wstring defId = GetCurrentDefaultEndpointId(en.Get(), eRender);
                if (!defId.empty() && defId != gameStream.deviceId) {
                    Logger::Log("AudioEngine: default render device changed, reopening loopback");
                    gameStream.Open(en.Get(), true, L"");
                    gameQ.clear();
                }
            }
            if (wantMic && micDeviceId.empty() && micStream.active) {
                std::wstring defMicId = GetCurrentDefaultEndpointId(en.Get(), eCapture);
                if (!defMicId.empty() && defMicId != micStream.deviceId) {
                    Logger::Log("AudioEngine: default capture device changed, reopening mic");
                    micStream.Open(en.Get(), false, L"");
                    micQ.clear();
                }
            }
        }

        if (en && QpcTo100Ns(nowQpc - lastReopenTryQpc) >= 10000000LL) {
            lastReopenTryQpc = nowQpc;
            if (wantGame && !gameStream.active) {
                gameStream.Open(en.Get(), true, L"");
            }
            if (wantMic && !micStream.active) {
                micStream.Open(en.Get(), false, micDeviceId);
            }
        }

        if (!wantGame && gameStream.active) {
            gameStream.Close();
            gameQ.clear();
        }
        if (!wantMic && micStream.active) {
            micStream.Close();
            micQ.clear();
        }

        if (gameStream.active && !gameStream.DrainTo48k(gameQ, wantGame)) {
            Logger::Log("AudioEngine: loopback stream lost, resetting");
            gameStream.Close();
        }
        if (micStream.active && !micStream.DrainTo48k(micQ, wantMic && !micMuted_.load())) {
            Logger::Log("AudioEngine: mic stream lost, resetting");
            micStream.Close();
        }

        // Prevent runaway latency (>150ms)
        while (gameQ.size() > 7200) gameQ.pop_front();
        while (micQ.size() > 7200) micQ.pop_front();

        int64_t elapsed100Ns = QpcTo100Ns(nowQpc - masterStartQpc);
        if (elapsed100Ns < 0) elapsed100Ns = 0;
        int64_t targetSamples = (elapsed100Ns * 48000LL) / 10000000LL;
        int64_t needed = targetSamples - totalEmitted;
        if (needed <= 0) continue;
        if (needed > 4800) {
            // Catch up smoothly without large stalls if system paused
            needed = 4800;
            totalEmitted = targetSamples - needed;
        }

        const float gGain = wantGame ? (static_cast<float>(gameVolPct_.load()) / 100.0f) : 0.0f;
        const bool micActive = wantMic && !micMuted_.load();
        const float mGain = micActive ? (static_cast<float>(micVolPct_.load()) / 100.0f) : 0.0f;
        const bool useGate = micNoiseGate_.load();
        const int gateDb = micGateThreshDb_.load();
        const float openLin = std::pow(10.0f, static_cast<float>(gateDb) / 20.0f);
        const float closeLin = std::pow(10.0f, static_cast<float>(gateDb - 5) / 20.0f);
        const float openSq = openLin * openLin;
        const float closeSq = closeLin * closeLin;

        if (!micActive) {
            noiseGate.Reset();
            micLevelDb_.store(-60.0f, std::memory_order_relaxed);
        }

        for (int64_t i = 0; i < needed; ++i) {
            StereoSample gs{0.0f, 0.0f};
            if (!gameQ.empty()) {
                gs = gameQ.front();
                gameQ.pop_front();
            }
            StereoSample ms{0.0f, 0.0f};
            if (!micQ.empty()) {
                ms = micQ.front();
                micQ.pop_front();
            }
            float micMul = mGain;
            if (micActive) {
                bool meterUpd = false;
                float gGate = noiseGate.Process(0.5f * (ms.l + ms.r), useGate, openSq, closeSq, meterUpd);
                if (meterUpd) {
                    micLevelDb_.store(noiseGate.meterDb, std::memory_order_relaxed);
                }
                micMul *= gGate;
            }
            float l = SoftClip(gs.l * gGain + ms.l * micMul);
            float r = SoftClip(gs.r * gGain + ms.r * micMul);
            int32_t il = static_cast<int32_t>(std::lrintf(l * 32767.0f));
            int32_t ir = static_cast<int32_t>(std::lrintf(r * 32767.0f));
            pcmAccum.push_back(static_cast<int16_t>(std::max(-32768, std::min(32767, il))));
            pcmAccum.push_back(static_cast<int16_t>(std::max(-32768, std::min(32767, ir))));
        }
        totalEmitted += needed;

        // Encode 1024-sample AAC frames
        const size_t chunkFloats = 1024 * 2;
        size_t offset = 0;
        while (pcmAccum.size() - offset >= chunkFloats) {
            ComPtr<IMFMediaBuffer> inBuf;
            if (FAILED(MFCreateMemoryBuffer(1024 * 4, inBuf.Put()))) break;
            BYTE* pDst = nullptr;
            if (SUCCEEDED(inBuf->Lock(&pDst, nullptr, nullptr))) {
                memcpy(pDst, pcmAccum.data() + offset, 1024 * 4);
                inBuf->Unlock();
                inBuf->SetCurrentLength(1024 * 4);
            }

            ComPtr<IMFSample> inSamp;
            MFCreateSample(inSamp.Put());
            inSamp->AddBuffer(inBuf.Get());
            int64_t pts = (aacSamplesFed * 10000000LL) / 48000LL;
            int64_t dur = (1024LL * 10000000LL) / 48000LL;
            inSamp->SetSampleTime(pts);
            inSamp->SetSampleDuration(dur);

            if (SUCCEEDED(aenc->ProcessInput(0, inSamp.Get(), 0))) {
                aacSamplesFed += 1024;
            }
            offset += chunkFloats;

            while (true) {
                ComPtr<IMFSample> outSamp;
                MFCreateSample(outSamp.Put());
                ComPtr<IMFMediaBuffer> outBuf;
                MFCreateMemoryBuffer(outBufSize, outBuf.Put());
                outSamp->AddBuffer(outBuf.Get());

                MFT_OUTPUT_DATA_BUFFER odb{};
                odb.pSample = outSamp.Get();
                DWORD status = 0;
                HRESULT ohr = aenc->ProcessOutput(0, 1, &odb, &status);
                if (odb.pEvents) odb.pEvents->Release();
                if (ohr == MF_E_TRANSFORM_NEED_MORE_INPUT || FAILED(ohr)) break;

                ComPtr<IMFMediaBuffer> contig;
                if (SUCCEEDED(outSamp->ConvertToContiguousBuffer(contig.Put()))) {
                    BYTE* pOut = nullptr;
                    DWORD len = 0;
                    if (SUCCEEDED(contig->Lock(&pOut, nullptr, &len)) && len > 0) {
                        EncodedPacket pkt;
                        pkt.data = std::make_shared<const std::vector<uint8_t>>(pOut, pOut + len);
                        LONGLONG sPts = 0, sDur = 0;
                        outSamp->GetSampleTime(&sPts);
                        outSamp->GetSampleDuration(&sDur);
                        pkt.pts100ns = sPts;
                        pkt.dur100ns = sDur;
                        pkt.keyframe = true;
                        ring_.PushAudio(std::move(pkt));
                        contig->Unlock();
                    }
                }
            }
        }
        if (offset > 0) {
            pcmAccum.erase(pcmAccum.begin(), pcmAccum.begin() + static_cast<ptrdiff_t>(offset));
        }
    }

    if (timer) CloseHandle(timer);
    gameStream.Close();
    micStream.Close();
    aenc->ProcessMessage(MFT_MESSAGE_NOTIFY_END_OF_STREAM, 0);
    aenc->ProcessMessage(MFT_MESSAGE_COMMAND_DRAIN, 0);
}

#include "sound_fx.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <mmsystem.h>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

namespace {

std::vector<uint8_t> BuildWavPcm16(const std::vector<int16_t>& samples, uint32_t sampleRate) {
    uint32_t dataBytes = static_cast<uint32_t>(samples.size() * sizeof(int16_t));
    uint32_t fileSize = 36 + dataBytes;
    std::vector<uint8_t> wav(44 + dataBytes);

    auto writeU32 = [&](size_t off, uint32_t v) { memcpy(wav.data() + off, &v, 4); };
    auto writeU16 = [&](size_t off, uint16_t v) { memcpy(wav.data() + off, &v, 2); };

    memcpy(wav.data() + 0, "RIFF", 4);
    writeU32(4, fileSize);
    memcpy(wav.data() + 8, "WAVE", 4);
    memcpy(wav.data() + 12, "fmt ", 4);
    writeU32(16, 16);            // PCM fmt chunk size
    writeU16(20, 1);             // PCM format
    writeU16(22, 1);             // mono
    writeU32(24, sampleRate);
    writeU32(28, sampleRate * 2);
    writeU16(32, 2);             // block align
    writeU16(34, 16);            // bits per sample
    memcpy(wav.data() + 36, "data", 4);
    writeU32(40, dataBytes);
    if (dataBytes > 0) {
        memcpy(wav.data() + 44, samples.data(), dataBytes);
    }
    return wav;
}

const std::vector<uint8_t>& GetChimeWav() {
    static const std::vector<uint8_t> wav = []() {
        constexpr uint32_t sr = 44100;
        constexpr double durSec = 0.34;
        const size_t n = static_cast<size_t>(sr * durSec);
        std::vector<int16_t> pcm(n, 0);
        constexpr double kPi = 3.14159265358979323846;

        auto addNote = [&](double startSec, double freqHz, double amp, double decayRate) {
            size_t startIdx = static_cast<size_t>(startSec * sr);
            for (size_t i = startIdx; i < n; ++i) {
                double t = static_cast<double>(i - startIdx) / sr;
                double attack = (t < 0.004) ? (t / 0.004) : 1.0;
                double env = attack * std::exp(-decayRate * t);
                double s = std::sin(2.0 * kPi * freqHz * t) * 0.80 +
                           std::sin(2.0 * kPi * (freqHz * 2.0) * t) * 0.16 +
                           std::sin(2.0 * kPi * (freqHz * 3.0) * t) * 0.04;
                double cur = static_cast<double>(pcm[i]) + s * env * amp * 32767.0;
                if (cur > 32767.0) cur = 32767.0;
                if (cur < -32768.0) cur = -32768.0;
                pcm[i] = static_cast<int16_t>(cur);
            }
        };

        // Crisp pleasant two-note chime: C6 -> G6 ("çıng!")
        addNote(0.00, 1046.50, 0.34, 14.0);
        addNote(0.075, 1567.98, 0.42, 10.5);
        return BuildWavPcm16(pcm, sr);
    }();
    return wav;
}

const std::vector<uint8_t>& GetErrorWav() {
    static const std::vector<uint8_t> wav = []() {
        constexpr uint32_t sr = 44100;
        constexpr double durSec = 0.24;
        const size_t n = static_cast<size_t>(sr * durSec);
        std::vector<int16_t> pcm(n, 0);
        constexpr double kPi = 3.14159265358979323846;

        auto addTone = [&](double startSec, double endSec, double freqHz) {
            size_t s0 = static_cast<size_t>(startSec * sr);
            size_t s1 = std::min(n, static_cast<size_t>(endSec * sr));
            for (size_t i = s0; i < s1; ++i) {
                double t = static_cast<double>(i - s0) / sr;
                double env = std::exp(-12.0 * t);
                double v = std::sin(2.0 * kPi * freqHz * t) * env * 0.30 * 32767.0;
                pcm[i] = static_cast<int16_t>(v);
            }
        };
        addTone(0.00, 0.10, 330.0);
        addTone(0.11, 0.23, 247.0);
        return BuildWavPcm16(pcm, sr);
    }();
    return wav;
}

} // namespace

void PlayClipChime() {
    const auto& wav = GetChimeWav();
    PlaySoundW(reinterpret_cast<LPCWSTR>(wav.data()), nullptr, SND_MEMORY | SND_ASYNC | SND_NODEFAULT);
}

void PlayErrorTone() {
    const auto& wav = GetErrorWav();
    PlaySoundW(reinterpret_cast<LPCWSTR>(wav.data()), nullptr, SND_MEMORY | SND_ASYNC | SND_NODEFAULT);
}

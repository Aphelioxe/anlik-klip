#pragma once

#include "ring_buffer.h"
#include <atomic>
#include <string>
#include <thread>
#include <vector>

struct AudioDeviceInfo {
    std::wstring id;
    std::wstring name;
};

std::vector<AudioDeviceInfo> EnumerateMicDevices();

class AudioEngine {
public:
    explicit AudioEngine(RingBuffer& ring);
    ~AudioEngine();

    bool Start(int64_t masterStartQpc,
        bool captureGame, int gameVolPct,
        bool captureMic, int micVolPct, bool micMuted,
        const std::wstring& micDeviceId,
        bool micNoiseGate = true, int micGateThresholdDb = -36);
    void Stop();

    void UpdateMixSettings(bool captureGame, int gameVolPct,
        bool captureMic, int micVolPct, bool micMuted,
        bool micNoiseGate = true, int micGateThresholdDb = -36);

    float GetMicLevelDb() const { return micLevelDb_.load(std::memory_order_relaxed); }
    std::wstring GetLastError() const;

private:
    void ThreadMain(int64_t masterStartQpc, std::wstring micDeviceId);

    RingBuffer& ring_;
    std::thread thread_;
    std::atomic<bool> stop_{false};

    std::atomic<bool> captureGame_{true};
    std::atomic<int> gameVolPct_{100};
    std::atomic<bool> captureMic_{true};
    std::atomic<int> micVolPct_{100};
    std::atomic<bool> micMuted_{false};
    std::atomic<bool> micNoiseGate_{true};
    std::atomic<int> micGateThreshDb_{-36};
    std::atomic<float> micLevelDb_{-60.0f};

    mutable std::mutex errMutex_;
    std::wstring lastError_;
};

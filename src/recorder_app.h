#pragma once

#include "audio_engine.h"
#include "config.h"
#include "mobile_sync.h"
#include "ring_buffer.h"
#include "video_engine.h"

#include <atomic>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

struct AppRuntimeStats {
    bool running = false;
    bool saving = false;
    double workingSetMb = 0.0;
    double privateMb = 0.0;
    double micLevelDb = -60.0;
    RingStats ring;
    VideoStats video;
    std::wstring audioError;
};

struct ClipSaveResult {
    bool ok = false;
    std::wstring filePath;
    std::wstring message;
    double durationSeconds = 0.0;
};

class RecorderApp {
public:
    using SaveCallback = std::function<void(const ClipSaveResult&)>;

    explicit RecorderApp(std::wstring iniPath);
    ~RecorderApp();

    const AppConfig& Config() const { return cfg_; }
    bool IsRunning() const { return running_.load(); }

    void StartBuffering();
    void StopBuffering();
    void ToggleBuffering();

    bool ToggleMicMute();
    void ApplyConfig(const AppConfig& newCfg);

    bool SaveClipAsync(int secondsOverride, SaveCallback cb);
    AppRuntimeStats GetStats() const;

    bool IsMobileSyncRunning() const { return mobile_.IsRunning(); }
    std::wstring GetMobileLanIp() const { return mobile_.LanIp(); }
    std::wstring GetMobilePairUrl() const { return mobile_.PairUrl(); }
    std::wstring GetMobileError() const { return mobile_.LastError(); }
    void RegenerateMobilePin();

private:
    void SyncMobileServer();

    std::wstring iniPath_;
    AppConfig cfg_;
    RingBuffer ring_;
    AudioEngine audio_;
    VideoEngine video_;
    MobileSyncServer mobile_;

    std::atomic<bool> running_{false};
    std::atomic<bool> saving_{false};
    std::mutex saveThreadMutex_;
    std::thread saveThread_;
};

#pragma once

#include "ring_buffer.h"
#include <atomic>
#include <mutex>
#include <string>
#include <thread>

struct VideoStats {
    bool capturingDesktop = false;
    bool hardwareEncoder = false;
    uint32_t srcWidth = 0;
    uint32_t srcHeight = 0;
    double actualFps = 0.0;
    std::wstring encoderName;
    std::wstring statusText;
};

class VideoEngine {
public:
    explicit VideoEngine(RingBuffer& ring);
    ~VideoEngine();

    bool Start(int64_t masterStartQpc, uint32_t width, uint32_t height, uint32_t fps, uint32_t bitrateKbps);
    void Stop();

    VideoStats GetStats() const;

private:
    void ThreadMain(int64_t masterStartQpc, uint32_t dstW, uint32_t dstH, uint32_t fps, uint32_t bitrateKbps);
    void SetStatus(bool capturing, bool hw, uint32_t sw, uint32_t sh, double fps,
        const std::wstring& encName, const std::wstring& text);

    RingBuffer& ring_;
    std::thread thread_;
    std::atomic<bool> stop_{false};

    mutable std::mutex statsMutex_;
    VideoStats stats_;
};

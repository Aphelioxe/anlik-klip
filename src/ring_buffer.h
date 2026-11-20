#pragma once

#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <vector>

struct EncodedPacket {
    std::shared_ptr<const std::vector<uint8_t>> data;
    int64_t pts100ns = 0;
    int64_t dur100ns = 0;
    bool keyframe = false;
};

struct RingStats {
    double bufferedSeconds = 0.0;
    size_t videoBytes = 0;
    size_t audioBytes = 0;
    size_t videoPackets = 0;
    size_t audioPackets = 0;
};

struct ClipSnapshot {
    std::vector<uint8_t> h264SeqHeader;
    std::vector<uint8_t> aacUserData;
    uint32_t width = 1280;
    uint32_t height = 720;
    uint32_t fps = 60;
    uint32_t videoBitrateBps = 6000000;
    std::vector<EncodedPacket> video;
    std::vector<EncodedPacket> audio;      // Track 1: Mix (Game + Mic)
    std::vector<EncodedPacket> audioGame;  // Track 2: Isolated Game/PC audio
    std::vector<EncodedPacket> audioMic;   // Track 3: Isolated Microphone audio
    int64_t basePts100ns = 0;
    double durationSeconds = 0.0;
};

class RingBuffer {
public:
    explicit RingBuffer(int targetSeconds = 60);

    void SetTargetSeconds(int seconds);
    int TargetSeconds() const;

    void SetStreamFormats(
        std::vector<uint8_t> h264SeqHeader,
        std::vector<uint8_t> aacUserData,
        uint32_t width, uint32_t height, uint32_t fps, uint32_t videoBitrateBps);

    void UpdateH264SeqHeader(std::vector<uint8_t> h264SeqHeader);
    void UpdateAacUserData(std::vector<uint8_t> aacUserData);

    void PushVideo(EncodedPacket pkt);
    void PushAudio(EncodedPacket pkt);
    void PushAudioGame(EncodedPacket pkt);
    void PushAudioMic(EncodedPacket pkt);
    void Clear();

    RingStats GetStats() const;
    bool TakeSnapshot(int maxSeconds, ClipSnapshot& out) const;

private:
    void PushAudioQueue(std::deque<EncodedPacket>& q, EncodedPacket pkt);
    void TrimAudioQueueLocked(std::deque<EncodedPacket>& q, int64_t cutoffPts);
    void TrimLocked();

    mutable std::mutex mutex_;
    int targetSeconds_;
    std::deque<EncodedPacket> video_;
    std::deque<EncodedPacket> audio_;
    std::deque<EncodedPacket> audioGame_;
    std::deque<EncodedPacket> audioMic_;
    size_t videoBytes_ = 0;
    size_t audioBytes_ = 0;

    std::vector<uint8_t> h264SeqHeader_;
    std::vector<uint8_t> aacUserData_;
    uint32_t width_ = 1280;
    uint32_t height_ = 720;
    uint32_t fps_ = 60;
    uint32_t videoBitrateBps_ = 6000000;
};

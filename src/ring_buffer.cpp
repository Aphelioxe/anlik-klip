#include "ring_buffer.h"
#include <algorithm>

RingBuffer::RingBuffer(int targetSeconds)
    : targetSeconds_(std::max(10, targetSeconds)) {}

void RingBuffer::SetTargetSeconds(int seconds) {
    std::lock_guard<std::mutex> lk(mutex_);
    targetSeconds_ = std::max(10, seconds);
    TrimLocked();
}

int RingBuffer::TargetSeconds() const {
    std::lock_guard<std::mutex> lk(mutex_);
    return targetSeconds_;
}

void RingBuffer::SetStreamFormats(
    std::vector<uint8_t> h264SeqHeader,
    std::vector<uint8_t> aacUserData,
    uint32_t width, uint32_t height, uint32_t fps, uint32_t videoBitrateBps) {
    std::lock_guard<std::mutex> lk(mutex_);
    if (!h264SeqHeader.empty()) h264SeqHeader_ = std::move(h264SeqHeader);
    if (!aacUserData.empty()) aacUserData_ = std::move(aacUserData);
    width_ = width;
    height_ = height;
    fps_ = fps;
    videoBitrateBps_ = videoBitrateBps;
}

void RingBuffer::UpdateH264SeqHeader(std::vector<uint8_t> h264SeqHeader) {
    std::lock_guard<std::mutex> lk(mutex_);
    if (!h264SeqHeader.empty()) h264SeqHeader_ = std::move(h264SeqHeader);
}

void RingBuffer::UpdateAacUserData(std::vector<uint8_t> aacUserData) {
    std::lock_guard<std::mutex> lk(mutex_);
    if (!aacUserData.empty()) aacUserData_ = std::move(aacUserData);
}

void RingBuffer::PushVideo(EncodedPacket pkt) {
    if (!pkt.data || pkt.data->empty()) return;
    std::lock_guard<std::mutex> lk(mutex_);
    // Guard against non-monotonic PTS after device reset
    if (!video_.empty() && pkt.pts100ns < video_.back().pts100ns) {
        pkt.pts100ns = video_.back().pts100ns + 1;
    }
    videoBytes_ += pkt.data->size();
    video_.push_back(std::move(pkt));
    TrimLocked();
}

void RingBuffer::PushAudio(EncodedPacket pkt) {
    if (!pkt.data || pkt.data->empty()) return;
    std::lock_guard<std::mutex> lk(mutex_);
    if (!audio_.empty() && pkt.pts100ns < audio_.back().pts100ns) {
        pkt.pts100ns = audio_.back().pts100ns + 1;
    }
    audioBytes_ += pkt.data->size();
    audio_.push_back(std::move(pkt));
    TrimLocked();
}

void RingBuffer::Clear() {
    std::lock_guard<std::mutex> lk(mutex_);
    video_.clear();
    audio_.clear();
    videoBytes_ = 0;
    audioBytes_ = 0;
}

void RingBuffer::TrimLocked() {
    const int64_t targetNs = static_cast<int64_t>(targetSeconds_) * 10000000LL;
    const size_t maxSafetyBytes = static_cast<size_t>(std::max(128, targetSeconds_ * 4)) * 1024ULL * 1024ULL;

    // Trim video at GOP (keyframe) boundaries while keeping >= targetNs
    while (video_.size() > 1) {
        int64_t newestPts = video_.back().pts100ns;
        // Find next keyframe after index 0
        size_t nextKf = 1;
        while (nextKf < video_.size() && !video_[nextKf].keyframe) {
            ++nextKf;
        }
        if (nextKf < video_.size()) {
            int64_t spanFromNextKf = newestPts - video_[nextKf].pts100ns;
            if (spanFromNextKf >= targetNs + 4000000LL || videoBytes_ > maxSafetyBytes) {
                for (size_t i = 0; i < nextKf; ++i) {
                    videoBytes_ -= video_.front().data->size();
                    video_.pop_front();
                }
                continue;
            }
        } else if (videoBytes_ > maxSafetyBytes) {
            // Emergency cap if no keyframes exist
            videoBytes_ -= video_.front().data->size();
            video_.pop_front();
            continue;
        }
        break;
    }

    // Trim audio aligned to oldest video packet (with 1s margin)
    if (!video_.empty()) {
        int64_t cutoffPts = video_.front().pts100ns - 10000000LL;
        while (!audio_.empty() && audio_.front().pts100ns < cutoffPts) {
            audioBytes_ -= audio_.front().data->size();
            audio_.pop_front();
        }
    } else if (!audio_.empty()) {
        int64_t cutoffPts = audio_.back().pts100ns - (targetNs + 50000000LL);
        while (!audio_.empty() && audio_.front().pts100ns < cutoffPts) {
            audioBytes_ -= audio_.front().data->size();
            audio_.pop_front();
        }
    }
}

RingStats RingBuffer::GetStats() const {
    std::lock_guard<std::mutex> lk(mutex_);
    RingStats st;
    st.videoBytes = videoBytes_;
    st.audioBytes = audioBytes_;
    st.videoPackets = video_.size();
    st.audioPackets = audio_.size();
    if (video_.size() >= 2) {
        int64_t span = video_.back().pts100ns - video_.front().pts100ns;
        st.bufferedSeconds = span > 0 ? static_cast<double>(span) / 1e7 : 0.0;
    }
    return st;
}

bool RingBuffer::TakeSnapshot(int maxSeconds, ClipSnapshot& out) const {
    std::lock_guard<std::mutex> lk(mutex_);
    if (video_.empty()) return false;

    int sec = (maxSeconds > 0) ? maxSeconds : targetSeconds_;
    int64_t targetNs = static_cast<int64_t>(sec) * 10000000LL;
    int64_t latestPts = video_.back().pts100ns;
    int64_t desiredStart = latestPts - targetNs;

    // Never span a sleep/resync timeline discontinuity (where audio stts=1 cannot represent large holes)
    int64_t minSegmentPts = 0;
    for (size_t i = video_.size(); i > 1; --i) {
        if ((video_[i - 1].pts100ns - video_[i - 2].pts100ns) > 50000000LL) { // > 5s video jump
            minSegmentPts = std::max(minSegmentPts, video_[i - 1].pts100ns);
            break;
        }
    }
    for (size_t i = audio_.size(); i > 1; --i) {
        if ((audio_[i - 1].pts100ns - audio_[i - 2].pts100ns) > 5000000LL) { // > 0.5s audio discontinuity
            minSegmentPts = std::max(minSegmentPts, audio_[i - 1].pts100ns);
            break;
        }
    }
    if (desiredStart < minSegmentPts) {
        desiredStart = minSegmentPts;
    }

    // Pick the keyframe closest to (and <=) desiredStart, or earliest available keyframe after discontinuity
    size_t startIdx = video_.size();
    for (size_t i = 0; i < video_.size(); ++i) {
        if (video_[i].pts100ns < minSegmentPts) continue;
        if (!video_[i].keyframe) continue;
        if (startIdx == video_.size()) {
            startIdx = i; // first keyframe fallback in continuous segment
        }
        if (video_[i].pts100ns <= desiredStart) {
            startIdx = i;
        } else {
            break;
        }
    }
    if (startIdx == video_.size()) {
        return false; // no keyframe in buffer yet
    }

    int64_t basePts = video_[startIdx].pts100ns;
    out.h264SeqHeader = h264SeqHeader_;
    out.aacUserData = aacUserData_;
    out.width = width_;
    out.height = height_;
    out.fps = fps_;
    out.videoBitrateBps = videoBitrateBps_;
    out.basePts100ns = basePts;
    out.durationSeconds = static_cast<double>(latestPts - basePts) / 1e7;

    out.video.clear();
    out.video.reserve(video_.size() - startIdx);
    for (size_t i = startIdx; i < video_.size(); ++i) {
        out.video.push_back(video_[i]);
    }

    out.audio.clear();
    for (const auto& ap : audio_) {
        if (ap.pts100ns >= basePts) {
            out.audio.push_back(ap);
        }
    }
    return !out.video.empty();
}

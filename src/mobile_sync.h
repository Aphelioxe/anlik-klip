#pragma once

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

class MobileSyncServer {
public:
    MobileSyncServer() = default;
    ~MobileSyncServer();

    bool Start(const std::wstring& clipsDir, uint16_t httpPort, const std::wstring& pin);
    void Stop();

    void UpdateConfig(const std::wstring& clipsDir, uint16_t httpPort, const std::wstring& pin);
    bool IsRunning() const { return running_.load(); }
    uint16_t Port() const { return port_.load(); }
    std::wstring LanIp() const;
    std::wstring PairUrl() const;
    std::wstring LastError() const;

    static std::wstring GenerateRandomPin();
    static std::wstring DetectPrimaryLanIpv4();

private:
    void HttpAcceptLoop();
    void UdpDiscoveryLoop();
    void HandleHttpClient(uintptr_t sockVal);

    bool ExtractJpegThumb(const std::wstring& fullPath, std::vector<uint8_t>& outJpeg);

    std::atomic<bool> stop_{true};
    std::atomic<bool> running_{false};
    std::atomic<uint16_t> port_{47820};

    mutable std::mutex cfgMutex_;
    std::wstring clipsDir_;
    std::wstring pin_;
    std::wstring lanIp_;
    std::wstring lastError_;

    uintptr_t listenSock_ = ~uintptr_t(0);
    uintptr_t udpSock_ = ~uintptr_t(0);
    std::thread httpThread_;
    std::thread udpThread_;

    std::mutex thumbMutex_;
    std::unordered_map<std::wstring, std::vector<uint8_t>> thumbCache_;
};

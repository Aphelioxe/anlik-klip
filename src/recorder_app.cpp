#include "recorder_app.h"
#include "com_ptr.h"
#include "mp4_writer.h"

#include <objbase.h>
#include <psapi.h>
#include <cstdio>

RecorderApp::RecorderApp(std::wstring iniPath)
    : iniPath_(std::move(iniPath)),
      cfg_(LoadConfig(iniPath_)),
      ring_(cfg_.clipSeconds),
      audio_(ring_),
      video_(ring_) {
    if (cfg_.mobileSyncEnabled) {
        SyncMobileServer();
    }
}

RecorderApp::~RecorderApp() {
    mobile_.Stop();
    StopBuffering();
    std::lock_guard<std::mutex> lk(saveThreadMutex_);
    if (saveThread_.joinable()) {
        saveThread_.join();
    }
}

void RecorderApp::StartBuffering() {
    if (running_.load()) return;
    ring_.Clear();
    ring_.SetTargetSeconds(cfg_.clipSeconds);

    const int64_t masterStartQpc = GetQpcNow();
    Logger::Log("RecorderApp: starting (%dx%d@%d %dkbps, %ds buffer)",
        cfg_.width, cfg_.height, cfg_.fps, cfg_.bitrateKbps, cfg_.clipSeconds);

    audio_.Start(masterStartQpc,
        cfg_.captureGameAudio, cfg_.gameVolumePct,
        cfg_.captureMic, cfg_.micVolumePct, cfg_.micMuted,
        cfg_.micDeviceId,
        cfg_.micNoiseGate, cfg_.micGateThresholdDb);
    video_.Start(masterStartQpc,
        static_cast<uint32_t>(cfg_.width),
        static_cast<uint32_t>(cfg_.height),
        static_cast<uint32_t>(cfg_.fps),
        static_cast<uint32_t>(cfg_.bitrateKbps));
    running_.store(true);
}

void RecorderApp::StopBuffering() {
    if (!running_.exchange(false)) return;
    Logger::Log("RecorderApp: stopping buffering");
    video_.Stop();
    audio_.Stop();
    ring_.Clear();
}

void RecorderApp::ToggleBuffering() {
    if (running_.load()) StopBuffering();
    else StartBuffering();
}

bool RecorderApp::ToggleMicMute() {
    cfg_.micMuted = !cfg_.micMuted;
    audio_.UpdateMixSettings(cfg_.captureGameAudio, cfg_.gameVolumePct,
        cfg_.captureMic, cfg_.micVolumePct, cfg_.micMuted,
        cfg_.micNoiseGate, cfg_.micGateThresholdDb);
    SaveConfig(iniPath_, cfg_);
    return cfg_.micMuted;
}

void RecorderApp::ApplyConfig(const AppConfig& newCfg) {
    bool needRestart = (cfg_.width != newCfg.width) ||
                       (cfg_.height != newCfg.height) ||
                       (cfg_.fps != newCfg.fps) ||
                       (cfg_.bitrateKbps != newCfg.bitrateKbps) ||
                       (cfg_.micDeviceId != newCfg.micDeviceId);

    bool wasRunning = running_.load();
    cfg_ = newCfg;
    EnsureDirectoryExists(cfg_.outputDir);
    SaveConfig(iniPath_, cfg_);

    ring_.SetTargetSeconds(cfg_.clipSeconds);
    audio_.UpdateMixSettings(cfg_.captureGameAudio, cfg_.gameVolumePct,
        cfg_.captureMic, cfg_.micVolumePct, cfg_.micMuted,
        cfg_.micNoiseGate, cfg_.micGateThresholdDb);

    if (wasRunning && needRestart) {
        StopBuffering();
        StartBuffering();
    }
    SyncMobileServer();
}

void RecorderApp::SyncMobileServer() {
    if (!cfg_.mobileSyncEnabled) {
        mobile_.Stop();
        return;
    }
    if (cfg_.mobilePin.empty()) {
        cfg_.mobilePin = MobileSyncServer::GenerateRandomPin();
        SaveConfig(iniPath_, cfg_);
    }
    uint16_t port = static_cast<uint16_t>(cfg_.mobileSyncPort);
    if (!mobile_.IsRunning() || mobile_.Port() != port) {
        mobile_.Start(cfg_.outputDir, port, cfg_.mobilePin);
    } else {
        mobile_.UpdateConfig(cfg_.outputDir, port, cfg_.mobilePin);
    }
}

void RecorderApp::RegenerateMobilePin() {
    cfg_.mobilePin = MobileSyncServer::GenerateRandomPin();
    SaveConfig(iniPath_, cfg_);
    if (cfg_.mobileSyncEnabled) {
        mobile_.UpdateConfig(cfg_.outputDir, static_cast<uint16_t>(cfg_.mobileSyncPort), cfg_.mobilePin);
    }
}

namespace {
std::wstring DetectForegroundAppLabel() {
    HWND fg = GetForegroundWindow();
    if (!fg) return L"Masaustu";
    DWORD pid = 0;
    GetWindowThreadProcessId(fg, &pid);
    if (!pid) return L"Masaustu";

    HANDLE hProc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    std::wstring exeName;
    if (hProc) {
        wchar_t path[MAX_PATH] = {};
        DWORD len = MAX_PATH;
        if (QueryFullProcessImageNameW(hProc, 0, path, &len) && len > 0) {
            std::wstring full(path, len);
            size_t sl = full.find_last_of(L"\\/");
            exeName = (sl != std::wstring::npos) ? full.substr(sl + 1) : full;
        }
        CloseHandle(hProc);
    }
    if (exeName.empty()) return L"Klip";

    auto eq = [&](const wchar_t* s) { return _wcsicmp(exeName.c_str(), s) == 0; };
    if (eq(L"VALORANT-Win64-Shipping.exe") || eq(L"VALORANT.exe")) return L"Valorant";
    if (eq(L"League of Legends.exe")) return L"LeagueOfLegends";
    if (eq(L"cs2.exe") || eq(L"csgo.exe")) return L"CS2";
    if (eq(L"osu!.exe")) return L"osu";
    if (eq(L"chrome.exe")) return L"Chrome";
    if (eq(L"msedge.exe")) return L"Edge";
    if (eq(L"firefox.exe")) return L"Firefox";
    if (eq(L"Discord.exe")) return L"Discord";
    if (eq(L"explorer.exe") || eq(L"ShellExperienceHost.exe")) return L"Masaustu";
    if (eq(L"AnlikKlip.exe")) return L"Klip";

    size_t dot = exeName.find_last_of(L'.');
    std::wstring stem = (dot != std::wstring::npos) ? exeName.substr(0, dot) : exeName;
    std::wstring clean;
    for (wchar_t ch : stem) {
        if ((ch >= L'0' && ch <= L'9') || (ch >= L'A' && ch <= L'Z') ||
            (ch >= L'a' && ch <= L'z') || ch == L'-' || ch == L'_') {
            clean.push_back(ch);
        }
        if (clean.size() >= 24) break;
    }
    return clean.empty() ? L"Klip" : clean;
}
} // namespace

bool RecorderApp::SaveClipAsync(int secondsOverride, SaveCallback cb) {
    if (!running_.load()) {
        if (cb) {
            cb({false, L"", L"Kayıt tamponu şu an durdurulmuş durumda.", 0.0});
        }
        return false;
    }
    if (saving_.exchange(true)) {
        if (cb) {
            cb({false, L"", L"Önceki klip halen kaydediliyor...", 0.0});
        }
        return false;
    }

    auto snap = std::make_shared<ClipSnapshot>();
    int targetSec = (secondsOverride > 0) ? secondsOverride : cfg_.clipSeconds;
    if (!ring_.TakeSnapshot(targetSec, *snap)) {
        saving_.store(false);
        if (cb) {
            cb({false, L"", L"Tamponda henüz yeterli video karesi birikmedi.", 0.0});
        }
        return false;
    }

    std::wstring appLabel = DetectForegroundAppLabel();
    std::wstring outPath = BuildUniqueClipPath(cfg_.outputDir, appLabel);

    std::lock_guard<std::mutex> lk(saveThreadMutex_);
    if (saveThread_.joinable()) {
        saveThread_.join();
    }
    saveThread_ = std::thread([this, snap, outPath, cb]() {
        ClipSaveResult res;
        {
            ComScope comScope(COINIT_MULTITHREADED);
            std::string errUtf8;
            bool ok = WriteClipToMp4(*snap, outPath, errUtf8);
            res.ok = ok;
            res.filePath = outPath;
            res.durationSeconds = snap->durationSeconds;
            if (ok) {
                wchar_t msg[160] = {};
                swprintf(msg, 160, L"Klip kaydedildi (%.1f sn)", snap->durationSeconds);
                res.message = msg;
                Logger::Log("Clip saved: %ls (%.1fs, %zu V pkts, %zu A pkts)",
                    outPath.c_str(), snap->durationSeconds, snap->video.size(), snap->audio.size());
            } else {
                int wlen = MultiByteToWideChar(CP_UTF8, 0, errUtf8.c_str(), -1, nullptr, 0);
                std::wstring werr(wlen > 0 ? wlen - 1 : 0, L'\0');
                if (wlen > 1) MultiByteToWideChar(CP_UTF8, 0, errUtf8.c_str(), -1, &werr[0], wlen);
                res.message = L"Kayıt hatası: " + werr;
                Logger::Log("Clip save failed: %s", errUtf8.c_str());
            }
        }
        saving_.store(false);
        if (cb) cb(res);
    });
    return true;
}

AppRuntimeStats RecorderApp::GetStats() const {
    AppRuntimeStats st;
    st.running = running_.load();
    st.saving = saving_.load();
    st.ring = ring_.GetStats();
    st.video = video_.GetStats();
    st.audioError = audio_.GetLastError();
    st.micLevelDb = audio_.GetMicLevelDb();

    PROCESS_MEMORY_COUNTERS_EX pmc{};
    pmc.cb = sizeof(pmc);
    if (GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&pmc), sizeof(pmc))) {
        st.workingSetMb = static_cast<double>(pmc.WorkingSetSize) / (1024.0 * 1024.0);
        st.privateMb = static_cast<double>(pmc.PrivateUsage) / (1024.0 * 1024.0);
    }
    return st;
}

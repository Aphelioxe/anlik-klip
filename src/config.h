#pragma once

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <string>

struct AppConfig {
    int clipSeconds = 60;
    int width = 1280;
    int height = 720;
    int fps = 60;
    int bitrateKbps = 6000;

    UINT hotkeyMods = 0;
    UINT hotkeyVk = VK_F9;

    UINT micHotkeyMods = 0;
    UINT micHotkeyVk = 0;

    bool captureGameAudio = true;
    int gameVolumePct = 100;

    bool captureMic = true;
    int micVolumePct = 100;
    bool micMuted = false;
    bool micNoiseGate = true;
    int micGateThresholdDb = -36; // -60..-15 dB
    std::wstring micDeviceId; // empty = default Windows mic

    std::wstring outputDir;
    bool soundNotify = true;
    bool startMinimized = true;
};

std::wstring GetExeDirectory();
std::wstring GetDefaultOutputDirectory();
bool EnsureDirectoryExists(const std::wstring& dir);

AppConfig LoadConfig(const std::wstring& iniPath);
bool SaveConfig(const std::wstring& iniPath, const AppConfig& cfg);

std::wstring FormatHotkey(UINT mods, UINT vk);

bool IsAutoStartEnabled();
bool SetAutoStartEnabled(bool enable);

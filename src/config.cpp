#include "config.h"
#include <shlobj.h>
#include <knownfolders.h>
#include <algorithm>
#include <vector>

std::wstring GetExeDirectory() {
    wchar_t buf[MAX_PATH] = {};
    DWORD len = GetModuleFileNameW(nullptr, buf, MAX_PATH);
    std::wstring s(buf, len);
    size_t pos = s.find_last_of(L"\\/");
    return (pos != std::wstring::npos) ? s.substr(0, pos) : L".";
}

std::wstring GetDefaultOutputDirectory() {
    return GetExeDirectory() + L"\\klipler";
}

bool EnsureDirectoryExists(const std::wstring& dir) {
    if (dir.empty()) return false;
    int res = SHCreateDirectoryExW(nullptr, dir.c_str(), nullptr);
    return (res == ERROR_SUCCESS || res == ERROR_ALREADY_EXISTS || res == ERROR_FILE_EXISTS);
}

static int ClampInt(int v, int lo, int hi) {
    return std::max(lo, std::min(hi, v));
}

static std::wstring ReadIniString(const wchar_t* sec, const wchar_t* key, const wchar_t* def, const std::wstring& iniPath) {
    wchar_t buf[1024] = {};
    GetPrivateProfileStringW(sec, key, def, buf, 1024, iniPath.c_str());
    return std::wstring(buf);
}

AppConfig LoadConfig(const std::wstring& iniPath) {
    AppConfig cfg;
    cfg.outputDir = GetDefaultOutputDirectory();

    if (GetFileAttributesW(iniPath.c_str()) == INVALID_FILE_ATTRIBUTES) {
        EnsureDirectoryExists(cfg.outputDir);
        SaveConfig(iniPath, cfg);
        return cfg;
    }

    cfg.clipSeconds = ClampInt(GetPrivateProfileIntW(L"Video", L"ClipSeconds", 60, iniPath.c_str()), 10, 600);
    cfg.width = ClampInt(GetPrivateProfileIntW(L"Video", L"Width", 1280, iniPath.c_str()), 640, 3840);
    cfg.height = ClampInt(GetPrivateProfileIntW(L"Video", L"Height", 720, iniPath.c_str()), 360, 2160);
    cfg.fps = (GetPrivateProfileIntW(L"Video", L"Fps", 60, iniPath.c_str()) <= 30) ? 30 : 60;
    cfg.bitrateKbps = ClampInt(GetPrivateProfileIntW(L"Video", L"BitrateKbps", 6000, iniPath.c_str()), 1500, 25000);

    cfg.hotkeyMods = GetPrivateProfileIntW(L"Hotkeys", L"ClipMods", 0, iniPath.c_str());
    cfg.hotkeyVk = GetPrivateProfileIntW(L"Hotkeys", L"ClipVk", VK_F9, iniPath.c_str());
    cfg.micHotkeyMods = GetPrivateProfileIntW(L"Hotkeys", L"MicMods", 0, iniPath.c_str());
    cfg.micHotkeyVk = GetPrivateProfileIntW(L"Hotkeys", L"MicVk", 0, iniPath.c_str());

    cfg.captureGameAudio = GetPrivateProfileIntW(L"Audio", L"CaptureGame", 1, iniPath.c_str()) != 0;
    cfg.gameVolumePct = ClampInt(GetPrivateProfileIntW(L"Audio", L"GameVolumePct", 100, iniPath.c_str()), 0, 200);
    cfg.captureMic = GetPrivateProfileIntW(L"Audio", L"CaptureMic", 1, iniPath.c_str()) != 0;
    cfg.micVolumePct = ClampInt(GetPrivateProfileIntW(L"Audio", L"MicVolumePct", 100, iniPath.c_str()), 0, 200);
    cfg.micMuted = GetPrivateProfileIntW(L"Audio", L"MicMuted", 0, iniPath.c_str()) != 0;
    cfg.micNoiseGate = GetPrivateProfileIntW(L"Audio", L"MicNoiseGate", 1, iniPath.c_str()) != 0;
    cfg.micGateThresholdDb = ClampInt(GetPrivateProfileIntW(L"Audio", L"MicGateThresholdDb", -44, iniPath.c_str()), -60, -15);
    cfg.micDeviceId = ReadIniString(L"Audio", L"MicDeviceId", L"", iniPath);

    std::wstring outDir = ReadIniString(L"General", L"OutputDir", cfg.outputDir.c_str(), iniPath);
    if (!outDir.empty()) cfg.outputDir = outDir;
    cfg.soundNotify = GetPrivateProfileIntW(L"General", L"SoundNotify", 1, iniPath.c_str()) != 0;
    cfg.startMinimized = GetPrivateProfileIntW(L"General", L"StartMinimized", 1, iniPath.c_str()) != 0;

    cfg.mobileSyncEnabled = GetPrivateProfileIntW(L"Mobile", L"Enabled", 0, iniPath.c_str()) != 0;
    cfg.mobileSyncPort = ClampInt(GetPrivateProfileIntW(L"Mobile", L"Port", 47820, iniPath.c_str()), 1024, 65535);
    cfg.mobilePin = ReadIniString(L"Mobile", L"Pin", L"", iniPath);

    EnsureDirectoryExists(cfg.outputDir);
    return cfg;
}

static void WriteIniInt(const wchar_t* sec, const wchar_t* key, int val, const std::wstring& iniPath) {
    wchar_t buf[32] = {};
    swprintf(buf, 32, L"%d", val);
    WritePrivateProfileStringW(sec, key, buf, iniPath.c_str());
}

bool SaveConfig(const std::wstring& iniPath, const AppConfig& cfg) {
    WriteIniInt(L"Video", L"ClipSeconds", cfg.clipSeconds, iniPath);
    WriteIniInt(L"Video", L"Width", cfg.width, iniPath);
    WriteIniInt(L"Video", L"Height", cfg.height, iniPath);
    WriteIniInt(L"Video", L"Fps", cfg.fps, iniPath);
    WriteIniInt(L"Video", L"BitrateKbps", cfg.bitrateKbps, iniPath);

    WriteIniInt(L"Hotkeys", L"ClipMods", (int)cfg.hotkeyMods, iniPath);
    WriteIniInt(L"Hotkeys", L"ClipVk", (int)cfg.hotkeyVk, iniPath);
    WriteIniInt(L"Hotkeys", L"MicMods", (int)cfg.micHotkeyMods, iniPath);
    WriteIniInt(L"Hotkeys", L"MicVk", (int)cfg.micHotkeyVk, iniPath);

    WriteIniInt(L"Audio", L"CaptureGame", cfg.captureGameAudio ? 1 : 0, iniPath);
    WriteIniInt(L"Audio", L"GameVolumePct", cfg.gameVolumePct, iniPath);
    WriteIniInt(L"Audio", L"CaptureMic", cfg.captureMic ? 1 : 0, iniPath);
    WriteIniInt(L"Audio", L"MicVolumePct", cfg.micVolumePct, iniPath);
    WriteIniInt(L"Audio", L"MicMuted", cfg.micMuted ? 1 : 0, iniPath);
    WriteIniInt(L"Audio", L"MicNoiseGate", cfg.micNoiseGate ? 1 : 0, iniPath);
    WriteIniInt(L"Audio", L"MicGateThresholdDb", cfg.micGateThresholdDb, iniPath);
    WritePrivateProfileStringW(L"Audio", L"MicDeviceId", cfg.micDeviceId.c_str(), iniPath.c_str());

    WritePrivateProfileStringW(L"General", L"OutputDir", cfg.outputDir.c_str(), iniPath.c_str());
    WriteIniInt(L"General", L"SoundNotify", cfg.soundNotify ? 1 : 0, iniPath);
    WriteIniInt(L"General", L"StartMinimized", cfg.startMinimized ? 1 : 0, iniPath);

    WriteIniInt(L"Mobile", L"Enabled", cfg.mobileSyncEnabled ? 1 : 0, iniPath);
    WriteIniInt(L"Mobile", L"Port", cfg.mobileSyncPort, iniPath);
    WritePrivateProfileStringW(L"Mobile", L"Pin", cfg.mobilePin.c_str(), iniPath.c_str());
    return true;
}

std::wstring FormatHotkey(UINT mods, UINT vk) {
    if (vk == 0) return L"Devre Dışı";
    std::wstring s;
    if (mods & MOD_CONTROL) s += L"Ctrl + ";
    if (mods & MOD_ALT) s += L"Alt + ";
    if (mods & MOD_SHIFT) s += L"Shift + ";
    if (mods & MOD_WIN) s += L"Win + ";

    UINT scan = MapVirtualKeyW(vk, MAPVK_VK_TO_VSC);
    switch (vk) {
        case VK_LEFT: case VK_UP: case VK_RIGHT: case VK_DOWN:
        case VK_PRIOR: case VK_NEXT: case VK_END: case VK_HOME:
        case VK_INSERT: case VK_DELETE: case VK_DIVIDE: case VK_NUMLOCK:
            scan |= 0x100;
            break;
    }
    wchar_t name[64] = {};
    if (GetKeyNameTextW((LONG)(scan << 16), name, 64) > 0) {
        s += name;
    } else {
        wchar_t hex[16] = {};
        swprintf(hex, 16, L"VK_%02X", vk);
        s += hex;
    }
    return s;
}

static const wchar_t* kRunRegKey = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
static const wchar_t* kRunValueName = L"AnlikKlip";

bool IsAutoStartEnabled() {
    HKEY hKey = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunRegKey, 0, KEY_READ, &hKey) != ERROR_SUCCESS) {
        return false;
    }
    DWORD type = 0, cb = 0;
    LONG r = RegQueryValueExW(hKey, kRunValueName, nullptr, &type, nullptr, &cb);
    RegCloseKey(hKey);
    return (r == ERROR_SUCCESS && cb > 0);
}

bool SetAutoStartEnabled(bool enable) {
    HKEY hKey = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunRegKey, 0, KEY_WRITE, &hKey) != ERROR_SUCCESS) {
        return false;
    }
    LONG r = ERROR_SUCCESS;
    if (enable) {
        wchar_t exePath[MAX_PATH] = {};
        GetModuleFileNameW(nullptr, exePath, MAX_PATH);
        std::wstring cmd = L"\"" + std::wstring(exePath) + L"\" --minimized";
        r = RegSetValueExW(hKey, kRunValueName, 0, REG_SZ,
            reinterpret_cast<const BYTE*>(cmd.c_str()),
            static_cast<DWORD>((cmd.size() + 1) * sizeof(wchar_t)));
    } else {
        r = RegDeleteValueW(hKey, kRunValueName);
        if (r == ERROR_FILE_NOT_FOUND) r = ERROR_SUCCESS;
    }
    RegCloseKey(hKey);
    return r == ERROR_SUCCESS;
}

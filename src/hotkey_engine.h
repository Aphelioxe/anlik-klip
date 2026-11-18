#pragma once

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <atomic>
#include <thread>

class HotkeyEngine {
public:
    HotkeyEngine();
    ~HotkeyEngine();

    bool Start(HWND notifyHwnd, UINT msgClip, UINT msgMic);
    void Stop();

    void SetHotkeys(UINT clipMods, UINT clipVk, UINT micMods, UINT micVk);
    void SetPaused(bool paused);

    bool IsHookActive() const { return hookActive_.load(); }
    bool IsRawInputActive() const { return rawActive_.load(); }

    static bool IsProcessElevated();
    static bool RestartElevated();

private:
    static LRESULT CALLBACK MsgWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
    static LRESULT CALLBACK LowLevelProc(int nCode, WPARAM wParam, LPARAM lParam);

    void ThreadMain();
    void HandleKeyEvent(UINT vk, bool isDown, bool isUp, UINT mods);

    static HotkeyEngine* s_instance_;

    HWND notifyHwnd_ = nullptr;
    UINT msgClip_ = 0;
    UINT msgMic_ = 0;

    std::thread thread_;
    DWORD threadId_ = 0;
    HWND msgHwnd_ = nullptr;
    HHOOK hook_ = nullptr;

    std::atomic<bool> stop_{false};
    std::atomic<bool> started_{false};
    std::atomic<bool> paused_{false};
    std::atomic<bool> hookActive_{false};
    std::atomic<bool> rawActive_{false};

    std::atomic<UINT> clipMods_{0};
    std::atomic<UINT> clipVk_{0};
    std::atomic<UINT> micMods_{0};
    std::atomic<UINT> micVk_{0};

    std::atomic<bool> clipKeyDown_{false};
    std::atomic<bool> micKeyDown_{false};
    std::atomic<ULONGLONG> lastClipFireMs_{0};
    std::atomic<ULONGLONG> lastMicFireMs_{0};

    // Track modifier state inside raw input as well
    bool rawCtrlL_ = false, rawCtrlR_ = false;
    bool rawAltL_ = false, rawAltR_ = false;
    bool rawShiftL_ = false, rawShiftR_ = false;
};

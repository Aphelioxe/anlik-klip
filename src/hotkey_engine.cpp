#include "hotkey_engine.h"
#include "com_ptr.h"

#include <shellapi.h>

namespace {
constexpr UINT WM_HK_REAPPLY = WM_APP + 50;
constexpr int HK_BACKUP_CLIP = 1;
constexpr int HK_BACKUP_MIC  = 2;
const wchar_t* kHkWndClass = L"AnlikKlipHotkeySink";

UINT QueryAsyncMods() {
    UINT m = 0;
    if (GetAsyncKeyState(VK_CONTROL) & 0x8000) m |= MOD_CONTROL;
    if (GetAsyncKeyState(VK_MENU)    & 0x8000) m |= MOD_ALT;
    if (GetAsyncKeyState(VK_SHIFT)   & 0x8000) m |= MOD_SHIFT;
    return m;
}
} // namespace

HotkeyEngine* HotkeyEngine::s_instance_ = nullptr;

HotkeyEngine::HotkeyEngine() = default;

HotkeyEngine::~HotkeyEngine() {
    Stop();
}

bool HotkeyEngine::Start(HWND notifyHwnd, UINT msgClip, UINT msgMic) {
    Stop();
    notifyHwnd_ = notifyHwnd;
    msgClip_ = msgClip;
    msgMic_ = msgMic;
    stop_.store(false);
    started_.store(false);
    s_instance_ = this;
    thread_ = std::thread(&HotkeyEngine::ThreadMain, this);

    for (int i = 0; i < 100 && !started_.load(); ++i) {
        Sleep(5);
    }
    return started_.load();
}

void HotkeyEngine::Stop() {
    stop_.store(true);
    if (threadId_ != 0) {
        PostThreadMessageW(threadId_, WM_QUIT, 0, 0);
    }
    if (thread_.joinable()) {
        thread_.join();
    }
    threadId_ = 0;
    s_instance_ = nullptr;
}

void HotkeyEngine::SetHotkeys(UINT clipMods, UINT clipVk, UINT micMods, UINT micVk) {
    clipMods_.store(clipMods);
    clipVk_.store(clipVk);
    micMods_.store(micMods);
    micVk_.store(micVk);
    clipKeyDown_.store(false);
    micKeyDown_.store(false);
    if (msgHwnd_) {
        PostMessageW(msgHwnd_, WM_HK_REAPPLY, 0, 0);
    }
}

void HotkeyEngine::SetPaused(bool paused) {
    paused_.store(paused);
    clipKeyDown_.store(false);
    micKeyDown_.store(false);
}

void HotkeyEngine::HandleKeyEvent(UINT vk, bool isDown, bool isUp, UINT mods) {
    if (paused_.load() || vk == 0) return;

    const UINT cVk = clipVk_.load();
    if (cVk != 0 && vk == cVk) {
        if (isUp) {
            clipKeyDown_.store(false);
        } else if (isDown) {
            if (!clipKeyDown_.load() && mods == clipMods_.load()) {
                clipKeyDown_.store(true);
                ULONGLONG now = GetTickCount64();
                ULONGLONG prev = lastClipFireMs_.load();
                if (now - prev > 450 && lastClipFireMs_.compare_exchange_strong(prev, now)) {
                    if (notifyHwnd_) PostMessageW(notifyHwnd_, msgClip_, 0, 0);
                }
            }
        }
    }

    const UINT mVk = micVk_.load();
    if (mVk != 0 && vk == mVk) {
        if (isUp) {
            micKeyDown_.store(false);
        } else if (isDown) {
            if (!micKeyDown_.load() && mods == micMods_.load()) {
                micKeyDown_.store(true);
                ULONGLONG now = GetTickCount64();
                ULONGLONG prev = lastMicFireMs_.load();
                if (now - prev > 350 && lastMicFireMs_.compare_exchange_strong(prev, now)) {
                    if (notifyHwnd_) PostMessageW(notifyHwnd_, msgMic_, 0, 0);
                }
            }
        }
    }
}

LRESULT CALLBACK HotkeyEngine::LowLevelProc(int nCode, WPARAM wParam, LPARAM lParam) {
    HotkeyEngine* self = s_instance_;
    if (nCode == HC_ACTION && self && !self->paused_.load()) {
        const auto* kb = reinterpret_cast<const KBDLLHOOKSTRUCT*>(lParam);
        if (kb && (kb->flags & LLKHF_INJECTED) == 0) {
            const bool isDown = (wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN);
            const bool isUp   = (wParam == WM_KEYUP   || wParam == WM_SYSKEYUP);
            if (isDown || isUp) {
                self->HandleKeyEvent(kb->vkCode, isDown, isUp, QueryAsyncMods());
            }
        }
    }
    return CallNextHookEx(nullptr, nCode, wParam, lParam);
}

LRESULT CALLBACK HotkeyEngine::MsgWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    auto* self = reinterpret_cast<HotkeyEngine*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (msg == WM_NCCREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
        self = reinterpret_cast<HotkeyEngine*>(cs->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        return TRUE;
    }
    if (!self) return DefWindowProcW(hwnd, msg, wParam, lParam);

    switch (msg) {
        case WM_HK_REAPPLY: {
            UnregisterHotKey(hwnd, HK_BACKUP_CLIP);
            UnregisterHotKey(hwnd, HK_BACKUP_MIC);
            UINT cv = self->clipVk_.load();
            if (cv != 0) RegisterHotKey(hwnd, HK_BACKUP_CLIP, self->clipMods_.load() | MOD_NOREPEAT, cv);
            UINT mv = self->micVk_.load();
            if (mv != 0) RegisterHotKey(hwnd, HK_BACKUP_MIC, self->micMods_.load() | MOD_NOREPEAT, mv);
            return 0;
        }
        case WM_HOTKEY:
            if (wParam == HK_BACKUP_CLIP && !self->paused_.load()) {
                ULONGLONG now = GetTickCount64();
                ULONGLONG prev = self->lastClipFireMs_.load();
                if (now - prev > 450 && self->lastClipFireMs_.compare_exchange_strong(prev, now)) {
                    if (self->notifyHwnd_) PostMessageW(self->notifyHwnd_, self->msgClip_, 0, 0);
                }
            } else if (wParam == HK_BACKUP_MIC && !self->paused_.load()) {
                ULONGLONG now = GetTickCount64();
                ULONGLONG prev = self->lastMicFireMs_.load();
                if (now - prev > 350 && self->lastMicFireMs_.compare_exchange_strong(prev, now)) {
                    if (self->notifyHwnd_) PostMessageW(self->notifyHwnd_, self->msgMic_, 0, 0);
                }
            }
            return 0;

        case WM_INPUT: {
            UINT size = 0;
            GetRawInputData(reinterpret_cast<HRAWINPUT>(lParam), RID_INPUT, nullptr, &size, sizeof(RAWINPUTHEADER));
            if (size == 0 || size > 256) return 0;
            BYTE buf[256];
            if (GetRawInputData(reinterpret_cast<HRAWINPUT>(lParam), RID_INPUT, buf, &size, sizeof(RAWINPUTHEADER)) != size) {
                return 0;
            }
            const auto* raw = reinterpret_cast<const RAWINPUT*>(buf);
            if (raw->header.dwType == RIM_TYPEKEYBOARD) {
                const RAWKEYBOARD& rk = raw->data.keyboard;
                const bool isUp = (rk.Flags & RI_KEY_BREAK) != 0;
                const bool isDown = !isUp;
                const bool e0 = (rk.Flags & RI_KEY_E0) != 0;
                const UINT vk = rk.VKey;

                if (vk == VK_CONTROL) {
                    if (e0) self->rawCtrlR_ = isDown; else self->rawCtrlL_ = isDown;
                } else if (vk == VK_MENU) {
                    if (e0) self->rawAltR_ = isDown; else self->rawAltL_ = isDown;
                } else if (vk == VK_SHIFT) {
                    if (rk.MakeCode == 0x36) self->rawShiftR_ = isDown; else self->rawShiftL_ = isDown;
                }

                UINT mods = 0;
                if (self->rawCtrlL_ || self->rawCtrlR_ || (GetAsyncKeyState(VK_CONTROL) & 0x8000)) mods |= MOD_CONTROL;
                if (self->rawAltL_  || self->rawAltR_  || (GetAsyncKeyState(VK_MENU)    & 0x8000)) mods |= MOD_ALT;
                if (self->rawShiftL_|| self->rawShiftR_|| (GetAsyncKeyState(VK_SHIFT)   & 0x8000)) mods |= MOD_SHIFT;

                self->HandleKeyEvent(vk, isDown, isUp, mods);
            }
            return 0;
        }
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

void HotkeyEngine::ThreadMain() {
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
    threadId_ = GetCurrentThreadId();

    HINSTANCE hInst = GetModuleHandleW(nullptr);
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = HotkeyEngine::MsgWndProc;
    wc.hInstance = hInst;
    wc.lpszClassName = kHkWndClass;
    RegisterClassExW(&wc);

    msgHwnd_ = CreateWindowExW(0, kHkWndClass, L"", 0, 0, 0, 0, 0,
        HWND_MESSAGE, nullptr, hInst, this);

    // 1. Passive background Raw Input (works even when games ignore LL hooks)
    if (msgHwnd_) {
        RAWINPUTDEVICE rid{};
        rid.usUsagePage = 0x01; // Generic Desktop
        rid.usUsage = 0x06;     // Keyboard
        rid.dwFlags = RIDEV_INPUTSINK;
        rid.hwndTarget = msgHwnd_;
        rawActive_.store(RegisterRawInputDevices(&rid, 1, sizeof(rid)) != FALSE);
    }

    // 2. Low-level keyboard hook on this dedicated non-blocking thread
    hook_ = SetWindowsHookExW(WH_KEYBOARD_LL, HotkeyEngine::LowLevelProc, hInst, 0);
    hookActive_.store(hook_ != nullptr);

    Logger::Log("HotkeyEngine started: rawInput=%d llHook=%d",
        rawActive_.load() ? 1 : 0, hookActive_.load() ? 1 : 0);

    if (msgHwnd_) {
        PostMessageW(msgHwnd_, WM_HK_REAPPLY, 0, 0);
    }
    started_.store(true);

    MSG msg{};
    while (!stop_.load() && GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    if (hook_) {
        UnhookWindowsHookEx(hook_);
        hook_ = nullptr;
    }
    hookActive_.store(false);

    if (msgHwnd_) {
        RAWINPUTDEVICE rid{};
        rid.usUsagePage = 0x01;
        rid.usUsage = 0x06;
        rid.dwFlags = RIDEV_REMOVE;
        rid.hwndTarget = nullptr;
        RegisterRawInputDevices(&rid, 1, sizeof(rid));
        UnregisterHotKey(msgHwnd_, HK_BACKUP_CLIP);
        UnregisterHotKey(msgHwnd_, HK_BACKUP_MIC);
        DestroyWindow(msgHwnd_);
        msgHwnd_ = nullptr;
    }
    rawActive_.store(false);
}

bool HotkeyEngine::IsProcessElevated() {
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return false;
    TOKEN_ELEVATION elev{};
    DWORD sz = sizeof(elev);
    bool ok = GetTokenInformation(token, TokenElevation, &elev, sizeof(elev), &sz) != FALSE;
    CloseHandle(token);
    return ok && elev.TokenIsElevated != 0;
}

bool HotkeyEngine::RestartElevated() {
    wchar_t exe[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    HINSTANCE res = ShellExecuteW(nullptr, L"runas", exe, L"--restarted", nullptr, SW_SHOWNORMAL);
    return reinterpret_cast<INT_PTR>(res) > 32;
}

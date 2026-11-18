#pragma once

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include "hotkey_engine.h"
#include "main_window.h"
#include "recorder_app.h"
#include <memory>

constexpr UINT WM_APP_TRAY         = WM_APP + 1;
constexpr UINT WM_APP_CLIP_SAVED   = WM_APP + 2;
constexpr UINT WM_APP_SHOW_UI      = WM_APP + 3;
constexpr UINT WM_APP_TRIGGER_CLIP = WM_APP + 4;
constexpr UINT WM_APP_TRIGGER_MIC  = WM_APP + 5;
constexpr UINT WM_APP_DESTROY_UI   = WM_APP + 6;

class TrayUI {
public:
    TrayUI(HINSTANCE hInst, RecorderApp& app);
    ~TrayUI();

    bool Create(bool showWindow);
    void Destroy();
    void ShowSettingsWindow(int tabIndex = 0);
    void CloseMainWindowToTray();

    bool HandlePreTranslateMessage(const MSG& msg);
    HWND MainHwnd() const { return mainWin_ ? mainWin_->Hwnd() : nullptr; }

private:
    static LRESULT CALLBACK TrayWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
    LRESULT OnMessage(UINT msg, WPARAM wParam, LPARAM lParam);

    void ApplyConfigFromUi(const AppConfig& newCfg);
    void UpdateTrayIcon();
    void ShowTrayMenu();
    void ShowBalloon(const wchar_t* title, const wchar_t* text, DWORD flags);
    void TriggerSaveClip(int secondsOverride = 0);
    void OpenFolder(const std::wstring& path);
    HICON CreateStatusIcon(COLORREF dotColor);

    HINSTANCE hInst_;
    RecorderApp& app_;
    HotkeyEngine hotkeys_;
    std::unique_ptr<MainWindow> mainWin_;

    HWND trayHwnd_ = nullptr;
    HICON iconActive_ = nullptr;
    HICON iconPaused_ = nullptr;
    HICON iconSaving_ = nullptr;
    bool trayAdded_ = false;
    bool shownHideBalloon_ = false;
    UINT wmTaskbarCreated_ = 0;
};

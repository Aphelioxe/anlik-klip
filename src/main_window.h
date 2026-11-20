#pragma once

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "clip_library.h"
#include "recorder_app.h"
#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

class HotkeyEngine;
struct IMFPMediaPlayer;

class MainWindow {
public:
    struct Callbacks {
        std::function<void()> onClosed;
        std::function<void()> onExitRequested;
        std::function<void()> onSaveClipRequested;
        std::function<void()> onToggleBufferingRequested;
        std::function<void()> onToggleMicMuteRequested;
        std::function<void(const AppConfig&)> onConfigApplied;
    };

    MainWindow(HINSTANCE hInst, RecorderApp& app, HotkeyEngine& hotkeys, Callbacks cb);
    ~MainWindow();

    bool Create(int initialTab = 0);
    void BringToFront(int tab = -1);
    void NotifyClipSaved(const ClipSaveResult& res);
    void RefreshLiveStats();

    bool HandlePreTranslateMessage(const MSG& msg);
    HWND Hwnd() const { return hwnd_; }
    bool IsPlayerOpen() const { return playerOpen_; }
    bool IsPlayerReady() const { return playerReady_; }

private:
    static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
    static LRESULT CALLBACK VideoHostProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
    LRESULT OnMessage(UINT msg, WPARAM wParam, LPARAM lParam);

    void CreateControls();
    void SwitchTab(int tabIndex);
    void LoadSettingsControls();
    void ReadSettingsControls(AppConfig& out);

    // Clip gallery + async thumbnail loader
    void RefreshClips(const std::wstring& selectPath = L"");
    void RebuildFilteredView(const std::wstring& preferPath = L"");
    void UpdateGalleryControlVisibility();
    void StartThumbnailWorker();
    void StopThumbnailWorker();
    void ClearThumbnails();
    void ClampScroll();

    // Embedded MFPlay player + trimmer
    void OpenEmbeddedPlayer(int viewIdx);
    void CloseEmbeddedPlayer();
    void TogglePlayerPlayPause();
    void SeekPlayerRelative(double deltaSec);
    void SeekPlayerAbsolute(double targetSec);
    void SetPlayerVolume(float vol01);
    void TogglePlayerMute();
    void CyclePlayerRate();
    void UpdatePlayerMetrics();
    void TogglePlayerFullscreen();
    void ToggleTrimMode();
    void StartTrimAsync(bool replaceOriginal, int targetSizeMb = 0);
    void StopTrimWorker();
    void PickInsertClip();
    RECT GetSeekBarRect() const;
    RECT GetVolumeBarRect() const;
    RECT GetGameVolBarRect() const;
    RECT GetMicVolBarRect() const;

    // Hit-testing & custom painting
    struct UiButtonRect {
        int id = 0;
        RECT rc{};
        bool enabled = true;
    };
    void PaintWindow(HDC hdc);
    void FreeBackbuffer();
    void UpdateLayoutForSize(int cw, int ch);
    RECT GetGridViewportRect() const;
    void GetGridMetrics(int& outCols, int& outCardW, int& outCardH, int& outGapX, int& outGapY) const;
    int HitTestCard(int mx, int my) const;
    int HitTestCardStar(int mx, int my) const;
    int HitTestTrimHandle(int mx, int my) const;
    int HitTestCustomButton(int mx, int my) const;
    void UpdateHover(int mx, int my);

    // Clip actions
    void ToggleFavoriteForViewIdx(int viewIdx = -1);
    void PlaySelected();
    void PlayExternalSelected();
    void RevealSelected();
    void RenameSelected();
    void DeleteSelected();
    void PickOutputFolder();
    void ShowDarkComboDropdown(HWND comboHwnd, RECT anchorClientRc);
    void OpenFolder(const std::wstring& path);
    void SetFooterMessage(const std::wstring& msg);

    HINSTANCE hInst_;
    RecorderApp& app_;
    HotkeyEngine& hotkeys_;
    Callbacks cb_;

    HWND hwnd_ = nullptr;
    UINT dpi_ = 96;
    ULONG_PTR gdipToken_ = 0;

    HFONT font_ = nullptr;
    HFONT boldFont_ = nullptr;
    HBRUSH brBg_ = nullptr;
    HBRUSH brCard_ = nullptr;
    HBRUSH brInput_ = nullptr;

    int activeTab_ = 0; // 0 = Kliplerim, 1 = Ayarlar
    int hoverBtnId_ = 0;
    int pressedBtnId_ = 0;
    int hoverCardIdx_ = -1;
    int selectedIdx_ = -1;
    int scrollY_ = 0;
    int maxScrollY_ = 0;
    int settingsOffsetX_ = 0;
    HDC memDc_ = nullptr;
    HBITMAP memBmp_ = nullptr;
    HGDIOBJ memOldBmp_ = nullptr;
    int memW_ = 0;
    int memH_ = 0;
    int openAnimAlpha_ = 255;
    int openTargetX_ = 0;
    int openTargetY_ = 0;
    ULONGLONG openAnimStartMs_ = 0;
    ULONGLONG tabAnimStartMs_ = 0;
    float openAnimT_ = 1.0f;
    double animPhase_ = 0.0;
    float tabAnimT_ = 1.0f;
    int tabAnimDir_ = 1;
    float tabIndicatorY_ = 118.0f;
    bool trackingMouse_ = false;
    std::vector<UiButtonRect> customButtons_;

    // Embedded player state
    bool playerOpen_ = false;
    int playerClipIdx_ = -1;
    std::wstring playerClipPath_;
    std::wstring playerClipTitle_;
    HWND videoHwnd_ = nullptr;
    IMFPMediaPlayer* player_ = nullptr;
    class PlayerCallbackImpl* playerCb_ = nullptr;
    uint64_t playerGen_ = 0;
    bool playerReady_ = false;
    bool playerPlaying_ = false;
    bool playerEnded_ = false;
    double playerPosSec_ = 0.0;
    double playerDurSec_ = 0.0;
    float playerVol_ = 1.0f;
    bool playerMuted_ = false;
    float playerRate_ = 1.0f;
    bool draggingSeek_ = false;
    bool draggingVol_ = false;
    bool playerFullscreen_ = false;
    RECT savedWindowRc_{};
    DWORD savedWindowStyle_ = 0;
    DWORD savedWindowExStyle_ = 0;

    // Trim & low-level multi-track editor state (only active inside embedded player)
    bool trimMode_ = false;
    double trimStartSec_ = 0.0;
    double trimEndSec_ = 0.0;
    bool editCutActive_ = false;
    double cutStartSec_ = 0.0;
    double cutEndSec_ = 0.0;
    int draggingTrim_ = 0; // 0=none, 1=start, 2=end, 3=cutStart, 4=cutEnd
    bool editHasMultiTrack_ = false;
    int editGameVolPct_ = 100;
    int editMicVolPct_ = 100;
    bool editGameMuted_ = false;
    bool editMicMuted_ = false;
    bool draggingEditGameVol_ = false;
    bool draggingEditMicVol_ = false;
    double editSpeed_ = 1.0;
    bool editVertical916_ = false;
    std::wstring editInsertClipPath_;
    bool trimBusy_ = false;
    std::atomic<bool> trimCancel_{false};
    std::thread trimThread_;

    // Gallery search & favorites state
    bool favOnly_ = false;
    std::wstring searchQuery_;
    std::vector<size_t> filteredIndices_;
    HWND edtSearch_ = nullptr;

    // Hotkey capture state: 0=none, 1=clip, 2=mic
    int captureHotkeyMode_ = 0;
    UINT pendingClipMods_ = 0, pendingClipVk_ = 0;
    UINT pendingMicMods_ = 0, pendingMicVk_ = 0;

    // Clips & thumbnails (kept only while MainWindow is open)
    std::vector<ClipItem> clips_;
    std::unordered_map<std::wstring, HBITMAP> thumbBmps_;
    std::atomic<bool> thumbStop_{false};
    std::atomic<uint64_t> thumbGen_{0};
    std::thread thumbThread_;

    std::wstring footerMsg_ = L"Pencereyi kapattığınızda kayıt ve kısayol arka planda en düşük RAM ile devam eder.";
    std::vector<AudioDeviceInfo> micDevices_;
    std::vector<HWND> settingsCtrls_;

    // Settings tab native controls
    HWND cmbDuration_ = nullptr;
    HWND cmbResFps_ = nullptr;
    HWND cmbBitrate_ = nullptr;
    HWND chkGameAudio_ = nullptr;
    HWND trkGameVol_ = nullptr;
    HWND lblGameVol_ = nullptr;
    HWND chkMicAudio_ = nullptr;
    HWND chkMicMute_ = nullptr;
    HWND trkMicVol_ = nullptr;
    HWND lblMicVol_ = nullptr;
    HWND cmbMicDev_ = nullptr;
    HWND chkMicGate_ = nullptr;
    HWND trkMicGate_ = nullptr;
    HWND lblMicGate_ = nullptr;
    HWND edtClipHotkey_ = nullptr;
    HWND btnSetClipHotkey_ = nullptr;
    HWND edtMicHotkey_ = nullptr;
    HWND btnSetMicHotkey_ = nullptr;
    HWND btnClearMicHotkey_ = nullptr;
    HWND lblHotkeyStatus_ = nullptr;
    HWND btnRestartAdmin_ = nullptr;
    HWND edtOutDir_ = nullptr;
    HWND btnBrowseDir_ = nullptr;
    HWND btnOpenDir_ = nullptr;
    HWND chkSound_ = nullptr;
    HWND btnTestSound_ = nullptr;
    HWND chkAutoStart_ = nullptr;
    HWND chkMobileSync_ = nullptr;
    HWND btnShowQr_ = nullptr;
    HWND btnApply_ = nullptr;
    HWND btnExitApp_ = nullptr;

    void ShowMobileQrModal();

    int S(int px) const { return MulDiv(px, static_cast<int>(dpi_), 96); }
};

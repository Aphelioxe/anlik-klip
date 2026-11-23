#pragma once

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "clip_library.h"
#include "clip_trimmer.h"
#include "recorder_app.h"
#include <atomic>
#include <condition_variable>
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
    bool IsStudioOpen() const { return studioOpen_; }

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
    void ComputeGalleryLayout(std::vector<RECT>& outCardRects, int& outFavHeaderY, int& outNormHeaderY,
        int& outTotalH, int& outFavCount, int& outNormCount) const;
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
    void ShareSelected();
    void CopySelectedShareLink();
    void OpenSelectedShareLink();
    void StartUploadClip(const std::wstring& filePath);
    void StopUploadWorker();
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

    int activeTab_ = 0; // 0 = Kliplerim, 1 = Montaj, 2 = Ayarlar
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

    // Timeline audio waveform & hover-scrub preview (freed when player closes)
    std::vector<float> waveGame_;
    std::vector<float> waveMic_;
    std::thread waveThread_;
    std::atomic<bool> waveStop_{false};
    void StartWaveformWorker();
    void StopWaveformWorker();

    bool hoverSeekActive_ = false;
    int hoverSeekX_ = 0;
    double hoverSeekSec_ = -1.0;
    HBITMAP hoverScrubBmp_ = nullptr;
    double hoverScrubBmpSec_ = -1.0;
    std::thread hoverScrubThread_;
    std::atomic<bool> hoverScrubStop_{false};
    std::mutex hoverScrubMtx_;
    std::condition_variable hoverScrubCv_;
    double hoverScrubTargetSec_ = -1.0;
    uint64_t hoverScrubReqGen_ = 0;
    void RequestHoverScrub(double sec);
    void StopHoverScrubWorker();

    // Montage tab state & Medal-style Studio NLE
    struct StudioClipItem {
        std::wstring path;
        std::wstring name;
        double sourceDurSec = 0.0;
        double timelineStartSec = 0.0;
        double trimStartSec = 0.0;
        double trimEndSec = 0.0;
        double speed = 1.0;
        bool hasMultiTrack = false;
        int gameVolPct = 100;
        int micVolPct = 100;
        bool waveLoaded = false;
        std::vector<float> wavePeaks;
    };

    struct StudioMusicItem {
        std::wstring path;
        std::wstring name;
        double sourceDurSec = 0.0;
        double timelineStartSec = 0.0;
        double trimStartSec = 0.0;
        double durationSec = 0.0;
        int volPct = 85;
        bool fadeIn = true;
        bool fadeOut = true;
        bool waveLoaded = false;
        std::vector<float> wavePeaks;
    };

    struct StudioTransitionItem {
        MontageTransitionType type = MontageTransitionType::FlashWhite;
        double timelineSec = 0.0;
        double durationSec = 0.45;
    };

    class StudioMpCallback;

    std::vector<std::wstring> montagePaths_;
    int montageLibScroll_ = 0;
    int montageQueueScroll_ = 0;
    bool montageFade_ = true;
    bool montageVertical916_ = false;
    double montageSpeed_ = 1.0;
    bool montageGameMuted_ = false;
    bool montageMicMuted_ = false;
    void StartMontageExport(int targetSizeMb);

    bool studioOpen_ = false;
    int studioLeftTab_ = 0;
    int studioLibScroll_ = 0;
    int studioMusicLibScroll_ = 0;
    bool studioFade_ = true;
    bool studioVertical916_ = false;
    int studioTargetMb_ = 0;
    std::vector<StudioClipItem> studioClips_;
    std::vector<StudioMusicItem> studioMusic_;
    std::vector<StudioTransitionItem> studioTransitions_;
    std::vector<std::wstring> studioRecentAudio_;
    int studioSelClip_ = -1;
    int studioSelMusic_ = -1;
    int studioSelFx_ = -1;

    double studioPlayheadSec_ = 0.0;
    double studioTimelineScrollSec_ = 0.0;
    double studioPxPerSec_ = 76.0;

    bool studioPlaying_ = false;
    bool studioVideoReady_ = false;
    bool studioAudioReady_ = false;
    int studioLoadedClipIdx_ = -1;
    std::wstring studioLoadedClipPath_;
    std::wstring studioLoadedMusicPath_;
    double studioPendingSeekSec_ = -1.0;
    double studioPendingAudioSeekSec_ = -1.0;
    std::atomic<uint64_t> studioGen_{0};
    HWND studioVideoHwnd_ = nullptr;
    IMFPMediaPlayer* studioPlayer_ = nullptr;
    IMFPMediaPlayer* studioMusicPlayer_ = nullptr;
    StudioMpCallback* studioVideoCb_ = nullptr;
    StudioMpCallback* studioAudioCb_ = nullptr;

    std::thread studioWaveThread_;
    std::atomic<bool> studioWaveStop_{false};

    int studioDragMode_ = 0;
    int studioDragIdx_ = -1;
    int studioDragStartX_ = 0;
    double studioDragOrigA_ = 0.0;
    double studioDragOrigB_ = 0.0;
    double studioDragOrigC_ = 0.0;
    bool studioDragWasPlaying_ = false;
    bool studioClipDragged_ = false;
    std::vector<double> studioDragOrigStarts_;
    ULONGLONG studioGapLastTickMs_ = 0;
    ULONGLONG studioLastScrubMs_ = 0;
    ULONGLONG studioIgnoreEndUntilMs_ = 0;
    bool studioSeekPending_ = false;
    int studioHoverClipIdx_ = -1;
    int studioHoverClipSide_ = 0;  // -1=left, +1=right, 0=none
    int studioHoverMusicIdx_ = -1;
    int studioHoverMusicSide_ = 0; // -1=left, +1=right, 0=none
    int studioHoverFxIdx_ = -1;

    void OpenStudio(bool loadQueued = true);
    void CloseStudio();
    void UpdateStudioLayout(int cw, int ch);
    void UpdateStudioMetrics();
    double StudioTotalDuration() const;
    bool MapTimelineToClip(double tlSec, int& outIdx, double& outLocalSec, double& outClipStartSec) const;
    int StudioHitTestTransition(int mx, int my, int cw, int ch) const;
    bool StudioHitTestTrimEdge(int mx, int my, int cw, int ch, int& outKind, int& outIdx, int& outSide, double& outAccSec) const;
    int StudioHitTestClipBody(int mx, int my, int cw, int ch) const;
    void StudioFitZoomToView(int cw, int ch);
    void StudioTrimClipEdgeToPlayhead(bool leftEdge);
    void StudioAddClipFromLibrary(size_t rawClipIdx);
    void StudioAddMusicFile(const std::wstring& audioPath = L"");
    void StudioAddTransition(MontageTransitionType type);
    void StudioSplitAtPlayhead();
    void StudioDeleteSelection();
    void StudioMoveSelectedClip(int dir);
    void StudioCycleClipSpeed();
    void StudioTogglePlayPause();
    void StudioSeekTimeline(double targetSec, bool syncAudio = true);
    void AdvancePastStudioClip(int curIdx);
    void SyncStudioMediaState(bool forceSeekVideo);
    void StartStudioWaveformWorker();
    void StopStudioWaveformWorker();
    void StartStudioRender();
    void PaintMontajLanding(void* gPtr, int cw, int ch);
    void PaintStudio(void* gPtr, int cw, int ch);
    bool HandleStudioPreTranslate(const MSG& msg);
    bool HandleStudioAppMessage(UINT msg, WPARAM wParam, LPARAM lParam, LRESULT& outRes);
    bool OnStudioLButtonDown(int mx, int my);
    bool OnStudioMouseMove(int mx, int my);
    bool OnStudioLButtonUp(int mx, int my);
    bool OnStudioMouseWheel(int mx, int my, int delta, bool ctrl);
    bool OnStudioButtonClick(int targetBtnId);

    bool uploadBusy_ = false;
    std::atomic<bool> uploadCancel_{false};
    std::thread uploadThread_;
    std::wstring uploadFilePath_;
    ULONGLONG copyFeedbackUntilMs_ = 0;

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
    HWND chkSpotifyAudio_ = nullptr;
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

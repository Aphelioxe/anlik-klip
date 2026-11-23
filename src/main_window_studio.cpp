#include "ui_gfx.h"
#include "main_window.h"
#include "clip_trimmer.h"
#include "com_ptr.h"

#include <mfapi.h>
#include <mfplay.h>
#include <mfreadwrite.h>
#include <mfobjects.h>
#include <propvarutil.h>
#include <shobjidl.h>

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace {

using ui::Gc;
using ui::FillRoundRect;
using ui::FillRoundRectGrad;
using ui::StrokeRoundRect;

struct StudioWaveDoneMsg {
    uint64_t gen = 0;
    bool isMusic = false;
    std::wstring path;
    std::vector<float> peaks;
};

struct StudioMpEventMsg {
    uint64_t gen = 0;
    bool isAudio = false;
    MFP_EVENT_TYPE type = MFP_EVENT_TYPE_PLAY;
};

std::wstring FormatSecTenths(double sec) {
    if (sec < 0.0) sec = 0.0;
    int totalTenths = static_cast<int>(std::lround(sec * 10.0));
    int mins = totalTenths / 600;
    int secs = (totalTenths / 10) % 60;
    int tenths = totalTenths % 10;
    wchar_t buf[32] = {};
    swprintf(buf, 32, L"%02d:%02d.%d", mins, secs, tenths);
    return buf;
}

std::wstring BaseNameNoExt(const std::wstring& path) {
    size_t sl = path.find_last_of(L"\\/");
    std::wstring fn = (sl != std::wstring::npos) ? path.substr(sl + 1) : path;
    size_t dot = fn.find_last_of(L'.');
    if (dot != std::wstring::npos && dot > 0) return fn.substr(0, dot);
    return fn;
}

// Select highest-energy audio track inside multi-track MP4 so preview matches rendered Mix
void SelectBestPreviewAudioTrack(IMFPMediaItem* item, const std::wstring& path) {
    if (!item) return;
    DWORD nStreams = 0;
    if (FAILED(item->GetNumberOfStreams(&nStreams)) || nStreams <= 1) return;
    std::vector<DWORD> audioStreams;
    for (DWORD si = 0; si < nStreams; ++si) {
        PROPVARIANT vt;
        PropVariantInit(&vt);
        if (SUCCEEDED(item->GetStreamAttribute(si, MF_MT_MAJOR_TYPE, &vt)) &&
            vt.vt == VT_CLSID && vt.puuid && *vt.puuid == MFMediaType_Audio) {
            audioStreams.push_back(si);
        }
        PropVariantClear(&vt);
    }
    if (audioStreams.size() <= 1) return;

    DWORD chosenAudio = audioStreams.front();
    ClipAudioTrackLayout layout;
    if (DetectClipAudioLayout(path, layout) && layout.mixStreamPos >= 0 &&
        static_cast<size_t>(layout.mixStreamPos) < audioStreams.size()) {
        chosenAudio = audioStreams[static_cast<size_t>(layout.mixStreamPos)];
    }
    for (DWORD asi : audioStreams) {
        item->SetStreamSelection(asi, asi == chosenAudio ? TRUE : FALSE);
    }
}

LRESULT CALLBACK StudioVideoHostProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    auto* mw = reinterpret_cast<MainWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (msg == WM_NCCREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
        return TRUE;
    }
    switch (msg) {
        case WM_ERASEBKGND:
            return 1;
        case WM_PAINT: {
            PAINTSTRUCT ps{};
            HDC hdc = BeginPaint(hwnd, &ps);
            RECT rc{};
            GetClientRect(hwnd, &rc);
            HBRUSH br = CreateSolidBrush(RGB(6, 8, 14));
            FillRect(hdc, &rc, br);
            DeleteObject(br);
            EndPaint(hwnd, &ps);
            return 0;
        }
        case WM_LBUTTONDOWN:
            if (mw) SendMessageW(mw->Hwnd(), WM_COMMAND, MAKEWPARAM(ui::BTN_STU_PLAYPAUSE, 0), 0);
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

struct StudioGeometry {
    int leftW = 0;
    int topH = 0;
    int timelineTop = 0;
    int toolY = 0;
    int toolRow1H = 0;
    int toolRow2Y = 0;
    int toolRow2H = 0;
    int toolH = 0;
    int rulerH = 0;
    int v1Top = 0, v1H = 0;
    int a1Top = 0, a1H = 0;
    int m1Top = 0, m1H = 0;
    int hdrW = 0;
    RECT stageRc{};
    RECT tlViewRc{};
    RECT rulerRc{};
    RECT gameSlRc{};
    RECT micSlRc{};
    RECT musSlRc{};
};

StudioGeometry CalcStudioGeom(int cw, int ch, int dpi) {
    auto S = [dpi](int px) { return MulDiv(px, dpi, 96); };
    StudioGeometry gm;
    gm.leftW = S(246);
    gm.topH = S(48);
    int tlH = std::max(S(204), std::min(S(248), (ch * 34) / 100));
    gm.timelineTop = std::max(S(280), ch - tlH);
    gm.toolRow1H = S(36);
    gm.toolRow2H = S(34);
    gm.toolH = gm.toolRow1H + gm.toolRow2H;
    gm.toolY = gm.timelineTop - gm.toolH;
    gm.toolRow2Y = gm.toolY + gm.toolRow1H;
    gm.rulerH = S(24);
    gm.hdrW = S(118);

    int tracksAvail = std::max(S(154), ch - gm.timelineTop - gm.rulerH - S(12));
    gm.v1H = std::max(S(50), (tracksAvail * 38) / 100);
    gm.a1H = std::max(S(42), (tracksAvail * 30) / 100);
    gm.m1H = std::max(S(46), tracksAvail - gm.v1H - gm.a1H - S(8));
    gm.v1Top = gm.timelineTop + gm.rulerH + S(4);
    gm.a1Top = gm.v1Top + gm.v1H + S(4);
    gm.m1Top = gm.a1Top + gm.a1H + S(4);

    gm.stageRc = RECT{ gm.leftW + S(10), gm.topH + S(6), cw - S(10), gm.toolY - S(6) };
    gm.tlViewRc = RECT{ gm.hdrW + S(8), gm.timelineTop, cw - S(6), ch - S(4) };
    gm.rulerRc = RECT{ gm.hdrW + S(8), gm.timelineTop, cw - S(6), gm.timelineTop + gm.rulerH };

    // Audio mixer sliders cleanly placed in dedicated Row 2 with zero overlap at 920px+ width
    int slTop = gm.toolRow2Y + S(9);
    int slBot = gm.toolRow2Y + S(25);
    int gx = gm.leftW + S(176);
    gm.gameSlRc = RECT{ gx, slTop, gx + S(116), slBot };
    int mx = gm.gameSlRc.right + S(114);
    gm.micSlRc = RECT{ mx, slTop, mx + S(116), slBot };
    gm.musSlRc = RECT{ gm.leftW + S(136), slTop, gm.leftW + S(276), slBot };
    return gm;
}

} // namespace

class MainWindow::StudioMpCallback : public IMFPMediaPlayerCallback {
public:
    StudioMpCallback(HWND notifyHwnd, uint64_t gen, bool isAudio)
        : notifyHwnd_(notifyHwnd), gen_(gen), isAudio_(isAudio) {}

    void Detach() { alive_.store(false); notifyHwnd_ = nullptr; }

    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (riid == __uuidof(IUnknown) || riid == __uuidof(IMFPMediaPlayerCallback)) {
            *ppv = static_cast<IMFPMediaPlayerCallback*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }

    STDMETHODIMP_(ULONG) AddRef() override { return static_cast<ULONG>(InterlockedIncrement(&ref_)); }
    STDMETHODIMP_(ULONG) Release() override {
        ULONG r = static_cast<ULONG>(InterlockedDecrement(&ref_));
        if (r == 0) delete this;
        return r;
    }

    void STDMETHODCALLTYPE OnMediaPlayerEvent(MFP_EVENT_HEADER* pEventHeader) override {
        if (!pEventHeader || !alive_.load() || !notifyHwnd_) return;
        auto* m = new StudioMpEventMsg{ gen_, isAudio_, pEventHeader->eEventType };
        if (!PostMessageW(notifyHwnd_, ui::WM_MW_STUDIO_VIDEO_EV, 0, reinterpret_cast<LPARAM>(m))) {
            delete m;
        }
    }

private:
    virtual ~StudioMpCallback() = default;
    volatile LONG ref_ = 1;
    HWND notifyHwnd_;
    uint64_t gen_;
    bool isAudio_;
    std::atomic<bool> alive_{true};
};

double MainWindow::StudioTotalDuration() const {
    double sum = 0.0;
    for (const auto& c : studioClips_) {
        sum += std::max(0.1, c.trimEndSec - c.trimStartSec) / std::max(0.5, c.speed);
    }
    return sum;
}

bool MainWindow::MapTimelineToClip(double tlSec, int& outIdx, double& outLocalSec, double& outClipStartSec) const {
    outIdx = -1;
    outLocalSec = 0.0;
    outClipStartSec = 0.0;
    if (studioClips_.empty()) return false;
    double acc = 0.0;
    for (size_t i = 0; i < studioClips_.size(); ++i) {
        const auto& c = studioClips_[i];
        double outDur = std::max(0.1, c.trimEndSec - c.trimStartSec) / std::max(0.5, c.speed);
        if (tlSec < acc + outDur - 1e-6 || i + 1 == studioClips_.size()) {
            double relOut = std::max(0.0, std::min(outDur, tlSec - acc));
            outIdx = static_cast<int>(i);
            outClipStartSec = acc;
            outLocalSec = std::max(c.trimStartSec, std::min(c.trimEndSec, c.trimStartSec + relOut * c.speed));
            return true;
        }
        acc += outDur;
    }
    return false;
}

void MainWindow::StudioFitZoomToView(int cw, int ch) {
    if (cw <= 0 || ch <= 0) return;
    const StudioGeometry gm = CalcStudioGeom(cw, ch, static_cast<int>(dpi_));
    double dur = std::max(4.0, StudioTotalDuration());
    double availW = std::max(120.0, static_cast<double>(gm.tlViewRc.right - gm.tlViewRc.left - S(44)));
    studioPxPerSec_ = std::max(8.0, std::min(160.0, availW / dur));
    studioTimelineScrollSec_ = 0.0;
}

bool MainWindow::StudioHitTestTrimEdge(int mx, int my, int cw, int ch,
                                       int& outKind, int& outIdx, int& outSide, double& outAccSec) const {
    outKind = 0;
    outIdx = -1;
    outSide = 0;
    outAccSec = 0.0;
    if (!studioOpen_ || trimBusy_ || cw <= 0 || ch <= 0) return false;
    const StudioGeometry gm = CalcStudioGeom(cw, ch, static_cast<int>(dpi_));
    auto S = [&](int px) { return MulDiv(px, static_cast<int>(dpi_), 96); };
    if (mx < gm.tlViewRc.left - S(10) || mx > gm.tlViewRc.right + S(14)) return false;

    auto secToX = [&](double s) {
        return static_cast<int>(std::lround(gm.tlViewRc.left + (s - studioTimelineScrollSec_) * studioPxPerSec_));
    };

    if (my >= gm.v1Top - S(2) && my <= gm.a1Top + gm.a1H && !studioClips_.empty()) {
        double acc = 0.0;
        for (size_t i = 0; i < studioClips_.size(); ++i) {
            const auto& c = studioClips_[i];
            double outDur = std::max(0.1, c.trimEndSec - c.trimStartSec) / std::max(0.5, c.speed);
            int x0 = secToX(acc);
            int x1 = secToX(acc + outDur);
            int visX1 = std::max(x0 + S(16), x1);
            int w = visX1 - x0;
            int handleW = std::min<int>(S(11), std::max<int>(S(6), w / 3));
            int leftMin = (i == 0) ? (x0 - S(9)) : x0;
            int leftMax = std::min(x0 + handleW, (x0 + visX1) / 2);
            int rightMin = std::max(visX1 - handleW, (x0 + visX1) / 2 + 1);
            int rightMax = (i + 1 == studioClips_.size()) ? (visX1 + S(12)) : (visX1 - 1);
            if (x0 >= gm.tlViewRc.left - S(10) && x0 <= gm.tlViewRc.right + S(10) &&
                mx >= leftMin && mx <= leftMax) {
                outKind = 1;
                outIdx = static_cast<int>(i);
                outSide = -1;
                outAccSec = acc;
                return true;
            }
            if (visX1 >= gm.tlViewRc.left - S(8) && visX1 <= gm.tlViewRc.right + S(14) &&
                mx >= rightMin && mx <= rightMax) {
                outKind = 1;
                outIdx = static_cast<int>(i);
                outSide = +1;
                outAccSec = acc;
                return true;
            }
            acc += outDur;
        }
    }

    if (my >= gm.m1Top && my <= gm.m1Top + gm.m1H && !studioMusic_.empty()) {
        for (size_t i = 0; i < studioMusic_.size(); ++i) {
            const auto& m = studioMusic_[i];
            int x0 = secToX(m.timelineStartSec);
            int x1 = secToX(m.timelineStartSec + m.durationSec);
            int w = x1 - x0;
            int handleW = (w >= S(16)) ? std::min<int>(S(10), std::max<int>(S(5), w / 3)) : 0;
            if (handleW > 0) {
                if (mx >= x0 - S(4) && mx <= x0 + handleW) {
                    outKind = 2;
                    outIdx = static_cast<int>(i);
                    outSide = -1;
                    outAccSec = m.timelineStartSec;
                    return true;
                }
                if (mx >= x1 - handleW && mx <= x1 + S(4)) {
                    outKind = 2;
                    outIdx = static_cast<int>(i);
                    outSide = +1;
                    outAccSec = m.timelineStartSec;
                    return true;
                }
            }
        }
    }
    return false;
}

void MainWindow::StudioTrimClipEdgeToPlayhead(bool leftEdge) {
    if (trimBusy_ || studioClips_.empty()) return;
    int idx = -1;
    double localSec = 0.0, clipStartOut = 0.0;
    if (!MapTimelineToClip(studioPlayheadSec_, idx, localSec, clipStartOut) || idx < 0) return;
    auto& c = studioClips_[static_cast<size_t>(idx)];
    if (leftEdge) {
        if (localSec <= c.trimStartSec + 0.05 || localSec >= c.trimEndSec - 0.25) {
            SetFooterMessage(L"Baştan kesmek için imleci klibin biraz sağına getirin.");
            if (hwnd_) InvalidateRect(hwnd_, nullptr, FALSE);
            return;
        }
        c.trimStartSec = localSec;
        studioPlayheadSec_ = clipStartOut;
    } else {
        if (localSec >= c.trimEndSec - 0.05 || localSec <= c.trimStartSec + 0.25) {
            SetFooterMessage(L"Sondan kesmek için imleci klibin biraz soluna getirin.");
            if (hwnd_) InvalidateRect(hwnd_, nullptr, FALSE);
            return;
        }
        c.trimEndSec = localSec;
        double outDur = std::max(0.1, c.trimEndSec - c.trimStartSec) / std::max(0.5, c.speed);
        studioPlayheadSec_ = clipStartOut + std::max(0.02, outDur - 0.04);
    }
    studioSelClip_ = idx;
    studioSelMusic_ = -1;
    studioLoadedClipIdx_ = -1;
    SyncStudioMediaState(true);
    if (hwnd_) InvalidateRect(hwnd_, nullptr, FALSE);
}

void MainWindow::OpenStudio(bool loadQueued) {
    if (!hwnd_ || studioOpen_) return;
    if (playerOpen_) CloseEmbeddedPlayer();
    if (edtSearch_) ShowWindow(edtSearch_, SW_HIDE);
    for (HWND h : settingsCtrls_) ShowWindow(h, SW_HIDE);

    studioOpen_ = true;
    studioLeftTab_ = 0;
    studioFade_ = montageFade_;
    studioVertical916_ = montageVertical916_;
    studioTargetMb_ = 0;
    studioPlayheadSec_ = 0.0;
    studioTimelineScrollSec_ = 0.0;
    studioPlaying_ = false;
    studioVideoReady_ = false;
    studioAudioReady_ = false;
    studioSeekPending_ = false;
    studioLoadedClipIdx_ = -1;
    studioLoadedClipPath_.clear();
    studioLoadedMusicPath_.clear();
    studioPendingSeekSec_ = -1.0;
    studioPendingAudioSeekSec_ = -1.0;
    studioDragMode_ = 0;
    studioHoverClipIdx_ = -1;
    studioHoverClipSide_ = 0;
    studioHoverMusicIdx_ = -1;
    studioHoverMusicSide_ = 0;

    if (loadQueued && studioClips_.empty() && !montagePaths_.empty()) {
        for (const auto& mp : montagePaths_) {
            StudioClipItem item;
            item.path = mp;
            item.name = BaseNameNoExt(mp);
            for (const auto& c : clips_) {
                if (_wcsicmp(c.filePath.c_str(), mp.c_str()) == 0) {
                    item.sourceDurSec = c.durationSeconds;
                    break;
                }
            }
            bool multi = false;
            double probedDur = 0.0;
            ProbeClipAudioTracks(mp, multi, &probedDur);
            if (probedDur > 0.2) item.sourceDurSec = probedDur;
            if (item.sourceDurSec <= 0.2) continue;
            item.hasMultiTrack = multi;
            item.trimStartSec = 0.0;
            item.trimEndSec = item.sourceDurSec;
            item.speed = std::max(0.5, std::min(2.0, montageSpeed_));
            item.gameVolPct = montageGameMuted_ ? 0 : 100;
            item.micVolPct = montageMicMuted_ ? 0 : 100;
            studioClips_.push_back(std::move(item));
        }
    }
    if (!studioClips_.empty() && studioSelClip_ < 0) {
        studioSelClip_ = 0;
        studioSelMusic_ = -1;
    }

    static bool s_reg = false;
    const wchar_t* vCls = L"AnlikKlipStudioVideo";
    if (!s_reg) {
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = StudioVideoHostProc;
        wc.hInstance = hInst_;
        wc.lpszClassName = vCls;
        wc.hCursor = LoadCursorW(nullptr, IDC_HAND);
        RegisterClassExW(&wc);
        s_reg = true;
    }

    studioVideoHwnd_ = CreateWindowExW(0, vCls, L"", WS_CHILD | WS_CLIPSIBLINGS,
        0, 0, 160, 90, hwnd_, nullptr, hInst_, this);

    RECT cli{};
    GetClientRect(hwnd_, &cli);
    if (!studioClips_.empty()) {
        StudioFitZoomToView(cli.right, cli.bottom);
    }
    UpdateStudioLayout(cli.right, cli.bottom);
    StartStudioWaveformWorker();
    if (!studioClips_.empty()) {
        SyncStudioMediaState(true);
    }
    SetTimer(hwnd_, ui::TIMER_PLAYER_ID, 60, nullptr);
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void MainWindow::CloseStudio() {
    StopStudioWaveformWorker();
    if (hwnd_) KillTimer(hwnd_, ui::TIMER_PLAYER_ID);
    ++studioGen_;

    if (studioVideoCb_) {
        studioVideoCb_->Detach();
        studioVideoCb_->Release();
        studioVideoCb_ = nullptr;
    }
    if (studioPlayer_) {
        studioPlayer_->Shutdown();
        studioPlayer_->Release();
        studioPlayer_ = nullptr;
    }
    if (studioAudioCb_) {
        studioAudioCb_->Detach();
        studioAudioCb_->Release();
        studioAudioCb_ = nullptr;
    }
    if (studioMusicPlayer_) {
        studioMusicPlayer_->Shutdown();
        studioMusicPlayer_->Release();
        studioMusicPlayer_ = nullptr;
    }
    if (studioVideoHwnd_) {
        DestroyWindow(studioVideoHwnd_);
        studioVideoHwnd_ = nullptr;
    }

    studioOpen_ = false;
    studioPlaying_ = false;
    studioVideoReady_ = false;
    studioAudioReady_ = false;
    studioSeekPending_ = false;
    studioLoadedClipIdx_ = -1;
    studioLoadedClipPath_.clear();
    studioLoadedMusicPath_.clear();
    studioDragMode_ = 0;
    studioHoverClipIdx_ = -1;
    studioHoverClipSide_ = 0;
    studioHoverMusicIdx_ = -1;
    studioHoverMusicSide_ = 0;
    // Free cached waveform peaks when leaving studio so idle RAM returns near baseline
    for (auto& c : studioClips_) { c.waveLoaded = false; std::vector<float>().swap(c.wavePeaks); }
    for (auto& m : studioMusic_) { m.waveLoaded = false; std::vector<float>().swap(m.wavePeaks); }

    UpdateGalleryControlVisibility();
    if (hwnd_) InvalidateRect(hwnd_, nullptr, FALSE);
}

void MainWindow::UpdateStudioLayout(int cw, int ch) {
    if (!studioOpen_ || !studioVideoHwnd_ || cw <= 0 || ch <= 0) return;
    if (studioClips_.empty() || trimBusy_) {
        ShowWindow(studioVideoHwnd_, SW_HIDE);
        return;
    }
    StudioGeometry gm = CalcStudioGeom(cw, ch, static_cast<int>(dpi_));
    int availW = std::max(S(240), static_cast<int>(gm.stageRc.right - gm.stageRc.left) - S(16));
    int availH = std::max(S(140), static_cast<int>(gm.stageRc.bottom - gm.stageRc.top) - S(12));

    int vw = availW, vh = availH;
    if (studioVertical916_) {
        vh = availH;
        vw = (vh * 9) / 16;
        if (vw > availW) { vw = availW; vh = (vw * 16) / 9; }
    } else {
        vw = availW;
        vh = (vw * 9) / 16;
        if (vh > availH) { vh = availH; vw = (vh * 16) / 9; }
    }
    int vx = gm.stageRc.left + (static_cast<int>(gm.stageRc.right - gm.stageRc.left) - vw) / 2;
    int vy = gm.stageRc.top + (static_cast<int>(gm.stageRc.bottom - gm.stageRc.top) - vh) / 2;
    ShowWindow(studioVideoHwnd_, SW_SHOW);
    SetWindowPos(studioVideoHwnd_, nullptr, vx, vy, vw, vh, SWP_NOZORDER | SWP_NOACTIVATE);
    if (studioPlayer_) studioPlayer_->UpdateVideo();
}

void MainWindow::StopStudioWaveformWorker() {
    studioWaveStop_.store(true);
    if (studioWaveThread_.joinable()) {
        studioWaveThread_.join();
    }
    studioWaveStop_.store(false);
}

void MainWindow::StartStudioWaveformWorker() {
    StopStudioWaveformWorker();
    struct Task { bool isMusic; std::wstring path; };
    std::vector<Task> jobs;
    auto pushUnique = [&](bool isMus, const std::wstring& p) {
        for (const auto& j : jobs) {
            if (j.isMusic == isMus && _wcsicmp(j.path.c_str(), p.c_str()) == 0) return;
        }
        jobs.push_back({ isMus, p });
    };
    for (const auto& c : studioClips_) {
        if (!c.waveLoaded && !c.path.empty()) pushUnique(false, c.path);
    }
    for (const auto& m : studioMusic_) {
        if (!m.waveLoaded && !m.path.empty()) pushUnique(true, m.path);
    }
    if (jobs.empty() || !hwnd_) return;

    const uint64_t gen = studioGen_.load();
    HWND target = hwnd_;
    studioWaveThread_ = std::thread([this, target, gen, jobs = std::move(jobs)]() {
        ComScope com(COINIT_MULTITHREADED);
        for (const auto& job : jobs) {
            if (studioWaveStop_.load() || studioGen_.load() != gen) break;
            std::vector<float> peaks;
            if (ExtractAudioWaveform(job.path, 0, 140, peaks) && !peaks.empty()) {
                auto* msg = new StudioWaveDoneMsg{ gen, job.isMusic, job.path, std::move(peaks) };
                if (!PostMessageW(target, ui::WM_MW_STUDIO_WAVE_READY, 0, reinterpret_cast<LPARAM>(msg))) {
                    delete msg;
                }
            }
        }
    });
}

void MainWindow::SyncStudioMediaState(bool forceSeekVideo) {
    if (!studioOpen_ || trimBusy_ || !studioVideoHwnd_) return;
    if (studioClips_.empty()) {
        if (studioPlayer_) studioPlayer_->Pause();
        if (studioMusicPlayer_) studioMusicPlayer_->Pause();
        studioPlaying_ = false;
        ShowWindow(studioVideoHwnd_, SW_HIDE);
        return;
    }

    int segIdx = -1;
    double localSec = 0.0, clipStartOut = 0.0;
    if (!MapTimelineToClip(studioPlayheadSec_, segIdx, localSec, clipStartOut) || segIdx < 0) return;

    const auto& clip = studioClips_[static_cast<size_t>(segIdx)];
    ShowWindow(studioVideoHwnd_, SW_SHOW);

    if (!studioPlayer_) {
        studioVideoCb_ = new StudioMpCallback(hwnd_, studioGen_.load(), false);
        HRESULT hr = MFPCreateMediaPlayer(nullptr, FALSE, MFP_OPTION_NONE,
            studioVideoCb_, studioVideoHwnd_, &studioPlayer_);
        if (FAILED(hr) || !studioPlayer_) return;
    }

    const bool needNewItem = (_wcsicmp(studioLoadedClipPath_.c_str(), clip.path.c_str()) != 0);
    float previewVol = std::max(0.0f, std::min(1.0f,
        static_cast<float>(clip.hasMultiTrack ? std::max(clip.gameVolPct, clip.micVolPct) : clip.gameVolPct) / 100.0f));
    bool previewMute = (clip.gameVolPct == 0 && (!clip.hasMultiTrack || clip.micVolPct == 0));

    if (needNewItem) {
        studioVideoReady_ = false;
        studioSeekPending_ = true;
        studioIgnoreEndUntilMs_ = GetTickCount64() + 700;
        studioLoadedClipIdx_ = segIdx;
        studioLoadedClipPath_ = clip.path;
        studioPendingSeekSec_ = localSec;
        IMFPMediaItem* item = nullptr;
        if (SUCCEEDED(studioPlayer_->CreateMediaItemFromURL(clip.path.c_str(), TRUE, 0, &item)) && item) {
            SelectBestPreviewAudioTrack(item, clip.path);
            studioPlayer_->SetMediaItem(item);
            item->Release();
        } else {
            studioSeekPending_ = false;
        }
    } else if (studioVideoReady_) {
        studioLoadedClipIdx_ = segIdx;
        studioPlayer_->SetVolume(previewVol);
        studioPlayer_->SetMute(previewMute ? TRUE : FALSE);
        studioPlayer_->SetRate(static_cast<float>(clip.speed));
        if (forceSeekVideo) {
            PROPVARIANT pos;
            PropVariantInit(&pos);
            pos.vt = VT_I8;
            pos.hVal.QuadPart = static_cast<LONGLONG>(localSec * 1e7);
            studioSeekPending_ = true;
            studioIgnoreEndUntilMs_ = GetTickCount64() + 550;
            if (FAILED(studioPlayer_->SetPosition(MFP_POSITIONTYPE_100NS, &pos))) {
                studioSeekPending_ = false;
            }
            PropVariantClear(&pos);
        }
        if (studioPlaying_) studioPlayer_->Play();
        else studioPlayer_->Pause();
    }

    // Sync secondary audio-only MP3 preview player (created only when music exists)
    const StudioMusicItem* activeMus = nullptr;
    for (const auto& m : studioMusic_) {
        if (studioPlayheadSec_ >= m.timelineStartSec && studioPlayheadSec_ < m.timelineStartSec + m.durationSec - 0.04) {
            activeMus = &m;
            break;
        }
    }
    if (studioPlaying_ && activeMus && activeMus->volPct > 0) {
        if (!studioMusicPlayer_) {
            studioAudioCb_ = new StudioMpCallback(hwnd_, studioGen_.load(), true);
            MFPCreateMediaPlayer(nullptr, FALSE, MFP_OPTION_NONE, studioAudioCb_, nullptr, &studioMusicPlayer_);
        }
        if (studioMusicPlayer_) {
            double targetASec = activeMus->trimStartSec + (studioPlayheadSec_ - activeMus->timelineStartSec);
            float musVol = std::max(0.0f, std::min(1.0f, static_cast<float>(activeMus->volPct) / 100.0f));
            if (_wcsicmp(studioLoadedMusicPath_.c_str(), activeMus->path.c_str()) != 0) {
                studioAudioReady_ = false;
                studioLoadedMusicPath_ = activeMus->path;
                studioPendingAudioSeekSec_ = targetASec;
                IMFPMediaItem* aItem = nullptr;
                if (SUCCEEDED(studioMusicPlayer_->CreateMediaItemFromURL(activeMus->path.c_str(), TRUE, 0, &aItem)) && aItem) {
                    studioMusicPlayer_->SetMediaItem(aItem);
                    aItem->Release();
                }
            } else if (studioAudioReady_) {
                studioMusicPlayer_->SetVolume(musVol);
                studioMusicPlayer_->SetMute(FALSE);
                PROPVARIANT aPos;
                PropVariantInit(&aPos);
                double curASec = targetASec;
                if (SUCCEEDED(studioMusicPlayer_->GetPosition(MFP_POSITIONTYPE_100NS, &aPos)) && aPos.vt == VT_I8) {
                    curASec = static_cast<double>(aPos.hVal.QuadPart) / 1e7;
                }
                PropVariantClear(&aPos);
                if (forceSeekVideo || std::abs(curASec - targetASec) > 0.40) {
                    PROPVARIANT np;
                    PropVariantInit(&np);
                    np.vt = VT_I8;
                    np.hVal.QuadPart = static_cast<LONGLONG>(targetASec * 1e7);
                    studioMusicPlayer_->SetPosition(MFP_POSITIONTYPE_100NS, &np);
                    PropVariantClear(&np);
                }
                studioMusicPlayer_->Play();
            }
        }
    } else if (studioMusicPlayer_ && studioAudioReady_) {
        studioMusicPlayer_->Pause();
    }
}

void MainWindow::UpdateStudioMetrics() {
    if (!studioOpen_ || trimBusy_ || !studioPlaying_) return;
    if (studioClips_.empty()) {
        studioPlaying_ = false;
        return;
    }
    if (!studioPlayer_ || !studioVideoReady_) return;

    int curIdx = studioLoadedClipIdx_;
    if (curIdx < 0 || curIdx >= static_cast<int>(studioClips_.size())) {
        double tmpL = 0.0, tmpC = 0.0;
        MapTimelineToClip(studioPlayheadSec_, curIdx, tmpL, tmpC);
    }
    if (curIdx < 0 || curIdx >= static_cast<int>(studioClips_.size())) return;

    const auto& clip = studioClips_[static_cast<size_t>(curIdx)];
    if (studioSeekPending_) {
        if (GetTickCount64() < studioIgnoreEndUntilMs_) {
            if (hwnd_) InvalidateRect(hwnd_, nullptr, FALSE);
            return;
        }
        studioSeekPending_ = false;
    }

    double localPos = clip.trimStartSec;
    PROPVARIANT pos;
    PropVariantInit(&pos);
    if (SUCCEEDED(studioPlayer_->GetPosition(MFP_POSITIONTYPE_100NS, &pos)) && pos.vt == VT_I8) {
        localPos = static_cast<double>(pos.hVal.QuadPart) / 1e7;
    }
    PropVariantClear(&pos);

    double clipStartOut = 0.0;
    for (int i = 0; i < curIdx; ++i) {
        const auto& c = studioClips_[static_cast<size_t>(i)];
        clipStartOut += std::max(0.1, c.trimEndSec - c.trimStartSec) / std::max(0.5, c.speed);
    }

    if (GetTickCount64() >= studioIgnoreEndUntilMs_ && localPos >= clip.trimEndSec - 0.04) {
        double nextTl = clipStartOut + std::max(0.1, clip.trimEndSec - clip.trimStartSec) / std::max(0.5, clip.speed);
        if (curIdx + 1 < static_cast<int>(studioClips_.size())) {
            // If current clip finished near container EOF, force fresh MediaItem so same-file next clips start cleanly at 0s
            if (clip.trimEndSec >= clip.sourceDurSec - 0.15) {
                studioLoadedClipPath_.clear();
            }
            studioPlayheadSec_ = nextTl + 0.001;
            SyncStudioMediaState(true);
        } else {
            studioPlayheadSec_ = StudioTotalDuration();
            studioPlaying_ = false;
            studioPlayer_->Pause();
            if (studioMusicPlayer_) studioMusicPlayer_->Pause();
        }
    } else if (GetTickCount64() >= studioIgnoreEndUntilMs_) {
        double relLocal = std::max(0.0, std::min(clip.trimEndSec - clip.trimStartSec, localPos - clip.trimStartSec));
        studioPlayheadSec_ = clipStartOut + relLocal / std::max(0.5, clip.speed);
        SyncStudioMediaState(false);
    } else {
        SyncStudioMediaState(false);
    }

    // Auto-scroll timeline to keep playhead visible
    if (hwnd_) {
        RECT cli{};
        GetClientRect(hwnd_, &cli);
        StudioGeometry gm = CalcStudioGeom(cli.right, cli.bottom, static_cast<int>(dpi_));
        double visSec = std::max(1.0, static_cast<double>(gm.tlViewRc.right - gm.tlViewRc.left) / studioPxPerSec_);
        if (studioPlayheadSec_ > studioTimelineScrollSec_ + visSec * 0.92) {
            studioTimelineScrollSec_ = studioPlayheadSec_ - visSec * 0.25;
        } else if (studioPlayheadSec_ < studioTimelineScrollSec_) {
            studioTimelineScrollSec_ = std::max(0.0, studioPlayheadSec_ - 0.5);
        }
        InvalidateRect(hwnd_, nullptr, FALSE);
    }
}

void MainWindow::StudioAddClipFromLibrary(size_t rawClipIdx) {
    if (trimBusy_ || rawClipIdx >= clips_.size()) return;
    const auto& src = clips_[rawClipIdx];
    StudioClipItem item;
    item.path = src.filePath;
    item.name = BaseNameNoExt(src.fileName);
    item.sourceDurSec = src.durationSeconds;
    bool multi = false;
    double probedDur = 0.0;
    ProbeClipAudioTracks(src.filePath, multi, &probedDur);
    if (probedDur > 0.2) item.sourceDurSec = probedDur;
    if (item.sourceDurSec <= 0.2) return;
    item.hasMultiTrack = multi;
    item.trimStartSec = 0.0;
    item.trimEndSec = item.sourceDurSec;
    item.speed = 1.0;
    item.gameVolPct = 100;
    item.micVolPct = 100;
    for (const auto& existing : studioClips_) {
        if (existing.waveLoaded && _wcsicmp(existing.path.c_str(), src.filePath.c_str()) == 0) {
            item.waveLoaded = true;
            item.wavePeaks = existing.wavePeaks;
            break;
        }
    }

    studioClips_.push_back(std::move(item));
    studioSelClip_ = static_cast<int>(studioClips_.size()) - 1;
    studioSelMusic_ = -1;

    RECT cli{};
    if (hwnd_) GetClientRect(hwnd_, &cli);
    UpdateStudioLayout(cli.right, cli.bottom);
    StartStudioWaveformWorker();
    if (cli.right > 0 && cli.bottom > 0) {
        const StudioGeometry gm = CalcStudioGeom(cli.right, cli.bottom, static_cast<int>(dpi_));
        double visSec = std::max(1.0, static_cast<double>(gm.tlViewRc.right - gm.tlViewRc.left - S(44)) / studioPxPerSec_);
        if (studioClips_.size() == 1 || StudioTotalDuration() > studioTimelineScrollSec_ + visSec) {
            StudioFitZoomToView(cli.right, cli.bottom);
        }
    }
    if (studioClips_.size() == 1) {
        studioPlayheadSec_ = 0.0;
        SyncStudioMediaState(true);
    }
    if (hwnd_) InvalidateRect(hwnd_, nullptr, FALSE);
}

void MainWindow::StudioAddMusicFile(const std::wstring& givenPath) {
    if (trimBusy_) return;
    std::wstring chosen = givenPath;
    if (chosen.empty()) {
        ComPtr<IFileOpenDialog> dlg;
        if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_ALL, IID_PPV_ARGS(dlg.Put()))) || !dlg) return;
        COMDLG_FILTERSPEC spec[] = {
            { L"Ses Dosyaları (*.mp3;*.wav;*.m4a;*.aac;*.flac)", L"*.mp3;*.wav;*.m4a;*.aac;*.flac" },
            { L"Tüm Dosyalar (*.*)", L"*.*" }
        };
        dlg->SetFileTypes(2, spec);
        dlg->SetTitle(L"Zaman Çizelgesine Müzik / MP3 Ekle");
        if (SUCCEEDED(dlg->Show(hwnd_))) {
            ComPtr<IShellItem> item;
            if (SUCCEEDED(dlg->GetResult(item.Put())) && item) {
                PWSTR psz = nullptr;
                if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &psz)) && psz) {
                    chosen = psz;
                    CoTaskMemFree(psz);
                }
            }
        }
    }
    if (chosen.empty()) return;

    double dur = 0.0;
    if (!ProbeAudioFileDuration(chosen, dur) || dur <= 0.2) {
        SetFooterMessage(L"Seçilen müzik dosyası okunamadı.");
        if (hwnd_) InvalidateRect(hwnd_, nullptr, FALSE);
        return;
    }

    StudioMusicItem mus;
    mus.path = chosen;
    mus.name = BaseNameNoExt(chosen);
    mus.sourceDurSec = dur;
    mus.timelineStartSec = studioPlayheadSec_;
    mus.trimStartSec = 0.0;
    double vidDur = StudioTotalDuration();
    mus.durationSec = (vidDur > 1.0) ? std::min(dur, std::max(2.0, vidDur - mus.timelineStartSec)) : std::min(dur, 30.0);
    mus.volPct = 85;
    mus.fadeIn = true;
    mus.fadeOut = true;

    studioMusic_.push_back(std::move(mus));
    studioSelMusic_ = static_cast<int>(studioMusic_.size()) - 1;
    studioSelClip_ = -1;

    bool seen = false;
    for (const auto& r : studioRecentAudio_) {
        if (_wcsicmp(r.c_str(), chosen.c_str()) == 0) { seen = true; break; }
    }
    if (!seen) studioRecentAudio_.push_back(chosen);

    StartStudioWaveformWorker();
    SetFooterMessage(L"Müzik eklendi: Çubuğu sürükleyip istediğiniz saniyeye yerleştirebilirsiniz.");
    if (hwnd_) InvalidateRect(hwnd_, nullptr, FALSE);
}

void MainWindow::StudioSplitAtPlayhead() {
    if (trimBusy_ || studioClips_.empty()) return;
    int idx = -1;
    double localSec = 0.0, clipStartOut = 0.0;
    if (!MapTimelineToClip(studioPlayheadSec_, idx, localSec, clipStartOut) || idx < 0) return;

    StudioClipItem orig = studioClips_[static_cast<size_t>(idx)];
    if (localSec <= orig.trimStartSec + 0.25 || localSec >= orig.trimEndSec - 0.25) {
        SetFooterMessage(L"Bölmek için oynatma imlecini klibin ortalarına getirin.");
        if (hwnd_) InvalidateRect(hwnd_, nullptr, FALSE);
        return;
    }

    StudioClipItem rightPart = orig;
    orig.trimEndSec = localSec;
    rightPart.trimStartSec = localSec;
    studioClips_[static_cast<size_t>(idx)] = orig;
    studioClips_.insert(studioClips_.begin() + idx + 1, std::move(rightPart));
    studioSelClip_ = idx + 1;
    studioSelMusic_ = -1;
    studioLoadedClipIdx_ = -1;
    studioHoverClipIdx_ = -1;
    studioHoverClipSide_ = 0;
    if (hwnd_) InvalidateRect(hwnd_, nullptr, FALSE);
}

void MainWindow::StudioDeleteSelection() {
    if (trimBusy_) return;
    studioHoverClipIdx_ = -1;
    studioHoverClipSide_ = 0;
    studioHoverMusicIdx_ = -1;
    studioHoverMusicSide_ = 0;
    if (studioSelMusic_ >= 0 && studioSelMusic_ < static_cast<int>(studioMusic_.size())) {
        studioMusic_.erase(studioMusic_.begin() + studioSelMusic_);
        if (studioMusicPlayer_) studioMusicPlayer_->Pause();
        studioLoadedMusicPath_.clear();
        studioSelMusic_ = studioMusic_.empty() ? -1 : std::min(studioSelMusic_, static_cast<int>(studioMusic_.size()) - 1);
        if (studioSelMusic_ < 0 && !studioClips_.empty()) studioSelClip_ = 0;
    } else if (studioSelClip_ >= 0 && studioSelClip_ < static_cast<int>(studioClips_.size())) {
        studioClips_.erase(studioClips_.begin() + studioSelClip_);
        studioLoadedClipIdx_ = -1;
        studioLoadedClipPath_.clear();
        double tot = StudioTotalDuration();
        if (studioPlayheadSec_ > tot) studioPlayheadSec_ = tot;
        studioSelClip_ = studioClips_.empty() ? -1 : std::min(studioSelClip_, static_cast<int>(studioClips_.size()) - 1);
        RECT cli{};
        if (hwnd_) GetClientRect(hwnd_, &cli);
        UpdateStudioLayout(cli.right, cli.bottom);
        SyncStudioMediaState(true);
    }
    if (hwnd_) InvalidateRect(hwnd_, nullptr, FALSE);
}

void MainWindow::StudioMoveSelectedClip(int dir) {
    if (trimBusy_ || studioSelClip_ < 0 || studioSelClip_ >= static_cast<int>(studioClips_.size())) return;
    int next = studioSelClip_ + dir;
    if (next < 0 || next >= static_cast<int>(studioClips_.size())) return;
    std::swap(studioClips_[static_cast<size_t>(studioSelClip_)], studioClips_[static_cast<size_t>(next)]);
    studioSelClip_ = next;
    studioLoadedClipIdx_ = -1;
    SyncStudioMediaState(true);
    if (hwnd_) InvalidateRect(hwnd_, nullptr, FALSE);
}

void MainWindow::StudioCycleClipSpeed() {
    if (trimBusy_ || studioSelClip_ < 0 || studioSelClip_ >= static_cast<int>(studioClips_.size())) return;
    auto& c = studioClips_[static_cast<size_t>(studioSelClip_)];
    if (c.speed < 0.75) c.speed = 1.0;
    else if (c.speed < 1.25) c.speed = 1.5;
    else if (c.speed < 1.75) c.speed = 2.0;
    else c.speed = 0.5;
    studioPlayheadSec_ = std::min(studioPlayheadSec_, StudioTotalDuration());
    SyncStudioMediaState(false);
    if (hwnd_) InvalidateRect(hwnd_, nullptr, FALSE);
}

void MainWindow::StudioTogglePlayPause() {
    if (!studioOpen_ || trimBusy_ || studioClips_.empty()) return;
    studioPlaying_ = !studioPlaying_;
    if (studioPlaying_) {
        double tot = StudioTotalDuration();
        if (studioPlayheadSec_ >= tot - 0.08) studioPlayheadSec_ = 0.0;
        SyncStudioMediaState(true);
    } else {
        if (studioPlayer_) studioPlayer_->Pause();
        if (studioMusicPlayer_) studioMusicPlayer_->Pause();
    }
    if (hwnd_) InvalidateRect(hwnd_, nullptr, FALSE);
}

void MainWindow::StudioSeekTimeline(double targetSec, bool syncAudio) {
    double tot = StudioTotalDuration();
    studioPlayheadSec_ = std::max(0.0, std::min(tot, targetSec));
    if (syncAudio) SyncStudioMediaState(true);
    if (hwnd_) InvalidateRect(hwnd_, nullptr, FALSE);
}

void MainWindow::StartStudioRender() {
    if (trimBusy_ || studioClips_.empty()) return;
    if (trimThread_.joinable()) trimThread_.join();

    // Release preview players cleanly so source files have zero file lock during encode
    studioPlaying_ = false;
    ++studioGen_;
    if (studioVideoCb_) { studioVideoCb_->Detach(); studioVideoCb_->Release(); studioVideoCb_ = nullptr; }
    if (studioPlayer_) { studioPlayer_->Shutdown(); studioPlayer_->Release(); studioPlayer_ = nullptr; }
    if (studioAudioCb_) { studioAudioCb_->Detach(); studioAudioCb_->Release(); studioAudioCb_ = nullptr; }
    if (studioMusicPlayer_) { studioMusicPlayer_->Shutdown(); studioMusicPlayer_->Release(); studioMusicPlayer_ = nullptr; }
    studioVideoReady_ = false;
    studioAudioReady_ = false;
    studioLoadedClipPath_.clear();
    studioLoadedMusicPath_.clear();
    if (studioVideoHwnd_) ShowWindow(studioVideoHwnd_, SW_HIDE);

    trimCancel_.store(false);
    trimBusy_ = true;

    std::wstring outDir = app_.Config().outputDir;
    if (outDir.empty()) {
        size_t sl = studioClips_[0].path.find_last_of(L"\\/");
        if (sl != std::wstring::npos) outDir = studioClips_[0].path.substr(0, sl);
    }
    const std::wstring outPath = BuildMontageOutputPath(outDir);

    ClipEditSpec editSpec;
    editSpec.dstPath = outPath;
    editSpec.fadeTransitions = studioFade_;
    editSpec.vertical916 = studioVertical916_;
    editSpec.targetSizeMb = studioTargetMb_;
    for (const auto& c : studioClips_) {
        MontageSegmentSpec ms;
        ms.path = c.path;
        ms.startSec = c.trimStartSec;
        ms.endSec = c.trimEndSec;
        ms.speed = c.speed;
        ms.gameVolPct = c.gameVolPct;
        ms.micVolPct = c.micVolPct;
        editSpec.montageSegments.push_back(std::move(ms));
    }
    for (const auto& m : studioMusic_) {
        MontageMusicTrackSpec mt;
        mt.path = m.path;
        mt.timelineStartSec = m.timelineStartSec;
        mt.trimStartSec = m.trimStartSec;
        mt.durationSec = m.durationSec;
        mt.volPct = m.volPct;
        mt.fadeIn = m.fadeIn;
        mt.fadeOut = m.fadeOut;
        editSpec.musicTracks.push_back(std::move(mt));
    }

    SetFooterMessage(L"Montaj render alınıyor, lütfen bekleyin...");
    HWND target = hwnd_;
    trimThread_ = std::thread([this, target, outPath, editSpec]() {
        ComScope com(COINIT_MULTITHREADED);
        std::string errUtf8;
        bool ok = EditMp4Clip(editSpec, errUtf8, &trimCancel_);
        auto* msg = new ui::TrimDoneMsg{};
        if (trimCancel_.load()) {
            DeleteFileW(outPath.c_str());
            msg->ok = false;
            msg->message = L"Render iptal edildi.";
        } else {
            msg->ok = ok;
            msg->replaceOriginal = false;
            msg->origPath = editSpec.montageSegments.empty() ? L"" : editSpec.montageSegments[0].path;
            msg->outPath = outPath;
            if (!ok) {
                int wlen = MultiByteToWideChar(CP_UTF8, 0, errUtf8.c_str(), -1, nullptr, 0);
                std::wstring werr(wlen > 0 ? wlen - 1 : 0, L'\0');
                if (wlen > 1) MultiByteToWideChar(CP_UTF8, 0, errUtf8.c_str(), -1, &werr[0], wlen);
                msg->message = L"Render başarısız: " + werr;
            }
        }
        if (!PostMessageW(target, ui::WM_MW_TRIM_DONE, 0, reinterpret_cast<LPARAM>(msg))) {
            delete msg;
        }
    });
    if (hwnd_) InvalidateRect(hwnd_, nullptr, FALSE);
}

bool MainWindow::HandleStudioAppMessage(UINT msg, WPARAM /*wParam*/, LPARAM lParam, LRESULT& outRes) {
    if (msg == ui::WM_MW_STUDIO_VIDEO_EV) {
        auto* ev = reinterpret_cast<StudioMpEventMsg*>(lParam);
        if (ev) {
            if (ev->gen == studioGen_.load() && studioOpen_) {
                if (!ev->isAudio && studioPlayer_) {
                    if (ev->type == MFP_EVENT_TYPE_MEDIAITEM_SET) {
                        studioVideoReady_ = true;
                        int idx = studioLoadedClipIdx_;
                        if (idx >= 0 && idx < static_cast<int>(studioClips_.size())) {
                            const auto& c = studioClips_[static_cast<size_t>(idx)];
                            float vol = std::max(0.0f, std::min(1.0f,
                                static_cast<float>(c.hasMultiTrack ? std::max(c.gameVolPct, c.micVolPct) : c.gameVolPct) / 100.0f));
                            bool mute = (c.gameVolPct == 0 && (!c.hasMultiTrack || c.micVolPct == 0));
                            studioPlayer_->SetVolume(vol);
                            studioPlayer_->SetMute(mute ? TRUE : FALSE);
                            studioPlayer_->SetRate(static_cast<float>(c.speed));
                        }
                        if (studioPendingSeekSec_ > 0.01) {
                            PROPVARIANT sp;
                            PropVariantInit(&sp);
                            sp.vt = VT_I8;
                            sp.hVal.QuadPart = static_cast<LONGLONG>(studioPendingSeekSec_ * 1e7);
                            studioSeekPending_ = true;
                            studioIgnoreEndUntilMs_ = GetTickCount64() + 550;
                            if (FAILED(studioPlayer_->SetPosition(MFP_POSITIONTYPE_100NS, &sp))) {
                                studioSeekPending_ = false;
                            }
                            PropVariantClear(&sp);
                        } else {
                            studioSeekPending_ = false;
                            studioIgnoreEndUntilMs_ = GetTickCount64() + 180;
                        }
                        studioPendingSeekSec_ = -1.0;
                        studioPlayer_->UpdateVideo();
                        if (studioPlaying_) studioPlayer_->Play();
                        else studioPlayer_->Pause();
                    } else if (ev->type == MFP_EVENT_TYPE_POSITION_SET) {
                        studioSeekPending_ = false;
                        studioIgnoreEndUntilMs_ = GetTickCount64() + 120;
                    } else if (ev->type == MFP_EVENT_TYPE_PLAYBACK_ENDED) {
                        if (studioVideoReady_ && !studioSeekPending_ && GetTickCount64() >= studioIgnoreEndUntilMs_) {
                            int curIdx = studioLoadedClipIdx_;
                            if (curIdx < 0 || curIdx >= static_cast<int>(studioClips_.size())) {
                                double tmpL = 0.0, tmpC = 0.0;
                                MapTimelineToClip(studioPlayheadSec_, curIdx, tmpL, tmpC);
                            }
                            if (studioPlaying_ && curIdx >= 0 && curIdx + 1 < static_cast<int>(studioClips_.size())) {
                                double nextTl = 0.0;
                                for (int i = 0; i <= curIdx; ++i) {
                                    const auto& c = studioClips_[static_cast<size_t>(i)];
                                    nextTl += std::max(0.1, c.trimEndSec - c.trimStartSec) / std::max(0.5, c.speed);
                                }
                                studioLoadedClipPath_.clear();
                                studioPlayheadSec_ = nextTl + 0.001;
                                SyncStudioMediaState(true);
                            } else {
                                studioPlaying_ = false;
                                studioPlayheadSec_ = StudioTotalDuration();
                                if (studioMusicPlayer_) studioMusicPlayer_->Pause();
                                if (hwnd_) InvalidateRect(hwnd_, nullptr, FALSE);
                            }
                        }
                    }
                } else if (ev->isAudio && studioMusicPlayer_) {
                    if (ev->type == MFP_EVENT_TYPE_MEDIAITEM_SET) {
                        studioAudioReady_ = true;
                        for (const auto& m : studioMusic_) {
                            if (_wcsicmp(m.path.c_str(), studioLoadedMusicPath_.c_str()) == 0) {
                                studioMusicPlayer_->SetVolume(std::max(0.0f, std::min(1.0f, static_cast<float>(m.volPct) / 100.0f)));
                                break;
                            }
                        }
                        if (studioPendingAudioSeekSec_ >= 0.0) {
                            PROPVARIANT ap;
                            PropVariantInit(&ap);
                            ap.vt = VT_I8;
                            ap.hVal.QuadPart = static_cast<LONGLONG>(studioPendingAudioSeekSec_ * 1e7);
                            studioMusicPlayer_->SetPosition(MFP_POSITIONTYPE_100NS, &ap);
                            PropVariantClear(&ap);
                            studioPendingAudioSeekSec_ = -1.0;
                        }
                        if (studioPlaying_) studioMusicPlayer_->Play();
                        else studioMusicPlayer_->Pause();
                    }
                }
            }
            delete ev;
        }
        outRes = 0;
        return true;
    }

    if (msg == ui::WM_MW_STUDIO_WAVE_READY) {
        auto* wm = reinterpret_cast<StudioWaveDoneMsg*>(lParam);
        if (wm) {
            if (wm->gen == studioGen_.load() && studioOpen_) {
                if (!wm->isMusic) {
                    for (auto& c : studioClips_) {
                        if (_wcsicmp(c.path.c_str(), wm->path.c_str()) == 0) {
                            c.wavePeaks = wm->peaks;
                            c.waveLoaded = true;
                        }
                    }
                } else {
                    for (auto& m : studioMusic_) {
                        if (_wcsicmp(m.path.c_str(), wm->path.c_str()) == 0) {
                            m.wavePeaks = wm->peaks;
                            m.waveLoaded = true;
                        }
                    }
                }
                if (hwnd_) InvalidateRect(hwnd_, nullptr, FALSE);
            }
            delete wm;
        }
        outRes = 0;
        return true;
    }
    return false;
}

bool MainWindow::HandleStudioPreTranslate(const MSG& msg) {
    if (!studioOpen_ || activeTab_ != 1 || msg.message != WM_KEYDOWN) return false;
    UINT vk = static_cast<UINT>(msg.wParam);
    if (vk == VK_ESCAPE) {
        if (!trimBusy_) CloseStudio();
        return true;
    }
    if (trimBusy_) return false;
    if (vk == VK_SPACE) { StudioTogglePlayPause(); return true; }
    if (vk == VK_LEFT)  {
        double step = (GetKeyState(VK_SHIFT) & 0x8000) ? -0.2 : -1.0;
        StudioSeekTimeline(studioPlayheadSec_ + step);
        return true;
    }
    if (vk == VK_RIGHT) {
        double step = (GetKeyState(VK_SHIFT) & 0x8000) ? +0.2 : +1.0;
        StudioSeekTimeline(studioPlayheadSec_ + step);
        return true;
    }
    if (vk == 'S' || (vk == 'B' && (GetKeyState(VK_CONTROL) & 0x8000))) {
        StudioSplitAtPlayhead();
        return true;
    }
    if (vk == VK_OEM_4 || vk == 'I') {
        StudioTrimClipEdgeToPlayhead(true);
        return true;
    }
    if (vk == VK_OEM_6 || vk == 'O') {
        StudioTrimClipEdgeToPlayhead(false);
        return true;
    }
    if (vk == VK_DELETE) {
        StudioDeleteSelection();
        return true;
    }
    return false;
}

void MainWindow::PaintMontajLanding(void* gPtr, int cw, int ch) {
    auto& g = *reinterpret_cast<Gdiplus::Graphics*>(gPtr);
    Gdiplus::FontFamily ff(L"Segoe UI");
    Gdiplus::Font fTitle(&ff, 14.5f, Gdiplus::FontStyleBold, Gdiplus::UnitPoint);
    Gdiplus::Font fSub(&ff, 9.0f, Gdiplus::FontStyleRegular, Gdiplus::UnitPoint);
    Gdiplus::Font fBold(&ff, 9.8f, Gdiplus::FontStyleBold, Gdiplus::UnitPoint);
    Gdiplus::Font fSmall(&ff, 8.3f, Gdiplus::FontStyleRegular, Gdiplus::UnitPoint);

    Gdiplus::SolidBrush brText(Gc(238, 242, 248));
    Gdiplus::SolidBrush brMuted(Gc(148, 160, 180));
    Gdiplus::SolidBrush brBlueTxt(Gc(147, 197, 253));
    Gdiplus::SolidBrush brCyanTxt(Gc(103, 232, 249));
    Gdiplus::StringFormat sfLeft, sfCenter;
    sfLeft.SetAlignment(Gdiplus::StringAlignmentNear);
    sfLeft.SetLineAlignment(Gdiplus::StringAlignmentCenter);
    sfLeft.SetTrimming(Gdiplus::StringTrimmingEllipsisCharacter);
    sfLeft.SetFormatFlags(Gdiplus::StringFormatFlagsNoWrap);
    sfCenter.SetAlignment(Gdiplus::StringAlignmentCenter);
    sfCenter.SetLineAlignment(Gdiplus::StringAlignmentCenter);
    sfCenter.SetFormatFlags(Gdiplus::StringFormatFlagsNoWrap);

    auto addBtnPx = [&](int id, int px, int py, int pw, int ph, const std::wstring& label,
                        Gdiplus::Color baseCol, Gdiplus::Color hoverCol, Gdiplus::Color borderCol, bool enabled = true) {
        RECT rc{ px, py, px + pw, py + ph };
        customButtons_.push_back({ id, rc, enabled });
        bool hov = enabled && (hoverBtnId_ == id);
        bool prs = enabled && (pressedBtnId_ == id);
        Gdiplus::Color fill = !enabled ? Gc(26, 31, 42) : (prs ? hoverCol : (hov ? hoverCol : baseCol));
        if (enabled && !prs) {
            Gdiplus::Color topC(fill.GetA(),
                static_cast<uint8_t>(std::min(255, fill.GetR() + 14)),
                static_cast<uint8_t>(std::min(255, fill.GetG() + 14)),
                static_cast<uint8_t>(std::min(255, fill.GetB() + 16)));
            FillRoundRectGrad(g, (float)rc.left, (float)rc.top, (float)pw, (float)ph, (float)S(9), topC, fill);
        } else {
            FillRoundRect(g, (float)rc.left, (float)rc.top, (float)pw, (float)ph, (float)S(9), fill);
        }
        if (borderCol.GetA() > 0) StrokeRoundRect(g, (float)rc.left, (float)rc.top, (float)pw, (float)ph, (float)S(9), borderCol);
        Gdiplus::SolidBrush tb(!enabled ? Gc(100, 112, 132) : Gc(245, 248, 255));
        g.DrawString(label.c_str(), -1, &fBold, Gdiplus::RectF((float)rc.left, (float)rc.top, (float)pw, (float)ph), &sfCenter, &tb);
    };

    const int bodyW = std::max(S(640), cw - S(264));
    g.DrawString(L"Montaj & Stüdyo", -1, &fTitle,
        Gdiplus::RectF((float)S(242), (float)S(14), (float)S(320), (float)S(28)), &sfLeft, &brText);
    g.DrawString(L"Kliplerinizi birleştirin, zaman çizelgesinde MP3 müzik ekleyin, oyun & mikrofon seslerini miksleyin.", -1, &fSmall,
        Gdiplus::RectF((float)S(242), (float)S(42), (float)bodyW, (float)S(18)), &sfLeft, &brMuted);

    // Hero card to open full Medal-style Studio page
    const int heroY = S(68), heroH = S(104);
    FillRoundRectGrad(g, (float)S(242), (float)heroY, (float)bodyW, (float)heroH, (float)S(14),
        Gc(22, 30, 52), Gc(15, 20, 34));
    StrokeRoundRect(g, (float)S(242), (float)heroY, (float)bodyW, (float)heroH, (float)S(14), Gc(99, 102, 241, 135), 1.3f);
    FillRoundRect(g, (float)S(258), (float)(heroY + S(16)), (float)S(66), (float)S(20), (float)S(10), Gc(124, 58, 237, 190));
    g.DrawString(L"STÜDYO", -1, &fSmall,
        Gdiplus::RectF((float)S(258), (float)(heroY + S(16)), (float)S(66), (float)S(20)), &sfCenter, &brText);
    g.DrawString(L"Çok Kanallı Zaman Çizelgesi Düzenleyici", -1, &fTitle,
        Gdiplus::RectF((float)S(332), (float)(heroY + S(13)), (float)std::max(S(260), bodyW - S(340)), (float)S(26)), &sfLeft, &brText);
    g.DrawString(L"Soldan klip seçin  •  Alt kanaldan sesleri ayarlayın  •  İstediğiniz saniyeye MP3 müzik koyun", -1, &fSub,
        Gdiplus::RectF((float)S(258), (float)(heroY + S(46)), (float)std::max(S(300), bodyW - S(270)), (float)S(22)), &sfLeft, &brCyanTxt);
    g.DrawString(L"Böl / Kes  •  Süre Kırpma  •  Hız (0.5x-2x)  •  Karartma Geçişi  •  9:16 Dikey  •  Discord ≤10/25 MB", -1, &fSmall,
        Gdiplus::RectF((float)S(258), (float)(heroY + S(70)), (float)std::max(S(300), bodyW - S(270)), (float)S(20)), &sfLeft, &brMuted);

    addBtnPx(ui::BTN_MNT_OPEN_STUDIO, S(242) + bodyW - S(218), heroY + S(29), S(200), S(46),
        L"Düzenleyiciyi Aç  →", Gc(16, 185, 129), Gc(5, 150, 105), Gc(52, 211, 153), !trimBusy_);

    // Quick clip selector + pre-queue below
    const int colGap = S(16);
    const int leftW = (bodyW - colGap) / 2;
    const int rightX = S(242) + leftW + colGap;
    const int rightW = bodyW - leftW - colGap;
    const int listTop = heroY + heroH + S(14);
    const int listH = std::max(S(180), ch - listTop - S(24));

    FillRoundRect(g, (float)S(242), (float)listTop, (float)leftW, (float)listH, (float)S(12), Gc(16, 21, 33));
    StrokeRoundRect(g, (float)S(242), (float)listTop, (float)leftW, (float)listH, (float)S(12), Gc(44, 56, 80));
    g.DrawString(L"Kliplerim (Hızlı Seçim)", -1, &fBold,
        Gdiplus::RectF((float)S(256), (float)(listTop + S(10)), (float)(leftW - S(28)), (float)S(22)), &sfLeft, &brBlueTxt);

    const int rowH = S(40);
    const int rowStartY = listTop + S(38);
    const int maxVisLib = std::max(1, (listH - S(46)) / rowH);
    const int maxLibSc = std::max(0, static_cast<int>(clips_.size()) - maxVisLib);
    montageLibScroll_ = std::max(0, std::min(montageLibScroll_, maxLibSc));

    for (int vi = 0; vi < maxVisLib; ++vi) {
        int ci = montageLibScroll_ + vi;
        if (ci >= static_cast<int>(clips_.size())) break;
        const auto& c = clips_[static_cast<size_t>(ci)];
        int ry = rowStartY + vi * rowH;
        FillRoundRect(g, (float)S(252), (float)ry, (float)(leftW - S(20)), (float)(rowH - S(5)), (float)S(8), Gc(22, 29, 45));
        wchar_t meta[64] = {};
        swprintf(meta, 64, L"%.1f sn • %.1f MB", c.durationSeconds, static_cast<double>(c.fileSizeBytes) / (1024.0 * 1024.0));
        g.DrawString(c.fileName.c_str(), -1, &fSmall,
            Gdiplus::RectF((float)S(260), (float)(ry + S(3)), (float)(leftW - S(94)), (float)S(16)), &sfLeft, &brText);
        g.DrawString(meta, -1, &fSmall,
            Gdiplus::RectF((float)S(260), (float)(ry + S(18)), (float)(leftW - S(94)), (float)S(14)), &sfLeft, &brMuted);
        addBtnPx(ui::BTN_MNT_ADD_BASE + ci, S(242) + leftW - S(72), ry + S(5), S(54), S(25), L"+ Ekle",
            Gc(37, 99, 235), Gc(59, 130, 246), Gc(96, 165, 250), !trimBusy_ && ci < 900);
    }

    FillRoundRect(g, (float)rightX, (float)listTop, (float)rightW, (float)listH, (float)S(12), Gc(16, 21, 33));
    StrokeRoundRect(g, (float)rightX, (float)listTop, (float)rightW, (float)listH, (float)S(12), Gc(59, 130, 246, 110));
    wchar_t qHdr[96] = {};
    swprintf(qHdr, 96, L"Hazırlanan Sıra (%zu klip)", montagePaths_.size());
    g.DrawString(qHdr, -1, &fBold,
        Gdiplus::RectF((float)(rightX + S(14)), (float)(listTop + S(10)), (float)(rightW - S(100)), (float)S(22)), &sfLeft, &brBlueTxt);
    addBtnPx(ui::BTN_MNT_CLEAR, rightX + rightW - S(78), listTop + S(8), S(64), S(24), L"Temizle",
        Gc(45, 55, 75), Gc(65, 80, 108), Gc(90, 110, 140), !trimBusy_ && !montagePaths_.empty());

    if (montagePaths_.empty()) {
        g.DrawString(L"Doğrudan 'Düzenleyiciyi Aç'a basabilir veya soldan klip sırası seçebilirsiniz.", -1, &fSmall,
            Gdiplus::RectF((float)(rightX + S(16)), (float)(listTop + S(52)), (float)(rightW - S(32)), (float)S(24)), &sfLeft, &brMuted);
    } else {
        const int maxVisQ = std::max(1, (listH - S(46)) / rowH);
        const int maxQSc = std::max(0, static_cast<int>(montagePaths_.size()) - maxVisQ);
        montageQueueScroll_ = std::max(0, std::min(montageQueueScroll_, maxQSc));
        for (int vi = 0; vi < maxVisQ; ++vi) {
            int qi = montageQueueScroll_ + vi;
            if (qi >= static_cast<int>(montagePaths_.size())) break;
            const std::wstring& qp = montagePaths_[static_cast<size_t>(qi)];
            size_t sl = qp.find_last_of(L"\\/");
            std::wstring qName = (sl != std::wstring::npos) ? qp.substr(sl + 1) : qp;
            int ry = rowStartY + vi * rowH;
            FillRoundRect(g, (float)(rightX + S(10)), (float)ry, (float)(rightW - S(20)), (float)(rowH - S(5)), (float)S(8), Gc(22, 29, 45));
            wchar_t qLine[256] = {};
            swprintf(qLine, 256, L"#%d  %ls", qi + 1, qName.c_str());
            g.DrawString(qLine, -1, &fSmall,
                Gdiplus::RectF((float)(rightX + S(18)), (float)(ry + S(8)), (float)(rightW - S(124)), (float)S(18)), &sfLeft, &brText);
            int bx = rightX + rightW - S(104);
            addBtnPx(ui::BTN_MNT_UP_BASE + qi, bx, ry + S(5), S(26), S(25), L"↑",
                Gc(34, 42, 58), Gc(50, 62, 86), Gc(70, 86, 116), !trimBusy_ && qi > 0);
            addBtnPx(ui::BTN_MNT_DOWN_BASE + qi, bx + S(30), ry + S(5), S(26), S(25), L"↓",
                Gc(34, 42, 58), Gc(50, 62, 86), Gc(70, 86, 116), !trimBusy_ && (qi + 1 < static_cast<int>(montagePaths_.size())));
            addBtnPx(ui::BTN_MNT_DEL_BASE + qi, bx + S(60), ry + S(5), S(26), S(25), L"×",
                Gc(127, 29, 29), Gc(185, 28, 28), Gc(220, 38, 38), !trimBusy_);
        }
    }
}

void MainWindow::PaintStudio(void* gPtr, int cw, int ch) {
    auto& g = *reinterpret_cast<Gdiplus::Graphics*>(gPtr);
    customButtons_.clear();

    Gdiplus::FontFamily ff(L"Segoe UI");
    Gdiplus::Font fTitle(&ff, 11.5f, Gdiplus::FontStyleBold, Gdiplus::UnitPoint);
    Gdiplus::Font fBold(&ff, 9.2f, Gdiplus::FontStyleBold, Gdiplus::UnitPoint);
    Gdiplus::Font fSmall(&ff, 8.2f, Gdiplus::FontStyleRegular, Gdiplus::UnitPoint);
    Gdiplus::Font fTiny(&ff, 7.6f, Gdiplus::FontStyleBold, Gdiplus::UnitPoint);

    Gdiplus::SolidBrush brText(Gc(238, 242, 248));
    Gdiplus::SolidBrush brMuted(Gc(148, 160, 180));
    Gdiplus::SolidBrush brCyan(Gc(103, 232, 249));
    Gdiplus::SolidBrush brPurple(Gc(216, 180, 254));
    Gdiplus::StringFormat sfLeft, sfCenter, sfRight;
    sfLeft.SetAlignment(Gdiplus::StringAlignmentNear);
    sfLeft.SetLineAlignment(Gdiplus::StringAlignmentCenter);
    sfLeft.SetTrimming(Gdiplus::StringTrimmingEllipsisCharacter);
    sfLeft.SetFormatFlags(Gdiplus::StringFormatFlagsNoWrap);
    sfCenter.SetAlignment(Gdiplus::StringAlignmentCenter);
    sfCenter.SetLineAlignment(Gdiplus::StringAlignmentCenter);
    sfCenter.SetFormatFlags(Gdiplus::StringFormatFlagsNoWrap);
    sfRight.SetAlignment(Gdiplus::StringAlignmentFar);
    sfRight.SetLineAlignment(Gdiplus::StringAlignmentCenter);
    sfRight.SetFormatFlags(Gdiplus::StringFormatFlagsNoWrap);

    auto addBtnPx = [&](int id, int px, int py, int pw, int ph, const std::wstring& label,
                        Gdiplus::Color baseCol, Gdiplus::Color hoverCol, Gdiplus::Color borderCol,
                        bool enabled = true, bool smallFont = false) {
        RECT rc{ px, py, px + pw, py + ph };
        customButtons_.push_back({ id, rc, enabled });
        bool hov = enabled && (hoverBtnId_ == id);
        bool prs = enabled && (pressedBtnId_ == id);
        Gdiplus::Color fill = !enabled ? Gc(24, 29, 40) : (prs ? hoverCol : (hov ? hoverCol : baseCol));
        if (enabled && !prs) {
            Gdiplus::Color topC(fill.GetA(),
                static_cast<uint8_t>(std::min(255, fill.GetR() + 14)),
                static_cast<uint8_t>(std::min(255, fill.GetG() + 14)),
                static_cast<uint8_t>(std::min(255, fill.GetB() + 16)));
            FillRoundRectGrad(g, (float)rc.left, (float)rc.top, (float)pw, (float)ph, (float)S(8), topC, fill);
        } else {
            FillRoundRect(g, (float)rc.left, (float)rc.top, (float)pw, (float)ph, (float)S(8), fill);
        }
        if (borderCol.GetA() > 0) StrokeRoundRect(g, (float)rc.left, (float)rc.top, (float)pw, (float)ph, (float)S(8), borderCol);
        Gdiplus::SolidBrush tb(!enabled ? Gc(96, 108, 128) : Gc(244, 247, 255));
        g.DrawString(label.c_str(), -1, smallFont ? &fSmall : &fBold,
            Gdiplus::RectF((float)rc.left, (float)rc.top, (float)pw, (float)ph), &sfCenter, &tb);
    };

    const StudioGeometry gm = CalcStudioGeom(cw, ch, static_cast<int>(dpi_));
    const double totalDur = StudioTotalDuration();

    // Base dark Anlık Klip backdrop + subtle signature blue/violet ambient glow orbs
    g.Clear(Gc(9, 12, 19));
    {
        Gdiplus::GraphicsPath gp1;
        gp1.AddEllipse(S(140), -S(90), S(480), S(280));
        Gdiplus::PathGradientBrush pgb1(&gp1);
        pgb1.SetCenterColor(Gc(37, 99, 235, 34));
        Gdiplus::Color s1[1] = { Gc(37, 99, 235, 0) };
        int c1 = 1;
        pgb1.SetSurroundColors(s1, &c1);
        g.FillPath(&pgb1, &gp1);

        Gdiplus::GraphicsPath gp2;
        gp2.AddEllipse(cw - S(360), -S(80), S(440), S(260));
        Gdiplus::PathGradientBrush pgb2(&gp2);
        pgb2.SetCenterColor(Gc(139, 92, 246, 30));
        Gdiplus::Color s2[1] = { Gc(139, 92, 246, 0) };
        int c2 = 1;
        pgb2.SetSurroundColors(s2, &c2);
        g.FillPath(&pgb2, &gp2);
    }

    // ================= 1. TOP BAR (Anlık Klip Stüdyo Signature Header) =================
    {
        Gdiplus::LinearGradientBrush brTopGrad(
            Gdiplus::PointF(0, 0), Gdiplus::PointF(0, (float)gm.topH),
            Gc(19, 25, 40), Gc(12, 16, 26));
        g.FillRectangle(&brTopGrad, 0, 0, cw, gm.topH);
        Gdiplus::Pen pSep(Gc(46, 58, 86), 1.0f);
        g.DrawLine(&pSep, 0, gm.topH, cw, gm.topH);

        // Anlık Klip lens emblem (matching main sidebar & desktop icon DNA)
        FillRoundRectGrad(g, (float)S(12), (float)S(10), (float)S(28), (float)S(28), (float)S(8),
            Gc(59, 130, 246), Gc(124, 58, 237));
        {
            Gdiplus::SolidBrush brWhite(Gc(255, 255, 255));
            g.FillEllipse(&brWhite, (float)S(19), (float)S(17), (float)S(14), (float)S(14));
            Gdiplus::SolidBrush brCore(app_.IsRunning() ? Gc(239, 68, 68) : Gc(100, 116, 139));
            g.FillEllipse(&brCore, (float)S(22), (float)S(20), (float)S(8), (float)S(8));
        }

        g.DrawString(L"Anlık Klip", -1, &fTitle,
            Gdiplus::RectF((float)S(46), (float)S(7), (float)S(96), (float)S(34)), &sfLeft, &brText);
        FillRoundRectGrad(g, (float)S(142), (float)S(14), (float)S(60), (float)S(20), (float)S(10),
            Gc(37, 99, 235, 95), Gc(124, 58, 237, 95));
        StrokeRoundRect(g, (float)S(142), (float)S(14), (float)S(60), (float)S(20), (float)S(10), Gc(139, 92, 246, 175));
        g.DrawString(L"STÜDYO", -1, &fTiny,
            Gdiplus::RectF((float)S(142), (float)S(14), (float)S(60), (float)S(20)), &sfCenter, &brPurple);

        wchar_t statsTxt[64] = {};
        swprintf(statsTxt, 64, L"%zu klip • %.1f sn", studioClips_.size(), totalDur);
        FillRoundRect(g, (float)S(210), (float)S(12), (float)S(112), (float)S(24), (float)S(12), Gc(20, 27, 42));
        StrokeRoundRect(g, (float)S(210), (float)S(12), (float)S(112), (float)S(24), (float)S(12), Gc(56, 189, 248, 85));
        g.DrawString(statsTxt, -1, &fSmall,
            Gdiplus::RectF((float)S(210), (float)S(12), (float)S(112), (float)S(24)), &sfCenter, &brCyan);

        int midX = std::max(S(332), (cw - S(390)) / 2 + S(26));
        addBtnPx(ui::BTN_STU_ASPECT, midX, S(9), S(104), S(30),
            studioVertical916_ ? L"9:16 Dikey" : L"16:9 Yatay",
            studioVertical916_ ? Gc(109, 40, 217) : Gc(24, 31, 48),
            studioVertical916_ ? Gc(124, 58, 237) : Gc(36, 46, 70),
            studioVertical916_ ? Gc(167, 139, 250) : Gc(54, 70, 102), !trimBusy_, true);
        addBtnPx(ui::BTN_STU_FADE, midX + S(110), S(9), S(124), S(30),
            studioFade_ ? L"Geçiş: Karartma" : L"Geçiş: Keskin",
            studioFade_ ? Gc(30, 64, 175) : Gc(24, 31, 48),
            studioFade_ ? Gc(37, 99, 235) : Gc(36, 46, 70),
            studioFade_ ? Gc(96, 165, 250) : Gc(54, 70, 102), !trimBusy_, true);

        const wchar_t* tgtLbl = (studioTargetMb_ == 10) ? L"Discord ≤10 MB"
                              : (studioTargetMb_ == 25 ? L"Discord ≤25 MB" : L"Tam Kalite");
        addBtnPx(ui::BTN_STU_TARGET, midX + S(240), S(9), S(122), S(30), tgtLbl,
            studioTargetMb_ > 0 ? Gc(79, 70, 229) : Gc(24, 31, 48),
            studioTargetMb_ > 0 ? Gc(99, 102, 241) : Gc(36, 46, 70),
            studioTargetMb_ > 0 ? Gc(165, 180, 252) : Gc(54, 70, 102), !trimBusy_, true);

        if (trimBusy_) {
            addBtnPx(ui::BTN_STU_CANCEL, cw - S(120), S(9), S(104), S(30), L"İptal Et",
                Gc(153, 27, 27), Gc(185, 28, 28), Gc(248, 113, 113), true);
        } else {
            addBtnPx(ui::BTN_STU_EXIT, cw - S(236), S(9), S(84), S(30), L"← Çıkış",
                Gc(24, 31, 48), Gc(40, 50, 76), Gc(64, 78, 108), true, true);
            addBtnPx(ui::BTN_STU_RENDER, cw - S(144), S(9), S(130), S(30), L"Montajı Oluştur",
                Gc(16, 185, 129), Gc(5, 150, 105), Gc(110, 231, 183), !studioClips_.empty(), true);
        }
    }

    // ================= 2. FAR-LEFT LIBRARY & MUSIC DRAWER =================
    {
        Gdiplus::LinearGradientBrush brLeftGrad(
            Gdiplus::PointF(0, (float)gm.topH), Gdiplus::PointF(0, (float)gm.timelineTop),
            Gc(16, 21, 34), Gc(11, 15, 24));
        g.FillRectangle(&brLeftGrad, 0, gm.topH + 1, gm.leftW, gm.timelineTop - gm.topH - 1);
        Gdiplus::Pen pSep(Gc(42, 54, 80), 1.0f);
        g.DrawLine(&pSep, gm.leftW, gm.topH, gm.leftW, gm.timelineTop);

        int halfTabW = (gm.leftW - S(26)) / 2;
        addBtnPx(ui::BTN_STU_LEFT_CLIPS, S(10), gm.topH + S(10), halfTabW, S(30), L"Kliplerim",
            studioLeftTab_ == 0 ? Gc(37, 99, 235) : Gc(22, 28, 42),
            studioLeftTab_ == 0 ? Gc(59, 130, 246) : Gc(34, 43, 64),
            studioLeftTab_ == 0 ? Gc(96, 165, 250) : Gc(48, 60, 86), true, true);
        addBtnPx(ui::BTN_STU_LEFT_MUSIC, S(16) + halfTabW, gm.topH + S(10), halfTabW, S(30), L"Müzik / MP3",
            studioLeftTab_ == 1 ? Gc(124, 58, 237) : Gc(22, 28, 42),
            studioLeftTab_ == 1 ? Gc(139, 92, 246) : Gc(34, 43, 64),
            studioLeftTab_ == 1 ? Gc(196, 181, 253) : Gc(48, 60, 86), true, true);

        int bodyTop = gm.topH + S(48);
        int bodyH = std::max(S(80), gm.timelineTop - bodyTop - S(6));

        if (studioLeftTab_ == 0) {
            const int rowH = S(52);
            int maxVis = std::max(1, bodyH / rowH);
            int maxSc = std::max(0, static_cast<int>(clips_.size()) - maxVis);
            studioLibScroll_ = std::max(0, std::min(studioLibScroll_, maxSc));

            for (int vi = 0; vi < maxVis; ++vi) {
                int ci = studioLibScroll_ + vi;
                if (ci >= static_cast<int>(clips_.size())) break;
                const auto& c = clips_[static_cast<size_t>(ci)];
                int ry = bodyTop + vi * rowH;
                FillRoundRectGrad(g, (float)S(10), (float)ry, (float)(gm.leftW - S(20)), (float)(rowH - S(6)), (float)S(9),
                    Gc(24, 31, 48), Gc(18, 24, 38));
                StrokeRoundRect(g, (float)S(10), (float)ry, (float)(gm.leftW - S(20)), (float)(rowH - S(6)), (float)S(9), Gc(48, 62, 92));
                FillRoundRect(g, (float)S(10), (float)(ry + S(8)), (float)S(3), (float)(rowH - S(22)), 1.5f, Gc(56, 189, 248, 170));

                // Draw cached miniature thumbnail on far left of clip card
                RECT thRc{ S(16), ry + S(6), S(76), ry + rowH - S(12) };
                auto it = thumbBmps_.find(c.filePath);
                if (it != thumbBmps_.end() && it->second) {
                    Gdiplus::Bitmap bmp(it->second, nullptr);
                    g.DrawImage(&bmp, static_cast<INT>(thRc.left), static_cast<INT>(thRc.top),
                        static_cast<INT>(thRc.right - thRc.left), static_cast<INT>(thRc.bottom - thRc.top));
                    StrokeRoundRect(g, (float)thRc.left, (float)thRc.top, (float)(thRc.right - thRc.left), (float)(thRc.bottom - thRc.top), 3.0f, Gc(70, 90, 128));
                } else {
                    FillRoundRect(g, (float)thRc.left, (float)thRc.top, (float)(thRc.right - thRc.left), (float)(thRc.bottom - thRc.top), (float)S(4), Gc(12, 16, 26));
                }

                std::wstring stem = BaseNameNoExt(c.fileName);
                g.DrawString(stem.c_str(), -1, &fSmall,
                    Gdiplus::RectF((float)S(81), (float)(ry + S(5)), (float)(gm.leftW - S(149)), (float)S(16)), &sfLeft, &brText);
                wchar_t sub[48] = {};
                swprintf(sub, 48, L"%.1f sn • %.0f MB", c.durationSeconds, static_cast<double>(c.fileSizeBytes) / (1024.0 * 1024.0));
                g.DrawString(sub, -1, &fTiny,
                    Gdiplus::RectF((float)S(81), (float)(ry + S(22)), (float)(gm.leftW - S(149)), (float)S(16)), &sfLeft, &brCyan);

                addBtnPx(ui::BTN_STU_ADD_CLIP_BASE + ci, gm.leftW - S(62), ry + S(10), S(46), S(26), L"+ Ekle",
                    Gc(37, 99, 235), Gc(59, 130, 246), Gc(96, 165, 250), !trimBusy_ && ci < 900, true);
            }
        } else {
            addBtnPx(ui::BTN_STU_PICK_MP3, S(12), bodyTop + S(2), gm.leftW - S(24), S(36), L"+ PC'den MP3 / Ses Ekle",
                Gc(124, 58, 237), Gc(139, 92, 246), Gc(196, 181, 253), !trimBusy_, true);
            g.DrawString(L"Eklediğiniz müziği alttaki M1\nşeridinde istediğiniz saniyeye\nsürükleyebilirsiniz.", -1, &fTiny,
                Gdiplus::RectF((float)S(12), (float)(bodyTop + S(44)), (float)(gm.leftW - S(24)), (float)S(44)), &sfLeft, &brMuted);

            int recTop = bodyTop + S(96);
            int recH = S(40);
            int maxRec = std::max(0, (bodyH - S(98)) / recH);
            for (int ri = 0; ri < maxRec && ri < static_cast<int>(studioRecentAudio_.size()); ++ri) {
                int ry = recTop + ri * recH;
                std::wstring name = BaseNameNoExt(studioRecentAudio_[static_cast<size_t>(ri)]);
                FillRoundRectGrad(g, (float)S(10), (float)ry, (float)(gm.leftW - S(20)), (float)(recH - S(6)), (float)S(8),
                    Gc(32, 24, 58), Gc(22, 18, 42));
                StrokeRoundRect(g, (float)S(10), (float)ry, (float)(gm.leftW - S(20)), (float)(recH - S(6)), (float)S(8), Gc(139, 92, 246, 130));
                g.DrawString(name.c_str(), -1, &fSmall,
                    Gdiplus::RectF((float)S(18), (float)ry, (float)(gm.leftW - S(90)), (float)(recH - S(6))), &sfLeft, &brPurple);
                addBtnPx(ui::BTN_STU_ADD_MUSIC_BASE + ri, gm.leftW - S(64), ry + S(4), S(48), S(26), L"+ Koy",
                    Gc(124, 58, 237), Gc(139, 92, 246), Gc(196, 181, 253), !trimBusy_ && ri < 500, true);
            }
        }
    }

    // ================= 3. PREVIEW STAGE (Framed Anlık Klip Theater Canvas) =================
    {
        Gdiplus::LinearGradientBrush brStageGrad(
            Gdiplus::PointF((float)gm.leftW, (float)gm.topH),
            Gdiplus::PointF((float)cw, (float)gm.toolY),
            Gc(7, 10, 17), Gc(11, 15, 26));
        g.FillRectangle(&brStageGrad, gm.leftW + 1, gm.topH + 1, cw - gm.leftW - 1, gm.toolY - gm.topH - 1);

        int availW = std::max(S(240), static_cast<int>(gm.stageRc.right - gm.stageRc.left) - S(16));
        int availH = std::max(S(140), static_cast<int>(gm.stageRc.bottom - gm.stageRc.top) - S(12));
        int vw = studioVertical916_ ? (availH * 9) / 16 : availW;
        int vh = studioVertical916_ ? availH : (vw * 9) / 16;
        if (!studioVertical916_ && vh > availH) { vh = availH; vw = (vh * 16) / 9; }
        if (studioVertical916_ && vw > availW) { vw = availW; vh = (vw * 16) / 9; }
        int vx = gm.stageRc.left + (static_cast<int>(gm.stageRc.right - gm.stageRc.left) - vw) / 2;
        int vy = gm.stageRc.top + (static_cast<int>(gm.stageRc.bottom - gm.stageRc.top) - vh) / 2;

        // Ambient frame glow around video viewport
        StrokeRoundRect(g, (float)(vx - 2), (float)(vy - 2), (float)(vw + 4), (float)(vh + 4), (float)S(10),
            studioVertical916_ ? Gc(139, 92, 246, 165) : Gc(59, 130, 246, 145), 1.6f);

        if (trimBusy_) {
            int pw = S(380), ph = S(84);
            int px = gm.stageRc.left + (static_cast<int>(gm.stageRc.right - gm.stageRc.left) - pw) / 2;
            int py = gm.stageRc.top + (static_cast<int>(gm.stageRc.bottom - gm.stageRc.top) - ph) / 2;
            FillRoundRectGrad(g, (float)px, (float)py, (float)pw, (float)ph, (float)S(12), Gc(22, 30, 50), Gc(14, 19, 32));
            StrokeRoundRect(g, (float)px, (float)py, (float)pw, (float)ph, (float)S(12), Gc(52, 211, 153));
            g.DrawString(L"Anlık Klip Stüdyo render alıyor, lütfen bekleyin...", -1, &fBold,
                Gdiplus::RectF((float)px, (float)py, (float)pw, (float)ph), &sfCenter, &brCyan);
        } else if (studioClips_.empty()) {
            FillRoundRectGrad(g, (float)vx, (float)vy, (float)vw, (float)vh, (float)S(8), Gc(15, 20, 32), Gc(10, 14, 24));
            FillRoundRectGrad(g, (float)(vx + vw / 2 - S(22)), (float)(vy + vh / 2 - S(52)), (float)S(44), (float)S(44), (float)S(12),
                Gc(59, 130, 246, 120), Gc(124, 58, 237, 120));
            g.DrawString(L"▶", -1, &fTitle,
                Gdiplus::RectF((float)(vx + vw / 2 - S(22)), (float)(vy + vh / 2 - S(52)), (float)S(44), (float)S(44)), &sfCenter, &brText);
            g.DrawString(L"Sol taraftan klip seçip '+ Ekle'ye basın\nÖnizleme burada canlı olarak oynatılacak.", -1, &fBold,
                Gdiplus::RectF((float)vx, (float)(vy + S(14)), (float)vw, (float)(vh - S(14))), &sfCenter, &brMuted);
        }
    }

    // ================= 4. TOOL BAR & DEDICATED AUDIO MIXER STRIP =================
    {
        Gdiplus::SolidBrush brBar(Gc(14, 18, 28));
        g.FillRectangle(&brBar, gm.leftW + 1, gm.toolY, cw - gm.leftW - 1, gm.toolH);
        Gdiplus::SolidBrush brMixBar(Gc(12, 16, 25));
        g.FillRectangle(&brMixBar, gm.leftW + 1, gm.toolRow2Y, cw - gm.leftW - 1, gm.toolRow2H);
        Gdiplus::Pen pLine(Gc(34, 44, 66), 1.0f);
        g.DrawLine(&pLine, gm.leftW, gm.toolY, cw, gm.toolY);
        g.DrawLine(&pLine, gm.leftW, gm.toolRow2Y, cw, gm.toolRow2Y);
        g.DrawLine(&pLine, 0, gm.timelineTop, cw, gm.timelineTop);

        int bx = gm.leftW + S(10);
        int by = gm.toolY + S(5);
        bool hasClipSel = (studioSelClip_ >= 0 && studioSelClip_ < static_cast<int>(studioClips_.size()));
        bool hasMusSel = (studioSelMusic_ >= 0 && studioSelMusic_ < static_cast<int>(studioMusic_.size()));
        bool canSplit = false;
        if (!studioClips_.empty() && !trimBusy_) {
            int si = -1; double lSec = 0, cStart = 0;
            if (MapTimelineToClip(studioPlayheadSec_, si, lSec, cStart) && si >= 0) {
                const auto& sc = studioClips_[static_cast<size_t>(si)];
                canSplit = (lSec > sc.trimStartSec + 0.25 && lSec < sc.trimEndSec - 0.25);
            }
        }

        // Row 1: Edit Tools (left) + Transport (center) + Timeline Zoom (right)
        addBtnPx(ui::BTN_STU_SPLIT, bx, by, S(60), S(26), L"✂ Böl",
            Gc(30, 41, 66), Gc(48, 64, 102), Gc(71, 85, 118), canSplit, true);
        addBtnPx(ui::BTN_STU_DELETE, bx + S(64), by, S(44), S(26), L"Sil",
            Gc(127, 29, 29), Gc(185, 28, 28), Gc(248, 113, 113), !trimBusy_ && (hasClipSel || hasMusSel), true);
        addBtnPx(ui::BTN_STU_MOVE_LEFT, bx + S(112), by, S(26), S(26), L"◀",
            Gc(26, 33, 50), Gc(40, 50, 76), Gc(56, 70, 104), !trimBusy_ && hasClipSel && studioSelClip_ > 0, true);
        addBtnPx(ui::BTN_STU_MOVE_RIGHT, bx + S(140), by, S(26), S(26), L"▶",
            Gc(26, 33, 50), Gdiplus::Color(40, 50, 76), Gc(56, 70, 104),
            !trimBusy_ && hasClipSel && (studioSelClip_ + 1 < static_cast<int>(studioClips_.size())), true);

        wchar_t spdLbl[24] = L"Hız: 1.0x";
        if (hasClipSel) swprintf(spdLbl, 24, L"Hız: %.1fx", studioClips_[static_cast<size_t>(studioSelClip_)].speed);
        addBtnPx(ui::BTN_STU_SPEED, bx + S(170), by, S(68), S(26), spdLbl,
            Gc(26, 33, 50), Gc(40, 50, 76), Gc(56, 70, 104), !trimBusy_ && hasClipSel, true);

        int tx = std::max(bx + S(248), (gm.leftW + cw) / 2 - S(92));
        addBtnPx(ui::BTN_STU_TO_START, tx, by, S(28), S(26), L"⏮",
            Gc(26, 33, 50), Gc(40, 50, 76), Gc(56, 70, 104), !trimBusy_ && !studioClips_.empty(), true);
        addBtnPx(ui::BTN_STU_PLAYPAUSE, tx + S(32), by, S(34), S(26), studioPlaying_ ? L"⏸" : L"▶",
            Gc(37, 99, 235), Gc(59, 130, 246), Gc(96, 165, 250), !trimBusy_ && !studioClips_.empty(), true);
        std::wstring timeDisp = FormatSecTenths(studioPlayheadSec_) + L" / " + FormatSecTenths(totalDur);
        g.DrawString(timeDisp.c_str(), -1, &fSmall,
            Gdiplus::RectF((float)(tx + S(72)), (float)by, (float)S(102), (float)S(26)), &sfLeft, &brText);

        addBtnPx(ui::BTN_STU_ZOOM_OUT, cw - S(100), by + 1, S(26), S(24), L"-",
            Gc(26, 33, 50), Gc(40, 50, 76), Gc(56, 70, 104), true, true);
        addBtnPx(ui::BTN_STU_ZOOM_IN, cw - S(70), by + 1, S(26), S(24), L"+",
            Gc(26, 33, 50), Gc(40, 50, 76), Gc(56, 70, 104), true, true);
        addBtnPx(ui::BTN_STU_ZOOM_FIT, cw - S(40), by + 1, S(30), S(24), L"Fit",
            Gc(26, 33, 50), Gc(40, 50, 76), Gc(56, 70, 104), true, true);

        // Row 2: Selected Clip / Music Audio Mixer Sliders
        auto drawMiniSlider = [&](RECT sr, int pct, Gdiplus::Color fillCol) {
            FillRoundRect(g, (float)sr.left, (float)(sr.top + S(6)), (float)(sr.right - sr.left), (float)S(5), 2.5f, Gc(34, 42, 62));
            float r = std::max(0.0f, std::min(1.0f, static_cast<float>(pct) / 200.0f));
            float fw = (sr.right - sr.left) * r;
            if (fw >= 2.0f) FillRoundRect(g, (float)sr.left, (float)(sr.top + S(6)), fw, (float)S(5), 2.5f, fillCol);
            float kx = sr.left + fw - S(5);
            FillRoundRect(g, kx, (float)(sr.top + S(2)), (float)S(10), (float)S(13), 5.0f, Gc(242, 246, 255));
        };

        int mby = gm.toolRow2Y + S(5);
        if (hasMusSel) {
            const auto& m = studioMusic_[static_cast<size_t>(studioSelMusic_)];
            wchar_t selTag[64] = {};
            swprintf(selTag, 64, L"♪ Müzik @%.1fs", m.timelineStartSec);
            g.DrawString(selTag, -1, &fTiny,
                Gdiplus::RectF((float)bx, (float)mby, (float)S(118), (float)S(24)), &sfLeft, &brPurple);
            drawMiniSlider(gm.musSlRc, m.volPct, Gc(168, 85, 247));
            wchar_t ml[24] = {};
            swprintf(ml, 24, L"%%%d", m.volPct);
            g.DrawString(ml, -1, &fTiny,
                Gdiplus::RectF((float)(gm.musSlRc.right + S(6)), (float)mby, (float)S(38), (float)S(24)), &sfLeft, &brPurple);
            addBtnPx(ui::BTN_STU_MUS_FADEIN, gm.musSlRc.right + S(48), mby, S(76), S(24),
                m.fadeIn ? L"Giriş Fade ✓" : L"Giriş Fade",
                m.fadeIn ? Gc(109, 40, 217) : Gc(26, 33, 50), Gc(124, 58, 237), Gc(167, 139, 250), !trimBusy_, true);
            addBtnPx(ui::BTN_STU_MUS_FADEOUT, gm.musSlRc.right + S(130), mby, S(76), S(24),
                m.fadeOut ? L"Çıkış Fade ✓" : L"Çıkış Fade",
                m.fadeOut ? Gc(109, 40, 217) : Gc(26, 33, 50), Gc(124, 58, 237), Gc(167, 139, 250), !trimBusy_, true);
        } else if (hasClipSel || !studioClips_.empty()) {
            int ci = hasClipSel ? studioSelClip_ : 0;
            const auto& c = studioClips_[static_cast<size_t>(ci)];
            wchar_t clbl[32] = {};
            swprintf(clbl, 32, L"Klip #%d Ses:", ci + 1);
            g.DrawString(clbl, -1, &fTiny,
                Gdiplus::RectF((float)bx, (float)mby, (float)S(72), (float)S(24)), &sfLeft, &brCyan);

            addBtnPx(ui::BTN_STU_GAME_MUTE, bx + S(74), mby, S(86), S(24),
                c.gameVolPct == 0 ? L"Oyun: Sustu" : L"Oyun Sesi",
                c.gameVolPct == 0 ? Gc(153, 27, 27) : Gc(24, 34, 54), Gc(44, 62, 92), Gc(56, 189, 248, 120), !trimBusy_, true);
            drawMiniSlider(gm.gameSlRc, c.gameVolPct, Gc(56, 189, 248));
            wchar_t gl[24] = {};
            swprintf(gl, 24, L"%%%d", c.gameVolPct);
            g.DrawString(gl, -1, &fTiny,
                Gdiplus::RectF((float)(gm.gameSlRc.right + S(6)), (float)mby, (float)S(38), (float)S(24)), &sfLeft, &brCyan);

            if (c.hasMultiTrack) {
                addBtnPx(ui::BTN_STU_MIC_MUTE, gm.gameSlRc.right + S(48), mby, S(56), S(24),
                    c.micVolPct == 0 ? L"Mic×" : L"Mikrofon",
                    c.micVolPct == 0 ? Gc(153, 27, 27) : Gc(24, 34, 54), Gc(44, 62, 92), Gc(52, 211, 153, 120), !trimBusy_, true);
                drawMiniSlider(gm.micSlRc, c.micVolPct, Gc(52, 211, 153));
                wchar_t ml[24] = {};
                swprintf(ml, 24, L"%%%d", c.micVolPct);
                g.DrawString(ml, -1, &fTiny,
                    Gdiplus::RectF((float)(gm.micSlRc.right + S(6)), (float)mby, (float)S(38), (float)S(24)), &sfLeft, &brText);
            }
        } else {
            g.DrawString(L"Klip veya MP3 seçtiğinizde ses ayarları burada görünür.", -1, &fTiny,
                Gdiplus::RectF((float)bx, (float)mby, (float)S(380), (float)S(24)), &sfLeft, &brMuted);
        }
    }

    // ================= 5. BOTTOM MULTI-TRACK TIMELINE =================
    {
        Gdiplus::SolidBrush brTlBg(Gc(11, 14, 22));
        g.FillRectangle(&brTlBg, 0, gm.timelineTop + 1, cw, ch - gm.timelineTop);

        // Track headers column (sticky left)
        Gdiplus::SolidBrush brHdrBg(Gc(15, 19, 30));
        g.FillRectangle(&brHdrBg, 0, gm.timelineTop + 1, gm.hdrW, ch - gm.timelineTop);
        Gdiplus::Pen pGrid(Gc(30, 38, 56), 1.0f);
        g.DrawLine(&pGrid, gm.hdrW, gm.timelineTop, gm.hdrW, ch);
        g.DrawLine(&pGrid, 0, gm.timelineTop + gm.rulerH, cw, gm.timelineTop + gm.rulerH);

        g.DrawString(L"Zaman Çizelgesi", -1, &fTiny,
            Gdiplus::RectF((float)S(8), (float)gm.timelineTop, (float)(gm.hdrW - S(14)), (float)gm.rulerH), &sfLeft, &brMuted);

        auto drawTrackHdr = [&](int top, int h, const wchar_t* tag, const wchar_t* sub, Gdiplus::Color pillCol) {
            FillRoundRect(g, (float)S(8), (float)(top + S(6)), (float)S(26), (float)S(18), (float)S(5), pillCol);
            g.DrawString(tag, -1, &fTiny, Gdiplus::RectF((float)S(8), (float)(top + S(6)), (float)S(26), (float)S(18)), &sfCenter, &brText);
            g.DrawString(sub, -1, &fSmall,
                Gdiplus::RectF((float)S(38), (float)(top + S(5)), (float)(gm.hdrW - S(44)), (float)S(20)), &sfLeft, &brText);
            g.DrawLine(&pGrid, 0, top + h + S(2), cw, top + h + S(2));
        };
        drawTrackHdr(gm.v1Top, gm.v1H, L"V1", L"Video", Gc(37, 99, 235));
        drawTrackHdr(gm.a1Top, gm.a1H, L"A1", L"Klip Sesi", Gc(14, 116, 144));
        drawTrackHdr(gm.m1Top, gm.m1H, L"M1", L"MP3 / Müzik", Gc(124, 58, 237));
        addBtnPx(ui::BTN_STU_PICK_MP3, S(10), gm.m1Top + S(26), gm.hdrW - S(20), S(20), L"+ MP3 Ekle",
            Gc(91, 33, 182), Gc(124, 58, 237), Gc(167, 139, 250), !trimBusy_, true);

        // Clip drawing strictly inside scrollable track viewport (allowing slight breath room left of 0:00)
        g.SetClip(Gdiplus::Rect(gm.hdrW + 1, gm.timelineTop,
            cw - gm.hdrW - 1, ch - gm.timelineTop));

        const double maxScroll = std::max(0.0, totalDur + 4.0 - (gm.tlViewRc.right - gm.tlViewRc.left) / studioPxPerSec_);
        studioTimelineScrollSec_ = std::max(0.0, std::min(studioTimelineScrollSec_, maxScroll));
        auto secToX = [&](double s) -> float {
            return static_cast<float>(gm.tlViewRc.left + (s - studioTimelineScrollSec_) * studioPxPerSec_);
        };

        // Time Ruler ticks
        int stepSec = (studioPxPerSec_ >= 100.0) ? 1
                    : (studioPxPerSec_ >= 48.0 ? 2 : (studioPxPerSec_ >= 20.0 ? 5 : 10));
        int maxSecTick = static_cast<int>(std::ceil(studioTimelineScrollSec_ + (gm.tlViewRc.right - gm.tlViewRc.left) / studioPxPerSec_)) + 2;
        Gdiplus::Pen pTick(Gc(58, 70, 96), 1.0f);
        for (int s = 0; s <= maxSecTick; ++s) {
            float x = secToX(static_cast<double>(s));
            if (x < gm.tlViewRc.left - 20) continue;
            bool major = (s % stepSec == 0);
            g.DrawLine(&pTick, x, (float)(gm.timelineTop + (major ? S(14) : S(20))), x, (float)(gm.timelineTop + gm.rulerH));
            if (major) {
                wchar_t tb[16] = {};
                swprintf(tb, 16, L"%d:%02d", s / 60, s % 60);
                g.DrawString(tb, -1, &fTiny, Gdiplus::RectF(x + 2, (float)(gm.timelineTop + 1), 40.0f, (float)S(14)), &sfLeft, &brMuted);
            }
        }

        // Video (V1) and Audio (A1) clip blocks with Anlık Klip filmstrip, live L/R cut preview & visible grip handles
        bool hasTrimBadge = false;
        float trimBadgeX = 0.0f;
        int trimBadgeTrackY = gm.v1Top;
        std::wstring trimBadgeText;

        double curOut = 0.0;
        for (size_t i = 0; i < studioClips_.size(); ++i) {
            const auto& c = studioClips_[i];
            double outDur = std::max(0.1, c.trimEndSec - c.trimStartSec) / std::max(0.5, c.speed);
            double drawStartSec = curOut;
            double drawEndSec = curOut + outDur;
            const bool isDraggingThis = ((studioDragMode_ == 2 || studioDragMode_ == 3) && studioDragIdx_ == static_cast<int>(i));

            if (isDraggingThis) {
                double origStart = studioDragOrigB_;
                double origEnd = studioDragOrigB_ + studioDragOrigC_;
                if (studioDragMode_ == 2) {
                    double cutDelta = (c.trimStartSec - studioDragOrigA_) / std::max(0.5, c.speed);
                    double rawStart = origStart + cutDelta;
                    if (rawStart >= 0.0) {
                        drawStartSec = rawStart;
                        drawEndSec = std::max(drawStartSec + 0.1, origEnd);
                    } else {
                        drawStartSec = 0.0;
                        drawEndSec = std::max(0.1, origEnd - rawStart);
                    }
                    if (drawStartSec > origStart + 0.01) {
                        float gx0 = secToX(origStart);
                        float gx1 = secToX(drawStartSec);
                        float gw = std::max(2.0f, gx1 - gx0);
                        FillRoundRect(g, gx0 + 1.0f, (float)gm.v1Top, gw - 1.0f, (float)gm.v1H, (float)S(5), Gc(38, 22, 34, 175));
                        StrokeRoundRect(g, gx0 + 1.0f, (float)gm.v1Top, gw - 1.0f, (float)gm.v1H, (float)S(5), Gc(248, 113, 113, 130), 1.0f);
                        FillRoundRect(g, gx0 + 1.0f, (float)gm.a1Top, gw - 1.0f, (float)gm.a1H, (float)S(5), Gc(38, 22, 34, 150));
                        if (gw >= S(46)) {
                            wchar_t cutLbl[32] = {};
                            swprintf(cutLbl, 32, L"-%.1fs", drawStartSec - origStart);
                            g.DrawString(cutLbl, -1, &fTiny,
                                Gdiplus::RectF(gx0 + 3.0f, (float)gm.v1Top, gw - 6.0f, (float)gm.v1H), &sfCenter, &brMuted);
                        }
                    }
                    hasTrimBadge = true;
                    trimBadgeX = secToX(drawStartSec);
                    trimBadgeTrackY = gm.v1Top;
                    wchar_t btxt[64] = {};
                    double deltaSrc = c.trimStartSec - studioDragOrigA_;
                    swprintf(btxt, 64, L"Baş: %ls  (%+.1fs)", FormatSecTenths(c.trimStartSec).c_str(), deltaSrc);
                    trimBadgeText = btxt;
                } else {
                    drawStartSec = origStart;
                    drawEndSec = origStart + outDur;
                    if (drawEndSec < origEnd - 0.01) {
                        float gx0 = secToX(drawEndSec);
                        float gx1 = secToX(origEnd);
                        float gw = std::max(2.0f, gx1 - gx0);
                        FillRoundRect(g, gx0, (float)gm.v1Top, gw - 1.0f, (float)gm.v1H, (float)S(5), Gc(38, 22, 34, 175));
                        StrokeRoundRect(g, gx0, (float)gm.v1Top, gw - 1.0f, (float)gm.v1H, (float)S(5), Gc(248, 113, 113, 130), 1.0f);
                        FillRoundRect(g, gx0, (float)gm.a1Top, gw - 1.0f, (float)gm.a1H, (float)S(5), Gc(38, 22, 34, 150));
                        if (gw >= S(46)) {
                            wchar_t cutLbl[32] = {};
                            swprintf(cutLbl, 32, L"-%.1fs", origEnd - drawEndSec);
                            g.DrawString(cutLbl, -1, &fTiny,
                                Gdiplus::RectF(gx0 + 3.0f, (float)gm.v1Top, gw - 6.0f, (float)gm.v1H), &sfCenter, &brMuted);
                        }
                    }
                    hasTrimBadge = true;
                    trimBadgeX = secToX(drawEndSec);
                    trimBadgeTrackY = gm.v1Top;
                    wchar_t btxt[64] = {};
                    if (c.trimEndSec >= c.sourceDurSec - 0.01) {
                        swprintf(btxt, 64, L"Son: %ls (Tam Süre)", FormatSecTenths(c.trimEndSec).c_str());
                    } else {
                        swprintf(btxt, 64, L"Son: %ls  (Süre %.1fs)", FormatSecTenths(c.trimEndSec).c_str(), outDur);
                    }
                    trimBadgeText = btxt;
                }
                curOut = std::max(origEnd, drawEndSec);
            } else {
                curOut += outDur;
            }

            float bx = secToX(drawStartSec);
            float bxRight = secToX(drawEndSec);
            float bw = std::max<float>((float)S(16), bxRight - bx);
            bool sel = (studioSelClip_ == static_cast<int>(i));

            // V1 block with subtle filmstrip thumbnail tiling + Anlık Klip dark-blue gradient overlay
            FillRoundRectGrad(g, bx + 1.0f, (float)gm.v1Top, bw - 2.0f, (float)gm.v1H, (float)S(7),
                sel ? Gc(34, 62, 120) : Gc(22, 31, 52),
                sel ? Gc(20, 38, 78) : Gc(15, 21, 36));
            if (bw >= S(36)) {
                auto it = thumbBmps_.find(c.path);
                if (it != thumbBmps_.end() && it->second) {
                    Gdiplus::Bitmap bmp(it->second, nullptr);
                    Gdiplus::Region prevClip;
                    g.GetClip(&prevClip);
                    g.SetClip(Gdiplus::RectF(bx + 2.0f, (float)(gm.v1Top + 2), std::max(1.0f, bw - 4.0f), (float)(gm.v1H - 4)), Gdiplus::CombineModeIntersect);
                    int tileW = std::max(S(56), (gm.v1H * 16) / 9);
                    for (float tx = bx + 2.0f; tx < bx + bw - 2.0f; tx += static_cast<float>(tileW + 2)) {
                        g.DrawImage(&bmp, tx, static_cast<Gdiplus::REAL>(gm.v1Top + 2),
                            static_cast<Gdiplus::REAL>(tileW), static_cast<Gdiplus::REAL>(gm.v1H - 4));
                    }
                    g.SetClip(&prevClip);
                    FillRoundRectGrad(g, bx + 2.0f, (float)(gm.v1Top + 2), bw - 4.0f, (float)(gm.v1H - 4), (float)S(6),
                        Gc(12, 18, 34, 200), Gc(15, 22, 40, 145));
                }
            }
            StrokeRoundRect(g, bx + 1.0f, (float)gm.v1Top, bw - 2.0f, (float)gm.v1H, (float)S(7),
                sel ? Gc(56, 189, 248) : Gc(59, 130, 246, 125), sel ? 1.8f : 1.0f);

            float textLeft = bx + S(11);
            wchar_t titleLine[160] = {};
            swprintf(titleLine, 160, L"#%zu  %ls", i + 1, c.name.c_str());
            g.DrawString(titleLine, -1, &fTiny,
                Gdiplus::RectF(textLeft, (float)(gm.v1Top + S(4)), std::max(8.0f, bw - S(24)), (float)S(16)), &sfLeft, &brText);
            wchar_t meta[48] = {};
            swprintf(meta, 48, L"%.1fs • %.1fx", outDur, c.speed);
            if (bw >= S(54)) {
                FillRoundRect(g, textLeft, (float)(gm.v1Top + gm.v1H - S(21)), std::min<float>(bw - S(24), (float)S(68)), (float)S(16), (float)S(4), Gc(12, 18, 32, 205));
                g.DrawString(meta, -1, &fTiny,
                    Gdiplus::RectF(textLeft + 3.0f, (float)(gm.v1Top + gm.v1H - S(21)), std::max(8.0f, bw - S(28)), (float)S(16)), &sfLeft, &brCyan);
            }

            // Always-visible L/R trim grip caps on V1 & linked edge highlights on A1
            const bool activeL = (isDraggingThis && studioDragMode_ == 2) ||
                                 (studioHoverClipIdx_ == static_cast<int>(i) && studioHoverClipSide_ < 0);
            const bool activeR = (isDraggingThis && studioDragMode_ == 3) ||
                                 (studioHoverClipIdx_ == static_cast<int>(i) && studioHoverClipSide_ > 0);
            if (studioFade_ && i > 0) {
                FillRoundRectGrad(g, bx - S(6), (float)(gm.v1Top + gm.v1H / 2 - S(8)), (float)S(12), (float)S(16), 4.0f,
                    Gc(59, 130, 246), Gc(124, 58, 237));
            }

            // A1 Clip Audio Waveform block directly underneath V1 (symmetrical Anlık Klip neon wave)
            const bool aMuted = (c.gameVolPct == 0 && (!c.hasMultiTrack || c.micVolPct == 0));
            FillRoundRectGrad(g, bx + 1.0f, (float)gm.a1Top, bw - 2.0f, (float)gm.a1H, (float)S(6),
                aMuted ? Gc(44, 18, 26) : (sel ? Gc(14, 56, 72) : Gc(12, 36, 48)),
                aMuted ? Gc(28, 12, 18) : (sel ? Gc(10, 38, 52) : Gc(10, 26, 36)));
            StrokeRoundRect(g, bx + 1.0f, (float)gm.a1Top, bw - 2.0f, (float)gm.a1H, (float)S(6),
                aMuted ? Gc(220, 38, 38) : (sel ? Gc(45, 212, 191) : Gc(28, 96, 114)), sel ? 1.5f : 1.0f);

            if (!aMuted && !c.wavePeaks.empty() && c.sourceDurSec > 0.1 && bw > S(14)) {
                int bars = std::max(2, static_cast<int>((bw - 8.0f) / 3.0f));
                double u0 = std::max(0.0, std::min(1.0, c.trimStartSec / c.sourceDurSec));
                double u1 = std::max(u0, std::min(1.0, c.trimEndSec / c.sourceDurSec));
                float gainScale = std::max(0.15f, std::min(1.8f,
                    static_cast<float>(c.hasMultiTrack ? std::max(c.gameVolPct, c.micVolPct) : c.gameVolPct) / 100.0f));
                Gdiplus::SolidBrush brWave(Gc(45, 212, 191, 205));
                float midY = static_cast<float>(gm.a1Top + gm.a1H / 2 + S(4));
                float maxHalf = static_cast<float>(gm.a1H - S(16)) * 0.5f;
                for (int b = 0; b < bars; ++b) {
                    double frac = u0 + (u1 - u0) * (static_cast<double>(b) / bars);
                    size_t pidx = std::min(c.wavePeaks.size() - 1, static_cast<size_t>(frac * c.wavePeaks.size()));
                    float amp = std::min(1.0f, c.wavePeaks[pidx] * gainScale);
                    float hh = std::max(1.5f, amp * maxHalf);
                    g.FillRectangle(&brWave, bx + 4.0f + b * 3.0f, midY - hh, 2.0f, hh * 2.0f);
                }
            }
            wchar_t albl[32] = {};
            if (aMuted) swprintf(albl, 32, L"Ses Kapalı");
            else if (c.hasMultiTrack) swprintf(albl, 32, L"Oyun %%%d • Mic %%%d", c.gameVolPct, c.micVolPct);
            else swprintf(albl, 32, L"Klip Sesi %%%d", c.gameVolPct);
            g.DrawString(albl, -1, &fTiny,
                Gdiplus::RectF(bx + S(9), (float)(gm.a1Top + 2), std::max(8.0f, bw - S(18)), (float)S(15)), &sfLeft, &brText);

            if (bw >= S(16)) {
                auto drawV1Grip = [&](float gx, float gw, bool bright) {
                    Gdiplus::Color col = bright ? Gc(186, 230, 253)
                        : (sel ? Gc(56, 189, 248, 225) : Gc(148, 163, 184, 130));
                    FillRoundRect(g, gx, (float)(gm.v1Top + S(4)), gw, (float)(gm.v1H - S(8)), 2.5f, col);
                    if (bright || sel) {
                        float mxLine = gx + gw * 0.5f;
                        Gdiplus::Pen pNotch(Gc(12, 24, 48, 225), 1.4f);
                        g.DrawLine(&pNotch, mxLine, (float)(gm.v1Top + gm.v1H / 2 - S(7)),
                            mxLine, (float)(gm.v1Top + gm.v1H / 2 + S(7)));
                    }
                };
                float gwl = activeL ? (float)S(8) : (sel ? (float)S(6) : (float)S(4));
                float gwr = activeR ? (float)S(8) : (sel ? (float)S(6) : (float)S(4));
                drawV1Grip(bx + 2.0f, gwl, activeL);
                drawV1Grip(bx + bw - 2.0f - gwr, gwr, activeR);
                if (activeL || sel) {
                    FillRoundRect(g, bx + 2.0f, (float)(gm.a1Top + S(4)), gwl * 0.7f, (float)(gm.a1H - S(8)), 2.0f,
                        activeL ? Gc(186, 230, 253) : Gc(45, 212, 191, 175));
                }
                if (activeR || sel) {
                    FillRoundRect(g, bx + bw - 2.0f - gwr * 0.7f, (float)(gm.a1Top + S(4)), gwr * 0.7f, (float)(gm.a1H - S(8)), 2.0f,
                        activeR ? Gc(186, 230, 253) : Gc(45, 212, 191, 175));
                }
            }
        }

        // M1 Music / MP3 blocks placed anywhere along timeline
        if (studioMusic_.empty()) {
            g.DrawString(L"Müzik yok — Soldaki '+ MP3 Ekle'ye basın, ekledikten sonra müzik şeridini istediğiniz saniyeye sürükleyin",
                -1, &fTiny,
                Gdiplus::RectF((float)(gm.tlViewRc.left + S(12)), (float)gm.m1Top,
                    (float)std::max(S(200), static_cast<int>(gm.tlViewRc.right - gm.tlViewRc.left) - S(24)), (float)gm.m1H),
                &sfLeft, &brMuted);
        } else {
            for (size_t i = 0; i < studioMusic_.size(); ++i) {
                const auto& m = studioMusic_[i];
                float bx = secToX(m.timelineStartSec);
                float bw = std::max<float>((float)S(20), static_cast<float>(m.durationSec * studioPxPerSec_));
                bool sel = (studioSelMusic_ == static_cast<int>(i));

                FillRoundRectGrad(g, bx + 1.0f, (float)gm.m1Top, bw - 2.0f, (float)gm.m1H, (float)S(6),
                    sel ? Gc(109, 40, 217) : Gc(68, 28, 134),
                    sel ? Gc(76, 29, 149) : Gc(42, 18, 86));
                StrokeRoundRect(g, bx + 1.0f, (float)gm.m1Top, bw - 2.0f, (float)gm.m1H, (float)S(6),
                    sel ? Gc(216, 180, 254) : Gc(139, 92, 246), sel ? 1.8f : 1.0f);

                if (!m.wavePeaks.empty() && m.sourceDurSec > 0.1 && bw > S(14) && m.volPct > 0) {
                    int bars = std::max(2, static_cast<int>((bw - 8.0f) / 3.0f));
                    double u0 = std::max(0.0, std::min(1.0, m.trimStartSec / m.sourceDurSec));
                    double u1 = std::max(u0, std::min(1.0, (m.trimStartSec + m.durationSec) / m.sourceDurSec));
                    float gScale = std::max(0.15f, std::min(1.8f, static_cast<float>(m.volPct) / 100.0f));
                    Gdiplus::SolidBrush brMw(Gc(224, 195, 255, 200));
                    float midY = static_cast<float>(gm.m1Top + gm.m1H / 2 + S(4));
                    float maxHalf = static_cast<float>(gm.m1H - S(16)) * 0.5f;
                    for (int b = 0; b < bars; ++b) {
                        double frac = u0 + (u1 - u0) * (static_cast<double>(b) / bars);
                        size_t pidx = std::min(m.wavePeaks.size() - 1, static_cast<size_t>(frac * m.wavePeaks.size()));
                        float amp = std::min(1.0f, m.wavePeaks[pidx] * gScale);
                        float hh = std::max(1.5f, amp * maxHalf);
                        g.FillRectangle(&brMw, bx + 4.0f + b * 3.0f, midY - hh, 2.0f, hh * 2.0f);
                    }
                }
                // Draw subtle fade-in / fade-out slopes on corners
                if (bw >= S(36)) {
                    Gdiplus::Pen pFade(Gc(243, 232, 255, 190), 1.4f);
                    float fW = std::min<float>(bw * 0.18f, (float)S(18));
                    if (m.fadeIn)  g.DrawLine(&pFade, bx + 3.0f, (float)(gm.m1Top + gm.m1H - 3), bx + 3.0f + fW, (float)(gm.m1Top + S(14)));
                    if (m.fadeOut) g.DrawLine(&pFade, bx + bw - 3.0f - fW, (float)(gm.m1Top + S(14)), bx + bw - 3.0f, (float)(gm.m1Top + gm.m1H - 3));
                }
                const bool dragThisM = ((studioDragMode_ == 5 || studioDragMode_ == 6) && studioDragIdx_ == static_cast<int>(i));
                const bool activeML = (dragThisM && studioDragMode_ == 5) ||
                                      (studioHoverMusicIdx_ == static_cast<int>(i) && studioHoverMusicSide_ < 0);
                const bool activeMR = (dragThisM && studioDragMode_ == 6) ||
                                      (studioHoverMusicIdx_ == static_cast<int>(i) && studioHoverMusicSide_ > 0);
                if (bw >= S(16)) {
                    float mgl = activeML ? (float)S(7) : (sel ? (float)S(6) : (float)S(4));
                    float mgr = activeMR ? (float)S(7) : (sel ? (float)S(6) : (float)S(4));
                    FillRoundRect(g, bx + 2.0f, (float)(gm.m1Top + S(5)), mgl, (float)(gm.m1H - S(10)), 2.5f,
                        activeML ? Gc(250, 245, 255) : (sel ? Gc(233, 213, 255) : Gc(196, 181, 253, 130)));
                    FillRoundRect(g, bx + bw - 2.0f - mgr, (float)(gm.m1Top + S(5)), mgr, (float)(gm.m1H - S(10)), 2.5f,
                        activeMR ? Gc(250, 245, 255) : (sel ? Gc(233, 213, 255) : Gc(196, 181, 253, 130)));
                }
                if (dragThisM) {
                    hasTrimBadge = true;
                    trimBadgeX = (studioDragMode_ == 5) ? bx : (bx + bw);
                    trimBadgeTrackY = gm.m1Top;
                    wchar_t mtxt[64] = {};
                    swprintf(mtxt, 64, L"Müzik Süre: %.1fs", m.durationSec);
                    trimBadgeText = mtxt;
                }
                wchar_t mlbl[160] = {};
                swprintf(mlbl, 160, L"♪ %ls (@%.1fs • %%%d)", m.name.c_str(), m.timelineStartSec, m.volPct);
                g.DrawString(mlbl, -1, &fTiny,
                    Gdiplus::RectF(bx + S(9), (float)(gm.m1Top + 2), std::max(8.0f, bw - S(18)), (float)S(16)), &sfLeft, &brText);
            }
        }

        // Playhead Needle + Anlık Klip Neon Cyan Scrubber Head
        float pxHead = secToX(studioPlayheadSec_);
        if (pxHead >= gm.tlViewRc.left - 2 && pxHead <= gm.tlViewRc.right + 8) {
            Gdiplus::Pen pPlayHalo(Gc(56, 189, 248, 85), 5.0f);
            g.DrawLine(&pPlayHalo, pxHead, (float)(gm.timelineTop + S(12)), pxHead, (float)ch);
            Gdiplus::Pen pPlay(Gc(56, 189, 248), 2.0f);
            g.DrawLine(&pPlay, pxHead, (float)(gm.timelineTop + S(12)), pxHead, (float)ch);
        }
        g.ResetClip();
        if (pxHead >= gm.tlViewRc.left - 2 && pxHead <= gm.tlViewRc.right + 8) {
            FillRoundRectGrad(g, pxHead - S(6), (float)(gm.timelineTop + S(2)), (float)S(12), (float)S(14), 4.0f,
                Gc(125, 211, 252), Gc(37, 99, 235));
            StrokeRoundRect(g, pxHead - S(6), (float)(gm.timelineTop + S(2)), (float)S(12), (float)S(14), 4.0f, Gc(224, 242, 254));
        }
        if (maxScroll > 0.05) {
            double visSecView = std::max(1.0, static_cast<double>(gm.tlViewRc.right - gm.tlViewRc.left) / studioPxPerSec_);
            double totalTrackSec = std::max(totalDur + 4.0, studioTimelineScrollSec_ + visSecView);
            float trackW = static_cast<float>(gm.tlViewRc.right - gm.tlViewRc.left);
            float thumbW = std::max<float>((float)S(28), trackW * static_cast<float>(visSecView / totalTrackSec));
            float thumbX = static_cast<float>(gm.tlViewRc.left) +
                (trackW - thumbW) * static_cast<float>(studioTimelineScrollSec_ / maxScroll);
            FillRoundRect(g, (float)gm.tlViewRc.left, (float)(ch - S(5)), trackW, (float)S(3), 1.5f, Gc(30, 38, 56, 150));
            FillRoundRect(g, thumbX, (float)(ch - S(5)), thumbW, (float)S(3), 1.5f, Gc(56, 189, 248, 190));
        }
        if (hasTrimBadge && !trimBadgeText.empty()) {
            float pillW = (float)S(144);
            float pillH = (float)S(20);
            float px = std::max<float>((float)gm.hdrW + 4.0f, std::min<float>((float)(cw - S(152)), trimBadgeX - pillW * 0.5f));
            float py = (float)(trimBadgeTrackY - S(22));
            FillRoundRect(g, px, py, pillW, pillH, 5.0f, Gc(12, 18, 34, 242));
            StrokeRoundRect(g, px, py, pillW, pillH, 5.0f, Gc(56, 189, 248), 1.2f);
            g.DrawString(trimBadgeText.c_str(), -1, &fTiny, Gdiplus::RectF(px, py, pillW, pillH), &sfCenter, &brCyan);
        }
    }
}

bool MainWindow::OnStudioLButtonDown(int mx, int my) {
    if (!studioOpen_ || activeTab_ != 1) return false;
    RECT cli{}; GetClientRect(hwnd_, &cli);
    const StudioGeometry gm = CalcStudioGeom(cli.right, cli.bottom, static_cast<int>(dpi_));

    int bid = HitTestCustomButton(mx, my);
    if (bid != 0) {
        pressedBtnId_ = bid;
        SetCapture(hwnd_);
        InvalidateRect(hwnd_, nullptr, FALSE);
        return true;
    }

    if (trimBusy_) return true;

    // 1. Tool bar volume sliders
    bool hasMusSel = (studioSelMusic_ >= 0 && studioSelMusic_ < static_cast<int>(studioMusic_.size()));
    bool hasClipSel = (studioSelClip_ >= 0 && studioSelClip_ < static_cast<int>(studioClips_.size()));
    if (hasMusSel && mx >= gm.musSlRc.left - S(4) && mx <= gm.musSlRc.right + S(4) && my >= gm.musSlRc.top - S(4) && my <= gm.musSlRc.bottom + S(4)) {
        studioDragMode_ = 9;
        SetCapture(hwnd_);
        double r = std::max(0.0, std::min(1.0, static_cast<double>(mx - gm.musSlRc.left) / std::max(1L, gm.musSlRc.right - gm.musSlRc.left)));
        studioMusic_[static_cast<size_t>(studioSelMusic_)].volPct = static_cast<int>(std::lround(r * 200.0));
        SyncStudioMediaState(false);
        InvalidateRect(hwnd_, nullptr, FALSE);
        return true;
    }
    if (!hasMusSel && (hasClipSel || !studioClips_.empty())) {
        int ci = hasClipSel ? studioSelClip_ : 0;
        studioSelClip_ = ci;
        if (mx >= gm.gameSlRc.left - S(4) && mx <= gm.gameSlRc.right + S(4) && my >= gm.gameSlRc.top - S(4) && my <= gm.gameSlRc.bottom + S(4)) {
            studioDragMode_ = 7;
            studioDragIdx_ = ci;
            SetCapture(hwnd_);
            double r = std::max(0.0, std::min(1.0, static_cast<double>(mx - gm.gameSlRc.left) / std::max(1L, gm.gameSlRc.right - gm.gameSlRc.left)));
            studioClips_[static_cast<size_t>(ci)].gameVolPct = static_cast<int>(std::lround(r * 200.0));
            SyncStudioMediaState(false);
            InvalidateRect(hwnd_, nullptr, FALSE);
            return true;
        }
        if (studioClips_[static_cast<size_t>(ci)].hasMultiTrack &&
            mx >= gm.micSlRc.left - S(4) && mx <= gm.micSlRc.right + S(4) && my >= gm.micSlRc.top - S(4) && my <= gm.micSlRc.bottom + S(4)) {
            studioDragMode_ = 8;
            studioDragIdx_ = ci;
            SetCapture(hwnd_);
            double r = std::max(0.0, std::min(1.0, static_cast<double>(mx - gm.micSlRc.left) / std::max(1L, gm.micSlRc.right - gm.micSlRc.left)));
            studioClips_[static_cast<size_t>(ci)].micVolPct = static_cast<int>(std::lround(r * 200.0));
            SyncStudioMediaState(false);
            InvalidateRect(hwnd_, nullptr, FALSE);
            return true;
        }
    }

    // 2. Timeline tracks & ruler hit-testing (generous left-margin tolerance for edge handle at 0:00)
    if (mx >= gm.tlViewRc.left - S(10) && my >= gm.timelineTop) {
        auto xToSec = [&](int x) {
            return std::max(0.0, studioTimelineScrollSec_ + static_cast<double>(x - gm.tlViewRc.left) / studioPxPerSec_);
        };
        auto secToX = [&](double s) {
            return static_cast<int>(std::lround(gm.tlViewRc.left + (s - studioTimelineScrollSec_) * studioPxPerSec_));
        };

        int hKind = 0, hIdx = -1, hSide = 0;
        double hAcc = 0.0;
        if (StudioHitTestTrimEdge(mx, my, cli.right, cli.bottom, hKind, hIdx, hSide, hAcc)) {
            if (hKind == 1 && hIdx >= 0 && hIdx < static_cast<int>(studioClips_.size())) {
                const auto& c = studioClips_[static_cast<size_t>(hIdx)];
                double outDur = std::max(0.1, c.trimEndSec - c.trimStartSec) / std::max(0.5, c.speed);
                studioSelClip_ = hIdx;
                studioSelMusic_ = -1;
                studioDragMode_ = (hSide < 0) ? 2 : 3;
                studioDragIdx_ = hIdx;
                studioDragStartX_ = mx;
                studioDragOrigA_ = (hSide < 0) ? c.trimStartSec : c.trimEndSec;
                studioDragOrigB_ = hAcc;
                studioDragOrigC_ = outDur;
                studioPlaying_ = false;
                studioPlayheadSec_ = (hSide < 0) ? hAcc : (hAcc + std::max(0.02, outDur - 0.04));
                SetCapture(hwnd_);
                SetCursor(LoadCursorW(nullptr, IDC_SIZEWE));
                SyncStudioMediaState(true);
                InvalidateRect(hwnd_, nullptr, FALSE);
                return true;
            }
            if (hKind == 2 && hIdx >= 0 && hIdx < static_cast<int>(studioMusic_.size())) {
                const auto& m = studioMusic_[static_cast<size_t>(hIdx)];
                studioSelMusic_ = hIdx;
                studioSelClip_ = -1;
                studioDragIdx_ = hIdx;
                studioDragStartX_ = mx;
                studioDragOrigA_ = m.timelineStartSec;
                studioDragOrigB_ = m.durationSec;
                studioDragOrigC_ = m.trimStartSec;
                studioDragMode_ = (hSide < 0) ? 5 : 6;
                SetCapture(hwnd_);
                SetCursor(LoadCursorW(nullptr, IDC_SIZEWE));
                InvalidateRect(hwnd_, nullptr, FALSE);
                return true;
            }
        }

        if (my <= gm.timelineTop + gm.rulerH) {
            studioDragMode_ = 1;
            studioDragWasPlaying_ = studioPlaying_;
            studioPlaying_ = false;
            SetCapture(hwnd_);
            StudioSeekTimeline(xToSec(mx), false);
            return true;
        }

        // Check V1 & A1 clip body selection & seek
        if (my >= gm.v1Top && my <= gm.a1Top + gm.a1H) {
            double acc = 0.0;
            for (size_t i = 0; i < studioClips_.size(); ++i) {
                const auto& c = studioClips_[i];
                double outDur = std::max(0.1, c.trimEndSec - c.trimStartSec) / std::max(0.5, c.speed);
                int x0 = secToX(acc);
                int x1 = secToX(acc + outDur);
                int hitLeft = (i == 0) ? (x0 - S(4)) : x0;
                int hitRight = (i + 1 == studioClips_.size()) ? (x1 + S(4)) : (x1 - 1);
                if (mx >= hitLeft && mx <= hitRight) {
                    studioSelClip_ = static_cast<int>(i);
                    studioSelMusic_ = -1;
                    StudioSeekTimeline(xToSec(mx), true);
                    InvalidateRect(hwnd_, nullptr, FALSE);
                    return true;
                }
                acc += outDur;
            }
        }

        // Check M1 music body (horizontal drag)
        if (my >= gm.m1Top && my <= gm.m1Top + gm.m1H) {
            for (size_t i = 0; i < studioMusic_.size(); ++i) {
                const auto& m = studioMusic_[i];
                int x0 = secToX(m.timelineStartSec);
                int x1 = secToX(m.timelineStartSec + m.durationSec);
                if (mx >= x0 - S(2) && mx <= x1 + S(2)) {
                    studioSelMusic_ = static_cast<int>(i);
                    studioSelClip_ = -1;
                    studioDragIdx_ = static_cast<int>(i);
                    studioDragStartX_ = mx;
                    studioDragOrigA_ = m.timelineStartSec;
                    studioDragOrigB_ = m.durationSec;
                    studioDragOrigC_ = m.trimStartSec;
                    studioDragMode_ = 4;
                    SetCapture(hwnd_);
                    InvalidateRect(hwnd_, nullptr, FALSE);
                    return true;
                }
            }
        }

        // Clicking empty timeline moves playhead
        studioDragMode_ = 1;
        SetCapture(hwnd_);
        StudioSeekTimeline(xToSec(mx), false);
        return true;
    }

    // 3. Clicking a clip row in left panel also appends it cleanly
    if (mx >= S(10) && mx <= gm.leftW - S(10) && my >= gm.topH + S(48) && my < gm.timelineTop && studioLeftTab_ == 0) {
        int vi = (my - (gm.topH + S(48))) / S(52);
        int ci = studioLibScroll_ + vi;
        if (ci >= 0 && ci < static_cast<int>(clips_.size())) {
            StudioAddClipFromLibrary(static_cast<size_t>(ci));
            return true;
        }
    }
    return true;
}

bool MainWindow::OnStudioMouseMove(int mx, int my) {
    if (!studioOpen_ || activeTab_ != 1) return false;
    RECT cli{}; GetClientRect(hwnd_, &cli);
    if (studioDragMode_ == 0) {
        int hKind = 0, hIdx = -1, hSide = 0;
        double dummyAcc = 0.0;
        StudioHitTestTrimEdge(mx, my, cli.right, cli.bottom, hKind, hIdx, hSide, dummyAcc);
        int newCIdx = (hKind == 1) ? hIdx : -1;
        int newCSide = (hKind == 1) ? hSide : 0;
        int newMIdx = (hKind == 2) ? hIdx : -1;
        int newMSide = (hKind == 2) ? hSide : 0;
        if (newCIdx != studioHoverClipIdx_ || newCSide != studioHoverClipSide_ ||
            newMIdx != studioHoverMusicIdx_ || newMSide != studioHoverMusicSide_) {
            studioHoverClipIdx_ = newCIdx;
            studioHoverClipSide_ = newCSide;
            studioHoverMusicIdx_ = newMIdx;
            studioHoverMusicSide_ = newMSide;
            InvalidateRect(hwnd_, nullptr, FALSE);
        }
        UpdateHover(mx, my);
        return true;
    }
    const StudioGeometry gm = CalcStudioGeom(cli.right, cli.bottom, static_cast<int>(dpi_));

    if (studioDragMode_ == 1) {
        double target = std::max(0.0, studioTimelineScrollSec_ + static_cast<double>(mx - gm.tlViewRc.left) / studioPxPerSec_);
        ULONGLONG now = GetTickCount64();
        bool doScrub = (now - studioLastScrubMs_ >= 85);
        if (doScrub) studioLastScrubMs_ = now;
        StudioSeekTimeline(target, doScrub);
        return true;
    }
    if (studioDragMode_ == 2 && studioDragIdx_ >= 0 && studioDragIdx_ < static_cast<int>(studioClips_.size())) {
        auto& c = studioClips_[static_cast<size_t>(studioDragIdx_)];
        double dt = (static_cast<double>(mx - studioDragStartX_) / studioPxPerSec_) * c.speed;
        c.trimStartSec = std::max(0.0, std::min(c.trimEndSec - 0.25, studioDragOrigA_ + dt));
        double cutDelta = (c.trimStartSec - studioDragOrigA_) / std::max(0.5, c.speed);
        studioPlayheadSec_ = std::max(0.0, studioDragOrigB_ + cutDelta);
        ULONGLONG now = GetTickCount64();
        if (now - studioLastScrubMs_ >= 75) { studioLastScrubMs_ = now; SyncStudioMediaState(true); }
        InvalidateRect(hwnd_, nullptr, FALSE);
        return true;
    }
    if (studioDragMode_ == 3 && studioDragIdx_ >= 0 && studioDragIdx_ < static_cast<int>(studioClips_.size())) {
        auto& c = studioClips_[static_cast<size_t>(studioDragIdx_)];
        double dt = (static_cast<double>(mx - studioDragStartX_) / studioPxPerSec_) * c.speed;
        double rawEnd = studioDragOrigA_ + dt;
        c.trimEndSec = std::min(c.sourceDurSec, std::max(c.trimStartSec + 0.25, rawEnd));
        double outDur = (c.trimEndSec - c.trimStartSec) / std::max(0.5, c.speed);
        double rightEdgeSec = studioDragOrigB_ + outDur;
        double visSec = std::max(1.0, static_cast<double>(gm.tlViewRc.right - gm.tlViewRc.left - S(36)) / studioPxPerSec_);
        if (rightEdgeSec > studioTimelineScrollSec_ + visSec) {
            double shiftSec = rightEdgeSec - (studioTimelineScrollSec_ + visSec);
            studioTimelineScrollSec_ += shiftSec;
            studioDragStartX_ -= static_cast<int>(std::lround(shiftSec * studioPxPerSec_));
        }
        if (rawEnd > c.sourceDurSec + 0.10) {
            SetFooterMessage(L"Klip zaten maksimum süresinde — daha uzun yapmak için 'Hız' butonundan hızı düşürebilirsiniz.");
        }
        studioPlayheadSec_ = studioDragOrigB_ + std::max(0.02, outDur - 0.04);
        ULONGLONG now = GetTickCount64();
        if (now - studioLastScrubMs_ >= 75) { studioLastScrubMs_ = now; SyncStudioMediaState(true); }
        InvalidateRect(hwnd_, nullptr, FALSE);
        return true;
    }
    if (studioDragMode_ == 4 && studioDragIdx_ >= 0 && studioDragIdx_ < static_cast<int>(studioMusic_.size())) {
        auto& m = studioMusic_[static_cast<size_t>(studioDragIdx_)];
        double dt = static_cast<double>(mx - studioDragStartX_) / studioPxPerSec_;
        double cand = std::max(0.0, studioDragOrigA_ + dt);
        double snapThresh = 8.0 / studioPxPerSec_;
        if (std::abs(cand - 0.0) < snapThresh) cand = 0.0;
        if (std::abs(cand - studioPlayheadSec_) < snapThresh) cand = studioPlayheadSec_;
        double acc = 0.0;
        for (const auto& c : studioClips_) {
            acc += std::max(0.1, c.trimEndSec - c.trimStartSec) / std::max(0.5, c.speed);
            if (std::abs(cand - acc) < snapThresh) { cand = acc; break; }
        }
        m.timelineStartSec = cand;
        InvalidateRect(hwnd_, nullptr, FALSE);
        return true;
    }
    if (studioDragMode_ == 5 && studioDragIdx_ >= 0 && studioDragIdx_ < static_cast<int>(studioMusic_.size())) {
        auto& m = studioMusic_[static_cast<size_t>(studioDragIdx_)];
        double dt = static_cast<double>(mx - studioDragStartX_) / studioPxPerSec_;
        double minDt = std::max(-studioDragOrigA_, -studioDragOrigC_);
        double maxDt = studioDragOrigB_ - 0.3;
        dt = std::max(minDt, std::min(maxDt, dt));
        m.timelineStartSec = studioDragOrigA_ + dt;
        m.trimStartSec = studioDragOrigC_ + dt;
        m.durationSec = studioDragOrigB_ - dt;
        InvalidateRect(hwnd_, nullptr, FALSE);
        return true;
    }
    if (studioDragMode_ == 6 && studioDragIdx_ >= 0 && studioDragIdx_ < static_cast<int>(studioMusic_.size())) {
        auto& m = studioMusic_[static_cast<size_t>(studioDragIdx_)];
        double dt = static_cast<double>(mx - studioDragStartX_) / studioPxPerSec_;
        m.durationSec = std::max(0.3, std::min(m.sourceDurSec - m.trimStartSec, studioDragOrigB_ + dt));
        InvalidateRect(hwnd_, nullptr, FALSE);
        return true;
    }
    if (studioDragMode_ == 7 && studioDragIdx_ >= 0 && studioDragIdx_ < static_cast<int>(studioClips_.size())) {
        double r = std::max(0.0, std::min(1.0, static_cast<double>(mx - gm.gameSlRc.left) / std::max(1L, gm.gameSlRc.right - gm.gameSlRc.left)));
        studioClips_[static_cast<size_t>(studioDragIdx_)].gameVolPct = static_cast<int>(std::lround(r * 200.0));
        SyncStudioMediaState(false);
        InvalidateRect(hwnd_, nullptr, FALSE);
        return true;
    }
    if (studioDragMode_ == 8 && studioDragIdx_ >= 0 && studioDragIdx_ < static_cast<int>(studioClips_.size())) {
        double r = std::max(0.0, std::min(1.0, static_cast<double>(mx - gm.micSlRc.left) / std::max(1L, gm.micSlRc.right - gm.micSlRc.left)));
        studioClips_[static_cast<size_t>(studioDragIdx_)].micVolPct = static_cast<int>(std::lround(r * 200.0));
        SyncStudioMediaState(false);
        InvalidateRect(hwnd_, nullptr, FALSE);
        return true;
    }
    if (studioDragMode_ == 9 && studioSelMusic_ >= 0 && studioSelMusic_ < static_cast<int>(studioMusic_.size())) {
        double r = std::max(0.0, std::min(1.0, static_cast<double>(mx - gm.musSlRc.left) / std::max(1L, gm.musSlRc.right - gm.musSlRc.left)));
        studioMusic_[static_cast<size_t>(studioSelMusic_)].volPct = static_cast<int>(std::lround(r * 200.0));
        SyncStudioMediaState(false);
        InvalidateRect(hwnd_, nullptr, FALSE);
        return true;
    }
    return true;
}

bool MainWindow::OnStudioLButtonUp(int mx, int my) {
    if (!studioOpen_ || activeTab_ != 1) return false;
    if (studioDragMode_ != 0) {
        int m = studioDragMode_;
        int dIdx = studioDragIdx_;
        studioDragMode_ = 0;
        studioDragIdx_ = -1;
        studioHoverClipIdx_ = -1;
        studioHoverClipSide_ = 0;
        studioHoverMusicIdx_ = -1;
        studioHoverMusicSide_ = 0;
        ReleaseCapture();
        if (m == 2 && dIdx >= 0 && dIdx < static_cast<int>(studioClips_.size())) {
            studioPlayheadSec_ = std::max(0.0, std::min(StudioTotalDuration(), studioDragOrigB_));
        } else if (m == 3 && dIdx >= 0 && dIdx < static_cast<int>(studioClips_.size())) {
            const auto& c = studioClips_[static_cast<size_t>(dIdx)];
            double outDur = std::max(0.1, c.trimEndSec - c.trimStartSec) / std::max(0.5, c.speed);
            studioPlayheadSec_ = std::max(0.0, std::min(StudioTotalDuration(), studioDragOrigB_ + std::max(0.02, outDur - 0.04)));
        }
        RECT cli{}; GetClientRect(hwnd_, &cli);
        int hKind = 0, hIdx = -1, hSide = 0;
        double dummyAcc = 0.0;
        if (StudioHitTestTrimEdge(mx, my, cli.right, cli.bottom, hKind, hIdx, hSide, dummyAcc)) {
            if (hKind == 1) { studioHoverClipIdx_ = hIdx; studioHoverClipSide_ = hSide; }
            else if (hKind == 2) { studioHoverMusicIdx_ = hIdx; studioHoverMusicSide_ = hSide; }
        }
        if (m == 1 || m == 2 || m == 3) SyncStudioMediaState(true);
        else if (m == 4 || m == 5 || m == 6) SyncStudioMediaState(false);
        InvalidateRect(hwnd_, nullptr, FALSE);
        return true;
    }
    if (pressedBtnId_ != 0) {
        int target = pressedBtnId_;
        pressedBtnId_ = 0;
        ReleaseCapture();
        InvalidateRect(hwnd_, nullptr, FALSE);
        if (HitTestCustomButton(mx, my) == target) OnStudioButtonClick(target);
        return true;
    }
    return true;
}

bool MainWindow::OnStudioMouseWheel(int mx, int my, int delta, bool ctrl) {
    if (!studioOpen_ || activeTab_ != 1) return false;
    RECT cli{}; GetClientRect(hwnd_, &cli);
    const StudioGeometry gm = CalcStudioGeom(cli.right, cli.bottom, static_cast<int>(dpi_));
    if (my >= gm.timelineTop && mx >= gm.tlViewRc.left - S(10)) {
        if (ctrl) {
            double factor = (delta > 0) ? 1.18 : (1.0 / 1.18);
            studioPxPerSec_ = std::max(8.0, std::min(220.0, studioPxPerSec_ * factor));
        } else {
            double shift = ((delta > 0) ? -1.0 : 1.0) * (120.0 / studioPxPerSec_);
            studioTimelineScrollSec_ = std::max(0.0, studioTimelineScrollSec_ + shift);
        }
        InvalidateRect(hwnd_, nullptr, FALSE);
        return true;
    }
    if (mx < gm.leftW && studioLeftTab_ == 0) {
        studioLibScroll_ = std::max(0, studioLibScroll_ + ((delta > 0) ? -1 : 1));
        InvalidateRect(hwnd_, nullptr, FALSE);
        return true;
    }
    return true;
}

bool MainWindow::OnStudioButtonClick(int target) {
    if (target >= ui::BTN_STU_ADD_CLIP_BASE && target < ui::BTN_STU_ADD_MUSIC_BASE) {
        int ci = target - ui::BTN_STU_ADD_CLIP_BASE;
        if (ci >= 0 && ci < static_cast<int>(clips_.size())) StudioAddClipFromLibrary(static_cast<size_t>(ci));
        return true;
    }
    if (target >= ui::BTN_STU_ADD_MUSIC_BASE && target < ui::BTN_STU_ADD_MUSIC_BASE + 500) {
        int ri = target - ui::BTN_STU_ADD_MUSIC_BASE;
        if (ri >= 0 && ri < static_cast<int>(studioRecentAudio_.size())) {
            StudioAddMusicFile(studioRecentAudio_[static_cast<size_t>(ri)]);
        }
        return true;
    }

    switch (target) {
        case ui::BTN_STU_EXIT: CloseStudio(); return true;
        case ui::BTN_STU_RENDER: StartStudioRender(); return true;
        case ui::BTN_STU_CANCEL:
            trimCancel_.store(true);
            SetFooterMessage(L"Render iptal ediliyor...");
            return true;
        case ui::BTN_STU_ASPECT:
            if (!trimBusy_) {
                studioVertical916_ = !studioVertical916_;
                RECT cli{}; GetClientRect(hwnd_, &cli);
                UpdateStudioLayout(cli.right, cli.bottom);
                InvalidateRect(hwnd_, nullptr, FALSE);
            }
            return true;
        case ui::BTN_STU_FADE:
            if (!trimBusy_) { studioFade_ = !studioFade_; InvalidateRect(hwnd_, nullptr, FALSE); }
            return true;
        case ui::BTN_STU_TARGET:
            if (!trimBusy_) {
                if (studioTargetMb_ == 0) studioTargetMb_ = 10;
                else if (studioTargetMb_ == 10) studioTargetMb_ = 25;
                else studioTargetMb_ = 0;
                InvalidateRect(hwnd_, nullptr, FALSE);
            }
            return true;
        case ui::BTN_STU_LEFT_CLIPS:
            studioLeftTab_ = 0; InvalidateRect(hwnd_, nullptr, FALSE); return true;
        case ui::BTN_STU_LEFT_MUSIC:
            studioLeftTab_ = 1; InvalidateRect(hwnd_, nullptr, FALSE); return true;
        case ui::BTN_STU_PICK_MP3:
            StudioAddMusicFile(L""); return true;
        case ui::BTN_STU_PLAYPAUSE:
            StudioTogglePlayPause(); return true;
        case ui::BTN_STU_TO_START:
            StudioSeekTimeline(0.0, true); return true;
        case ui::BTN_STU_SPLIT:
            StudioSplitAtPlayhead(); return true;
        case ui::BTN_STU_DELETE:
            StudioDeleteSelection(); return true;
        case ui::BTN_STU_MOVE_LEFT:
            StudioMoveSelectedClip(-1); return true;
        case ui::BTN_STU_MOVE_RIGHT:
            StudioMoveSelectedClip(+1); return true;
        case ui::BTN_STU_SPEED:
            StudioCycleClipSpeed(); return true;
        case ui::BTN_STU_GAME_MUTE:
            if (studioSelClip_ >= 0 && studioSelClip_ < static_cast<int>(studioClips_.size())) {
                auto& c = studioClips_[static_cast<size_t>(studioSelClip_)];
                c.gameVolPct = (c.gameVolPct > 0) ? 0 : 100;
                SyncStudioMediaState(false);
                InvalidateRect(hwnd_, nullptr, FALSE);
            }
            return true;
        case ui::BTN_STU_MIC_MUTE:
            if (studioSelClip_ >= 0 && studioSelClip_ < static_cast<int>(studioClips_.size())) {
                auto& c = studioClips_[static_cast<size_t>(studioSelClip_)];
                c.micVolPct = (c.micVolPct > 0) ? 0 : 100;
                SyncStudioMediaState(false);
                InvalidateRect(hwnd_, nullptr, FALSE);
            }
            return true;
        case ui::BTN_STU_MUS_FADEIN:
            if (studioSelMusic_ >= 0 && studioSelMusic_ < static_cast<int>(studioMusic_.size())) {
                studioMusic_[static_cast<size_t>(studioSelMusic_)].fadeIn = !studioMusic_[static_cast<size_t>(studioSelMusic_)].fadeIn;
                InvalidateRect(hwnd_, nullptr, FALSE);
            }
            return true;
        case ui::BTN_STU_MUS_FADEOUT:
            if (studioSelMusic_ >= 0 && studioSelMusic_ < static_cast<int>(studioMusic_.size())) {
                studioMusic_[static_cast<size_t>(studioSelMusic_)].fadeOut = !studioMusic_[static_cast<size_t>(studioSelMusic_)].fadeOut;
                InvalidateRect(hwnd_, nullptr, FALSE);
            }
            return true;
        case ui::BTN_STU_ZOOM_IN:
            studioPxPerSec_ = std::min(220.0, studioPxPerSec_ * 1.25);
            InvalidateRect(hwnd_, nullptr, FALSE);
            return true;
        case ui::BTN_STU_ZOOM_OUT:
            studioPxPerSec_ = std::max(8.0, studioPxPerSec_ / 1.25);
            InvalidateRect(hwnd_, nullptr, FALSE);
            return true;
        case ui::BTN_STU_ZOOM_FIT: {
            RECT cli{}; GetClientRect(hwnd_, &cli);
            StudioFitZoomToView(cli.right, cli.bottom);
            InvalidateRect(hwnd_, nullptr, FALSE);
            return true;
        }
    }
    return false;
}

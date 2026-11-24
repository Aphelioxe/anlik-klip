#pragma once

#include <windows.h>
#include <objidl.h>
#include <gdiplus.h>
#ifdef min
#undef min
#endif
#ifdef max
#undef max
#endif

#include <algorithm>
#include <cstdint>
#include <string>

namespace ui {

inline constexpr UINT_PTR TIMER_PLAYER_ID        = 2;
inline constexpr UINT_PTR TIMER_MIC_METER_ID     = 3;
inline constexpr UINT_PTR TIMER_UI_ANIM_ID       = 4;

inline constexpr UINT WM_MW_THUMB_READY          = WM_APP + 20;
inline constexpr UINT WM_MW_PLAYER_EVENT         = WM_APP + 21;
inline constexpr UINT WM_MW_TRIM_DONE            = WM_APP + 22;
inline constexpr UINT WM_MW_UPLOAD_PROGRESS      = WM_APP + 24;
inline constexpr UINT WM_MW_UPLOAD_DONE          = WM_APP + 25;
inline constexpr UINT WM_MW_WAVE_READY           = WM_APP + 26;
inline constexpr UINT WM_MW_SCRUB_READY          = WM_APP + 27;
inline constexpr UINT WM_MW_STUDIO_VIDEO_EV      = WM_APP + 28;
inline constexpr UINT WM_MW_STUDIO_AUDIO_EV      = WM_APP + 29;
inline constexpr UINT WM_MW_STUDIO_WAVE_READY    = WM_APP + 30;

struct TrimDoneMsg {
    bool ok = false;
    bool replaceOriginal = false;
    std::wstring origPath;
    std::wstring outPath;
    std::wstring message;
};

enum CustomBtnId {
    BTN_TAB_CLIPS = 1,
    BTN_TAB_MONTAGE,
    BTN_TAB_SETTINGS,
    BTN_SIDE_SAVE,
    BTN_SIDE_TOGGLE,
    BTN_SIDE_MIC,
    BTN_TOP_FAV_FILTER,
    BTN_TOP_CLEAR_SEARCH,
    BTN_TOP_FOLDER,
    BTN_TOP_REFRESH,
    BTN_CLIP_PLAY,
    BTN_CLIP_FAV,
    BTN_CLIP_SHARE,
    BTN_CLIP_COPY_LINK,
    BTN_CLIP_OPEN_LINK,
    BTN_CLIP_RENAME,
    BTN_CLIP_REVEAL,
    BTN_CLIP_DELETE,
    BTN_SET_720P,
    BTN_SET_1080P,

    // Player & Trim controls
    BTN_PLY_BACK = 50,
    BTN_PLY_FAV,
    BTN_PLY_SHARE,
    BTN_PLY_RENAME,
    BTN_PLY_FOLDER,
    BTN_PLY_DELETE,
    BTN_PLY_PLAYPAUSE,
    BTN_PLY_MINUS5,
    BTN_PLY_PLUS5,
    BTN_PLY_RESTART,
    BTN_PLY_RATE,
    BTN_PLY_TRIM,
    BTN_PLY_MUTE,
    BTN_PLY_FULLSCREEN,
    BTN_PLY_EXTERNAL,
    BTN_TRIM_SET_START,
    BTN_TRIM_SET_END,
    BTN_EDIT_CUT_TOGGLE,
    BTN_EDIT_SPEED,
    BTN_EDIT_ADD_CLIP,
    BTN_EDIT_GAME_MUTE,
    BTN_EDIT_MIC_MUTE,
    BTN_EDIT_VERTICAL,
    BTN_EDIT_DISCORD10,
    BTN_EDIT_DISCORD25,
    BTN_TRIM_SAVE_COPY,
    BTN_TRIM_REPLACE,
    BTN_TRIM_CANCEL,
    BTN_CMB_DURATION,
    BTN_CMB_RESFPS,
    BTN_CMB_BITRATE,
    BTN_CMB_MIC_DEV,

    // Montage landing & Studio editor controls
    BTN_MNT_CLEAR = 100,
    BTN_MNT_FADE,
    BTN_MNT_V916,
    BTN_MNT_SPEED,
    BTN_MNT_GAME_MUTE,
    BTN_MNT_MIC_MUTE,
    BTN_MNT_EXPORT_NORMAL,
    BTN_MNT_EXPORT_DISC10,
    BTN_MNT_EXPORT_DISC25,
    BTN_MNT_CANCEL,
    BTN_MNT_OPEN_STUDIO,

    // Medal-style Montaj Studio buttons
    BTN_STU_EXIT = 200,
    BTN_STU_RENDER,
    BTN_STU_CANCEL,
    BTN_STU_ASPECT,
    BTN_STU_FADE,
    BTN_STU_TARGET,
    BTN_STU_LEFT_CLIPS,
    BTN_STU_LEFT_MUSIC,
    BTN_STU_LEFT_FX,
    BTN_STU_PICK_MP3,
    BTN_STU_PLAYPAUSE,
    BTN_STU_TO_START,
    BTN_STU_SPLIT,
    BTN_STU_DELETE,
    BTN_STU_MOVE_LEFT,
    BTN_STU_MOVE_RIGHT,
    BTN_STU_SPEED,
    BTN_STU_GAME_MUTE,
    BTN_STU_MIC_MUTE,
    BTN_STU_MUS_FADEIN,
    BTN_STU_MUS_FADEOUT,
    BTN_STU_FX_TYPE,
    BTN_STU_FX_DUR,
    BTN_STU_FX_TO_PH,
    BTN_STU_FRM_SET_START,
    BTN_STU_FRM_SET_END,
    BTN_STU_FRM_RESET,
    BTN_STU_ZOOM_IN,
    BTN_STU_ZOOM_OUT,
    BTN_STU_ZOOM_FIT,
    BTN_STU_TOGGLE_TRACK,

    BTN_MNT_ADD_BASE      = 2000,
    BTN_MNT_UP_BASE       = 3000,
    BTN_MNT_DOWN_BASE     = 4000,
    BTN_MNT_DEL_BASE      = 5000,
    BTN_STU_ADD_CLIP_BASE = 6000,
    BTN_STU_ADD_MUSIC_BASE = 7000,
    BTN_STU_ADD_FX_BASE   = 8000,
    BTN_STU_ADD_OVR_BASE  = 9000
};

inline Gdiplus::Color Gc(uint8_t r, uint8_t g, uint8_t b, uint8_t a = 255) {
    return Gdiplus::Color(a, r, g, b);
}

inline void AddRoundedRect(Gdiplus::GraphicsPath& path, float x, float y, float w, float h, float r) {
    float d = r * 2.0f;
    if (d > w) d = w;
    if (d > h) d = h;
    if (d < 0.5f) d = 0.5f;
    path.Reset();
    path.AddArc(x, y, d, d, 180.0f, 90.0f);
    path.AddArc(x + w - d, y, d, d, 270.0f, 90.0f);
    path.AddArc(x + w - d, y + h - d, d, d, 0.0f, 90.0f);
    path.AddArc(x, y + h - d, d, d, 90.0f, 90.0f);
    path.CloseFigure();
}

inline void FillRoundRect(Gdiplus::Graphics& g, float x, float y, float w, float h, float r, Gdiplus::Color fill) {
    if (w < 1.0f || h < 1.0f) return;
    Gdiplus::GraphicsPath path;
    AddRoundedRect(path, x, y, w, h, r);
    Gdiplus::SolidBrush br(fill);
    g.FillPath(&br, &path);
}

inline void FillRoundRectGrad(Gdiplus::Graphics& g, float x, float y, float w, float h, float r,
                              Gdiplus::Color cTop, Gdiplus::Color cBot) {
    if (w < 1.0f || h < 1.0f) return;
    Gdiplus::GraphicsPath path;
    AddRoundedRect(path, x, y, w, h, r);
    Gdiplus::LinearGradientBrush br(
        Gdiplus::PointF(x, y), Gdiplus::PointF(x, y + std::max(1.0f, h)), cTop, cBot);
    g.FillPath(&br, &path);
}

inline void StrokeRoundRect(Gdiplus::Graphics& g, float x, float y, float w, float h, float r,
                            Gdiplus::Color col, float width = 1.0f) {
    if (w < 1.0f || h < 1.0f) return;
    Gdiplus::GraphicsPath path;
    AddRoundedRect(path, x, y, w, h, r);
    Gdiplus::Pen pen(col, width);
    g.DrawPath(&pen, &path);
}

} // namespace ui

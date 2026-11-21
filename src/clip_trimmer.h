#pragma once

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <atomic>
#include <string>
#include <vector>

struct MontageSegmentSpec {
    std::wstring path;
    double startSec = 0.0;
    double endSec = 0.0; // <= 0.0 means full clip duration
};

struct ClipEditSpec {
    std::wstring srcPath;
    std::wstring dstPath;
    double startSec = 0.0;
    double endSec = 0.0;

    // Optional middle cut: removes [cutStartSec, cutEndSec] when cutEndSec > cutStartSec
    double cutStartSec = 0.0;
    double cutEndSec = 0.0;

    // Optional second clip to insert (at cut point if cut is active, otherwise appended at end)
    std::wstring insertClipPath;

    // Playback/export speed (0.5 .. 2.0) and per-track audio volumes (0 .. 200%)
    double speed = 1.0;
    int gameVolPct = 100;
    int micVolPct = 100;

    // Optional target file size in MB (e.g. 10 or 25 for Discord; 0 = keep original)
    int targetSizeMb = 0;

    // Optional 9:16 vertical Shorts/Reels export (centered 16:9 gameplay + dimmed vertical background)
    bool vertical916 = false;

    // Optional multi-clip montage (overrides srcPath/startSec/endSec/cut/insert when non-empty)
    std::vector<MontageSegmentSpec> montageSegments;

    // Optional 0.35s fade-to-black & audio smooth transitions between montage clips
    bool fadeTransitions = false;
};

/** Returns a non-colliding copy path alongside srcPath (e.g. "Clip_kesit.mp4"). */
std::wstring BuildTrimmedCopyPath(const std::wstring& srcPath);

/** Returns a non-colliding montage output path in dir (e.g. "Montaj_2026-11-21_22-00-00.mp4"). */
std::wstring BuildMontageOutputPath(const std::wstring& dir);

/** Probes whether an MP4 clip contains separate Game (Track 2) and Mic (Track 3) audio streams. */
bool ProbeClipAudioTracks(const std::wstring& path, bool& outHasSeparateGameMic, double* outDurationSec = nullptr);

/**
 * Edits an MP4 clip (trim, optional middle cut, optional clip insertion, speed, and
 * independent Game/Mic audio mixing) while preserving BT.709 color metadata and A/V sync.
 */
bool EditMp4Clip(const ClipEditSpec& spec, std::string& outError,
                 std::atomic<bool>* cancelFlag = nullptr);

/** Frame-accurately trims [startSec, endSec] from an MP4 file. */
bool TrimMp4Clip(const std::wstring& srcPath, const std::wstring& dstPath,
                 double startSec, double endSec, std::string& outError,
                 std::atomic<bool>* cancelFlag = nullptr);

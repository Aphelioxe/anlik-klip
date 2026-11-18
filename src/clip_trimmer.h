#pragma once

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <atomic>
#include <string>

/** Returns a non-colliding copy path alongside srcPath (e.g. "Clip_kesit.mp4"). */
std::wstring BuildTrimmedCopyPath(const std::wstring& srcPath);

/**
 * Frame-accurately trims [startSec, endSec] from an MP4 file while preserving
 * BT.709 color metadata and exact A/V sync. Writes via .tmp.mp4 before moving to dstPath.
 */
bool TrimMp4Clip(const std::wstring& srcPath, const std::wstring& dstPath,
                 double startSec, double endSec, std::string& outError,
                 std::atomic<bool>* cancelFlag = nullptr);

#pragma once

#include <atomic>
#include <functional>
#include <string>

struct ClipUploadResult {
    bool ok = false;
    std::wstring url;
    std::wstring error;
};

ClipUploadResult UploadClipToShareLink(
    const std::wstring& filePath,
    const std::function<void(int pct)>& onProgress,
    std::atomic<bool>& cancelFlag);

#pragma once

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <cstdint>
#include <string>
#include <unordered_set>
#include <vector>

struct ClipItem {
    std::wstring filePath;
    std::wstring fileName;
    std::wstring displayDate;
    uint64_t fileSizeBytes = 0;
    uint64_t writeTime64 = 0;
    double durationSeconds = 0.0;
    uint32_t width = 0;
    uint32_t height = 0;
    bool isFavorite = false;
};

std::wstring GetFavoritesFilePath();
std::unordered_set<std::wstring> LoadFavorites(const std::wstring& favPath);
bool SetClipFavorite(const std::wstring& favPath, const std::wstring& fileName, bool favorite);
void UpdateFavoriteOnRename(const std::wstring& favPath, const std::wstring& oldFileName, const std::wstring& newFileName);

std::vector<ClipItem> ScanClipsFolder(const std::wstring& dir, const std::wstring& favPath = L"");
void ClearClipMetadataCache();
void InvalidateClipMetadata(const std::wstring& path);
HBITMAP DecodeClipThumbnail(const std::wstring& filePath, int targetW, int targetH,
                            double* outDurationSec = nullptr, uint32_t* outW = nullptr, uint32_t* outH = nullptr);
bool TrashClipFile(const std::wstring& filePath);
bool RenameClipFile(const std::wstring& oldPath, const std::wstring& newName, std::wstring& outNewPath);

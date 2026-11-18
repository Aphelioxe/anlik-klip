#include "clip_library.h"
#include "com_ptr.h"

#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <propvarutil.h>
#include <shellapi.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <unordered_map>

namespace {

struct CachedMeta {
    uint64_t size = 0;
    uint64_t mtime = 0;
    double duration = 0.0;
    uint32_t width = 0;
    uint32_t height = 0;
};

std::mutex g_metaMutex;
std::unordered_map<std::wstring, CachedMeta> g_metaCache;

bool EndsWithNoCase(const std::wstring& s, const std::wstring& suffix) {
    if (s.size() < suffix.size()) return false;
    return _wcsicmp(s.c_str() + s.size() - suffix.size(), suffix.c_str()) == 0;
}

void ProbeClipMetadata(const std::wstring& path, uint64_t size, uint64_t mtime,
                       double& outDur, uint32_t& outW, uint32_t& outH) {
    {
        std::lock_guard<std::mutex> lk(g_metaMutex);
        auto it = g_metaCache.find(path);
        if (it != g_metaCache.end() && it->second.size == size && it->second.mtime == mtime) {
            outDur = it->second.duration;
            outW = it->second.width;
            outH = it->second.height;
            return;
        }
    }

    outDur = 0.0;
    outW = 0;
    outH = 0;

    ComPtr<IMFSourceReader> sr;
    if (SUCCEEDED(MFCreateSourceReaderFromURL(path.c_str(), nullptr, sr.Put())) && sr) {
        PROPVARIANT pv;
        PropVariantInit(&pv);
        if (SUCCEEDED(sr->GetPresentationAttribute(MF_SOURCE_READER_MEDIASOURCE, MF_PD_DURATION, &pv))) {
            if (pv.vt == VT_UI8) {
                outDur = static_cast<double>(pv.uhVal.QuadPart) / 1e7;
            }
        }
        PropVariantClear(&pv);

        ComPtr<IMFMediaType> mt;
        if (SUCCEEDED(sr->GetNativeMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM, 0, mt.Put())) && mt) {
            UINT32 w = 0, h = 0;
            if (SUCCEEDED(MFGetAttributeSize(mt.Get(), MF_MT_FRAME_SIZE, &w, &h))) {
                outW = w;
                outH = h;
            }
        }
    }

    std::lock_guard<std::mutex> lk(g_metaMutex);
    if (g_metaCache.size() > 512) g_metaCache.clear();
    g_metaCache[path] = CachedMeta{ size, mtime, outDur, outW, outH };
}

} // namespace

void ClearClipMetadataCache() {
    std::lock_guard<std::mutex> lk(g_metaMutex);
    std::unordered_map<std::wstring, CachedMeta>().swap(g_metaCache);
}

std::vector<ClipItem> ScanClipsFolder(const std::wstring& dir) {
    std::vector<ClipItem> items;
    if (dir.empty()) return items;

    std::wstring pattern = dir;
    if (pattern.back() != L'\\' && pattern.back() != L'/') pattern += L'\\';
    std::wstring baseDir = pattern;
    pattern += L"*.mp4";

    WIN32_FIND_DATAW fd{};
    HANDLE hFind = FindFirstFileW(pattern.c_str(), &fd);
    if (hFind == INVALID_HANDLE_VALUE) return items;

    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        std::wstring name = fd.cFileName;
        if (EndsWithNoCase(name, L".tmp.mp4")) continue;

        ClipItem item;
        item.fileName = name;
        item.filePath = baseDir + name;
        item.fileSizeBytes = (static_cast<uint64_t>(fd.nFileSizeHigh) << 32) | fd.nFileSizeLow;
        item.writeTime64 = (static_cast<uint64_t>(fd.ftLastWriteTime.dwHighDateTime) << 32) |
                           fd.ftLastWriteTime.dwLowDateTime;

        FILETIME localFt{};
        SYSTEMTIME st{};
        if (FileTimeToLocalFileTime(&fd.ftLastWriteTime, &localFt) &&
            FileTimeToSystemTime(&localFt, &st)) {
            wchar_t dbuf[64] = {};
            swprintf(dbuf, 64, L"%02u.%02u.%04u %02u:%02u",
                st.wDay, st.wMonth, st.wYear, st.wHour, st.wMinute);
            item.displayDate = dbuf;
        }
        items.push_back(std::move(item));
    } while (FindNextFileW(hFind, &fd));
    FindClose(hFind);

    std::sort(items.begin(), items.end(), [](const ClipItem& a, const ClipItem& b) {
        return a.writeTime64 > b.writeTime64;
    });

    // Limit metadata probing to newest 120 files so huge folders open instantaneously
    const size_t probeLimit = std::min<size_t>(items.size(), 120);
    for (size_t i = 0; i < probeLimit; ++i) {
        ProbeClipMetadata(items[i].filePath, items[i].fileSizeBytes, items[i].writeTime64,
            items[i].durationSeconds, items[i].width, items[i].height);
    }
    return items;
}

HBITMAP DecodeClipThumbnail(const std::wstring& filePath, int targetW, int targetH,
                            double* outDurationSec, uint32_t* outW, uint32_t* outH) {
    if (filePath.empty() || targetW <= 0 || targetH <= 0) return nullptr;

    ComPtr<IMFAttributes> attr;
    MFCreateAttributes(attr.Put(), 1);
    if (attr) {
        attr->SetUINT32(MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING, TRUE);
    }

    ComPtr<IMFSourceReader> sr;
    if (FAILED(MFCreateSourceReaderFromURL(filePath.c_str(), attr.Get(), sr.Put())) || !sr) {
        return nullptr;
    }

    double dur = 0.0;
    PROPVARIANT pv;
    PropVariantInit(&pv);
    if (SUCCEEDED(sr->GetPresentationAttribute(MF_SOURCE_READER_MEDIASOURCE, MF_PD_DURATION, &pv))) {
        if (pv.vt == VT_UI8) dur = static_cast<double>(pv.uhVal.QuadPart) / 1e7;
    }
    PropVariantClear(&pv);
    if (outDurationSec) *outDurationSec = dur;

    ComPtr<IMFMediaType> rgbType;
    MFCreateMediaType(rgbType.Put());
    rgbType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    rgbType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32);
    if (FAILED(sr->SetCurrentMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM, nullptr, rgbType.Get()))) {
        return nullptr;
    }

    ComPtr<IMFMediaType> curType;
    UINT32 srcW = 0, srcH = 0;
    INT32 stride = 0;
    if (SUCCEEDED(sr->GetCurrentMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM, curType.Put())) && curType) {
        MFGetAttributeSize(curType.Get(), MF_MT_FRAME_SIZE, &srcW, &srcH);
        stride = static_cast<INT32>(MFGetAttributeUINT32(curType.Get(), MF_MT_DEFAULT_STRIDE, srcW * 4));
    }
    if (srcW == 0 || srcH == 0) return nullptr;
    if (outW) *outW = srcW;
    if (outH) *outH = srcH;

    // Seek ~15% into clip (max 2.0s) for a representative frame
    if (dur > 0.5) {
        double seekSec = std::min(2.0, dur * 0.15);
        PROPVARIANT pos;
        PropVariantInit(&pos);
        pos.vt = VT_I8;
        pos.hVal.QuadPart = static_cast<LONGLONG>(seekSec * 1e7);
        sr->SetCurrentPosition(GUID_NULL, pos);
        PropVariantClear(&pos);
    }

    ComPtr<IMFSample> sample;
    for (int tries = 0; tries < 6; ++tries) {
        DWORD streamIdx = 0, flags = 0;
        LONGLONG ts = 0;
        ComPtr<IMFSample> s;
        HRESULT hr = sr->ReadSample(MF_SOURCE_READER_FIRST_VIDEO_STREAM, 0, &streamIdx, &flags, &ts, s.Put());
        if (FAILED(hr) || (flags & MF_SOURCE_READERF_ENDOFSTREAM)) break;
        if (s) {
            sample = s;
            break;
        }
    }
    if (!sample) return nullptr;

    ComPtr<IMFMediaBuffer> buf;
    if (FAILED(sample->ConvertToContiguousBuffer(buf.Put())) || !buf) return nullptr;

    BYTE* srcBytes = nullptr;
    DWORD srcLen = 0;
    if (FAILED(buf->Lock(&srcBytes, nullptr, &srcLen)) || !srcBytes) return nullptr;

    // Draw scaled into top-down 32bpp DIBSection
    BITMAPINFO dstBi{};
    dstBi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    dstBi.bmiHeader.biWidth = targetW;
    dstBi.bmiHeader.biHeight = -targetH;
    dstBi.bmiHeader.biPlanes = 1;
    dstBi.bmiHeader.biBitCount = 32;
    dstBi.bmiHeader.biCompression = BI_RGB;

    HDC screenDc = GetDC(nullptr);
    void* dstBits = nullptr;
    HBITMAP hBmp = CreateDIBSection(screenDc, &dstBi, DIB_RGB_COLORS, &dstBits, nullptr, 0);
    if (hBmp && dstBits) {
        HDC memDc = CreateCompatibleDC(screenDc);
        HGDIOBJ oldObj = SelectObject(memDc, hBmp);

        // Fill dark background for aspect-preserving letterbox
        RECT rc{ 0, 0, targetW, targetH };
        HBRUSH bgBrush = CreateSolidBrush(RGB(14, 17, 23));
        FillRect(memDc, &rc, bgBrush);
        DeleteObject(bgBrush);

        double sx = static_cast<double>(targetW) / static_cast<double>(srcW);
        double sy = static_cast<double>(targetH) / static_cast<double>(srcH);
        double sc = std::min(sx, sy);
        int dw = std::max(1, static_cast<int>(srcW * sc + 0.5));
        int dh = std::max(1, static_cast<int>(srcH * sc + 0.5));
        int dx = (targetW - dw) / 2;
        int dy = (targetH - dh) / 2;

        BITMAPINFO srcBi{};
        srcBi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        srcBi.bmiHeader.biWidth = static_cast<LONG>(srcW);
        srcBi.bmiHeader.biHeight = (stride >= 0) ? -static_cast<LONG>(srcH) : static_cast<LONG>(srcH);
        srcBi.bmiHeader.biPlanes = 1;
        srcBi.bmiHeader.biBitCount = 32;
        srcBi.bmiHeader.biCompression = BI_RGB;

        SetStretchBltMode(memDc, HALFTONE);
        SetBrushOrgEx(memDc, 0, 0, nullptr);
        StretchDIBits(memDc, dx, dy, dw, dh,
            0, 0, static_cast<int>(srcW), static_cast<int>(srcH),
            srcBytes, &srcBi, DIB_RGB_COLORS, SRCCOPY);

        SelectObject(memDc, oldObj);
        DeleteDC(memDc);
    }
    ReleaseDC(nullptr, screenDc);
    buf->Unlock();
    return hBmp;
}

bool TrashClipFile(const std::wstring& filePath) {
    if (filePath.empty()) return false;
    std::wstring zz = filePath;
    zz.push_back(L'\0');
    zz.push_back(L'\0');

    SHFILEOPSTRUCTW op{};
    op.wFunc = FO_DELETE;
    op.pFrom = zz.c_str();
    op.fFlags = FOF_ALLOWUNDO | FOF_NOCONFIRMATION | FOF_SILENT;
    return SHFileOperationW(&op) == 0 && !op.fAnyOperationsAborted;
}

bool RenameClipFile(const std::wstring& oldPath, const std::wstring& newName, std::wstring& outNewPath) {
    if (oldPath.empty() || newName.empty()) return false;
    std::wstring clean = newName;
    for (wchar_t ch : clean) {
        if (ch == L'\\' || ch == L'/' || ch == L':' || ch == L'*' ||
            ch == L'?' || ch == L'"' || ch == L'<' || ch == L'>' || ch == L'|') {
            return false;
        }
    }
    if (!EndsWithNoCase(clean, L".mp4")) {
        clean += L".mp4";
    }
    size_t pos = oldPath.find_last_of(L"\\/");
    std::wstring dir = (pos != std::wstring::npos) ? oldPath.substr(0, pos + 1) : L"";
    outNewPath = dir + clean;
    if (_wcsicmp(oldPath.c_str(), outNewPath.c_str()) == 0) return true;
    return MoveFileW(oldPath.c_str(), outNewPath.c_str()) != FALSE;
}

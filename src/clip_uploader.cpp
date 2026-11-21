#include "clip_uploader.h"
#include "clip_trimmer.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <winhttp.h>

#include <algorithm>
#include <cstdio>
#include <vector>

namespace {

std::string WideToUtf8(const std::wstring& ws) {
    int n = WideCharToMultiByte(CP_UTF8, 0, ws.c_str(), static_cast<int>(ws.size()), nullptr, 0, nullptr, nullptr);
    std::string utf8(n > 0 ? static_cast<size_t>(n) : 0, '\0');
    if (n > 0) WideCharToMultiByte(CP_UTF8, 0, ws.c_str(), static_cast<int>(ws.size()), &utf8[0], n, nullptr, nullptr);
    return utf8;
}

std::wstring Utf8ToWide(const std::string& s) {
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring ws(n > 0 ? static_cast<size_t>(n) : 0, L'\0');
    if (n > 0) MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), &ws[0], n);
    return ws;
}

std::string TrimAscii(const std::string& s) {
    size_t a = 0;
    while (a < s.size() && (s[a] == ' ' || s[a] == '\r' || s[a] == '\n' || s[a] == '\t')) ++a;
    size_t b = s.size();
    while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\r' || s[b - 1] == '\n' || s[b - 1] == '\t')) --b;
    return s.substr(a, b - a);
}

std::string ExtractJsonStringField(const std::string& json, const char* key) {
    std::string pat = std::string("\"") + key + "\"";
    size_t kpos = json.find(pat);
    if (kpos == std::string::npos) return {};
    size_t colon = json.find(':', kpos + pat.size());
    if (colon == std::string::npos) return {};
    size_t q1 = json.find('"', colon + 1);
    if (q1 == std::string::npos) return {};
    size_t q2 = json.find('"', q1 + 1);
    if (q2 == std::string::npos || q2 <= q1 + 1) return {};
    return json.substr(q1 + 1, q2 - q1 - 1);
}

bool WriteAllWinHttp(HINTERNET hReq, const void* data, DWORD len, DWORD& outErr) {
    const BYTE* p = static_cast<const BYTE*>(data);
    DWORD rem = len;
    while (rem > 0) {
        DWORD written = 0;
        if (!WinHttpWriteData(hReq, p, rem, &written)) {
            outErr = GetLastError();
            return false;
        }
        p += written;
        rem -= written;
    }
    return true;
}

bool MultipartUploadFile(
    const wchar_t* host,
    const wchar_t* path,
    const std::string& extraFieldsPrefix,
    const char* fileFieldName,
    const std::wstring& filePath,
    ULONGLONG fileSize,
    const std::function<void(int pct)>& onProgress,
    std::atomic<bool>& cancelFlag,
    DWORD& outStatus,
    std::string& outBody,
    std::wstring& outErr) {
    outStatus = 0;
    outBody.clear();

    size_t slash = filePath.find_last_of(L"\\/");
    std::wstring wfname = (slash != std::wstring::npos) ? filePath.substr(slash + 1) : L"klip.mp4";
    std::string fnameUtf8 = WideToUtf8(wfname);
    for (char& ch : fnameUtf8) {
        if (ch == '"' || ch == '\r' || ch == '\n') ch = '_';
    }

    static const char kBoundary[] = "----AnlikKlipBoundary7F9A2C";
    std::string prefix = extraFieldsPrefix;
    prefix += "--";
    prefix += kBoundary;
    prefix += "\r\nContent-Disposition: form-data; name=\"";
    prefix += fileFieldName;
    prefix += "\"; filename=\"";
    prefix += fnameUtf8;
    prefix += "\"\r\nContent-Type: video/mp4\r\n\r\n";

    std::string suffix = "\r\n--";
    suffix += kBoundary;
    suffix += "--\r\n";

    ULONGLONG totalLen = prefix.size() + fileSize + suffix.size();
    if (totalLen > 0xFFFFFFFFULL) {
        outErr = L"Dosya boyutu WinHTTP tek istek sınırını aşıyor.";
        return false;
    }

    HANDLE hFile = CreateFileW(filePath.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (hFile == INVALID_HANDLE_VALUE) {
        outErr = L"Klip dosyası açılamadı.";
        return false;
    }

    HINTERNET hSession = WinHttpOpen(L"AnlikKlip/1.4",
        WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hSession) {
        CloseHandle(hFile);
        outErr = L"WinHTTP oturumu başlatılamadı.";
        return false;
    }
    WinHttpSetTimeouts(hSession, 10000, 15000, 120000, 120000);

    HINTERNET hConnect = WinHttpConnect(hSession, host, INTERNET_DEFAULT_HTTPS_PORT, 0);
    if (!hConnect) {
        WinHttpCloseHandle(hSession);
        CloseHandle(hFile);
        outErr = L"Sunucuya bağlanılamadı.";
        return false;
    }

    HINTERNET hReq = WinHttpOpenRequest(hConnect, L"POST", path,
        nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
    if (!hReq) {
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        CloseHandle(hFile);
        outErr = L"Yükleme isteği oluşturulamadı.";
        return false;
    }

    std::wstring hdr = L"Content-Type: multipart/form-data; boundary=----AnlikKlipBoundary7F9A2C\r\n";
    DWORD lastErr = 0;
    BOOL ok = WinHttpSendRequest(hReq, hdr.c_str(), static_cast<DWORD>(hdr.size()),
        WINHTTP_NO_REQUEST_DATA, 0, static_cast<DWORD>(totalLen), 0);
    if (!ok) lastErr = GetLastError();

    if (ok) {
        ok = WriteAllWinHttp(hReq, prefix.data(), static_cast<DWORD>(prefix.size()), lastErr);
    }

    if (ok) {
        std::vector<BYTE> chunk(65536);
        ULONGLONG sent = 0;
        int lastPct = -1;
        while (!cancelFlag.load()) {
            DWORD bytesRead = 0;
            if (!ReadFile(hFile, chunk.data(), static_cast<DWORD>(chunk.size()), &bytesRead, nullptr)) {
                lastErr = GetLastError();
                ok = FALSE;
                break;
            }
            if (bytesRead == 0) break;
            if (!WriteAllWinHttp(hReq, chunk.data(), bytesRead, lastErr)) {
                ok = FALSE;
                break;
            }
            sent += bytesRead;
            int pct = static_cast<int>((sent * 100ULL) / std::max<ULONGLONG>(1, fileSize));
            if (pct != lastPct && onProgress) {
                lastPct = pct;
                onProgress(std::max(0, std::min(100, pct)));
            }
        }
        if (cancelFlag.load()) {
            ok = FALSE;
            outErr = L"Yükleme iptal edildi.";
        }
    }

    if (ok) {
        ok = WriteAllWinHttp(hReq, suffix.data(), static_cast<DWORD>(suffix.size()), lastErr);
    }

    CloseHandle(hFile);

    if (ok) {
        ok = WinHttpReceiveResponse(hReq, nullptr);
        if (!ok) lastErr = GetLastError();
    }

    if (ok) {
        DWORD scSize = sizeof(outStatus);
        WinHttpQueryHeaders(hReq, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
            WINHTTP_HEADER_NAME_BY_INDEX, &outStatus, &scSize, WINHTTP_NO_HEADER_INDEX);

        DWORD avail = 0;
        while (WinHttpQueryDataAvailable(hReq, &avail) && avail > 0) {
            std::string part(avail, '\0');
            DWORD got = 0;
            if (WinHttpReadData(hReq, &part[0], avail, &got) && got > 0) {
                part.resize(got);
                outBody += part;
            } else {
                break;
            }
            avail = 0;
        }
    }

    WinHttpCloseHandle(hReq);
    WinHttpCloseHandle(hConnect);
    WinHttpCloseHandle(hSession);

    if (!ok) {
        if (outErr.empty()) {
            wchar_t eb[96] = {};
            swprintf(eb, 96, L"Yükleme başarısız (hata %lu).", lastErr);
            outErr = eb;
        }
        return false;
    }
    return true;
}

} // namespace

ClipUploadResult UploadClipToShareLink(
    const std::wstring& filePath,
    const std::function<void(int pct)>& onProgress,
    std::atomic<bool>& cancelFlag) {
    ClipUploadResult res;

    WIN32_FILE_ATTRIBUTE_DATA fad{};
    if (!GetFileAttributesExW(filePath.c_str(), GetFileExInfoStandard, &fad)) {
        res.error = L"Klip dosyası bulunamadı.";
        return res;
    }
    ULONGLONG fileSize = (static_cast<ULONGLONG>(fad.nFileSizeHigh) << 32) | fad.nFileSizeLow;
    if (fileSize == 0) {
        res.error = L"Klip dosyası boş.";
        return res;
    }

    std::wstring uploadPath = filePath;
    std::wstring tempFixed;
    {
        bool hasMulti = false;
        if (ProbeClipAudioTracks(filePath, hasMulti, nullptr) && hasMulti) {
            tempFixed = filePath + L".upload.tmp.mp4";
            if (CopyFileW(filePath.c_str(), tempFixed.c_str(), FALSE)) {
                FixMp4AudioTrackFlags(tempFixed);
                uploadPath = tempFixed;
            } else {
                tempFixed.clear();
            }
        }
    }
    struct TempGuard {
        std::wstring& p;
        ~TempGuard() { if (!p.empty()) DeleteFileW(p.c_str()); }
    } tempGuard{ tempFixed };

    // 1) Primary for <= 195 MB: Catbox.moe (gives direct .mp4 URL that plays in browser & embeds in Discord)
    if (fileSize <= 195ULL * 1024ULL * 1024ULL) {
        std::string catPrefix = "------AnlikKlipBoundary7F9A2C\r\n"
                                "Content-Disposition: form-data; name=\"reqtype\"\r\n\r\n"
                                "fileupload\r\n";
        DWORD status = 0;
        std::string body;
        std::wstring err;
        if (MultipartUploadFile(L"catbox.moe", L"/user/api.php", catPrefix, "fileToUpload",
                uploadPath, fileSize, onProgress, cancelFlag, status, body, err)) {
            std::string url = TrimAscii(body);
            if (status == 200 && url.rfind("https://", 0) == 0) {
                res.ok = true;
                res.url = Utf8ToWide(url);
                return res;
            }
        }
        if (cancelFlag.load()) {
            res.error = L"Yükleme iptal edildi.";
            return res;
        }
    }

    // 2) Fallback (or > 195 MB): Gofile.io (gives watch/download page URL)
    DWORD status = 0;
    std::string body;
    std::wstring err;
    if (MultipartUploadFile(L"upload.gofile.io", L"/uploadfile", "", "file",
            uploadPath, fileSize, onProgress, cancelFlag, status, body, err)) {
        std::string dlPage = ExtractJsonStringField(body, "downloadPage");
        if (status == 200 && !dlPage.empty()) {
            res.ok = true;
            res.url = Utf8ToWide(dlPage);
            return res;
        }
        wchar_t eb[96] = {};
        swprintf(eb, 96, L"Sunucu yanıtı geçersiz (HTTP %lu).", status);
        res.error = eb;
        return res;
    }

    res.error = err.empty() ? L"Yükleme başarısız." : err;
    return res;
}

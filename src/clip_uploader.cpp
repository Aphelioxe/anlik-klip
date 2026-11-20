#include "clip_uploader.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <winhttp.h>

#include <algorithm>
#include <cstdio>
#include <vector>

namespace {

std::wstring UrlEncodeUtf8(const std::wstring& ws) {
    int n = WideCharToMultiByte(CP_UTF8, 0, ws.c_str(), static_cast<int>(ws.size()), nullptr, 0, nullptr, nullptr);
    std::string utf8(n > 0 ? static_cast<size_t>(n) : 0, '\0');
    if (n > 0) WideCharToMultiByte(CP_UTF8, 0, ws.c_str(), static_cast<int>(ws.size()), &utf8[0], n, nullptr, nullptr);

    static const wchar_t kHex[] = L"0123456789ABCDEF";
    std::wstring out;
    out.reserve(utf8.size() * 2);
    for (unsigned char c : utf8) {
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
            c == '-' || c == '_' || c == '.' || c == '~') {
            out.push_back(static_cast<wchar_t>(c));
        } else {
            out.push_back(L'%');
            out.push_back(kHex[(c >> 4) & 0xF]);
            out.push_back(kHex[c & 0xF]);
        }
    }
    return out;
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

} // namespace

ClipUploadResult UploadClipToPixeldrain(
    const std::wstring& filePath,
    const std::function<void(int pct)>& onProgress,
    std::atomic<bool>& cancelFlag) {
    ClipUploadResult res;

    HANDLE hFile = CreateFileW(filePath.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (hFile == INVALID_HANDLE_VALUE) {
        res.error = L"Klip dosyası açılamadı.";
        return res;
    }

    LARGE_INTEGER liSize{};
    if (!GetFileSizeEx(hFile, &liSize) || liSize.QuadPart <= 0) {
        CloseHandle(hFile);
        res.error = L"Klip dosyası boş veya okunamadı.";
        return res;
    }
    const ULONGLONG totalBytes = static_cast<ULONGLONG>(liSize.QuadPart);
    if (totalBytes > 0xFFFFFFFFULL * 4ULL) {
        CloseHandle(hFile);
        res.error = L"Dosya çok büyük.";
        return res;
    }

    size_t slash = filePath.find_last_of(L"\\/");
    std::wstring fname = (slash != std::wstring::npos) ? filePath.substr(slash + 1) : L"klip.mp4";
    std::wstring objectPath = L"/api/file/" + UrlEncodeUtf8(fname);

    HINTERNET hSession = WinHttpOpen(L"AnlikKlip/1.4",
        WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hSession) {
        CloseHandle(hFile);
        res.error = L"WinHTTP oturumu başlatılamadı.";
        return res;
    }

    // Generous send/receive timeouts for larger clips
    WinHttpSetTimeouts(hSession, 10000, 15000, 60000, 60000);

    HINTERNET hConnect = WinHttpConnect(hSession, L"pixeldrain.com", INTERNET_DEFAULT_HTTPS_PORT, 0);
    if (!hConnect) {
        WinHttpCloseHandle(hSession);
        CloseHandle(hFile);
        res.error = L"Sunucuya bağlanılamadı.";
        return res;
    }

    HINTERNET hReq = WinHttpOpenRequest(hConnect, L"PUT", objectPath.c_str(),
        nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
    if (!hReq) {
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        CloseHandle(hFile);
        res.error = L"Yükleme isteği oluşturulamadı.";
        return res;
    }

    const wchar_t* hdr = L"Content-Type: video/mp4\r\n";
    BOOL ok = WinHttpSendRequest(hReq, hdr, static_cast<DWORD>(-1),
        WINHTTP_NO_REQUEST_DATA, 0, static_cast<DWORD>(totalBytes), 0);

    if (ok) {
        std::vector<BYTE> chunk(65536); // 64 KB streaming buffer
        ULONGLONG sent = 0;
        int lastPct = -1;
        while (!cancelFlag.load()) {
            DWORD bytesRead = 0;
            if (!ReadFile(hFile, chunk.data(), static_cast<DWORD>(chunk.size()), &bytesRead, nullptr)) {
                ok = FALSE;
                break;
            }
            if (bytesRead == 0) break;

            DWORD written = 0;
            if (!WinHttpWriteData(hReq, chunk.data(), bytesRead, &written)) {
                ok = FALSE;
                break;
            }
            sent += written;
            int pct = static_cast<int>((sent * 100ULL) / totalBytes);
            if (pct != lastPct && onProgress) {
                lastPct = pct;
                onProgress(std::max(0, std::min(100, pct)));
            }
        }
        if (cancelFlag.load()) {
            ok = FALSE;
            res.error = L"Yükleme iptal edildi.";
        }
    }

    CloseHandle(hFile);

    if (ok) {
        ok = WinHttpReceiveResponse(hReq, nullptr);
    }

    DWORD statusCode = 0;
    DWORD scSize = sizeof(statusCode);
    if (ok) {
        WinHttpQueryHeaders(hReq, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
            WINHTTP_HEADER_NAME_BY_INDEX, &statusCode, &scSize, WINHTTP_NO_HEADER_INDEX);
    }

    std::string respBody;
    if (ok) {
        DWORD avail = 0;
        while (WinHttpQueryDataAvailable(hReq, &avail) && avail > 0) {
            std::string part(avail, '\0');
            DWORD got = 0;
            if (WinHttpReadData(hReq, &part[0], avail, &got) && got > 0) {
                part.resize(got);
                respBody += part;
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
        if (res.error.empty()) {
            wchar_t eb[96] = {};
            swprintf(eb, 96, L"Yükleme başarısız (WinHTTP hata %lu).", GetLastError());
            res.error = eb;
        }
        return res;
    }

    std::string id = ExtractJsonStringField(respBody, "id");
    if ((statusCode == 200 || statusCode == 201) && !id.empty()) {
        std::wstring wid(id.begin(), id.end());
        res.ok = true;
        res.url = L"https://pixeldrain.com/u/" + wid;
        return res;
    }

    wchar_t eb[128] = {};
    swprintf(eb, 128, L"Sunucu yanıtı geçersiz (HTTP %lu).", statusCode);
    res.error = eb;
    return res;
}

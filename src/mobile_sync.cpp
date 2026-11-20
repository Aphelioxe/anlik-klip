#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#include <windows.h>
#include <objidl.h>
#include <gdiplus.h>

#include "mobile_sync.h"
#include "clip_library.h"
#include "com_ptr.h"
#include "config.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <random>
#include <sstream>

namespace {

constexpr uint16_t kUdpDiscoveryPort = 47821;

struct WsaScope {
    bool ok = false;
    WsaScope() {
        WSADATA wd{};
        ok = (WSAStartup(MAKEWORD(2, 2), &wd) == 0);
    }
    ~WsaScope() {
        if (ok) WSACleanup();
    }
};

std::string WideToUtf8(const std::wstring& w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    if (n <= 0) return {};
    std::string out(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), &out[0], n, nullptr, nullptr);
    return out;
}

std::wstring Utf8ToWide(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), nullptr, 0);
    if (n <= 0) return {};
    std::wstring out(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), &out[0], n);
    return out;
}

std::string JsonEscape(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 8);
    for (unsigned char c : s) {
        switch (c) {
            case '\"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20) {
                    char buf[8];
                    snprintf(buf, sizeof(buf), "\\u%04x", c);
                    out += buf;
                } else {
                    out += static_cast<char>(c);
                }
        }
    }
    return out;
}

std::string UrlDecode(const std::string& in) {
    std::string out;
    out.reserve(in.size());
    for (size_t i = 0; i < in.size(); ++i) {
        if (in[i] == '%' && i + 2 < in.size()) {
            unsigned int v = 0;
            if (sscanf(in.substr(i + 1, 2).c_str(), "%x", &v) == 1) {
                out.push_back(static_cast<char>(v));
                i += 2;
                continue;
            }
        } else if (in[i] == '+') {
            out.push_back(' ');
            continue;
        }
        out.push_back(in[i]);
    }
    return out;
}

std::string GetQueryParam(const std::string& query, const std::string& key) {
    size_t pos = 0;
    while (pos < query.size()) {
        size_t amp = query.find('&', pos);
        if (amp == std::string::npos) amp = query.size();
        size_t eq = query.find('=', pos);
        if (eq != std::string::npos && eq < amp) {
            if (query.substr(pos, eq - pos) == key) {
                return UrlDecode(query.substr(eq + 1, amp - (eq + 1)));
            }
        }
        pos = amp + 1;
    }
    return {};
}

bool IsSafeMp4FileName(const std::wstring& name) {
    if (name.empty() || name.size() > 220) return false;
    if (name.find(L"..") != std::wstring::npos) return false;
    if (name.find(L'\\') != std::wstring::npos || name.find(L'/') != std::wstring::npos) return false;
    if (name.find(L':') != std::wstring::npos) return false;
    return name.size() > 4 && _wcsicmp(name.c_str() + name.size() - 4, L".mp4") == 0;
}

bool SendAll(SOCKET s, const char* data, size_t len) {
    size_t sent = 0;
    while (sent < len) {
        int chunk = static_cast<int>(std::min<size_t>(len - sent, 65536));
        int r = send(s, data + sent, chunk, 0);
        if (r <= 0) return false;
        sent += static_cast<size_t>(r);
    }
    return true;
}

void SendHttpResponse(SOCKET s, int code, const char* statusText, const char* contentType,
                      const std::string& body, const std::string& extraHeaders = "") {
    char hdr[1024];
    int n = snprintf(hdr, sizeof(hdr),
        "HTTP/1.1 %d %s\r\n"
        "Content-Type: %s\r\n"
        "Content-Length: %zu\r\n"
        "Access-Control-Allow-Origin: *\r\n"
        "Cache-Control: no-store\r\n"
        "Connection: close\r\n"
        "%s\r\n",
        code, statusText, contentType, body.size(), extraHeaders.c_str());
    if (n > 0) SendAll(s, hdr, static_cast<size_t>(n));
    if (!body.empty()) SendAll(s, body.data(), body.size());
}

bool GetJpegEncoderClsid(CLSID& outClsid) {
    UINT num = 0, size = 0;
    Gdiplus::GetImageEncodersSize(&num, &size);
    if (size == 0) return false;
    std::vector<uint8_t> buf(size);
    auto* info = reinterpret_cast<Gdiplus::ImageCodecInfo*>(buf.data());
    if (Gdiplus::GetImageEncoders(num, size, info) != Gdiplus::Ok) return false;
    for (UINT i = 0; i < num; ++i) {
        if (info[i].MimeType && wcscmp(info[i].MimeType, L"image/jpeg") == 0) {
            outClsid = info[i].Clsid;
            return true;
        }
    }
    return false;
}

const char kMobileWebHtml[] = R"HTML(<!doctype html>
<html lang="tr"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>Anlık Klip — Mobil Aktarım</title>
<style>
body{margin:0;background:#0e121a;color:#eef2ff;font-family:system-ui,-apple-system,sans-serif;padding:16px}
h1{font-size:20px;margin:0 0 6px} .sub{color:#94a3b8;font-size:13px;margin-bottom:16px}
.row{display:flex;gap:8px;margin-bottom:14px}
input{flex:1;padding:10px 12px;border-radius:10px;border:1px solid #334155;background:#171d29;color:#fff;font-size:15px}
button{padding:10px 16px;border-radius:10px;border:0;background:#2563eb;color:#fff;font-weight:600;cursor:pointer}
.card{background:#171d29;border:1px solid #293449;border-radius:12px;padding:12px;margin-bottom:12px}
.title{font-weight:600;font-size:15px;margin-bottom:4px;word-break:break-word}
.meta{font-size:12px;color:#94a3b8;margin-bottom:10px}
.acts{display:flex;gap:8px}
a.dl,button.pv{flex:1;text-align:center;text-decoration:none;padding:9px;border-radius:8px;font-size:13px;font-weight:600}
a.dl{background:#16a34a;color:#fff} button.pv{background:#334155;color:#fff}
video{width:100%;border-radius:10px;margin-top:8px;background:#000}
</style></head><body>
<h1>Anlık Klip</h1><div class="sub" id="st">PC bağlantısı için PIN girin</div>
<div class="row"><input id="pin" placeholder="6 haneli PIN" inputmode="numeric"><button onclick="load()">Bağlan</button></div>
<div id="list"></div>
<script>
const p=new URLSearchParams(location.search).get('pin')||localStorage.getItem('ak_pin')||'';
document.getElementById('pin').value=p;
async function load(){
  const pin=document.getElementById('pin').value.trim();
  if(!pin)return; localStorage.setItem('ak_pin',pin);
  const r=await fetch('/api/clips?pin='+encodeURIComponent(pin));
  if(!r.ok){document.getElementById('st').textContent='PIN hatalı veya erişim reddedildi.';return;}
  const items=await r.json();
  document.getElementById('st').textContent=items.length+' klip hazır';
  const el=document.getElementById('list'); el.innerHTML='';
  items.forEach(c=>{
    const d=document.createElement('div'); d.className='card';
    const url='/api/clip?file='+encodeURIComponent(c.fileName)+'&pin='+encodeURIComponent(pin);
    const mb=(c.sizeBytes/(1024*1024)).toFixed(1);
    d.innerHTML=`<div class="title">${c.isFavorite?'★ ':''}${c.fileName}</div>
      <div class="meta">${c.height}p • ${Math.round(c.durationSec)} sn • ${mb} MB • ${c.date}</div>
      <div class="acts"><button class="pv" onclick="prev(this,'${url}')">İzle</button>
      <a class="dl" href="${url}" download="${c.fileName}">Telefona İndir</a></div>`;
    el.appendChild(d);
  });
}
function prev(btn,u){const c=btn.closest('.card');let v=c.querySelector('video');
if(v){v.remove();return;} v=document.createElement('video');v.controls=true;v.playsInline=true;v.src=u;c.appendChild(v);v.play();}
if(p)load();
</script></body></html>)HTML";

} // namespace

MobileSyncServer::~MobileSyncServer() {
    Stop();
}

std::wstring MobileSyncServer::GenerateRandomPin() {
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<int> dist(100000, 999999);
    wchar_t buf[16] = {};
    swprintf(buf, 16, L"%d", dist(gen));
    return buf;
}

std::wstring MobileSyncServer::DetectPrimaryLanIpv4() {
    WsaScope wsa;
    ULONG sz = 16384;
    std::vector<uint8_t> buf(sz);
    auto* addrs = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buf.data());
    ULONG rc = GetAdaptersAddresses(AF_INET, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER,
        nullptr, addrs, &sz);
    if (rc == ERROR_BUFFER_OVERFLOW) {
        buf.resize(sz);
        addrs = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buf.data());
        rc = GetAdaptersAddresses(AF_INET, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER,
            nullptr, addrs, &sz);
    }
    if (rc != NO_ERROR) return L"127.0.0.1";

    std::wstring bestIp;
    int bestScore = -1000;
    for (auto* a = addrs; a != nullptr; a = a->Next) {
        if (a->OperStatus != IfOperStatusUp) continue;
        if (a->IfType == IF_TYPE_SOFTWARE_LOOPBACK) continue;
        std::wstring desc = a->Description ? a->Description : L"";
        bool isVirtual = (desc.find(L"Virtual") != std::wstring::npos ||
                          desc.find(L"Hyper-V") != std::wstring::npos ||
                          desc.find(L"VMware") != std::wstring::npos ||
                          desc.find(L"VirtualBox") != std::wstring::npos ||
                          desc.find(L"Loopback") != std::wstring::npos ||
                          desc.find(L"WSL") != std::wstring::npos);

        for (auto* u = a->FirstUnicastAddress; u != nullptr; u = u->Next) {
            if (!u->Address.lpSockaddr || u->Address.lpSockaddr->sa_family != AF_INET) continue;
            auto* sin = reinterpret_cast<sockaddr_in*>(u->Address.lpSockaddr);
            uint32_t ipHost = ntohl(sin->sin_addr.s_addr);
            uint8_t b0 = (ipHost >> 24) & 0xFF;
            uint8_t b1 = (ipHost >> 16) & 0xFF;
            if (b0 == 127 || (b0 == 169 && b1 == 254)) continue; // loopback or APIPA

            char ipStr[32] = {};
            inet_ntop(AF_INET, &sin->sin_addr, ipStr, sizeof(ipStr));
            int score = 0;
            if (!isVirtual) score += 100;
            if (a->IfType == IF_TYPE_IEEE80211 || a->IfType == IF_TYPE_ETHERNET_CSMACD) score += 50;
            if (b0 == 192 && b1 == 168) score += 30;
            else if (b0 == 10) score += 20;
            else if (b0 == 172 && b1 >= 16 && b1 <= 31) score += 10;

            if (score > bestScore) {
                bestScore = score;
                bestIp = Utf8ToWide(ipStr);
            }
        }
    }
    return bestIp.empty() ? L"127.0.0.1" : bestIp;
}

bool MobileSyncServer::Start(const std::wstring& clipsDir, uint16_t httpPort, const std::wstring& pin) {
    Stop();
    {
        std::lock_guard<std::mutex> lk(cfgMutex_);
        clipsDir_ = clipsDir;
        pin_ = pin;
        lanIp_ = DetectPrimaryLanIpv4();
        lastError_.clear();
    }
    port_.store(httpPort == 0 ? 47820 : httpPort);
    stop_.store(false);

    WsaScope wsaCheck;
    if (!wsaCheck.ok) {
        std::lock_guard<std::mutex> lk(cfgMutex_);
        lastError_ = L"Winsock başlatılamadı.";
        return false;
    }

    WSADATA wd{};
    if (WSAStartup(MAKEWORD(2, 2), &wd) != 0) {
        std::lock_guard<std::mutex> lk(cfgMutex_);
        lastError_ = L"WSAStartup başarısız.";
        return false;
    }

    SOCKET ls = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (ls == INVALID_SOCKET) {
        WSACleanup();
        std::lock_guard<std::mutex> lk(cfgMutex_);
        lastError_ = L"TCP soketi açılamadı.";
        return false;
    }
    BOOL reuse = TRUE;
    setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&reuse), sizeof(reuse));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(port_.load());
    if (bind(ls, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == SOCKET_ERROR ||
        listen(ls, 16) == SOCKET_ERROR) {
        int err = WSAGetLastError();
        closesocket(ls);
        WSACleanup();
        std::lock_guard<std::mutex> lk(cfgMutex_);
        wchar_t msg[96] = {};
        swprintf(msg, 96, L"Port %u dinlenemedi (kod: %d).", port_.load(), err);
        lastError_ = msg;
        return false;
    }
    listenSock_ = static_cast<uintptr_t>(ls);

    // Best-effort UDP discovery socket on port 47821
    SOCKET us = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (us != INVALID_SOCKET) {
        setsockopt(us, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&reuse), sizeof(reuse));
        BOOL bcast = TRUE;
        setsockopt(us, SOL_SOCKET, SO_BROADCAST, reinterpret_cast<const char*>(&bcast), sizeof(bcast));
        sockaddr_in uaddr{};
        uaddr.sin_family = AF_INET;
        uaddr.sin_addr.s_addr = INADDR_ANY;
        uaddr.sin_port = htons(kUdpDiscoveryPort);
        if (bind(us, reinterpret_cast<sockaddr*>(&uaddr), sizeof(uaddr)) == SOCKET_ERROR) {
            closesocket(us);
            us = INVALID_SOCKET;
        }
    }
    udpSock_ = (us != INVALID_SOCKET) ? static_cast<uintptr_t>(us) : ~uintptr_t(0);

    running_.store(true);
    httpThread_ = std::thread(&MobileSyncServer::HttpAcceptLoop, this);
    if (udpSock_ != ~uintptr_t(0)) {
        udpThread_ = std::thread(&MobileSyncServer::UdpDiscoveryLoop, this);
    }
    Logger::Log("MobileSyncServer: started on %ls:%u", LanIp().c_str(), port_.load());
    return true;
}

void MobileSyncServer::Stop() {
    if (stop_.exchange(true) && !running_.load()) {
        if (httpThread_.joinable()) httpThread_.join();
        if (udpThread_.joinable()) udpThread_.join();
        return;
    }
    if (listenSock_ != ~uintptr_t(0)) {
        closesocket(static_cast<SOCKET>(listenSock_));
        listenSock_ = ~uintptr_t(0);
    }
    if (udpSock_ != ~uintptr_t(0)) {
        closesocket(static_cast<SOCKET>(udpSock_));
        udpSock_ = ~uintptr_t(0);
    }
    if (httpThread_.joinable()) httpThread_.join();
    if (udpThread_.joinable()) udpThread_.join();
    if (running_.exchange(false)) {
        WSACleanup();
        Logger::Log("MobileSyncServer: stopped");
    }
    std::lock_guard<std::mutex> lk(thumbMutex_);
    thumbCache_.clear();
}

void MobileSyncServer::UpdateConfig(const std::wstring& clipsDir, uint16_t httpPort, const std::wstring& pin) {
    bool needRestart = (running_.load() && port_.load() != (httpPort == 0 ? 47820 : httpPort));
    {
        std::lock_guard<std::mutex> lk(cfgMutex_);
        clipsDir_ = clipsDir;
        pin_ = pin;
        lanIp_ = DetectPrimaryLanIpv4();
    }
    if (needRestart) {
        Start(clipsDir, httpPort, pin);
    }
}

std::wstring MobileSyncServer::LanIp() const {
    std::lock_guard<std::mutex> lk(cfgMutex_);
    return lanIp_.empty() ? L"127.0.0.1" : lanIp_;
}

std::wstring MobileSyncServer::PairUrl() const {
    std::lock_guard<std::mutex> lk(cfgMutex_);
    std::wstring ip = lanIp_.empty() ? L"127.0.0.1" : lanIp_;
    wchar_t buf[256] = {};
    swprintf(buf, 256, L"http://%ls:%u/?pin=%ls", ip.c_str(), port_.load(), pin_.c_str());
    return buf;
}

std::wstring MobileSyncServer::LastError() const {
    std::lock_guard<std::mutex> lk(cfgMutex_);
    return lastError_;
}

void MobileSyncServer::HttpAcceptLoop() {
    SOCKET ls = static_cast<SOCKET>(listenSock_);
    while (!stop_.load()) {
        sockaddr_in cli{};
        int cliLen = sizeof(cli);
        SOCKET cs = accept(ls, reinterpret_cast<sockaddr*>(&cli), &cliLen);
        if (cs == INVALID_SOCKET) {
            if (stop_.load()) break;
            Sleep(10);
            continue;
        }
        DWORD tv = 15000;
        setsockopt(cs, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&tv), sizeof(tv));
        setsockopt(cs, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&tv), sizeof(tv));
        std::thread(&MobileSyncServer::HandleHttpClient, this, static_cast<uintptr_t>(cs)).detach();
    }
}

void MobileSyncServer::UdpDiscoveryLoop() {
    SOCKET us = static_cast<SOCKET>(udpSock_);
    wchar_t hostW[128] = L"AnlikKlip-PC";
    DWORD hostLen = 128;
    GetComputerNameW(hostW, &hostLen);
    std::string hostUtf8 = JsonEscape(WideToUtf8(hostW));

    char buf[512];
    while (!stop_.load()) {
        sockaddr_in from{};
        int fromLen = sizeof(from);
        int r = recvfrom(us, buf, sizeof(buf) - 1, 0, reinterpret_cast<sockaddr*>(&from), &fromLen);
        if (r <= 0) {
            if (stop_.load()) break;
            continue;
        }
        buf[r] = '\0';
        if (strncmp(buf, "ANLIKKLIP_DISCOVER", 18) == 0) {
            std::string ip = WideToUtf8(LanIp());
            char reply[512];
            int rn = snprintf(reply, sizeof(reply),
                "{\"app\":\"AnlikKlip\",\"name\":\"%s\",\"ip\":\"%s\",\"port\":%u,\"version\":\"1.2.0\"}",
                hostUtf8.c_str(), ip.c_str(), static_cast<unsigned>(port_.load()));
            if (rn > 0) {
                sendto(us, reply, rn, 0, reinterpret_cast<sockaddr*>(&from), fromLen);
            }
        }
    }
}

bool MobileSyncServer::ExtractJpegThumb(const std::wstring& fullPath, std::vector<uint8_t>& outJpeg) {
    {
        std::lock_guard<std::mutex> lk(thumbMutex_);
        auto it = thumbCache_.find(fullPath);
        if (it != thumbCache_.end()) {
            outJpeg = it->second;
            return !outJpeg.empty();
        }
    }

    ComScope com(COINIT_MULTITHREADED);
    HBITMAP hbmp = DecodeClipThumbnail(fullPath, 320, 180);
    if (!hbmp) return false;

    Gdiplus::GdiplusStartupInput gsi;
    ULONG_PTR gdipToken = 0;
    Gdiplus::GdiplusStartup(&gdipToken, &gsi, nullptr);

    CLSID jpegClsid{};
    if (!GetJpegEncoderClsid(jpegClsid)) {
        if (gdipToken) Gdiplus::GdiplusShutdown(gdipToken);
        DeleteObject(hbmp);
        return false;
    }

    bool ok = false;
    {
        Gdiplus::Bitmap bmp(hbmp, nullptr);
        ComPtr<IStream> istm;
        if (SUCCEEDED(CreateStreamOnHGlobal(nullptr, TRUE, istm.Put())) && istm) {
            Gdiplus::EncoderParameters eps{};
            eps.Count = 1;
            ULONG q = 80;
            eps.Parameter[0].Guid = Gdiplus::EncoderQuality;
            eps.Parameter[0].Type = Gdiplus::EncoderParameterValueTypeLong;
            eps.Parameter[0].NumberOfValues = 1;
            eps.Parameter[0].Value = &q;
            if (bmp.Save(istm.Get(), &jpegClsid, &eps) == Gdiplus::Ok) {
                HGLOBAL hg = nullptr;
                if (SUCCEEDED(GetHGlobalFromStream(istm.Get(), &hg)) && hg) {
                    SIZE_T len = GlobalSize(hg);
                    void* ptr = GlobalLock(hg);
                    if (ptr && len > 0) {
                        outJpeg.assign(reinterpret_cast<const uint8_t*>(ptr),
                                       reinterpret_cast<const uint8_t*>(ptr) + len);
                        ok = true;
                        GlobalUnlock(hg);
                    }
                }
            }
        }
    }
    if (gdipToken) Gdiplus::GdiplusShutdown(gdipToken);
    DeleteObject(hbmp);

    if (ok) {
        std::lock_guard<std::mutex> lk(thumbMutex_);
        if (thumbCache_.size() >= 24) thumbCache_.clear();
        thumbCache_[fullPath] = outJpeg;
    }
    return ok;
}

void MobileSyncServer::HandleHttpClient(uintptr_t sockVal) {
    SOCKET s = static_cast<SOCKET>(sockVal);
    std::string req;
    req.reserve(2048);
    char tmp[2048];
    while (req.find("\r\n\r\n") == std::string::npos && req.size() < 8192) {
        int r = recv(s, tmp, sizeof(tmp), 0);
        if (r <= 0) break;
        req.append(tmp, static_cast<size_t>(r));
    }

    size_t lineEnd = req.find("\r\n");
    if (lineEnd == std::string::npos) {
        closesocket(s);
        return;
    }
    std::string reqLine = req.substr(0, lineEnd);
    std::istringstream iss(reqLine);
    std::string method, target;
    iss >> method >> target;

    if (method == "OPTIONS") {
        SendHttpResponse(s, 204, "No Content", "text/plain", "",
            "Access-Control-Allow-Headers: X-Pin, Range\r\nAccess-Control-Allow-Methods: GET, HEAD, OPTIONS\r\n");
        closesocket(s);
        return;
    }
    if (method != "GET" && method != "HEAD") {
        SendHttpResponse(s, 405, "Method Not Allowed", "application/json", "{\"error\":\"method\"}");
        closesocket(s);
        return;
    }

    std::string path = target;
    std::string query;
    size_t qpos = target.find('?');
    if (qpos != std::string::npos) {
        path = target.substr(0, qpos);
        query = target.substr(qpos + 1);
    }

    if (path == "/" || path == "/index.html") {
        SendHttpResponse(s, 200, "OK", "text/html; charset=utf-8",
            (method == "HEAD") ? std::string() : std::string(kMobileWebHtml));
        closesocket(s);
        return;
    }

    // Extract headers (X-Pin and Range)
    std::string hdrPin, hdrRange;
    {
        size_t pos = lineEnd + 2;
        while (pos < req.size()) {
            size_t nxt = req.find("\r\n", pos);
            if (nxt == std::string::npos || nxt == pos) break;
            std::string h = req.substr(pos, nxt - pos);
            size_t col = h.find(':');
            if (col != std::string::npos) {
                std::string k = h.substr(0, col);
                std::string v = h.substr(col + 1);
                size_t first = v.find_first_not_of(" \t");
                if (first != std::string::npos) v = v.substr(first);
                if (_stricmp(k.c_str(), "X-Pin") == 0) hdrPin = v;
                else if (_stricmp(k.c_str(), "Range") == 0) hdrRange = v;
            }
            pos = nxt + 2;
        }
    }

    std::wstring clipsDir, expectedPin;
    {
        std::lock_guard<std::mutex> lk(cfgMutex_);
        clipsDir = clipsDir_;
        expectedPin = pin_;
    }

    std::string gotPin = !hdrPin.empty() ? hdrPin : GetQueryParam(query, "pin");
    if (expectedPin.empty() || Utf8ToWide(gotPin) != expectedPin) {
        SendHttpResponse(s, 401, "Unauthorized", "application/json", "{\"error\":\"invalid_pin\"}");
        closesocket(s);
        return;
    }

    if (path == "/api/info") {
        wchar_t hostW[128] = L"AnlikKlip-PC";
        DWORD hostLen = 128;
        GetComputerNameW(hostW, &hostLen);
        auto clips = ScanClipsFolder(clipsDir, GetFavoritesFilePath());
        char body[512];
        snprintf(body, sizeof(body),
            "{\"pcName\":\"%s\",\"port\":%u,\"clipCount\":%zu,\"version\":\"1.2.0\"}",
            JsonEscape(WideToUtf8(hostW)).c_str(),
            static_cast<unsigned>(port_.load()),
            clips.size());
        SendHttpResponse(s, 200, "OK", "application/json; charset=utf-8",
            (method == "HEAD") ? std::string() : std::string(body));
        closesocket(s);
        return;
    }

    if (path == "/api/clips") {
        auto clips = ScanClipsFolder(clipsDir, GetFavoritesFilePath());
        std::string json = "[";
        for (size_t i = 0; i < clips.size(); ++i) {
            const auto& c = clips[i];
            if (i > 0) json += ',';
            char row[768];
            snprintf(row, sizeof(row),
                "{\"fileName\":\"%s\",\"sizeBytes\":%llu,\"durationSec\":%.2f,\"width\":%u,\"height\":%u,\"date\":\"%s\",\"isFavorite\":%s}",
                JsonEscape(WideToUtf8(c.fileName)).c_str(),
                static_cast<unsigned long long>(c.fileSizeBytes),
                c.durationSeconds,
                c.width,
                c.height,
                JsonEscape(WideToUtf8(c.displayDate)).c_str(),
                c.isFavorite ? "true" : "false");
            json += row;
        }
        json += "]";
        SendHttpResponse(s, 200, "OK", "application/json; charset=utf-8",
            (method == "HEAD") ? std::string() : json);
        closesocket(s);
        return;
    }

    if (path == "/api/thumb") {
        std::wstring fn = Utf8ToWide(GetQueryParam(query, "file"));
        if (!IsSafeMp4FileName(fn)) {
            SendHttpResponse(s, 400, "Bad Request", "application/json", "{\"error\":\"bad_file\"}");
            closesocket(s);
            return;
        }
        std::wstring fullPath = clipsDir + L"\\" + fn;
        std::vector<uint8_t> jpeg;
        if (!ExtractJpegThumb(fullPath, jpeg) || jpeg.empty()) {
            SendHttpResponse(s, 404, "Not Found", "application/json", "{\"error\":\"no_thumb\"}");
            closesocket(s);
            return;
        }
        std::string body(reinterpret_cast<const char*>(jpeg.data()), jpeg.size());
        SendHttpResponse(s, 200, "OK", "image/jpeg", (method == "HEAD") ? std::string() : body,
            "Cache-Control: public, max-age=300\r\n");
        closesocket(s);
        return;
    }

    if (path == "/api/clip") {
        std::wstring fn = Utf8ToWide(GetQueryParam(query, "file"));
        if (!IsSafeMp4FileName(fn)) {
            SendHttpResponse(s, 400, "Bad Request", "application/json", "{\"error\":\"bad_file\"}");
            closesocket(s);
            return;
        }
        std::wstring fullPath = clipsDir + L"\\" + fn;
        HANDLE hf = CreateFileW(fullPath.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
            nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
        if (hf == INVALID_HANDLE_VALUE) {
            SendHttpResponse(s, 404, "Not Found", "application/json", "{\"error\":\"not_found\"}");
            closesocket(s);
            return;
        }
        LARGE_INTEGER fileSz{};
        GetFileSizeEx(hf, &fileSz);
        const uint64_t totalSz = static_cast<uint64_t>(fileSz.QuadPart);
        if (totalSz == 0) {
            CloseHandle(hf);
            SendHttpResponse(s, 200, "OK", "video/mp4", "");
            closesocket(s);
            return;
        }

        uint64_t rangeStart = 0;
        uint64_t rangeEnd = totalSz - 1;
        bool isPartial = false;
        if (!hdrRange.empty() && _strnicmp(hdrRange.c_str(), "bytes=", 6) == 0) {
            std::string spec = hdrRange.substr(6);
            size_t dash = spec.find('-');
            if (dash != std::string::npos) {
                std::string s0 = spec.substr(0, dash);
                std::string s1 = spec.substr(dash + 1);
                if (!s0.empty()) {
                    rangeStart = _strtoui64(s0.c_str(), nullptr, 10);
                    if (!s1.empty()) rangeEnd = _strtoui64(s1.c_str(), nullptr, 10);
                } else if (!s1.empty()) {
                    uint64_t suffix = _strtoui64(s1.c_str(), nullptr, 10);
                    rangeStart = (suffix < totalSz) ? (totalSz - suffix) : 0;
                }
                if (rangeStart >= totalSz) {
                    CloseHandle(hf);
                    char cr[64];
                    snprintf(cr, sizeof(cr), "Content-Range: bytes */%llu\r\n", static_cast<unsigned long long>(totalSz));
                    SendHttpResponse(s, 416, "Range Not Satisfiable", "text/plain", "", cr);
                    closesocket(s);
                    return;
                }
                if (rangeEnd >= totalSz) rangeEnd = totalSz - 1;
                if (rangeEnd < rangeStart) rangeEnd = rangeStart;
                isPartial = true;
            }
        }

        const uint64_t sendLen = (rangeEnd - rangeStart) + 1;
        std::string utf8Fn = WideToUtf8(fn);
        char hdr[1024];
        int hn = 0;
        if (isPartial) {
            hn = snprintf(hdr, sizeof(hdr),
                "HTTP/1.1 206 Partial Content\r\n"
                "Content-Type: video/mp4\r\n"
                "Accept-Ranges: bytes\r\n"
                "Content-Range: bytes %llu-%llu/%llu\r\n"
                "Content-Length: %llu\r\n"
                "Content-Disposition: inline; filename=\"%s\"\r\n"
                "Access-Control-Allow-Origin: *\r\n"
                "Connection: close\r\n\r\n",
                static_cast<unsigned long long>(rangeStart),
                static_cast<unsigned long long>(rangeEnd),
                static_cast<unsigned long long>(totalSz),
                static_cast<unsigned long long>(sendLen),
                utf8Fn.c_str());
        } else {
            hn = snprintf(hdr, sizeof(hdr),
                "HTTP/1.1 200 OK\r\n"
                "Content-Type: video/mp4\r\n"
                "Accept-Ranges: bytes\r\n"
                "Content-Length: %llu\r\n"
                "Content-Disposition: attachment; filename=\"%s\"\r\n"
                "Access-Control-Allow-Origin: *\r\n"
                "Connection: close\r\n\r\n",
                static_cast<unsigned long long>(sendLen),
                utf8Fn.c_str());
        }

        if (hn > 0 && SendAll(s, hdr, static_cast<size_t>(hn)) && method == "GET") {
            LARGE_INTEGER seekPos{};
            seekPos.QuadPart = static_cast<LONGLONG>(rangeStart);
            SetFilePointerEx(hf, seekPos, nullptr, FILE_BEGIN);

            std::vector<char> buf(65536);
            uint64_t left = sendLen;
            while (left > 0 && !stop_.load()) {
                DWORD want = static_cast<DWORD>(std::min<uint64_t>(left, buf.size()));
                DWORD got = 0;
                if (!ReadFile(hf, buf.data(), want, &got, nullptr) || got == 0) break;
                if (!SendAll(s, buf.data(), got)) break;
                left -= got;
            }
        }
        CloseHandle(hf);
        closesocket(s);
        return;
    }

    SendHttpResponse(s, 404, "Not Found", "application/json", "{\"error\":\"not_found\"}");
    closesocket(s);
}

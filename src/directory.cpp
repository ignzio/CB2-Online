#include "directory.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <winhttp.h>
#pragma comment(lib, "winhttp.lib")
#pragma comment(lib, "ws2_32.lib")
#endif

namespace cb2net {
namespace {

double clockSeconds() {
    static const auto start = std::chrono::steady_clock::now();
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
}

std::vector<std::string> split(const std::string& s, char sep) {
    std::vector<std::string> out;
    size_t pos = 0;
    for (;;) {
        const size_t e = s.find(sep, pos);
        out.push_back(s.substr(pos, e == std::string::npos ? std::string::npos : e - pos));
        if (e == std::string::npos) break;
        pos = e + 1;
    }
    return out;
}

// Only characters the Worker accepts go into the request body, so no escaping is needed.
std::string only(const std::string& s, const char* allowed, size_t maxLen) {
    std::string o;
    for (char c : s) if (std::strchr(allowed, c) && c && o.size() < maxLen) o += c;
    return o;
}
const char* kNameChars = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789 _.-";
const char* kHostChars = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789.:-";
const char* kIpChars = "0123456789.";

}  // namespace

bool parseEndpoint(const std::string& text, Endpoint& out) {
#ifdef _WIN32
    const size_t colon = text.rfind(':');
    if (colon == std::string::npos) return false;
    in_addr a{};
    if (inet_pton(AF_INET, text.substr(0, colon).c_str(), &a) != 1) return false;
    const int port = std::atoi(text.c_str() + colon + 1);
    if (port <= 0 || port > 65535) return false;
    out.ip = a.s_addr;
    out.port = htons(uint16_t(port));
    return true;
#else
    (void)text; (void)out;
    return false;
#endif
}

std::string endpointText(const Endpoint& e) {
#ifdef _WIN32
    char b[64] = {};
    in_addr a;
    a.s_addr = e.ip;
    inet_ntop(AF_INET, &a, b, sizeof b);
    return std::string(b) + ":" + std::to_string(ntohs(e.port));
#else
    (void)e;
    return {};
#endif
}

std::string ipText(const Endpoint& e) {
    const std::string t = endpointText(e);
    return t.substr(0, t.rfind(':'));
}

#ifdef _WIN32

namespace {
std::wstring widen(const std::string& s) { return std::wstring(s.begin(), s.end()); }
}

bool DirectoryClient::http(const std::string& method, const std::string& path, const std::string& body, Response& out) {
    const std::wstring wurl = widen(baseUrl);
    URL_COMPONENTS uc{};
    wchar_t host[256] = {}, urlPath[1024] = {};
    uc.dwStructSize = sizeof uc;
    uc.lpszHostName = host;
    uc.dwHostNameLength = 255;
    uc.lpszUrlPath = urlPath;
    uc.dwUrlPathLength = 1023;
    if (!WinHttpCrackUrl(wurl.c_str(), 0, 0, &uc)) { std::lock_guard<std::mutex> l(mu); error = "bad directory address"; return false; }
    const HINTERNET session = WinHttpOpen(L"cb2-lobby/1", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session) return false;
    WinHttpSetTimeouts(session, 4000, 4000, 4000, 6000);
    bool ok = false;
    if (const HINTERNET conn = WinHttpConnect(session, host, uc.nPort, 0)) {
        std::wstring p = urlPath;
        while (p.size() > 1 && p.back() == L'/') p.pop_back();
        if (p == L"/") p.clear();
        p += widen(path);
        const bool secure = uc.nScheme == INTERNET_SCHEME_HTTPS;
        if (const HINTERNET req = WinHttpOpenRequest(conn, widen(method).c_str(), p.c_str(), nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, secure ? WINHTTP_FLAG_SECURE : 0)) {
            const wchar_t* headers = L"Content-Type: application/json";
            const BOOL sent = WinHttpSendRequest(req, body.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : headers, body.empty() ? 0 : DWORD(-1L),
                                                 body.empty() ? WINHTTP_NO_REQUEST_DATA : const_cast<char*>(body.data()), DWORD(body.size()), DWORD(body.size()), 0);
            if (sent && WinHttpReceiveResponse(req, nullptr)) {
                DWORD status = 0, size = sizeof status;
                WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &status, &size, WINHTTP_NO_HEADER_INDEX);
                out.status = int(status);
                out.body.clear();
                for (;;) {
                    DWORD avail = 0;
                    if (!WinHttpQueryDataAvailable(req, &avail) || avail == 0) break;
                    std::string chunk(avail, '\0');
                    DWORD got = 0;
                    if (!WinHttpReadData(req, chunk.data(), avail, &got) || got == 0) break;
                    out.body.append(chunk.data(), got);
                    if (out.body.size() > 64 * 1024) break;
                }
                ok = true;
            }
            WinHttpCloseHandle(req);
        }
        WinHttpCloseHandle(conn);
    }
    WinHttpCloseHandle(session);
    if (!ok) { std::lock_guard<std::mutex> l(mu); error = "the lobby directory does not answer"; }
    return ok;
}

#else

bool DirectoryClient::http(const std::string&, const std::string&, const std::string&, Response&) { return false; }

#endif

bool DirectoryClient::start(const std::string& url) {
    stop();
    baseUrl = url;
    while (!baseUrl.empty() && baseUrl.back() == ' ') baseUrl.pop_back();
    if (baseUrl.rfind("http://", 0) != 0 && baseUrl.rfind("https://", 0) != 0) {
        if (baseUrl.empty()) return false;
        baseUrl = "https://" + baseUrl;
    }
    running = true;
    worker = std::thread([this] { loop(); });
    return true;
}

void DirectoryClient::stop() {
    if (!running && !worker.joinable()) return;
    running = false;
    wake();
    if (worker.joinable()) worker.join();
    std::string leaveBody;
    {
        std::lock_guard<std::mutex> l(mu);
        if (memberSet && !mRoom.empty() && mPort) leaveBody = "{\"room\":\"" + mRoom + "\",\"port\":" + std::to_string(mPort) + ",\"ip\":\"" + mIp + "\",\"region\":\"" + regionText(localRegion()) + "\"}";
        memberSet = joinPending = false;
    }
    if (!leaveBody.empty()) { Response r; http("POST", "/leave", leaveBody, r); }   // the room's count drops at once instead of after the entry expires
}

void DirectoryClient::setMember(const std::string& room, uint32_t uid, const std::string& name, const std::string& ip, uint16_t port, const std::string& rendezvous, bool locked) {
    std::lock_guard<std::mutex> l(mu);
    const std::string r = only(room, kNameChars, 15), n = only(name, kNameChars, 15), i = only(ip, kIpChars, 15), rv = only(rendezvous, kHostChars, 64);
    if (memberSet && r == mRoom && n == mName && i == mIp && rv == mRendezvous && uid == mUid && port == mPort && locked == mLocked) return;
    memberSet = true;
    mLocked = locked;
    mRoom = r; mName = n; mIp = i; mRendezvous = rv; mUid = uid; mPort = port;
    lastAnnounce = -1e9;   // announce at once
    cv.notify_all();
}

void DirectoryClient::clearMember() {
    std::lock_guard<std::mutex> l(mu);
    memberSet = false;
}

void DirectoryClient::requestJoin(const std::string& room, const std::string& ip, uint16_t port) {
    std::lock_guard<std::mutex> l(mu);
    joinPending = true;
    jRoom = only(room, kNameChars, 15);
    jIp = only(ip, kIpChars, 15);
    jPort = port;
    cv.notify_all();
}

void DirectoryClient::loop() {
    while (running) {
        const double now = clockSeconds();
        bool announce = false, join = false, list = false;
        std::string body, jbody;
        {
            std::lock_guard<std::mutex> l(mu);
            if (memberSet && now - lastAnnounce >= 5.0) {
                announce = true;
                lastAnnounce = now;
                body = "{\"kind\":\"member\",\"room\":\"" + mRoom + "\",\"uid\":" + std::to_string(mUid) + ",\"name\":\"" + mName + "\",\"port\":" + std::to_string(mPort) +
                       ",\"ip\":\"" + mIp + "\",\"version\":\"" + kVersion + "\",\"region\":\"" + regionText(localRegion()) + "\",\"locked\":" + (mLocked ? "true" : "false") + ",\"rendezvous\":\"" + mRendezvous + "\"}";
            }
            if (joinPending) {
                join = true;
                joinPending = false;
                jbody = "{\"room\":\"" + jRoom + "\",\"port\":" + std::to_string(jPort) + ",\"ip\":\"" + jIp + "\",\"version\":\"" + kVersion + "\",\"region\":\"" + regionText(localRegion()) + "\"}";
            }
            if (listWanted && now - lastList >= 3.0) { list = true; lastList = now; }
        }
        Response r;
        if (join && http("POST", "/join", jbody, r) && r.status == 200) {
            std::lock_guard<std::mutex> l(mu);
            lastOk = clockSeconds();
            for (const std::string& line : split(r.body, '\n')) {
                const auto f = split(line, '\t');
                Endpoint e;
                if (f.size() >= 2 && f[0] == "E" && parseEndpoint(f[1], e)) joinReply.push_back(e);
            }
        }
        if (announce && http("POST", "/announce", body, r) && r.status == 200) {
            std::lock_guard<std::mutex> l(mu);
            lastOk = clockSeconds();
            for (const std::string& line : split(r.body, '\n')) {
                const auto f = split(line, '\t');
                Endpoint e;
                if (f.size() >= 2 && f[0] == "J" && parseEndpoint(f[1], e) && std::find(joiners.begin(), joiners.end(), e) == joiners.end()) joiners.push_back(e);
            }
        }
        if (list && http("GET", std::string("/rooms?version=") + kVersion + "&region=" + regionText(localRegion()), "", r) && r.status == 200) {
            std::vector<DirRoom> rl;
            std::vector<DirServer> sl;
            for (const std::string& line : split(r.body, '\n')) {
                const auto f = split(line, '\t');
                if (f.size() >= 6 && f[0] == "R") {
                    DirRoom d;
                    d.name = f[1]; d.players = std::atoi(f[2].c_str()); d.version = f[4]; d.rendezvous = f[5]; d.locked = f.size() >= 7 && f[6] == "1";
                    for (const std::string& ep : split(f[3], ',')) { Endpoint e; if (parseEndpoint(ep, e)) d.endpoints.push_back(e); }
                    rl.push_back(d);
                } else if (f.size() >= 5 && f[0] == "S") {
                    DirServer s;
                    s.name = f[1]; s.players = std::atoi(f[2].c_str()); s.text = f[3]; s.version = f[4];
                    if (parseEndpoint(f[3], s.ep)) sl.push_back(s);
                }
            }
            std::lock_guard<std::mutex> l(mu);
            lastOk = clockSeconds();
            roomList.swap(rl);
            serverList.swap(sl);
        }
        std::unique_lock<std::mutex> lk(mu);
        cv.wait_for(lk, std::chrono::milliseconds(300));
    }
}

std::vector<DirRoom> DirectoryClient::rooms() const { std::lock_guard<std::mutex> l(mu); return roomList; }
std::vector<DirServer> DirectoryClient::servers() const { std::lock_guard<std::mutex> l(mu); return serverList; }
std::vector<Endpoint> DirectoryClient::takeJoiners() { std::lock_guard<std::mutex> l(mu); std::vector<Endpoint> out; out.swap(joiners); return out; }
std::vector<Endpoint> DirectoryClient::takeJoinReply() { std::lock_guard<std::mutex> l(mu); std::vector<Endpoint> out; out.swap(joinReply); return out; }
bool DirectoryClient::reachable() const { std::lock_guard<std::mutex> l(mu); return clockSeconds() - lastOk < 20.0; }
std::string DirectoryClient::lastError() const { std::lock_guard<std::mutex> l(mu); return error; }

}  // namespace cb2net

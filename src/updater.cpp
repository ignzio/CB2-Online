#include "updater.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <winhttp.h>
#include <bcrypt.h>
#pragma comment(lib, "winhttp.lib")
#pragma comment(lib, "bcrypt.lib")
#endif

namespace fs = std::filesystem;

namespace cb2update {
namespace {

std::vector<long> versionParts(std::string v) {
    if (!v.empty() && (v[0] == 'v' || v[0] == 'V')) v.erase(0, 1);
    v = v.substr(0, v.find_first_of("-+"));
    std::vector<long> parts;
    for (size_t pos = 0;;) {
        const size_t dot = v.find('.', pos);
        parts.push_back(std::atol(v.substr(pos, dot == std::string::npos ? std::string::npos : dot - pos).c_str()));
        if (dot == std::string::npos) break;
        pos = dot + 1;
    }
    return parts;
}

// ---- a small JSON reader: just enough for the Releases API ----
struct Json {
    enum Type { Null, Bool, Num, Str, Arr, Obj } type = Null;
    std::string text;
    double number = 0;
    std::vector<Json> items;
    std::vector<std::pair<std::string, Json>> members;
    const Json* get(const char* key) const {
        for (const auto& m : members) if (m.first == key) return &m.second;
        return nullptr;
    }
    std::string str(const char* key) const {
        const Json* j = get(key);
        return j && j->type == Str ? j->text : std::string();
    }
};

struct JsonReader {
    const std::string& s;
    size_t i = 0;
    bool ok = true;
    explicit JsonReader(const std::string& text) : s(text) {}

    void space() { while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i]))) ++i; }
    bool eat(char c) { space(); if (i < s.size() && s[i] == c) { ++i; return true; } return false; }

    std::string string() {
        std::string out;
        if (!eat('"')) { ok = false; return out; }
        while (i < s.size() && s[i] != '"') {
            char c = s[i++];
            if (c != '\\') { out += c; continue; }
            if (i >= s.size()) break;
            c = s[i++];
            switch (c) {
            case 'n': out += '\n'; break;
            case 't': out += '\t'; break;
            case 'r': out += '\r'; break;
            case 'b': out += '\b'; break;
            case 'f': out += '\f'; break;
            case 'u': {
                if (i + 4 > s.size()) { ok = false; return out; }
                const unsigned cp = unsigned(std::strtoul(s.substr(i, 4).c_str(), nullptr, 16));
                i += 4;
                if (cp < 0x80) out += char(cp);
                else if (cp < 0x800) { out += char(0xC0 | (cp >> 6)); out += char(0x80 | (cp & 0x3F)); }
                else if (cp >= 0xD800 && cp < 0xE000) out += '?';   // surrogate half: not needed here
                else { out += char(0xE0 | (cp >> 12)); out += char(0x80 | ((cp >> 6) & 0x3F)); out += char(0x80 | (cp & 0x3F)); }
                break;
            }
            default: out += c; break;   // " \ /
            }
        }
        if (i >= s.size()) ok = false; else ++i;
        return out;
    }

    Json value(int depth) {
        Json j;
        space();
        if (depth > 16 || i >= s.size()) { ok = false; return j; }
        const char c = s[i];
        if (c == '{') {
            ++i;
            j.type = Json::Obj;
            if (eat('}')) return j;
            do {
                space();
                std::string key = string();
                if (!eat(':')) { ok = false; return j; }
                j.members.emplace_back(std::move(key), value(depth + 1));
            } while (ok && eat(','));
            if (!eat('}')) ok = false;
        } else if (c == '[') {
            ++i;
            j.type = Json::Arr;
            if (eat(']')) return j;
            do j.items.push_back(value(depth + 1)); while (ok && eat(','));
            if (!eat(']')) ok = false;
        } else if (c == '"') {
            j.type = Json::Str;
            j.text = string();
        } else {
            const size_t start = i;
            while (i < s.size() && s[i] != ',' && s[i] != '}' && s[i] != ']' && !std::isspace(static_cast<unsigned char>(s[i]))) ++i;
            const std::string word = s.substr(start, i - start);
            if (word == "true" || word == "false") { j.type = Json::Bool; j.number = word == "true"; }
            else if (word == "null") j.type = Json::Null;
            else { j.type = Json::Num; j.number = std::atof(word.c_str()); }
        }
        return j;
    }
};

bool safeRepo(const std::string& repo) {
    if (repo.empty() || repo.find('/') == std::string::npos) return false;
    for (char c : repo) if (!std::isalnum(static_cast<unsigned char>(c)) && c != '/' && c != '-' && c != '_' && c != '.') return false;
    return true;
}

#ifdef _WIN32

fs::path selfExe() {
    wchar_t buf[MAX_PATH * 2] = {};
    const DWORD n = GetModuleFileNameW(nullptr, buf, DWORD(std::size(buf)));
    return n ? fs::path(std::wstring(buf, n)) : fs::path();
}

struct HttpResult {
    int status = 0;
    uint64_t length = 0;
};

// GET with redirects. `sink` gets the body in pieces and returns false to stop (too big, disk full ...).
bool httpGet(const std::string& url, const wchar_t* accept, const std::function<bool(const char*, size_t)>& sink, HttpResult& result) {
    const std::wstring wurl(url.begin(), url.end());
    URL_COMPONENTS uc{};
    wchar_t host[256] = {}, path[2048] = {}, extra[2048] = {};
    uc.dwStructSize = sizeof uc;
    uc.lpszHostName = host;
    uc.dwHostNameLength = 255;
    uc.lpszUrlPath = path;
    uc.dwUrlPathLength = 2047;
    uc.lpszExtraInfo = extra;
    uc.dwExtraInfoLength = 2047;
    if (!WinHttpCrackUrl(wurl.c_str(), 0, 0, &uc) || uc.nScheme != INTERNET_SCHEME_HTTPS) return false;
    const HINTERNET session = WinHttpOpen(L"CB2-Online-updater/1", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session) return false;
    WinHttpSetTimeouts(session, 10000, 10000, 20000, 30000);
    bool ok = false;
    if (const HINTERNET conn = WinHttpConnect(session, host, uc.nPort, 0)) {
        const std::wstring target = std::wstring(path) + extra;
        if (const HINTERNET req = WinHttpOpenRequest(conn, L"GET", target.c_str(), nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE)) {
            // never follow a redirect to plain http
            DWORD policy = WINHTTP_OPTION_REDIRECT_POLICY_DISALLOW_HTTPS_TO_HTTP;
            WinHttpSetOption(req, WINHTTP_OPTION_REDIRECT_POLICY, &policy, sizeof policy);
            const std::wstring headers = std::wstring(L"Accept: ") + accept;
            if (WinHttpSendRequest(req, headers.c_str(), DWORD(-1L), WINHTTP_NO_REQUEST_DATA, 0, 0, 0) && WinHttpReceiveResponse(req, nullptr)) {
                DWORD status = 0, size = sizeof status;
                WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &status, &size, WINHTTP_NO_HEADER_INDEX);
                result.status = int(status);
                wchar_t len[32] = {};
                size = sizeof len;
                if (WinHttpQueryHeaders(req, WINHTTP_QUERY_CONTENT_LENGTH, WINHTTP_HEADER_NAME_BY_INDEX, len, &size, WINHTTP_NO_HEADER_INDEX)) result.length = _wcstoui64(len, nullptr, 10);
                ok = status == 200;
                std::vector<char> chunk(64 * 1024);
                while (ok) {
                    DWORD got = 0;
                    if (!WinHttpReadData(req, chunk.data(), DWORD(chunk.size()), &got)) { ok = false; break; }
                    if (got == 0) break;
                    if (!sink(chunk.data(), got)) { ok = false; break; }
                }
            }
            WinHttpCloseHandle(req);
        }
        WinHttpCloseHandle(conn);
    }
    WinHttpCloseHandle(session);
    return ok;
}

bool sha256File(const fs::path& file, std::string& hex) {
    BCRYPT_ALG_HANDLE alg = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    bool ok = false;
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) == 0) {
        if (BCryptCreateHash(alg, &hash, nullptr, 0, nullptr, 0, 0) == 0) {
            std::ifstream in(file, std::ios::binary);
            std::vector<char> buf(64 * 1024);
            ok = bool(in);
            while (ok && in) {
                in.read(buf.data(), std::streamsize(buf.size()));
                const std::streamsize got = in.gcount();
                if (got > 0 && BCryptHashData(hash, reinterpret_cast<PUCHAR>(buf.data()), ULONG(got), 0) != 0) ok = false;
            }
            unsigned char digest[32] = {};
            if (ok && BCryptFinishHash(hash, digest, sizeof digest, 0) == 0) {
                static const char* digits = "0123456789abcdef";
                hex.clear();
                for (unsigned char b : digest) { hex += digits[b >> 4]; hex += digits[b & 15]; }
            } else ok = false;
            BCryptDestroyHash(hash);
        }
        BCryptCloseAlgorithmProvider(alg, 0);
    }
    return ok;
}

#endif  // _WIN32

}  // namespace

bool isNewer(const std::string& a, const std::string& b) {
    std::vector<long> pa = versionParts(a), pb = versionParts(b);
    const size_t n = std::max(pa.size(), pb.size());
    pa.resize(n);
    pb.resize(n);
    return pa > pb;
}

void cleanLeftovers() {
#ifdef _WIN32
    const fs::path self = selfExe();
    if (self.empty()) return;
    std::error_code ec;
    for (const fs::path& base : {self, self.parent_path() / L"Crash2 Server.exe"})
        for (const wchar_t* suffix : {L".old", L".new"}) {
            fs::path p = base;
            p += suffix;
            fs::remove(p, ec);
        }
#endif
}

std::string Updater::latest() const { std::lock_guard<std::mutex> l(mu); return latestVersion; }
std::string Updater::error() const { std::lock_guard<std::mutex> l(mu); return errorText; }
std::string Updater::notes() const { std::lock_guard<std::mutex> l(mu); return notesText; }

void Updater::fail(const std::string& why) {
    { std::lock_guard<std::mutex> l(mu); errorText = why; }
    st = State::Failed;
}

void Updater::startCheck(const std::string& repo, const std::string& currentVersion) {
    if (st == State::Checking || st == State::Downloading) return;
    join();
    st = State::Checking;
    worker = std::thread([this, repo, currentVersion] { check(repo, currentVersion); });
}

void Updater::startDownload() {
    if (st != State::Available) return;
    join();
    pct = 0;
    st = State::Downloading;
    worker = std::thread([this] { download(); });
}

#ifdef _WIN32

void Updater::check(std::string repo, std::string current) {
    if (!safeRepo(repo)) return fail("bad update address");
    std::string body;
    HttpResult res;
    const bool got = httpGet("https://api.github.com/repos/" + repo + "/releases/latest", L"application/vnd.github+json",
                             [&](const char* d, size_t n) { body.append(d, n); return body.size() < (2u << 20); }, res);
    if (!got) return fail(res.status == 404 ? "no release published yet" : "could not reach GitHub");
    JsonReader reader(body);
    const Json root = reader.value(0);
    if (!reader.ok || root.type != Json::Obj) return fail("unreadable answer from GitHub");
    const std::string tag = root.str("tag_name");
    if (tag.empty()) return fail("release without a version");
    if (!isNewer(tag, current)) { st = State::UpToDate; return; }

    const fs::path self = selfExe();
    if (self.empty()) return fail("cannot find this program");
    const std::string prefix = "https://github.com/" + repo + "/releases/download/";
    std::vector<Job> found;
    bool haveSelf = false;
    if (const Json* assets = root.get("assets")) {
        for (const Json& a : assets->items) {
            Job job;
            job.name = a.str("name");
            job.url = a.str("browser_download_url");
            std::string digest = a.str("digest");
            if (const Json* s = a.get("size")) job.size = uint64_t(s->number);
            fs::path target;
            if (job.name == "Crash2.exe") { target = self; haveSelf = true; }
            else if (job.name == "Crash2-Server.exe") {
                target = self.parent_path() / L"Crash2 Server.exe";
                std::error_code ec;
                if (!fs::exists(target, ec)) continue;   // only update a server the player actually has
            } else continue;
            // the download must come from this project's releases and carry a checksum, or it is not offered
            if (job.url.rfind(prefix, 0) != 0 || digest.rfind("sha256:", 0) != 0 || digest.size() != 7 + 64) return fail("release has no checksum");
            job.sha256 = digest.substr(7);
            std::transform(job.sha256.begin(), job.sha256.end(), job.sha256.begin(), [](unsigned char c) { return char(std::tolower(c)); });
            job.target = target.wstring();
            job.temp = target.wstring() + L".new";
            found.push_back(std::move(job));
        }
    }
    if (!haveSelf) return fail("release has no Crash2.exe");
    std::string notes = root.str("body");
    if (notes.size() > 400) notes.resize(400);
    {
        std::lock_guard<std::mutex> l(mu);
        latestVersion = tag[0] == 'v' || tag[0] == 'V' ? tag.substr(1) : tag;
        notesText = notes;
        jobs = std::move(found);
    }
    st = State::Available;
}

void Updater::download() {
    std::vector<Job> list;
    { std::lock_guard<std::mutex> l(mu); list = jobs; }
    uint64_t total = 0, done = 0;
    for (const Job& j : list) total += j.size;
    for (const Job& j : list) {
        const fs::path temp(j.temp);
        std::ofstream out(temp, std::ios::binary | std::ios::trunc);
        if (!out) return fail("the game folder is not writable");
        bool diskOk = true;
        HttpResult res;
        const bool got = httpGet(j.url, L"application/octet-stream", [&](const char* d, size_t n) {
            out.write(d, std::streamsize(n));
            done += n;
            if (total) pct = int(std::min<uint64_t>(99, done * 100 / total));
            diskOk = bool(out);
            return diskOk && (j.size == 0 || done <= total + (1u << 20));
        }, res);
        out.close();
        if (!got || !diskOk) {
            std::error_code ec;
            fs::remove(temp, ec);
            return fail(diskOk ? "download failed" : "no room to save the update");
        }
        std::string hex;
        char magic[2] = {};
        std::ifstream(temp, std::ios::binary).read(magic, 2);
        if (!sha256File(temp, hex) || hex != j.sha256 || magic[0] != 'M' || magic[1] != 'Z') {
            std::error_code ec;
            fs::remove(temp, ec);
            return fail("download did not match its checksum");
        }
    }
    pct = 100;
    st = State::Ready;
}

bool Updater::install(std::string& error) {
    if (st != State::Ready) { error = "nothing downloaded"; return false; }
    std::vector<Job> list;
    { std::lock_guard<std::mutex> l(mu); list = jobs; }
    std::vector<const Job*> swapped;
    for (const Job& j : list) {
        const std::wstring old = j.target + L".old";
        const bool hadOld = fs::exists(fs::path(j.target));
        if (hadOld && !MoveFileExW(j.target.c_str(), old.c_str(), MOVEFILE_REPLACE_EXISTING)) { error = "could not replace the old program"; goto rollback; }
        if (!MoveFileExW(j.temp.c_str(), j.target.c_str(), MOVEFILE_REPLACE_EXISTING)) {
            if (hadOld) MoveFileExW(old.c_str(), j.target.c_str(), MOVEFILE_REPLACE_EXISTING);
            error = "could not put the new program in place";
            goto rollback;
        }
        swapped.push_back(&j);
    }
    return true;
rollback:
    for (const Job* j : swapped) {
        MoveFileExW(j->target.c_str(), j->temp.c_str(), MOVEFILE_REPLACE_EXISTING);
        MoveFileExW((j->target + L".old").c_str(), j->target.c_str(), MOVEFILE_REPLACE_EXISTING);
    }
    return false;
}

#else

void Updater::check(std::string, std::string) { fail("updates are only built for Windows"); }
void Updater::download() { fail("updates are only built for Windows"); }
bool Updater::install(std::string& error) { error = "updates are only built for Windows"; return false; }

#endif

}  // namespace cb2update

#include "discimport.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <vector>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <commdlg.h>
#pragma comment(lib, "comdlg32.lib")
#endif

namespace fs = std::filesystem;

namespace cb2import {
namespace {

std::string upper(std::string s) {
    for (char& c : s) c = char(std::toupper(static_cast<unsigned char>(c)));
    return s;
}

std::string extensionOf(const std::string& path) { return upper(fs::path(path).extension().string()); }

// A disc image file with a fixed sector layout.
class Image {
public:
    bool open(const std::string& path) {
        in.open(fs::u8path(path), std::ios::binary);
        if (!in) return false;
        in.seekg(0, std::ios::end);
        size = uint64_t(in.tellg());
        // the volume descriptor "01 CD001" lives in sector 16; find the layout where it shows up
        struct Layout { uint32_t stride, offset; };
        static const Layout layouts[] = {{2048, 0}, {2352, 16}, {2352, 24}, {2336, 8}};
        for (const Layout& l : layouts) {
            uint8_t head[8] = {};
            if (!readAt(uint64_t(l.stride) * 16 + l.offset, head, 6)) continue;
            if (head[0] == 1 && std::memcmp(head + 1, "CD001", 5) == 0) { stride = l.stride; offset = l.offset; return true; }
        }
        return false;
    }
    bool sector(uint32_t lba, uint8_t* out2048) { return readAt(uint64_t(stride) * lba + offset, out2048, 2048); }

private:
    bool readAt(uint64_t pos, uint8_t* out, size_t n) {
        if (pos + n > size) return false;
        in.clear();
        in.seekg(std::streamoff(pos));
        in.read(reinterpret_cast<char*>(out), std::streamsize(n));
        return bool(in);
    }
    std::ifstream in;
    uint64_t size = 0;
    uint32_t stride = 2048, offset = 0;
};

uint32_t le32(const uint8_t* p) { return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24; }

struct Entry {
    std::string name;   // without ";1"
    bool dir = false;
    uint32_t lba = 0, size = 0;
};

bool readDirectory(Image& img, uint32_t lba, uint32_t size, std::vector<Entry>& out) {
    if (size == 0 || size > 4u << 20) return false;
    std::vector<uint8_t> data(size_t((size + 2047) / 2048) * 2048);
    for (size_t s = 0; s < data.size() / 2048; ++s)
        if (!img.sector(lba + uint32_t(s), data.data() + s * 2048)) return false;
    size_t pos = 0;
    while (pos < size) {
        const uint8_t len = data[pos];
        if (len == 0) { pos = (pos / 2048 + 1) * 2048; continue; }   // records never cross a sector; zero padding
        if (pos + len > data.size() || len < 34) return false;
        const uint8_t nameLen = data[pos + 32];
        if (33u + nameLen > len) return false;
        Entry e;
        e.lba = le32(&data[pos + 2]);
        e.size = le32(&data[pos + 10]);
        e.dir = (data[pos + 25] & 2) != 0;
        if (!(nameLen == 1 && data[pos + 33] <= 1)) {   // skip "." and ".."
            e.name.assign(reinterpret_cast<const char*>(&data[pos + 33]), nameLen);
            const size_t semi = e.name.find(';');
            if (semi != std::string::npos) e.name.resize(semi);
            if (!e.name.empty() && e.name.back() == '.') e.name.pop_back();
            out.push_back(std::move(e));
        }
        pos += len;
    }
    return true;
}

bool safeName(const std::string& n) {
    if (n.empty() || n == "." || n == "..") return false;
    for (char c : n) if (c == '/' || c == '\\' || c == ':' || c == '*' || c == '?' || c == '"' || c == '<' || c == '>' || c == '|' || static_cast<unsigned char>(c) < 32) return false;
    return true;
}

struct Item {
    std::string rel;   // path below the output folder
    uint32_t lba = 0, size = 0;
};

// The data track of a .cue sheet: the first FILE entry.
bool cueDataFile(const std::string& cue, std::string& file, std::string& error) {
    std::ifstream in(fs::u8path(cue));
    if (!in) { error = "the .cue file cannot be opened"; return false; }
    std::string line;
    while (std::getline(in, line)) {
        const size_t f = upper(line).find("FILE");
        if (f == std::string::npos || line.find_first_not_of(" \t") != f) continue;
        std::string rest = line.substr(f + 4);
        const size_t q1 = rest.find('"');
        std::string name;
        if (q1 != std::string::npos) {
            const size_t q2 = rest.find('"', q1 + 1);
            if (q2 == std::string::npos) continue;
            name = rest.substr(q1 + 1, q2 - q1 - 1);
        } else {
            std::istringstream ss(rest);
            ss >> name;
        }
        if (name.empty()) continue;
        fs::path p = fs::u8path(name);
        if (p.is_relative()) p = fs::u8path(cue).parent_path() / p;
        file = p.u8string();
        return true;
    }
    error = "the .cue file does not name a disc file";
    return false;
}

}  // namespace

bool looksLikeImage(const std::string& path) {
    const std::string e = extensionOf(path);
    return e == ".CUE" || e == ".BIN" || e == ".ISO" || e == ".IMG";
}

Result importDisc(const std::string& imagePath, const std::string& outDir, const Progress& progress) {
    Result r;
    std::string path = imagePath;
    if (extensionOf(path) == ".CUE") {
        std::string bin;
        if (!cueDataFile(path, bin, r.error)) return r;
        path = bin;
    }
    std::error_code ec;
    if (!fs::exists(fs::u8path(path), ec)) { r.error = "the disc file was not found: " + path; return r; }
    if (extensionOf(path) == ".CHD" || extensionOf(path) == ".ECM" || extensionOf(path) == ".PBP") { r.error = "compressed images are not supported: use a .cue/.bin or .iso image"; return r; }

    Image img;
    if (!img.open(path)) { r.error = "this does not look like a PlayStation disc image (.cue/.bin or .iso)"; return r; }
    uint8_t pvd[2048];
    if (!img.sector(16, pvd)) { r.error = "the disc image is damaged"; return r; }
    std::vector<Entry> root;
    if (!readDirectory(img, le32(pvd + 156 + 2), le32(pvd + 156 + 10), root)) { r.error = "the disc image is damaged (cannot read its file list)"; return r; }

    // what the game needs: the executable, its config and the level files in S0..S3
    std::vector<Item> items;
    bool haveExe = false;
    for (const Entry& e : root) {
        const std::string n = upper(e.name);
        if (!e.dir && (n == "SCUS_941.54" || n == "SYSTEM.CNF") && safeName(n)) {
            items.push_back({n, e.lba, e.size});
            if (n == "SCUS_941.54") haveExe = true;
        } else if (e.dir && (n == "S0" || n == "S1" || n == "S2" || n == "S3")) {
            std::vector<Entry> kids;
            if (!readDirectory(img, e.lba, e.size, kids)) { r.error = "the disc image is damaged (folder " + n + ")"; return r; }
            for (const Entry& k : kids)
                if (!k.dir && safeName(k.name)) items.push_back({n + "\\" + upper(k.name), k.lba, k.size});
        }
    }
    if (!haveExe) { r.error = "this is not Crash Bandicoot 2 (NTSC-U, SCUS-94154): SCUS_941.54 is not on the disc"; return r; }
    int levelFiles = 0;
    uint64_t total = 0;
    for (const Item& it : items) { total += it.size; if (it.rel.size() > 3 && it.rel[2] == '\\') ++levelFiles; }
    if (levelFiles < 8) { r.error = "this disc does not contain the level files (S0..S3); is it the right game?"; return r; }

    const fs::path out = fs::u8path(outDir);
    uint64_t done = 0;
    std::vector<uint8_t> buf(2048);
    for (const Item& it : items) {
        const fs::path target = out / fs::u8path(it.rel);
        fs::create_directories(target.parent_path(), ec);
        fs::path tmp = target;
        tmp += ".part";
        {
            std::ofstream file(tmp, std::ios::binary | std::ios::trunc);
            if (!file) { r.error = "cannot write to " + target.parent_path().u8string() + " (is the folder writable?)"; return r; }
            uint32_t left = it.size;
            for (uint32_t s = 0; left > 0; ++s) {
                if (!img.sector(it.lba + s, buf.data())) { file.close(); fs::remove(tmp, ec); r.error = "the disc image ends early (" + it.rel + "): the file is incomplete"; return r; }
                const uint32_t n = std::min<uint32_t>(left, 2048);
                file.write(reinterpret_cast<const char*>(buf.data()), n);
                left -= n;
                done += n;
                if (progress && (s & 255) == 0) progress(done, total);
            }
            file.flush();
            if (!file) { file.close(); fs::remove(tmp, ec); r.error = "no room to write " + it.rel; return r; }
        }
        fs::remove(target, ec);
        fs::rename(tmp, target, ec);
        if (ec) { fs::remove(tmp, ec); r.error = "cannot create " + it.rel; return r; }
        ++r.files;
    }
    if (progress) progress(total, total);
    r.bytes = total;
    r.ok = true;
    return r;
}

bool ask(const std::string& text) {
#ifdef _WIN32
    return MessageBoxA(nullptr, text.c_str(), "Crash Bandicoot 2", MB_OKCANCEL | MB_ICONINFORMATION) == IDOK;
#else
    (void)text;
    return false;
#endif
}

void inform(const std::string& text) {
#ifdef _WIN32
    MessageBoxA(nullptr, text.c_str(), "Crash Bandicoot 2", MB_OK | MB_ICONINFORMATION);
#else
    (void)text;
#endif
}

std::string pickImage(const std::string& startDir) {
#ifdef _WIN32
    wchar_t file[MAX_PATH * 2] = {};
    const std::wstring dir = fs::u8path(startDir).wstring();
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof ofn;
    ofn.lpstrFilter = L"Disc images (*.cue;*.bin;*.iso;*.img)\0*.cue;*.bin;*.iso;*.img\0All files\0*.*\0";
    ofn.lpstrFile = file;
    ofn.nMaxFile = DWORD(std::size(file));
    ofn.lpstrInitialDir = dir.empty() ? nullptr : dir.c_str();
    ofn.lpstrTitle = L"Choose your Crash Bandicoot 2 disc image";
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR | OFN_EXPLORER;
    if (!GetOpenFileNameW(&ofn)) return {};
    return fs::path(file).u8string();
#else
    (void)startDir;
    return {};
#endif
}

}  // namespace cb2import

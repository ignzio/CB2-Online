#include "bundle.hpp"

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <fstream>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

namespace psx {
namespace {

constexpr char kMagic[8] = {'C', 'B', '2', 'B', 'U', 'N', 'D', 'L'};
constexpr uint64_t kFooter = 24;

}  // namespace

std::string selfPath() {
#ifdef _WIN32
    wchar_t buf[32768];
    const DWORD n = GetModuleFileNameW(nullptr, buf, DWORD(sizeof buf / sizeof buf[0]));
    if (n == 0 || n >= sizeof buf / sizeof buf[0]) return {};
    return std::filesystem::path(std::wstring(buf, n)).string();
#else
    return {};
#endif
}

void showError(const std::string& text) {
#ifdef _WIN32
    MessageBoxA(nullptr, text.c_str(), "Crash Bandicoot 2", MB_OK | MB_ICONERROR);
#else
    (void)text;
#endif
}

bool Bundle::open(const std::string& exePath) {
    files.clear();
    file = exePath;
    std::ifstream f(exePath, std::ios::binary);
    if (!f) return false;
    f.seekg(0, std::ios::end);
    const uint64_t size = uint64_t(f.tellg());
    if (size < kFooter) return false;
    uint8_t foot[kFooter];
    f.seekg(std::streamoff(size - kFooter));
    f.read(reinterpret_cast<char*>(foot), kFooter);
    if (std::memcmp(foot + 16, kMagic, 8) != 0) return false;
    uint64_t indexOffset, indexSize;
    std::memcpy(&indexOffset, foot, 8);
    std::memcpy(&indexSize, foot + 8, 8);
    if (indexOffset + indexSize + kFooter != size || indexSize < 4) return false;
    std::vector<uint8_t> idx;
    idx.resize(size_t(indexSize));
    f.seekg(std::streamoff(indexOffset));
    f.read(reinterpret_cast<char*>(idx.data()), std::streamsize(idx.size()));
    if (!f) return false;
    uint32_t count;
    std::memcpy(&count, idx.data(), 4);
    size_t o = 4;
    for (uint32_t i = 0; i < count; ++i) {
        if (o + 2 > idx.size()) return false;
        uint16_t len;
        std::memcpy(&len, &idx[o], 2);
        o += 2;
        if (o + len + 16 > idx.size()) return false;
        BundledFile b;
        b.path.assign(reinterpret_cast<const char*>(&idx[o]), len);
        o += len;
        std::memcpy(&b.offset, &idx[o], 8);
        std::memcpy(&b.size, &idx[o + 8], 8);
        o += 16;
        if (b.offset + b.size > indexOffset) return false;
        files.push_back(std::move(b));
    }
    return !files.empty();
}

bool Bundle::openSelf() {
    const std::string p = selfPath();
    return !p.empty() && open(p);
}

const BundledFile* Bundle::find(const std::string& path) const {
    for (const BundledFile& b : files) if (b.path == path) return &b;
    return nullptr;
}

bool Bundle::read(const std::string& path, std::vector<uint8_t>& out) const {
    const BundledFile* b = find(path);
    if (!b) return false;
    std::ifstream f(file, std::ios::binary);
    if (!f) return false;
    out.resize(size_t(b->size));
    f.seekg(std::streamoff(b->offset));
    f.read(reinterpret_cast<char*>(out.data()), std::streamsize(out.size()));
    return bool(f);
}

uint64_t Bundle::dataStart() const {
    uint64_t lo = ~0ull;
    for (const BundledFile& b : files) lo = std::min(lo, b.offset);
    return files.empty() ? 0 : lo;
}

std::string writeBundle(const std::string& stub, const std::string& out, const std::vector<std::pair<std::string, std::string>>& files) {
    {
        Bundle already;
        if (already.open(stub)) return stub + " already carries a bundle; pack from the plain executable";
    }
    std::ifstream in(stub, std::ios::binary);
    if (!in) return "cannot read " + stub;
    std::ofstream o(out, std::ios::binary | std::ios::trunc);
    if (!o) return "cannot write " + out;
    o << in.rdbuf();
    in.close();
    uint64_t pos = uint64_t(o.tellp());
    // 2 KiB alignment keeps every embedded file sector-aligned in the packed executable's address space
    auto pad = [&] {
        static const char zero[2048] = {};
        const uint64_t want = (pos + 2047) & ~uint64_t(2047);
        o.write(zero, std::streamsize(want - pos));
        pos = want;
    };
    std::vector<BundledFile> index;
    std::vector<char> buf(1 << 20);
    for (const auto& [disc, host] : files) {
        std::ifstream f(host, std::ios::binary);
        if (!f) return "cannot read " + host;
        pad();
        BundledFile b;
        b.path = disc;
        b.offset = pos;
        while (f) {
            f.read(buf.data(), std::streamsize(buf.size()));
            const std::streamsize n = f.gcount();
            if (n <= 0) break;
            o.write(buf.data(), n);
            pos += uint64_t(n);
        }
        b.size = pos - b.offset;
        index.push_back(std::move(b));
    }
    const uint64_t indexOffset = pos;
    std::vector<uint8_t> idx;
    auto put = [&](const void* p, size_t n) { idx.insert(idx.end(), static_cast<const uint8_t*>(p), static_cast<const uint8_t*>(p) + n); };
    const uint32_t count = uint32_t(index.size());
    put(&count, 4);
    for (const BundledFile& b : index) {
        const uint16_t len = uint16_t(b.path.size());
        put(&len, 2);
        put(b.path.data(), b.path.size());
        put(&b.offset, 8);
        put(&b.size, 8);
    }
    o.write(reinterpret_cast<const char*>(idx.data()), std::streamsize(idx.size()));
    const uint64_t indexSize = idx.size();
    o.write(reinterpret_cast<const char*>(&indexOffset), 8);
    o.write(reinterpret_cast<const char*>(&indexSize), 8);
    o.write(kMagic, 8);
    o.flush();
    return o ? std::string() : "write failed (disk full?)";
}

}  // namespace psx

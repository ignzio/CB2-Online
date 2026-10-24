#include "memcard.hpp"

#include <algorithm>
#include <cstring>
#include <fstream>
#include <iterator>

namespace psx {

uint8_t MemCard::checksum(const uint8_t* f) {
    uint8_t x = 0;
    for (int i = 0; i < 127; ++i) x ^= f[i];
    return x;
}

void MemCard::seal(uint32_t n) { frame(n)[127] = checksum(frame(n)); }

void MemCard::format() {
    std::fill(data.begin(), data.end(), uint8_t(0));
    uint8_t* h = frame(0);
    h[0] = 'M'; h[1] = 'C';
    seal(0);
    for (uint32_t n = 1; n <= 15; ++n) {
        uint8_t* e = frame(n);
        e[0] = 0xA0;
        e[8] = e[9] = 0xFF;
        seal(n);
    }
    for (uint32_t n = 16; n <= 35; ++n) {
        uint8_t* e = frame(n);
        e[0] = e[1] = e[2] = e[3] = 0xFF;
        e[8] = e[9] = 0xFF;
        seal(n);
    }
    for (uint32_t n = 36; n <= 62; ++n) std::memset(frame(n), 0xFF, kFrame);
    handles[0] = Handle{};
    for (auto& hd : handles) hd = Handle{};
}

bool MemCard::formatted() const { return data[0] == 'M' && data[1] == 'C'; }

bool MemCard::load() {
    if (path.empty()) return false;
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    std::vector<uint8_t> b((std::istreambuf_iterator<char>(f)), {});
    if (b.size() == kSize) data = std::move(b);
    else if (b.size() == kSize + 3904) data.assign(b.begin() + 3904, b.end());  // DexDrive-style header
    else return false;
    return true;
}

bool MemCard::save() {
    if (path.empty()) return false;
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (!f) return false;
    f.write(reinterpret_cast<const char*>(data.data()), std::streamsize(data.size()));
    return bool(f);
}

bool MemCard::matches(const std::string& pat, const char* name) {
    size_t i = 0;
    for (; i < pat.size(); ++i) {
        if (pat[i] == '*') return true;
        if (pat[i] != '?' && pat[i] != name[i]) return false;
        if (!name[i]) return false;
    }
    return name[i] == 0;
}

void MemCard::fillEntry(int block, DirEntry& out) const {
    const uint8_t* e = &data[size_t(block) * kFrame];
    std::memset(out.name, 0, sizeof out.name);
    std::memcpy(out.name, e + 10, 20);
    std::memcpy(&out.size, e + 4, 4);
    out.block = block;
}

int MemCard::firstFile(const std::string& pattern, DirEntry& out) {
    scanPattern = pattern;
    scanBlock = 0;
    return nextFile(out);
}

int MemCard::nextFile(DirEntry& out) {
    for (int b = scanBlock + 1; b <= 15; ++b) {
        const uint8_t* e = &data[size_t(b) * kFrame];
        if (e[0] != 0x51) continue;
        char name[21] = {};
        std::memcpy(name, e + 10, 20);
        if (!matches(scanPattern, name)) continue;
        scanBlock = b;
        fillEntry(b, out);
        return b;
    }
    scanBlock = 15;
    return 0;
}

int MemCard::findByName(const std::string& name) const {
    for (int b = 1; b <= 15; ++b) {
        const uint8_t* e = &data[size_t(b) * kFrame];
        if (e[0] != 0x51) continue;
        char n[21] = {};
        std::memcpy(n, e + 10, 20);
        if (name == n) return b;
    }
    return 0;
}

std::vector<int> MemCard::chain(int first) const {
    std::vector<int> c;
    int b = first;
    while (b >= 1 && b <= 15 && c.size() < 15) {
        c.push_back(b);
        uint16_t next;
        std::memcpy(&next, &data[size_t(b) * kFrame + 8], 2);
        if (next == 0xFFFF) break;
        b = int(next) + 1;
    }
    return c;
}

int MemCard::open(const std::string& name, uint32_t mode) {
    if (!formatted()) return -1;
    int first = findByName(name);
    const uint32_t blocks = mode >> 16;
    if (!first) {
        if (!(mode & 0x200) || blocks == 0 || name.size() > 20) return -1;
        std::vector<int> freeBlocks;
        for (int b = 1; b <= 15 && freeBlocks.size() < blocks; ++b) {
            const uint8_t s = data[size_t(b) * kFrame];
            if (s == 0xA0 || s == 0xA1 || s == 0xA2 || s == 0xA3) freeBlocks.push_back(b);
        }
        if (freeBlocks.size() < blocks) return -1;
        for (size_t i = 0; i < freeBlocks.size(); ++i) {
            uint8_t* e = &data[size_t(freeBlocks[i]) * kFrame];
            std::memset(e, 0, kFrame);
            e[0] = i == 0 ? 0x51 : (i + 1 == freeBlocks.size() ? 0x53 : 0x52);
            const uint32_t size = i == 0 ? blocks * kBlock : 0;
            std::memcpy(e + 4, &size, 4);
            const uint16_t next = i + 1 == freeBlocks.size() ? 0xFFFF : uint16_t(freeBlocks[i + 1] - 1);
            std::memcpy(e + 8, &next, 2);
            if (i == 0) std::memcpy(e + 10, name.data(), name.size());
            e[127] = checksum(e);
            std::memset(&data[size_t(freeBlocks[i]) * kBlock], 0, kBlock);
        }
        first = freeBlocks[0];
    }
    for (int fd = 0; fd < 16; ++fd) {
        if (handles[fd].used) continue;
        uint32_t size;
        std::memcpy(&size, &data[size_t(first) * kFrame + 4], 4);
        handles[fd] = Handle{true, first, 0, size, (mode & 2) != 0};
        return fd;
    }
    return -1;
}

int MemCard::read(int fd, uint8_t* dst, uint32_t length) {
    if (fd < 0 || fd >= 16 || !handles[fd].used) return -1;
    Handle& h = handles[fd];
    const auto blocks = chain(h.firstBlock);
    uint32_t done = 0;
    while (done < length && h.pos < h.size) {
        const size_t bi = h.pos / kBlock;
        if (bi >= blocks.size()) break;
        const uint32_t off = h.pos % kBlock, n = std::min({length - done, kBlock - off, h.size - h.pos});
        std::memcpy(dst + done, &data[size_t(blocks[bi]) * kBlock + off], n);
        done += n; h.pos += n;
    }
    return int(done);
}

int MemCard::write(int fd, const uint8_t* src, uint32_t length) {
    if (fd < 0 || fd >= 16 || !handles[fd].used || !handles[fd].write) return -1;
    Handle& h = handles[fd];
    const auto blocks = chain(h.firstBlock);
    uint32_t done = 0;
    while (done < length && h.pos < h.size) {
        const size_t bi = h.pos / kBlock;
        if (bi >= blocks.size()) break;
        const uint32_t off = h.pos % kBlock, n = std::min({length - done, kBlock - off, h.size - h.pos});
        std::memcpy(&data[size_t(blocks[bi]) * kBlock + off], src + done, n);
        done += n; h.pos += n;
    }
    return int(done);
}

int MemCard::close(int fd) {
    if (fd < 0 || fd >= 16 || !handles[fd].used) return -1;
    handles[fd] = Handle{};
    save();
    return fd;
}

bool MemCard::erase(const std::string& name) {
    const int first = findByName(name);
    if (!first) return false;
    for (int b : chain(first)) {
        uint8_t* e = &data[size_t(b) * kFrame];
        e[0] = uint8_t(0xA0 | (e[0] & 3));  // 0x51/52/53 -> 0xA1/A2/A3 (deleted)
        e[127] = checksum(e);
    }
    save();
    return true;
}

}  // namespace psx

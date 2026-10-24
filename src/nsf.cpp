#include "nsf.hpp"

#include <cstring>
#include <fstream>
#include <stdexcept>

namespace cb2 {
namespace {

uint32_t rd32(const uint8_t* p) {
    uint32_t v;
    std::memcpy(&v, p, 4);
    return v;
}

uint16_t rd16(const uint8_t* p) {
    uint16_t v;
    std::memcpy(&v, p, 2);
    return v;
}

[[noreturn]] void fail(const std::string& what) { throw std::runtime_error("NSF: " + what); }

Bytes readFile(const std::string& path) {
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f) fail("cannot open " + path);
    const auto size = static_cast<size_t>(f.tellg());
    Bytes data(size);
    f.seekg(0);
    f.read(reinterpret_cast<char*>(data.data()), static_cast<std::streamsize>(size));
    if (!f) fail("read error on " + path);
    return data;
}

// Chunk compression is LZSS mixed with a literal-run encoding.
// `src` points at the 0x1235 header; returns the decoded 64 KiB page and the bytes consumed.
Bytes decompressChunk(const uint8_t* src, size_t avail, size_t& consumed) {
    if (avail < 0xC) fail("truncated compressed chunk header");
    const uint32_t decodedSize = rd32(src + 4);
    const uint32_t skipSize = rd32(src + 8);
    if (decodedSize > kChunkSize) fail("compressed chunk decoded size too large");

    Bytes out;
    out.reserve(kChunkSize);
    size_t pos = 0xC;
    while (out.size() < decodedSize) {
        if (pos >= avail) fail("compressed chunk ran past end of file");
        const uint8_t code = src[pos++];
        if (code < 0x80) {
            if (pos + code > avail) fail("literal run past end of file");
            out.insert(out.end(), src + pos, src + pos + code);
            pos += code;
        } else {
            if (pos >= avail) fail("truncated back-reference");
            const unsigned v = ((code & 0x7Fu) << 8) | src[pos++];
            const size_t disp = v >> 3;
            size_t len = (v & 7u) + 3;
            if (len == 0x0A) len = 0x40;
            if (disp == 0 || disp > out.size()) fail("invalid back-reference");
            for (size_t i = 0; i < len; ++i) out.push_back(out[out.size() - disp]);
        }
    }
    pos += skipSize;
    if (out.size() > kChunkSize) fail("decoded chunk overflow");
    const size_t tail = kChunkSize - out.size();
    if (pos + tail > avail) fail("compressed chunk tail past end of file");
    out.insert(out.end(), src + pos, src + pos + tail);
    pos += tail;
    consumed = pos;
    return out;
}

void parseEntryChunk(const Bytes& chunk, std::vector<Entry>& out) {
    if (chunk.size() != kChunkSize) fail("page is not 64 KiB");
    const uint32_t count = rd32(chunk.data() + 8);
    if (count > 0x3FC0) fail("implausible entry count");
    const size_t listEnd = 0x10 + (static_cast<size_t>(count) + 1) * 4;
    if (listEnd > chunk.size()) fail("entry list past chunk end");

    std::vector<uint32_t> offs(count + 1);
    std::memcpy(offs.data(), chunk.data() + 0x10, offs.size() * 4);

    for (uint32_t e = 0; e < count; ++e) {
        const size_t start = offs[e];
        const size_t end = offs[e + 1];
        if (start > end || end > chunk.size() || start + 16 > end) fail("bad entry bounds");
        if (rd32(chunk.data() + start) != 0x0100FFFF) fail("bad entry magic");

        Entry entry;
        entry.eid = rd32(chunk.data() + start + 4);
        entry.type = rd32(chunk.data() + start + 8);
        const uint32_t itemCount = rd32(chunk.data() + start + 12);
        if (start + 16 + (static_cast<size_t>(itemCount) + 1) * 4 > end) fail("bad item list");

        std::vector<uint32_t> io(itemCount + 1);
        std::memcpy(io.data(), chunk.data() + start + 16, io.size() * 4);
        for (uint32_t i = 0; i < itemCount; ++i) {
            if (io[i] > io[i + 1] || start + io[i + 1] > end) fail("bad item bounds");
            entry.items.emplace_back(chunk.begin() + start + io[i], chunk.begin() + start + io[i + 1]);
        }
        out.push_back(std::move(entry));
    }
}

void wr32(uint8_t* p, uint32_t v) { std::memcpy(p, &v, 4); }

}  // namespace

std::string eidName(uint32_t eid) {
    static const char kChars[] = "0123456789abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ_!";
    std::string s;
    for (int i = 0; i < 4; ++i) s += kChars[(eid >> (25 - 6 * i)) & 63];
    s += '.';
    s += kChars[(eid >> 1) & 63];
    return s;
}

std::vector<Entry> readEntryPage(const Bytes& page) {
    std::vector<Entry> entries;
    parseEntryChunk(page, entries);
    return entries;
}

std::vector<Bytes> loadPages(const std::string& path) {
    const Bytes file = readFile(path);
    std::vector<Bytes> pages;
    size_t off = 0;
    while (off < file.size()) {
        if (off + 0x10 > file.size()) fail("trailing bytes after last chunk");
        const uint16_t magic = rd16(file.data() + off);
        Bytes chunk;
        size_t used = 0;
        if (magic == 0x1234) {
            if (off + kChunkSize > file.size()) fail("truncated raw chunk");
            chunk.assign(file.begin() + off, file.begin() + off + kChunkSize);
            used = kChunkSize;
        } else if (magic == 0x1235) {
            chunk = decompressChunk(file.data() + off, file.size() - off, used);
        } else {
            fail("bad chunk magic at offset " + std::to_string(off));
        }
        pages.push_back(std::move(chunk));
        off += (used + kSectorSize - 1) / kSectorSize * kSectorSize;
    }
    return pages;
}

Nsf loadNsf(const std::string& path) {
    Nsf nsf;
    for (Bytes& page : loadPages(path)) {
        ++nsf.chunkCount;
        const uint16_t family = rd16(page.data() + 2);
        if (family == 1) {
            nsf.textures.push_back({rd32(page.data() + 4), std::move(page)});
        } else if (family == 0) {
            parseEntryChunk(page, nsf.entries);
        }  // families 2-5 hold sound data, which the viewer does not need
    }
    return nsf;
}

uint32_t pageChecksum(const uint8_t* page) {
    uint32_t c = 0x12345678u;
    for (size_t i = 0; i < kChunkSize; ++i) {
        if (i < 12 || i >= 16) c += page[i];
        c = (c << 3) | (c >> 29);
    }
    return c;
}

void finalizePage(Bytes& page) {
    if (page.size() != kChunkSize) fail("page is not 64 KiB");
    wr32(page.data() + 12, 0);
    wr32(page.data() + 12, pageChecksum(page.data()));
}

Bytes packEntryPage(uint32_t chunkId, const std::vector<Entry>& entries) {
    Bytes page(kChunkSize, 0);
    const uint16_t magic = 0x1234, family = 0;
    std::memcpy(page.data(), &magic, 2);
    std::memcpy(page.data() + 2, &family, 2);
    wr32(page.data() + 4, chunkId);
    wr32(page.data() + 8, uint32_t(entries.size()));

    size_t off = 0x10 + (entries.size() + 1) * 4;
    for (size_t e = 0; e < entries.size(); ++e) {
        const Entry& entry = entries[e];
        const size_t header = 16 + (entry.items.size() + 1) * 4;
        off = ((off + header + 3) & ~size_t(3)) - header;  // the item data, not the entry header, is aligned

        size_t total = header;
        for (const Bytes& item : entry.items) total += item.size();
        if (off + total > kChunkSize) throw std::length_error("entry " + eidName(entry.eid) + " does not fit in the chunk");

        wr32(page.data() + 0x10 + e * 4, uint32_t(off));
        uint8_t* p = page.data() + off;
        wr32(p, 0x0100FFFF);
        wr32(p + 4, entry.eid);
        wr32(p + 8, entry.type);
        wr32(p + 12, uint32_t(entry.items.size()));
        size_t itemOff = header;
        for (size_t i = 0; i < entry.items.size(); ++i) {
            wr32(p + 16 + i * 4, uint32_t(itemOff));
            if (!entry.items[i].empty()) std::memcpy(p + itemOff, entry.items[i].data(), entry.items[i].size());
            itemOff += entry.items[i].size();
        }
        wr32(p + 16 + entry.items.size() * 4, uint32_t(itemOff));
        off += total;
    }
    wr32(page.data() + 0x10 + entries.size() * 4, uint32_t(off));
    finalizePage(page);
    return page;
}

size_t repackPage(std::vector<Bytes>& pages, size_t pageIndex, std::vector<Entry> entries) {
    size_t added = 0;
    std::vector<Entry> spill;  // entries that did not fit, in original order
    for (;;) {
        try {
            pages[pageIndex] = packEntryPage(rd32(pages[pageIndex].data() + 4), entries);
            break;
        } catch (const std::length_error&) {
            if (entries.size() <= 1) throw;  // a single entry larger than a chunk cannot be stored
            spill.insert(spill.begin(), std::move(entries.back()));
            entries.pop_back();
        }
    }
    // Spilled entries go to new pages appended to the file; chunk ids follow the "index * 2 + 1" rule.
    while (!spill.empty()) {
        std::vector<Entry> batch;
        Bytes page;
        for (size_t take = spill.size(); take > 0; --take) {
            batch.assign(spill.begin(), spill.begin() + long(take));
            try {
                page = packEntryPage(uint32_t(pages.size() * 2 + 1), batch);
                break;
            } catch (const std::length_error&) {
                if (take == 1) throw;
            }
        }
        pages.push_back(std::move(page));
        spill.erase(spill.begin(), spill.begin() + long(batch.size()));
        ++added;
    }
    return added;
}

Bytes writeNsf(const std::vector<Bytes>& pages) {
    Bytes out;
    out.reserve(pages.size() * kChunkSize);
    for (const Bytes& p : pages) {
        if (p.size() != kChunkSize || rd16(p.data()) != 0x1234) fail("writeNsf expects raw 64 KiB pages");
        out.insert(out.end(), p.begin(), p.end());
    }
    return out;
}

Bytes loadFile(const std::string& path) { return readFile(path); }

void saveFile(const std::string& path, const Bytes& data) {
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (!f) fail("cannot create " + path);
    f.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
    if (!f) fail("write error on " + path);
}

}  // namespace cb2

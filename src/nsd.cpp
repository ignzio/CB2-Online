#include "nsd.hpp"

#include <algorithm>
#include <cstring>
#include <stdexcept>

namespace cb2 {
namespace {

constexpr size_t kTableBytes = 0x400;   // 256 bucket start indices
constexpr size_t kListOffset = 0x520;   // first lookup entry

uint32_t rd32(const uint8_t* p) { uint32_t v; std::memcpy(&v, p, 4); return v; }
void wr32(uint8_t* p, uint32_t v) { std::memcpy(p, &v, 4); }
uint32_t bucketOf(uint32_t eid) { return (eid >> 15) & 0xFF; }

}  // namespace

std::vector<LookupEntry> buildLookup(const std::vector<Bytes>& pages) {
    std::vector<LookupEntry> list;
    for (size_t i = 0; i < pages.size(); ++i) {
        const Bytes& page = pages[i];
        if (page.size() != kChunkSize) throw std::runtime_error("NSD: page is not 64 KiB");
        uint16_t family;
        std::memcpy(&family, page.data() + 2, 2);
        if (family == 1) {
            list.push_back({uint32_t(i * 2 + 1), rd32(page.data() + 4)});  // texture chunks are keyed by their EID
        } else {
            const uint32_t number = rd32(page.data() + 4);
            for (const Entry& e : readEntryPage(page)) list.push_back({number, e.eid});
        }
    }
    std::sort(list.begin(), list.end(), [](const LookupEntry& a, const LookupEntry& b) {
        if (bucketOf(a.eid) != bucketOf(b.eid)) return bucketOf(a.eid) < bucketOf(b.eid);
        return a.eid < b.eid;
    });
    for (size_t i = 1; i < list.size(); ++i)
        if (list[i].eid == list[i - 1].eid) throw std::runtime_error("NSD: duplicate entry id " + eidName(list[i].eid));
    return list;
}

Bytes rebuildNsd(const Bytes& oldNsd, const std::vector<Bytes>& pages) {
    if (oldNsd.size() < kListOffset) throw std::runtime_error("NSD: file too small");
    const uint32_t oldCount = rd32(oldNsd.data() + 0x404);
    const size_t tailStart = kListOffset + size_t(oldCount) * 8;
    if (tailStart > oldNsd.size()) throw std::runtime_error("NSD: entry list past end of file");

    const std::vector<LookupEntry> list = buildLookup(pages);
    const uint32_t n = uint32_t(list.size());

    Bytes out(oldNsd.begin(), oldNsd.begin() + kListOffset);
    wr32(out.data() + 0x400, uint32_t(pages.size()));
    wr32(out.data() + 0x404, n);

    // table[k] = index of the first entry whose bucket is >= k, never past the last entry.
    uint32_t next = 0;
    for (uint32_t k = 0; k < 256; ++k) {
        while (next < n && bucketOf(list[next].eid) < k) ++next;
        wr32(out.data() + k * 4, n ? std::min(next, n - 1) : 0);
    }

    out.resize(kListOffset + size_t(n) * 8);
    for (uint32_t i = 0; i < n; ++i) {
        wr32(out.data() + kListOffset + i * 8, list[i].chunk);
        wr32(out.data() + kListOffset + i * 8 + 4, list[i].eid);
    }
    out.insert(out.end(), oldNsd.begin() + tailStart, oldNsd.end());
    return out;
}

}  // namespace cb2

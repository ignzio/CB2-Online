#pragma once
// NSD index for a Crash 2 (v2) level. The NSD tells the game which chunk of the NSF holds each entry; a mod that
// adds, replaces or moves entries regenerates it with rebuildNsd(). Layout notes: docs/FORMAT.md.
#include <cstdint>
#include <vector>

#include "nsf.hpp"

namespace cb2 {

struct LookupEntry {
    uint32_t chunk;  // chunk number as stored in the page header (index * 2 + 1)
    uint32_t eid;
};

// All entries of all pages, in the order the game expects: bucket ((eid >> 15) & 0xFF), then eid ascending.
std::vector<LookupEntry> buildLookup(const std::vector<Bytes>& pages);

// Regenerates the lookup table, chunk count and entry list of `oldNsd`; everything else (compression info,
// level data and spawn points, loading bitmap) is carried over unchanged. Throws std::runtime_error when
// `oldNsd` is too small or has duplicate EIDs in `pages`.
Bytes rebuildNsd(const Bytes& oldNsd, const std::vector<Bytes>& pages);

}  // namespace cb2

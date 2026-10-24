#pragma once
// Crash Bandicoot 2 (PS1) NSF level archive reader.
// Format notes: docs/FORMAT.md. All integers are little-endian; this code assumes a little-endian host.
#include <cstdint>
#include <string>
#include <vector>

namespace cb2 {

using Bytes = std::vector<uint8_t>;

constexpr size_t kChunkSize = 0x10000;
constexpr size_t kSectorSize = 0x800;

// An "entry" (NSF child archive): one typed file made of several items.
struct Entry {
    uint32_t eid = 0;   // five 6-bit characters, see eidName()
    uint32_t type = 0;  // 3 = world scenery, 7 = zone/entity data, 11 = GOOL bytecode, ...
    std::vector<Bytes> items;
};

// A texture chunk is a raw 512x128-byte VRAM page: pixel indices plus CLUT rows.
struct TexturePage {
    uint32_t eid = 0;
    Bytes data;  // exactly kChunkSize bytes; row stride 512 bytes, 128 rows
};

struct Nsf {
    std::vector<Entry> entries;
    std::vector<TexturePage> textures;
    uint32_t chunkCount = 0;
};

constexpr uint32_t kEntryTypeScenery = 3;

// Throws std::runtime_error on malformed input.
Nsf loadNsf(const std::string& path);

// Decodes every chunk of an NSF file into raw 64 KiB pages (compressed chunks are expanded).
std::vector<Bytes> loadPages(const std::string& path);

// Entries stored in one family-0 page. Throws std::runtime_error on malformed input.
std::vector<Entry> readEntryPage(const Bytes& page);

// Decodes an entry/texture identifier such as 0x74DB216F into "xxxx.X" form.
std::string eidName(uint32_t eid);

// ---- Writing (modding) --------------------------------------------------------------------------------------

// Header checksum of a 64 KiB page (the four checksum bytes at +12 are excluded). Verified on all 2,548 chunks
// of the shipped Crash 2 levels.
uint32_t pageChecksum(const uint8_t* page);

// Sets the checksum field of a 64 KiB page in place.
void finalizePage(Bytes& page);

// Packs entries into one family-0 page with the game's layout (entries start so that their item data is
// 4-byte aligned; unused bytes are zero). `chunkId` is the header value at +4 (the game stores chunkNumber*2+1).
// Throws std::length_error when the entries do not fit in 64 KiB.
Bytes packEntryPage(uint32_t chunkId, const std::vector<Entry>& entries);

// Replaces the entries of page `pageIndex`. Entries that no longer fit in 64 KiB move to new pages appended to
// `pages` (the NSD must then be rebuilt with rebuildNsd). Returns the number of pages added; throws
// std::length_error when a single entry is larger than a chunk.
size_t repackPage(std::vector<Bytes>& pages, size_t pageIndex, std::vector<Entry> entries);

// Serialises pages as an uncompressed NSF (every page is a raw 0x1234 chunk; the game accepts these).
Bytes writeNsf(const std::vector<Bytes>& pages);

// Writes the file atomically enough for tooling: throws std::runtime_error on failure.
void saveFile(const std::string& path, const Bytes& data);

// Reads a whole file. Throws std::runtime_error on failure.
Bytes loadFile(const std::string& path);

}  // namespace cb2

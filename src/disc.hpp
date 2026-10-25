#pragma once
// A virtual CD-ROM: an ISO9660 volume synthesised from a host folder (loose game files), served as raw mode-2 sectors.
// Files are read from the host on demand, so edited or modded files show up without rebuilding anything.
#include <cstdint>
#include <fstream>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace psx {

// A file stored inside another file (the game data appended to the executable by cb2_pack).
struct BundledFile {
    std::string path;           // upper case, '\\' separated, as on the disc: S1\\S000001C.NSF
    uint64_t offset = 0, size = 0;
};

class VirtualDisc {
public:
    bool build(const std::string& rootFolder, const std::vector<std::string>& overlays = {}, const std::string& volumeId = "CRASH2");
    // The same disc, with the files read from `bundleFile` at the given offsets. Overlay folders still replace files.
    bool buildFromBundle(const std::string& bundleFile, const std::vector<BundledFile>& files, const std::vector<std::string>& overlays = {},
                         const std::string& volumeId = "CRASH2");
    bool readSector(uint32_t lba, uint8_t* out2352) const;  // false when past the end of the disc
    uint32_t sectorCount() const { return total; }
    uint32_t fileLba(const std::string& path) const;         // 0 when not found
    std::string fileAt(uint32_t lba) const;                  // host path of the file covering this sector, or empty

private:
    struct Node {
        std::string name, host;
        bool dir = false;
        int parent = 0;
        uint32_t lba = 0, size = 0;
        std::vector<int> kids;
        int pathIndex = 0;
        bool bundled = false;
        uint64_t offset = 0;   // position inside the bundle file
    };
    struct FileRef { std::string host; uint64_t size = 0, offset = 0; bool bundled = false; };
    bool finish(const std::map<std::string, FileRef>& files, const std::string& volumeId);
    std::string bundlePath;
    mutable std::ifstream bundleStream;
    // Reading host files sector by sector would open and close the file 2048 bytes at a time (very slow with a virus scanner or a slow disk:
    // every area load of the game stutters). Instead the files stay open and are read in 64 KB chunks, of which the most recent are kept.
    static constexpr uint32_t kChunkSectors = 32, kMaxChunks = 128;
    struct Chunk { int node = -1; uint32_t index = 0; uint64_t stamp = 0; std::vector<uint8_t> data; };
    mutable std::vector<Chunk> chunks;
    mutable std::map<int, std::ifstream> handles;
    mutable uint64_t stampCounter = 0;
    // The first time the game reads from a file, a helper thread reads the whole file once so that the rest of it is in the operating system's cache by the time the
    // game gets there (a cold disk or a virus scanner made the first read of each area stall for half a second).
    mutable std::set<int> warmed;
    void warm(int nodeIndex) const;
    mutable std::vector<std::pair<uint32_t, int>> extents;   // first sector of every file -> node index, sorted; built on first use
    const Chunk* chunkFor(int nodeIndex, uint32_t chunkIndex) const;
    int nodeForSector(uint32_t lba) const;
    std::vector<Node> nodes;
    std::map<uint32_t, std::vector<uint8_t>> meta;  // LBA -> 2048-byte synthetic sector
    uint32_t total = 0;
    std::map<std::string, uint32_t> byPath;

    void layout();
    void writeDirectory(int dirIndex);
    void writeVolumeDescriptors(const std::string& volumeId);
};

}  // namespace psx

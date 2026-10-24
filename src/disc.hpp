#pragma once
// A virtual CD-ROM: an ISO9660 volume synthesised from a host folder (loose game files), served as raw mode-2 sectors.
// Files are read from the host on demand, so edited or modded files show up without rebuilding anything.
#include <cstdint>
#include <fstream>
#include <map>
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
    std::vector<Node> nodes;
    std::map<uint32_t, std::vector<uint8_t>> meta;  // LBA -> 2048-byte synthetic sector
    uint32_t total = 0;
    std::map<std::string, uint32_t> byPath;

    void layout();
    void writeDirectory(int dirIndex);
    void writeVolumeDescriptors(const std::string& volumeId);
};

}  // namespace psx

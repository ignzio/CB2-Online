#pragma once
// Virtual PlayStation memory card: a 128 KiB image in the console's native layout (header frame, 15 directory
// frames, 15 data blocks of 8 KiB), so .mcr files from other emulators can be used directly.
#include <cstdint>
#include <string>
#include <vector>

namespace psx {

class MemCard {
public:
    static constexpr uint32_t kSize = 128 * 1024, kFrame = 128, kBlock = 8192;

    bool present = true;
    std::string path;                // image file; empty = in-memory only
    bool newCard = true;             // reported as "new card" until the game clears the flag

    bool load();
    bool save();
    void format();
    bool formatted() const;
    std::vector<uint8_t>& image() { return data; }

    // BIOS file API (names without the "bu00:" prefix)
    struct DirEntry { char name[21]; uint32_t size; int block; };
    int firstFile(const std::string& pattern, DirEntry& out);   // returns block index (1..15) or 0
    int nextFile(DirEntry& out);
    int open(const std::string& name, uint32_t mode);           // file handle or -1
    int read(int fd, uint8_t* dst, uint32_t length);
    int write(int fd, const uint8_t* src, uint32_t length);
    int close(int fd);
    bool erase(const std::string& name);

    uint8_t* frame(uint32_t n) { return &data[n * kFrame]; }

private:
    std::vector<uint8_t> data = std::vector<uint8_t>(kSize, 0);
    struct Handle { bool used = false; int firstBlock = 0; uint32_t pos = 0, size = 0; bool write = false; };
    Handle handles[16];
    std::string scanPattern;
    int scanBlock = 0;

    static uint8_t checksum(const uint8_t* frame128);
    void seal(uint32_t frameIndex);
    int findByName(const std::string& name) const;
    std::vector<int> chain(int firstBlock) const;
    static bool matches(const std::string& pattern, const char* name);
    void fillEntry(int block, DirEntry& out) const;
};

}  // namespace psx

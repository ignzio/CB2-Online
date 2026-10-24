#pragma once
// Game data appended to an executable. Layout, from the end of the file:
//   [file data ...][index][footer]
//   index  = u32 count, then per file: u16 pathLength, path bytes (upper case, '\' separated), u64 offset, u64 size
//   footer = u64 indexOffset, u64 indexSize, 8-byte magic "CB2BUNDL"
// The offsets are absolute positions in the executable. Appended data does not disturb the program: Windows ignores bytes after
// the last PE section.
#include <cstdint>
#include <string>
#include <vector>

#include "disc.hpp"

namespace psx {

struct Bundle {
    std::string file;                  // the executable that carries the data
    std::vector<BundledFile> files;

    bool open(const std::string& exePath);   // false when the file carries no bundle
    bool openSelf();                          // the running executable
    const BundledFile* find(const std::string& path) const;
    bool read(const std::string& path, std::vector<uint8_t>& out) const;
    uint64_t dataStart() const;               // where the bundle begins (= size of the original executable)
};

// Appends `files` (disc path -> host path) to a copy of `stub`, writing `out`. Returns an error text or an empty string.
std::string writeBundle(const std::string& stub, const std::string& out, const std::vector<std::pair<std::string, std::string>>& files);

std::string selfPath();   // full path of the running executable
void showError(const std::string& text);   // message box on Windows (the packed build has no console), nothing elsewhere

}  // namespace psx

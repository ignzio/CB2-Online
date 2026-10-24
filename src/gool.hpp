#pragma once
// GOOL (Game Object Oriented Lisp) script container, entry type 0x0B.
// Crash 2 runs every game object (Crash, crates, enemies, camera, HUD) as a GOOL process.
// Layout validated against the shipped files; see docs/FORMAT.md.
#include <cstdint>
#include <optional>
#include <vector>

#include "nsf.hpp"

namespace cb2 {

constexpr uint32_t kEntryTypeGool = 0x0B;

// 16 bytes per state. Meaning of the fields is still being established from the executable.
struct GoolState {
    int32_t a, b;
    int16_t c, d, e, f;
};

struct Gool {
    uint32_t eid = 0;
    int32_t id = 0;         // object type id used by spawn points
    int32_t objClass = 0;
    int32_t format = 0;
    int32_t heapBase = 0;
    int32_t eventCount = 0;
    int32_t entryCount = 0;
    // Verified on the shipped data: item 1 is the bytecode (94% of words are valid opcodes) and item 2 is the
    // constant pool (it begins with `entryCount` entry IDs). Some community loaders name them the other way round.
    std::vector<uint32_t> pool;      // item 2: 32-bit constants; the first `entryCount` are entry IDs
    std::vector<uint32_t> code;      // item 1: 32-bit instruction words
    std::vector<int16_t> stateMap;     // item 3
    std::vector<GoolState> states;     // item 4
    std::vector<uint8_t> frameGroups;  // item 5: animation / texture groups (opaque for now)
};

// Returns nullopt (never throws) when the entry does not match the Crash 2 layout.
std::optional<Gool> parseGool(const Entry& entry);

}  // namespace cb2

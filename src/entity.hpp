#pragma once
// Zone entry (type 7, "ZDAT") reader: the header plus the entities (spawn points, enemies, boxes, camera paths...) of one zone.
// Layout from CrashEdit's Entity/EntityEntry sources and the Crash hacking wiki. No emulation involved: this is pure data.
#include <cstdint>
#include <map>
#include <vector>

#include "nsf.hpp"

namespace cb2 {

struct EntityProp {
    uint8_t type = 0;          // 1 u8, 4 i32, 5 setting (u8 + i24), 6 position (3 x i16), 18 i16, 19 i32
    uint8_t elementSize = 0;
    std::vector<std::vector<int32_t>> rows;   // rows of elements; positions and settings expand to 3 and 2 values
};

struct Position { int x, y, z; };

struct Entity {
    int type = -1, subtype = -1, id = -1;      // GOOL object type and subtype, entity id (-1 = absent)
    std::vector<Position> positions;           // first = placement, the rest form a path
    std::map<int, EntityProp> props;           // every property by id (0x4B positions, 0xA9 type, 0xAA subtype, 0x9F id ...)
};

struct Zone {
    uint32_t eid = 0;
    int32_t x = 0, y = 0, z = 0, w = 0, h = 0, d = 0;   // zone box from the collision item (item 1), in world units
    uint32_t worldCount = 0;
    std::vector<uint32_t> worlds;              // world EIDs referenced by the header
    std::vector<uint32_t> neighbours;
    std::vector<Entity> entities;              // items 2+ that parsed as entities (camera paths included)
};

// Returns false when the entry is not a zone. Never throws.
bool parseZone(const Entry& entry, Zone& out);

// Spawn points from the NSD level data (24 bytes each): the zone to enter, a camera path index and the player's position.
struct Spawn {
    uint32_t zone = 0;
    int32_t flags = 0, camera = 0;
    int32_t x = 0, y = 0, z = 0;
};
struct LevelInfo {
    uint32_t levelId = 0;
    std::vector<Spawn> spawns;
    std::vector<uint32_t> objectScripts;   // object type -> EID of the GOOL entry that implements it (64 slots)
};
bool parseNsdLevel(const Bytes& nsd, LevelInfo& out);

}  // namespace cb2

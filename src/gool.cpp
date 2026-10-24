#include "gool.hpp"

#include <cstring>

namespace cb2 {
namespace {

int32_t rdS32(const uint8_t* p) { int32_t v; std::memcpy(&v, p, 4); return v; }
int16_t rdS16(const uint8_t* p) { int16_t v; std::memcpy(&v, p, 2); return v; }

}  // namespace

std::optional<Gool> parseGool(const Entry& e) {
    if (e.type != kEntryTypeGool) return std::nullopt;
    const size_t n = e.items.size();
    if (n != 3 && n != 5 && n != 6) return std::nullopt;
    if (e.items[0].size() != 24 || e.items[1].size() % 4 != 0 || e.items[2].size() % 4 != 0) return std::nullopt;
    if (n > 3 && (e.items[3].size() % 2 != 0 || e.items[4].size() % 16 != 0)) return std::nullopt;

    Gool g;
    g.eid = e.eid;
    const uint8_t* h = e.items[0].data();
    g.id = rdS32(h);
    g.objClass = rdS32(h + 4);
    g.format = rdS32(h + 8);
    g.heapBase = rdS32(h + 12);
    g.eventCount = rdS32(h + 16);
    g.entryCount = rdS32(h + 20);

    g.code.resize(e.items[1].size() / 4);
    if (!g.code.empty()) std::memcpy(g.code.data(), e.items[1].data(), g.code.size() * 4);
    g.pool.resize(e.items[2].size() / 4);
    if (!g.pool.empty()) std::memcpy(g.pool.data(), e.items[2].data(), g.pool.size() * 4);

    if (n > 3) {
        g.stateMap.resize(e.items[3].size() / 2);
        for (size_t i = 0; i < g.stateMap.size(); ++i) g.stateMap[i] = rdS16(&e.items[3][i * 2]);
        for (size_t i = 0; i + 16 <= e.items[4].size(); i += 16) {
            const uint8_t* s = &e.items[4][i];
            g.states.push_back({rdS32(s), rdS32(s + 4), rdS16(s + 8), rdS16(s + 10), rdS16(s + 12), rdS16(s + 14)});
        }
    }
    if (n > 5) g.frameGroups = e.items[5];
    return g;
}

}  // namespace cb2

#include "entity.hpp"

#include <cstring>

namespace cb2 {
namespace {

uint32_t u32(const Bytes& b, size_t o) { uint32_t v = 0; if (o + 4 <= b.size()) std::memcpy(&v, &b[o], 4); return v; }
int32_t i32(const Bytes& b, size_t o) { return int32_t(u32(b, o)); }
uint16_t u16(const Bytes& b, size_t o) { uint16_t v = 0; if (o + 2 <= b.size()) std::memcpy(&v, &b[o], 2); return v; }
int16_t i16(const Bytes& b, size_t o) { return int16_t(u16(b, o)); }

// One property body: rows of fixed-size elements, optionally sparse (per-row counts) and with per-row meta values.
bool readProp(uint8_t rawType, uint8_t elemSize, int rowCount, const Bytes& d, size_t begin, size_t end, EntityProp& p) {
    const bool sparse = rawType & 64, meta = rawType & 32;
    p.type = rawType & 31;
    p.elementSize = elemSize;
    int values = 0;
    switch (p.type) {
    case 1: values = 1; break;
    case 4: case 19: values = 1; break;
    case 18: values = 1; break;
    case 5: values = 2; break;
    case 6: values = 3; break;
    default: return false;
    }
    size_t o = begin;
    auto need = [&](size_t n) { return o + n <= end; };
    std::vector<int> counts(size_t(rowCount), 0);
    if (sparse) {
        if (!need(2 * size_t(rowCount))) return false;
        for (int& c : counts) { c = u16(d, o); o += 2; }
    } else {
        if (!need(2)) return false;
        const int c = u16(d, o); o += 2;
        for (int& x : counts) x = c;
    }
    if (meta) {
        if (!need(2 * size_t(rowCount))) return false;
        o += 2 * size_t(rowCount);
    }
    o = begin + ((o - begin + 3) & ~size_t(3));
    for (int r = 0; r < rowCount; ++r) {
        std::vector<int32_t> row;
        for (int i = 0; i < counts[size_t(r)]; ++i) {
            if (!need(elemSize)) return false;
            switch (p.type) {
            case 1: row.push_back(d[o]); break;
            case 4: case 19: row.push_back(i32(d, o)); break;
            case 18: row.push_back(i16(d, o)); break;
            case 5: row.push_back(d[o]); row.push_back(int32_t(u32(d, o) >> 8)); break;
            case 6: row.push_back(i16(d, o)); row.push_back(i16(d, o + 2)); row.push_back(i16(d, o + 4)); break;
            }
            o += elemSize;
        }
        p.rows.push_back(std::move(row));
    }
    (void)values;
    return true;
}

bool parseEntity(const Bytes& d, Entity& e) {
    if (d.size() < 16 || i32(d, 0) != int32_t(d.size())) return false;
    const uint32_t count = u32(d, 12);
    if (count > 0xFFFF || d.size() < 16 + size_t(count) * 8) return false;
    for (uint32_t i = 0; i < count; ++i) {
        const size_t t = 16 + size_t(i) * 8;
        const int id = u16(d, t);
        const size_t begin = size_t(u16(d, t + 2)) + 12;
        const size_t end = i + 1 == count ? d.size() : size_t(u16(d, t + 10)) + 12;
        if (begin > end || end > d.size()) return false;
        EntityProp p;
        if (readProp(d[t + 4], d[t + 5], u16(d, t + 6), d, begin, end, p)) e.props[id] = std::move(p);
    }
    auto first = [&](int id, int& out) {
        auto it = e.props.find(id);
        if (it == e.props.end() || it->second.rows.empty() || it->second.rows[0].empty()) return;
        out = it->second.rows[0][0];
        if (it->second.type == 5) out = it->second.rows[0][1];   // settings: the value is the 24-bit part
    };
    first(0xA9, e.type);
    first(0xAA, e.subtype);
    first(0x9F, e.id);
    auto pos = e.props.find(0x4B);
    if (pos != e.props.end() && pos->second.type == 6 && !pos->second.rows.empty()) {
        const auto& row = pos->second.rows[0];
        for (size_t i = 0; i + 2 < row.size(); i += 3) e.positions.push_back({row[i], row[i + 1], row[i + 2]});
    }
    return true;
}

}  // namespace

bool parseZone(const Entry& entry, Zone& z) {
    if (entry.type != 7 || entry.items.size() < 2 || entry.items[0].size() < 0x240) return false;
    const Bytes& h = entry.items[0];
    z = Zone{};
    z.eid = entry.eid;
    z.worldCount = u32(h, 0);
    for (uint32_t i = 0; i < z.worldCount && i < 8; ++i) z.worlds.push_back(u32(h, 4 + 0x40 * i));
    const uint32_t nb = u32(h, 0x210);
    for (uint32_t i = 0; i < nb && i < 8; ++i) z.neighbours.push_back(u32(h, 0x214 + 4 * i));
    const Bytes& box = entry.items[1];
    if (box.size() >= 24) { z.x = i32(box, 0); z.y = i32(box, 4); z.z = i32(box, 8); z.w = i32(box, 12); z.h = i32(box, 16); z.d = i32(box, 20); }
    for (size_t i = 2; i < entry.items.size(); ++i) {
        Entity e;
        if (parseEntity(entry.items[i], e)) z.entities.push_back(std::move(e));
    }
    return true;
}

bool parseNsdLevel(const Bytes& nsd, LevelInfo& out) {
    if (nsd.size() < 0x520) return false;
    const uint32_t entries = u32(nsd, 0x404);
    const size_t base = 0x520 + 8 * size_t(entries);
    if (entries > 100000 || base + 0x1DC > nsd.size()) return false;
    const uint32_t count = u32(nsd, base);
    if (base + 0x1DC + 24 * size_t(count) > nsd.size()) return false;
    out = LevelInfo{};
    out.levelId = u32(nsd, base + 8);
    for (int i = 0; i < 64; ++i) out.objectScripts.push_back(u32(nsd, base + 0x10 + 4 * size_t(i)));
    for (uint32_t i = 0; i < count; ++i) {
        const size_t o = base + 0x1DC + 24 * size_t(i);
        out.spawns.push_back({u32(nsd, o), i32(nsd, o + 4), i32(nsd, o + 8), i32(nsd, o + 12), i32(nsd, o + 16), i32(nsd, o + 20)});
    }
    return true;
}

}  // namespace cb2

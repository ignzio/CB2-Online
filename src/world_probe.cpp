// Research tool: which scenery section (and which shift) does a set of GTE input vertices belong to?
// usage: cb2_worldprobe <level.NSF> <samples.csv from CB2_GTEDUMPCSV> [pc-hex]
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <sstream>
#include <unordered_map>
#include <unordered_set>

#include "scenery.hpp"

namespace {
uint64_t pack(int x, int y, int z) { return (uint64_t(uint32_t(x + 70000)) << 40) | (uint64_t(uint32_t(y + 70000)) << 20) | uint64_t(uint32_t(z + 70000)); }
struct S { int vx, vy, vz; float sx, sy; };
}  // namespace

// usage: cb2_worldprobe level.NSF session.txt --session <level id hex>
// For each recorded group with T=0 (scenery projected with the camera matrix only), tests which sections' boxes, shifted by (section offset - camera), contain the group's input bounds.
int sessionMode(const char* nsfPath, const char* sessPath, uint32_t level) {
    cb2::Nsf nsf = cb2::loadNsf(nsfPath);
    std::vector<cb2::Scenery> worlds;
    for (const cb2::Entry& e : nsf.entries) if (e.type == cb2::kEntryTypeScenery) if (auto w = cb2::parseScenery(e)) worlds.push_back(std::move(*w));
    struct Box { int mn[3], mx[3]; };
    std::vector<Box> boxes;
    for (const auto& w : worlds) {
        Box b{{99999, 99999, 99999}, {-99999, -99999, -99999}};
        for (const auto& v : w.vertices) { const int p[3] = {v.x, v.y, v.z}; for (int k = 0; k < 3; ++k) { b.mn[k] = std::min(b.mn[k], p[k]); b.mx[k] = std::max(b.mx[k], p[k]); } }
        boxes.push_back(b);
    }
    std::map<uint32_t, int> pcGroups;
    std::vector<int> contained(worlds.size(), 0), sole(worlds.size(), 0);
    int groups = 0, unmatched = 0, frames = 0;
    bool inLevel = false;
    int cam[3] = {0, 0, 0};
    std::ifstream in(sessPath);
    for (std::string line; std::getline(in, line);) {
        if (line[0] == 'F') {
            unsigned long long fr; unsigned lv;
            std::sscanf(line.c_str(), "F %llu L %u C %d %d %d", &fr, &lv, &cam[0], &cam[1], &cam[2]);
            inLevel = lv == level;
            frames += inLevel;
        } else if (line[0] == 'G' && inLevel) {
            unsigned pc; size_t n; int r[9], t[3], v[6];
            if (std::sscanf(line.c_str(), "G %x %zu %d %d %d %d %d %d %d %d %d T %d %d %d V %d %d %d %d %d %d", &pc, &n, r, r + 1, r + 2, r + 3, r + 4, r + 5, r + 6, r + 7, r + 8, t, t + 1, t + 2, v, v + 1, v + 2, v + 3, v + 4, v + 5) != 20) continue;
            if (t[0] || t[1] || t[2] || n < 6) continue;
            ++groups; ++pcGroups[pc];
            std::vector<size_t> hit;
            for (size_t wi = 0; wi < worlds.size(); ++wi) {
                if (worlds[wi].vertices.empty()) continue;
                const int sh[3] = {worlds[wi].xOffset - cam[0], worlds[wi].yOffset - cam[1], worlds[wi].zOffset - cam[2]};
                bool ok = true;
                for (int k = 0; k < 3 && ok; ++k) ok = v[2 * k] >= 16 * boxes[wi].mn[k] + sh[k] - 32 && v[2 * k + 1] <= 16 * boxes[wi].mx[k] + sh[k] + 32;
                if (ok) hit.push_back(wi);
            }
            if (hit.empty()) ++unmatched;
            for (size_t wi : hit) ++contained[wi];
            if (hit.size() == 1) ++sole[hit[0]];
        }
    }
    std::printf("level %02x: %d frames, %d T=0 groups (n>=6), %d unmatched by any section\n", level, frames, groups, unmatched);
    for (auto& [pc, c] : pcGroups) std::printf("  pc %08x: %d groups\n", pc, c);
    for (size_t wi = 0; wi < worlds.size(); ++wi)
        std::printf("section %zu eid %08x sky=%d verts %zu offset %d,%d,%d: contains %d groups, sole match %d\n", wi, worlds[wi].eid, worlds[wi].sky, worlds[wi].vertices.size(), worlds[wi].xOffset, worlds[wi].yOffset, worlds[wi].zOffset, contained[wi], sole[wi]);
    return 0;
}

int main(int argc, char** argv) {
    if (argc < 3) { std::fprintf(stderr, "usage: cb2_worldprobe level.NSF samples.csv [pc]\n       cb2_worldprobe level.NSF session.txt --session <level id hex>\n"); return 1; }
    if (argc > 4 && !std::strcmp(argv[3], "--session")) return sessionMode(argv[1], argv[2], uint32_t(std::strtoul(argv[4], nullptr, 16)));
    const uint32_t wantPc = argc > 3 ? uint32_t(std::strtoul(argv[3], nullptr, 16)) : 0x80042420u;
    cb2::Nsf nsf = cb2::loadNsf(argv[1]);
    std::vector<cb2::Scenery> worlds;
    for (const cb2::Entry& e : nsf.entries) if (e.type == cb2::kEntryTypeScenery) if (auto w = cb2::parseScenery(e)) worlds.push_back(std::move(*w));
    std::printf("%zu scenery sections\n", worlds.size());
    std::vector<S> samples;
    std::ifstream in(argv[2]);
    for (std::string line; std::getline(in, line);) {
        std::istringstream ss(line);
        std::string f;
        std::vector<std::string> cols;
        while (std::getline(ss, f, ',')) cols.push_back(f);
        if (cols.size() < 7 || uint32_t(std::strtoul(cols[0].c_str(), nullptr, 16)) != wantPc) continue;
        samples.push_back({std::atoi(cols[1].c_str()), std::atoi(cols[2].c_str()), std::atoi(cols[3].c_str()), float(std::atof(cols[4].c_str())), float(std::atof(cols[5].c_str()))});
    }
    std::printf("%zu samples at %08x\n", samples.size(), wantPc);
    const size_t nAnchor = std::min<size_t>(samples.size(), 120);
    for (size_t wi = 0; wi < worlds.size(); ++wi) {
        const cb2::Scenery& w = worlds[wi];
        if (w.vertices.empty()) continue;
        std::unordered_set<uint64_t> set;
        for (const auto& v : w.vertices) set.insert(pack(v.x, v.y, v.z));
        std::unordered_map<uint64_t, int> votes;
        for (size_t a = 0; a < nAnchor; ++a) {
            const S& s = samples[a * samples.size() / nAnchor];
            for (const auto& v : w.vertices) ++votes[pack(s.vx - 16 * v.x, s.vy - 16 * v.y, s.vz - 16 * v.z)];
        }
        uint64_t best = 0; int bv = 0;
        for (const auto& [k, c] : votes) if (c > bv) { bv = c; best = k; }
        const int dx = int((best >> 40) & 0xFFFFF) - 70000, dy = int((best >> 20) & 0xFFFFF) - 70000, dz = int(best & 0xFFFFF) - 70000;
        size_t hits = 0;
        for (const S& s : samples) { const int ax = s.vx - dx, ay = s.vy - dy, az = s.vz - dz; if (ax % 16 == 0 && ay % 16 == 0 && az % 16 == 0 && set.count(pack(ax / 16, ay / 16, az / 16))) ++hits; }
        if (hits > samples.size() / 50 || bv > 12)
            std::printf("section %zu eid %08x sky=%d vertices %zu offset %d,%d,%d: shift (%d,%d,%d) => camera (offset - shift) = (%d,%d,%d); votes %d, samples explained %zu of %zu\n", wi, w.eid, w.sky, w.vertices.size(), w.xOffset, w.yOffset, w.zOffset, dx, dy, dz, w.xOffset - dx, w.yOffset - dy, w.zOffset - dz, bv, hits, samples.size());
    }
    return 0;
}

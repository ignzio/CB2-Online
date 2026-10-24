// cb2_entities <NSF>: lists the zones of a level and the entities in them.
#include <algorithm>
#include <cstdio>
#include <map>
#include <string>

#include "entity.hpp"
#include "nsf.hpp"
#include "scenery.hpp"

int main(int argc, char** argv) {
    if (argc < 2) { std::fprintf(stderr, "usage: cb2_entities <level.NSF> [-v]\n"); return 2; }
    const bool verbose = argc > 2;
    try {
        const cb2::Nsf nsf = cb2::loadNsf(argv[1]);
        {
            std::string nsdPath = argv[1];
            nsdPath = nsdPath.substr(0, nsdPath.find_last_of('.')) + ".NSD";
            cb2::LevelInfo info;
            if (cb2::parseNsdLevel(cb2::loadFile(nsdPath), info)) {
                std::printf("level id %X, %zu spawn points\n", info.levelId, info.spawns.size());
                for (const auto& s : info.spawns)
                    std::printf("  spawn zone %s flags %d cam %d at (%d, %d, %d)\n", cb2::eidName(s.zone).c_str(), s.flags, s.camera, s.x, s.y, s.z);
            }
        }
        for (const auto& e : nsf.entries) {
            if (auto w = cb2::parseScenery(e)) {
                int lo[3] = {1 << 30, 1 << 30, 1 << 30}, hi[3] = {-(1 << 30), -(1 << 30), -(1 << 30)};
                for (const auto& v : w->vertices) {
                    const int p[3] = {v.x * 16 + w->xOffset, v.y * 16 + w->yOffset, v.z * 16 + w->zOffset};
                    for (int k = 0; k < 3; ++k) { lo[k] = std::min(lo[k], p[k]); hi[k] = std::max(hi[k], p[k]); }
                }
                std::printf("world %s%s offset (%d %d %d) range x %d..%d y %d..%d z %d..%d\n", cb2::eidName(w->eid).c_str(), w->sky ? " [sky]" : "",
                            w->xOffset, w->yOffset, w->zOffset, lo[0], hi[0], lo[1], hi[1], lo[2], hi[2]);
            }
        }
        for (const auto& e : nsf.entries) {
            cb2::Zone z;
            if (!cb2::parseZone(e, z)) continue;
            std::printf("zone %s box (%d %d %d  %d %d %d) worlds %zu entities %zu\n", cb2::eidName(z.eid).c_str(), z.x, z.y, z.z, z.w, z.h, z.d,
                        z.worlds.size(), z.entities.size());
            std::map<int, int> byType;
            for (const auto& en : z.entities) ++byType[en.type];
            for (const auto& [t, n] : byType) std::printf("    type %3d x%d\n", t, n);
            if (verbose)
                for (const auto& en : z.entities) {
                    std::printf("  type %d sub %d id %d", en.type, en.subtype, en.id);
                    if (!en.positions.empty()) std::printf(" pos (%d,%d,%d) +%zu", en.positions[0].x, en.positions[0].y, en.positions[0].z, en.positions.size() - 1);
                    std::printf("\n");
                }
        }
    } catch (const std::exception& ex) {
        std::fprintf(stderr, "error: %s\n", ex.what());
        return 1;
    }
}

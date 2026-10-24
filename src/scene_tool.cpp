// Scenery editor tool: export a level's world meshes to OBJ, edit them in any 3D package, import them back.
//   cb2_scene test   <folder>                                  scenery -> OBJ -> scenery -> entry is byte-identical, all levels
//   cb2_scene list   <level.NSF>                               worlds in a level
//   cb2_scene export <level.NSF> <eid> <out.obj>
//   cb2_scene import <level.NSF> <eid> <in.obj> <out folder>   writes the modded NSF + NSD (use with cb2_play --mod)
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>

#include "nsd.hpp"
#include "nsf.hpp"
#include "scenery.hpp"
#include "scenery_edit.hpp"

namespace fs = std::filesystem;
using namespace cb2;

namespace {

std::vector<Entry> pageEntries(const Bytes& page) {
    uint16_t family;
    std::memcpy(&family, page.data() + 2, 2);
    return family == 0 ? readEntryPage(page) : std::vector<Entry>{};
}

int cmdTest(const std::string& folder) {
    size_t worlds = 0, bad = 0, verts = 0;
    for (auto& e : fs::recursive_directory_iterator(folder)) {
        if (!e.is_regular_file() || e.path().extension() != ".NSF" || e.path().string().find("cb2-viewer") != std::string::npos) continue;
        for (const Bytes& page : loadPages(e.path().string()))
            for (const Entry& en : pageEntries(page)) {
                const auto s = parseScenery(en);
                if (!s) continue;
                ++worlds; verts += s->vertices.size();
                bool ok = false;
                try {
                    // 1. re-encoding the parsed scenery reproduces the entry exactly
                    const Entry packed = packScenery(*s, en);
                    ok = packed.items == en.items;
                    // 2. the OBJ round trip reproduces the scenery (positions within rounding, same topology)
                    const Scenery back = sceneryFromObj(sceneryToObj(*s), *s);
                    const Entry again = packScenery(back, en);
                    if (!(ok && again.items == en.items)) {
                        for (size_t k = 0; k < 7; ++k) if (again.items[k] != en.items[k]) std::printf("  item %zu differs (vertices %zu->%zu colours %zu->%zu)\n", k, s->vertices.size(), back.vertices.size(), s->colors.size(), back.colors.size());
                    }
                    ok = ok && again.items == en.items;
                } catch (const std::exception& ex) {
                    std::printf("%s %s: %s\n", e.path().filename().string().c_str(), eidName(en.eid).c_str(), ex.what());
                }
                if (!ok) { ++bad; std::printf("MISMATCH %s %s\n", e.path().filename().string().c_str(), eidName(en.eid).c_str()); }
            }
    }
    std::printf("%zu worlds, %zu vertices: round-trip failures: %zu\n", worlds, verts, bad);
    return bad ? 1 : 0;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 3) { std::fprintf(stderr, "usage: cb2_scene test|list|export|import ...\n"); return 2; }
    const std::string cmd = argv[1];
    try {
        if (cmd == "test") return cmdTest(argv[2]);
        const auto pages = loadPages(argv[2]);
        if (cmd == "list") {
            for (const Bytes& page : pages)
                for (const Entry& en : pageEntries(page))
                    if (const auto s = parseScenery(en))
                        std::printf("%s  %zu vertices  %zu triangles  %zu quads%s\n", eidName(en.eid).c_str(), s->vertices.size(), s->triangles.size(), s->quads.size(), s->sky ? "  (sky)" : "");
            return 0;
        }
        if (argc < 5) return 2;
        for (size_t p = 0; p < pages.size(); ++p) {
            auto entries = pageEntries(pages[p]);
            for (size_t i = 0; i < entries.size(); ++i) {
                if (eidName(entries[i].eid) != argv[3]) continue;
                const auto s = parseScenery(entries[i]);
                if (!s) continue;
                if (cmd == "export") { std::ofstream(argv[4]) << sceneryToObj(*s); std::printf("wrote %s\n", argv[4]); return 0; }
                if (cmd == "import" && argc >= 6) {
                    std::ifstream in(argv[4]);
                    const std::string obj((std::istreambuf_iterator<char>(in)), {});
                    entries[i] = packScenery(sceneryFromObj(obj, *s), entries[i]);
                    std::vector<Bytes> out = pages;
                    const size_t added = repackPage(out, p, entries);
                    const fs::path nsf = argv[2], dir = argv[5];
                    fs::path nsd = nsf;
                    nsd.replace_extension(".NSD");
                    fs::create_directories(dir);
                    saveFile((dir / nsf.filename()).string(), writeNsf(out));
                    saveFile((dir / nsd.filename()).string(), rebuildNsd(loadFile(nsd.string()), out));
                    std::printf("imported %s (%zu pages added); wrote %s\n", argv[3], added, (dir / nsf.filename()).string().c_str());
                    return 0;
                }
            }
        }
        std::fprintf(stderr, "scenery %s not found\n", argc > 3 ? argv[3] : "");
        return 1;
    } catch (const std::exception& ex) {
        std::fprintf(stderr, "error: %s\n", ex.what());
        return 2;
    }
}

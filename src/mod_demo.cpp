// Demo mod: swaps red and blue in every scenery vertex colour of one level, then writes the modified
// NSF + regenerated NSD. Shows the whole modding path: read pages -> edit entries -> repack -> write -> NSD.
//   cb2_mod_demo <level.NSF> <output folder>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <utility>

#include "gool.hpp"
#include "nsd.hpp"
#include "nsf.hpp"
#include "scenery.hpp"

int main(int argc, char** argv) {
    namespace fs = std::filesystem;
    if (argc < 3) {
        std::fprintf(stderr, "usage: cb2_mod_demo <level.NSF> <output folder>\n");
        return 1;
    }
    try {
        const fs::path nsfPath = argv[1], outDir = argv[2];
        fs::path nsdPath = nsfPath;
        nsdPath.replace_extension(".NSD");

        std::vector<cb2::Bytes> pages = cb2::loadPages(nsfPath.string());
        size_t edited = 0;
        for (cb2::Bytes& page : pages) {
            uint16_t family;
            std::memcpy(&family, page.data() + 2, 2);
            if (family != 0) continue;
            std::vector<cb2::Entry> entries = cb2::readEntryPage(page);
            bool changed = false;
            for (cb2::Entry& e : entries) {
                if (e.type != cb2::kEntryTypeScenery || e.items.size() != 7) continue;
                for (size_t i = 0; i + 4 <= e.items[5].size(); i += 4) std::swap(e.items[5][i], e.items[5][i + 2]);
                changed = true;
                ++edited;
            }
            if (changed) {
                uint32_t id;
                std::memcpy(&id, page.data() + 4, 4);
                page = cb2::packEntryPage(id, entries);
            }
        }

        fs::create_directories(outDir);
        cb2::saveFile((outDir / nsfPath.filename()).string(), cb2::writeNsf(pages));
        cb2::saveFile((outDir / nsdPath.filename()).string(), cb2::rebuildNsd(cb2::loadFile(nsdPath.string()), pages));
        std::printf("edited %zu scenery entries; wrote %s\n", edited, (outDir / nsfPath.filename()).string().c_str());
    } catch (const std::exception& ex) {
        std::fprintf(stderr, "error: %s\n", ex.what());
        return 2;
    }
    return 0;
}

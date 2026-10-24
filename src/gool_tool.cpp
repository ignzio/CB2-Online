// GOOL script tool: list, disassemble, assemble and patch scripts inside NSF levels.
//   cb2_gool test  <folder>                       assemble(disassemble(x)) == x for every script of every NSF
//   cb2_gool list  <level.NSF>                    scripts in a level
//   cb2_gool dump  <level.NSF> <eid> [out.asm]    lossless script text
//   cb2_gool patch <level.NSF> <eid> <in.asm> <out folder>   write a modded NSF + NSD
// A patched script may only grow (new code and constants are appended) so that the state tables, which point
// into the code, stay valid.
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>

#include "gool.hpp"
#include "gool_asm.hpp"
#include "nsd.hpp"
#include "nsf.hpp"

namespace fs = std::filesystem;
using namespace cb2;

namespace {

struct Located { size_t page; size_t entry; };

std::vector<Entry> pageEntries(const Bytes& page) {
    uint16_t family;
    std::memcpy(&family, page.data() + 2, 2);
    return family == 0 ? readEntryPage(page) : std::vector<Entry>{};
}

GoolText toText(const Gool& g) { return GoolText{g.pool, g.code}; }

Bytes words(const std::vector<uint32_t>& w) {
    Bytes b(w.size() * 4);
    if (!w.empty()) std::memcpy(b.data(), w.data(), b.size());
    return b;
}

int cmdTest(const std::string& folder) {
    size_t scripts = 0, bad = 0, instr = 0, native = 0, raw = 0, labels = 0, words = 0;
    for (auto& e : fs::recursive_directory_iterator(folder)) {
        if (!e.is_regular_file() || e.path().extension() != ".NSF") continue;
        if (e.path().string().find("cb2-viewer") != std::string::npos) continue;
        for (const Bytes& page : loadPages(e.path().string())) {
            for (const Entry& en : pageEntries(page)) {
                if (en.type != kEntryTypeGool) continue;
                const auto g = parseGool(en);
                if (!g) continue;
                ++scripts;
                DisasmStats st;
                const GoolText t = toText(*g);
                const std::string text = disassembleScript(t, &st);
                instr += st.instructions; native += st.nativeWords; raw += st.rawWords; labels += st.labels; words += g->code.size();
                bool ok = false;
                try {
                    const GoolText back = assembleScript(text);
                    ok = back.code == t.code && back.pool == t.pool;
                } catch (const std::exception& ex) {
                    std::printf("%s %s: %s\n", e.path().filename().string().c_str(), eidName(en.eid).c_str(), ex.what());
                }
                if (!ok) { ++bad; std::printf("MISMATCH %s %s\n", e.path().filename().string().c_str(), eidName(en.eid).c_str()); }
            }
        }
    }
    std::printf("%zu scripts, %zu words: %zu instructions as text, %zu native-block words, %zu raw words, %zu labels; round-trip failures: %zu\n",
                scripts, words, instr, native, raw, labels, bad);
    return bad ? 1 : 0;
}

bool find(const std::vector<Bytes>& pages, const std::string& name, Located& at, Entry& entry) {
    for (size_t p = 0; p < pages.size(); ++p) {
        const auto es = pageEntries(pages[p]);
        for (size_t i = 0; i < es.size(); ++i)
            if (es[i].type == kEntryTypeGool && eidName(es[i].eid) == name) { at = {p, i}; entry = es[i]; return true; }
    }
    return false;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 3) { std::fprintf(stderr, "usage: cb2_gool test|list|dump|patch ...\n"); return 2; }
    const std::string cmd = argv[1];
    try {
        if (cmd == "test") return cmdTest(argv[2]);
        const auto pages = loadPages(argv[2]);
        if (cmd == "list") {
            for (const Bytes& page : pages)
                for (const Entry& en : pageEntries(page)) {
                    if (en.type != kEntryTypeGool) continue;
                    const auto g = parseGool(en);
                    if (g) std::printf("%s  id=%d  %zu words  %zu constants\n", eidName(en.eid).c_str(), g->id, g->code.size(), g->pool.size());
                }
            return 0;
        }
        if (argc < 4) return 2;
        Located at{};
        Entry entry;
        if (!find(pages, argv[3], at, entry)) { std::fprintf(stderr, "no script %s\n", argv[3]); return 1; }
        const auto g = parseGool(entry);
        if (!g) { std::fprintf(stderr, "script does not parse\n"); return 1; }
        if (cmd == "dump") {
            const std::string text = disassembleScript(toText(*g));
            if (argc > 4) std::ofstream(argv[4]) << text; else std::fputs(text.c_str(), stdout);
            return 0;
        }
        if (cmd == "patch" && argc >= 6) {
            std::ifstream in(argv[4]);
            const std::string src((std::istreambuf_iterator<char>(in)), {});
            const GoolText t = assembleScript(src);
            if (t.code.size() < g->code.size() || t.pool.size() < g->pool.size()) {
                std::fprintf(stderr, "the patched script is shorter than the original (%zu < %zu words); scripts may only grow\n", t.code.size(), g->code.size());
                return 1;
            }
            Bytes nsfPath = {};
            std::vector<Bytes> out = pages;
            auto entries = pageEntries(out[at.page]);
            entries[at.entry].items[1] = words(t.code);
            entries[at.entry].items[2] = words(t.pool);
            const size_t added = repackPage(out, at.page, entries);
            if (added) std::printf("note: the page was full, entries were moved to %zu new page(s)\n", added);
            const fs::path nsf = argv[2], dir = argv[5];
            fs::path nsd = nsf;
            nsd.replace_extension(".NSD");
            fs::create_directories(dir);
            saveFile((dir / nsf.filename()).string(), writeNsf(out));
            saveFile((dir / nsd.filename()).string(), rebuildNsd(loadFile(nsd.string()), out));
            std::printf("patched %s: %zu -> %zu words, %zu -> %zu constants; wrote %s\n", argv[3], g->code.size(), t.code.size(), g->pool.size(), t.pool.size(), (dir / nsf.filename()).string().c_str());
            return 0;
        }
    } catch (const std::exception& ex) {
        std::fprintf(stderr, "error: %s\n", ex.what());
        return 2;
    }
    std::fprintf(stderr, "bad command line\n");
    return 2;
}

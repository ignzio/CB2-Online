// Loads every NSF under a root folder and checks that the parser understands all of it.
// Exit status is non-zero on any failure, so it doubles as a regression test.
#include <cstdio>
#include <filesystem>
#include <map>

#include "gool.hpp"
#include "gool_decode.hpp"
#include "nsf.hpp"
#include "scenery.hpp"

int main(int argc, char** argv) {
    namespace fs = std::filesystem;
    const fs::path root = argc > 1 ? argv[1] : ".";
    const std::string dumpName = (argc > 3 && std::string(argv[2]) == "--dump") ? argv[3] : "";
    int files = 0, failures = 0;
    size_t chunks = 0, scenery = 0, badScenery = 0, polys = 0, unresolvedPages = 0;
    size_t gool = 0, badGool = 0, goolWords = 0, goolStates = 0, dataWords = 0, unspecced = 0;
    size_t branches = 0, badBranches = 0, mipsWords = 0;
    std::map<int, size_t> opHistogram;
    std::map<uint32_t, size_t> typeCounts;

    for (const auto& de : fs::recursive_directory_iterator(root)) {
        if (!de.is_regular_file()) continue;
        std::string ext = de.path().extension().string();
        for (auto& c : ext) c = char(std::toupper(static_cast<unsigned char>(c)));
        if (ext != ".NSF") continue;
        ++files;
        try {
            const cb2::Nsf nsf = cb2::loadNsf(de.path().string());
            const cb2::PageAtlas atlas = cb2::buildPageAtlas(nsf);
            chunks += nsf.chunkCount;
            for (const cb2::Entry& e : nsf.entries) {
                ++typeCounts[e.type];
                if (e.type == cb2::kEntryTypeGool) {
                    if (const auto g = cb2::parseGool(e)) {
                        ++gool;
                        goolWords += g->code.size();
                        goolStates += g->states.size();
                        if (!dumpName.empty() && cb2::eidName(g->eid) == dumpName) {
                            std::printf("GOOL %s: id=%d class=%d format=%d heapBase=%d events=%d entries=%d, %zu words, %zu states\n",
                                        dumpName.c_str(), g->id, g->objClass, g->format, g->heapBase, g->eventCount,
                                        g->entryCount, g->code.size(), g->states.size());
                            for (size_t i = 0; i < g->code.size(); ++i)
                                std::printf("%04zx: %08x  %s\n", i, g->code[i], cb2::disassemble(g->code[i]).c_str());
                        }
                        // Words after a MIPS marker (op 73) are native MIPS code up to `jalr s5, ra` (0x03E0A809)
                        // and its delay slot; that instruction also resumes the bytecode after the block.
                        bool inMips = false, afterEnd = false;
                        for (size_t i = 0; i < g->code.size(); ++i) {
                            const uint32_t w = g->code[i];
                            if (inMips) {
                                ++mipsWords;
                                if (afterEnd) inMips = afterEnd = false;
                                else if (w == 0x03E0A809u) afterEnd = true;
                                continue;
                            }
                            const int op = cb2::opcodeOf(w);
                            if (op >= cb2::kGoolOpcodeCount) { ++dataWords; continue; }
                            ++opHistogram[op];
                            if (!cb2::opSpec(op)) ++unspecced;
                            if (op == 73) inMips = true;
                            // BRA / BNEZ / BEQZ: target = next instruction + signed 10-bit word offset (bits 9..0)
                            if (op == 50 || op == 51 || op == 52) {
                                ++branches;
                                const int64_t target = int64_t(i) + 1 + ((int32_t(w << 22)) >> 22);
                                if (target < 0 || target >= int64_t(g->code.size())) {
                                    ++badBranches;
                                    if (badBranches <= 6) {
                                        std::printf("  out-of-range %s at %zu/%zu: %08x (prev %08x [%s], next %08x)\n",
                                                    cb2::eidName(g->eid).c_str(), i, g->code.size(), w,
                                                    i ? g->code[i - 1] : 0, i ? cb2::disassemble(g->code[i - 1]).c_str() : "-",
                                                    i + 1 < g->code.size() ? g->code[i + 1] : 0);
                                    }
                                }
                            }
                        }
                    } else {
                        ++badGool;
                    }
                    continue;
                }
                if (e.type != cb2::kEntryTypeScenery) continue;
                const auto world = cb2::parseScenery(e);
                if (!world) { ++badScenery; continue; }
                ++scenery;
                polys += world->triangles.size() + world->quads.size();
                for (uint32_t eid : world->tpages) unresolvedPages += atlas.index.count(eid) ? 0 : 1;
            }
        } catch (const std::exception& ex) {
            ++failures;
            std::fprintf(stderr, "FAIL %s: %s\n", de.path().string().c_str(), ex.what());
        }
    }

    std::printf("files=%d failed=%d chunks=%zu\n", files, failures, chunks);
    std::printf("scenery worlds=%zu invalid=%zu polygons=%zu unresolved texture pages=%zu\n", scenery, badScenery,
                polys, unresolvedPages);
    std::printf("GOOL scripts=%zu invalid=%zu instruction words=%zu states=%zu\n", gool, badGool, goolWords, goolStates);
    std::printf("GOOL decode: instruction words=%zu, embedded native MIPS words=%zu, other words=%zu, valid opcode without layout=%zu, distinct opcodes used=%zu\n",
                goolWords - dataWords - mipsWords, mipsWords, dataWords, unspecced, opHistogram.size());
    std::printf("GOOL branches: %zu, targets outside their script: %zu\n", branches, badBranches);
    if (dumpName.empty()) {
        std::printf("opcode usage:");
        for (const auto& [op, n] : opHistogram) {
            const cb2::OpSpec* s = cb2::opSpec(op);
            std::printf(" %s=%zu", s ? s->name : "?", n);
        }
        std::printf("\n");
    }
    for (const auto& [type, n] : typeCounts) std::printf("  entry type %02x: %zu\n", type, n);
    return (files == 0 || failures || badScenery || badGool || badBranches) ? 1 : 0;
}

// Determinism test for save states. Runs the game from the disc folder to frame A, saves, runs on to frame B and takes a digest of the whole
// console; then (1) a freshly built console loads the file and runs to B, and (2) the first console is rewound with the same file and runs to
// B again. All three digests must be identical, which only holds if the state file captured everything that matters.
// Usage: cb2_statetest <game folder> [A=4400] [B=4800] [state file]
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>

#include "savestate.hpp"

namespace {

struct Press { uint64_t from, to; uint16_t mask; };
const Press kScript[] = {{4000, 4004, 1u << 3}, {4500, 4560, 1u << 4}, {4620, 4630, 1u << 14}, {4700, 4800, 1u << 5}};   // start; up; cross; right

std::unique_ptr<psx::Platform> make(const std::string& folder) {
    auto p = std::make_unique<psx::Platform>();
    p->echoPrintf = false;
    p->card.path.clear();   // in memory only: the test must not touch a real card file
    if (!p->loadExe(folder + "\\SCUS_941.54") || !p->mountDisc(folder)) return nullptr;
    psx::Platform* raw = p.get();
    raw->onVblank = [raw] {
        uint16_t pad = 0xFFFF;
        for (const Press& pr : kScript) if (raw->frames >= pr.from && raw->frames <= pr.to) pad &= uint16_t(~pr.mask);
        raw->padButtons = pad;
    };
    return p;
}

bool runTo(psx::Platform& p, uint64_t frame) {
    while (p.frames < frame) {
        const std::string err = p.run(1 << 14);
        if (!err.empty()) { std::fprintf(stderr, "guest stopped: %s\n", err.c_str()); return false; }
    }
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) { std::fprintf(stderr, "usage: cb2_statetest <game folder> [A] [B] [state file]\n"); return 2; }
    const std::string folder = argv[1];
    const uint64_t A = argc > 2 ? std::strtoull(argv[2], nullptr, 10) : 4400, B = argc > 3 ? std::strtoull(argv[3], nullptr, 10) : 4800;
    const std::string file = argc > 4 ? argv[4] : (std::filesystem::temp_directory_path() / "cb2_statetest.cb2state").string();

    auto first = make(folder);
    if (!first) { std::fprintf(stderr, "cannot start the game from %s\n", folder.c_str()); return 2; }
    if (!runTo(*first, A)) return 1;
    std::string err;
    const uint64_t digestAtA = psx::stateDigest(*first);
    if (!psx::saveState(*first, file, {1, 2, 3, 42}, err)) { std::fprintf(stderr, "save failed: %s\n", err.c_str()); return 1; }
    std::printf("saved at frame %llu (%llu bytes), digest %016llx\n", (unsigned long long)first->frames, (unsigned long long)std::filesystem::file_size(file), (unsigned long long)digestAtA);
    if (!runTo(*first, B)) return 1;
    const uint64_t d1 = psx::stateDigest(*first);
    std::printf("original run to frame %llu: digest %016llx\n", (unsigned long long)first->frames, (unsigned long long)d1);

    int failures = 0;
    // (1) a fresh console
    {
        auto fresh = make(folder);
        std::vector<uint8_t> extra;
        if (!psx::loadState(*fresh, file, extra, err)) { std::fprintf(stderr, "load failed: %s\n", err.c_str()); return 1; }
        const bool extraOk = extra == std::vector<uint8_t>({1, 2, 3, 42});
        const uint64_t dLoaded = psx::stateDigest(*fresh);
        if (!runTo(*fresh, B)) return 1;
        const uint64_t d2 = psx::stateDigest(*fresh);
        std::printf("fresh console: extra %s, digest right after load %s, run to B %016llx %s\n", extraOk ? "ok" : "WRONG", dLoaded == digestAtA ? "matches the save" : "DIFFERS",
                    (unsigned long long)d2, d2 == d1 ? "== original" : "!= original (FAIL)");
        failures += (!extraOk) + (dLoaded != digestAtA) + (d2 != d1);
    }
    // (2) the same console rewound
    {
        std::vector<uint8_t> extra;
        if (!psx::loadState(*first, file, extra, err)) { std::fprintf(stderr, "reload failed: %s\n", err.c_str()); return 1; }
        if (!runTo(*first, B)) return 1;
        const uint64_t d3 = psx::stateDigest(*first);
        std::printf("rewound console: run to B %016llx %s\n", (unsigned long long)d3, d3 == d1 ? "== original" : "!= original (FAIL)");
        failures += d3 != d1;
    }
    // (3) a damaged file is refused and changes nothing
    {
        std::vector<uint8_t> bytes;
        { std::ifstream in(file, std::ios::binary); bytes.assign((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>()); }
        const std::string bad = file + ".bad";
        for (int variant = 0; variant < 3; ++variant) {
            std::vector<uint8_t> b = bytes;
            if (variant == 0) b[b.size() / 2] ^= 0x55;            // flipped byte
            else if (variant == 1) b.resize(b.size() - 100);       // cut short
            else b[3] = 'X';                                       // wrong magic
            { std::ofstream out(bad, std::ios::binary | std::ios::trunc); out.write(reinterpret_cast<const char*>(b.data()), std::streamsize(b.size())); }
            const uint64_t before = psx::stateDigest(*first);
            std::vector<uint8_t> extra;
            const bool loaded = psx::loadState(*first, bad, extra, err);
            const bool unchanged = psx::stateDigest(*first) == before;
            std::printf("damaged file %d: %s (%s), console %s\n", variant, loaded ? "ACCEPTED (FAIL)" : "refused", err.c_str(), unchanged ? "unchanged" : "CHANGED (FAIL)");
            failures += loaded || !unchanged;
        }
        std::filesystem::remove(bad);
    }
    std::filesystem::remove(file);
    std::printf(failures ? "FAILED (%d)\n" : "all passed\n", failures);
    return failures ? 1 : 0;
}

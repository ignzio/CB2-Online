// Dev tool: find where a memory card save's data sits in the console's RAM.
// Usage: cb2_cardfind <game folder> <state file> <memcard.mcr> [block=1]
// Takes the save's 8 KB block, and for every run of >= 16 bytes of it that also occurs in RAM, prints the card offset and the RAM address.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <vector>

#include "savestate.hpp"

int main(int argc, char** argv) {
    if (argc < 4) { std::fprintf(stderr, "usage: cb2_cardfind <game folder> <state> <card.mcr> [block]\n"); return 2; }
    psx::Platform p;
    p.echoPrintf = false;
    p.card.path.clear();
    if (!p.loadExe(std::string(argv[1]) + "\\SCUS_941.54") || !p.mountDisc(argv[1])) { std::fprintf(stderr, "cannot start the game\n"); return 2; }
    std::vector<uint8_t> extra;
    std::string err;
    if (!psx::loadState(p, argv[2], extra, err)) { std::fprintf(stderr, "load: %s\n", err.c_str()); return 2; }
    std::ifstream in(argv[3], std::ios::binary);
    std::vector<uint8_t> card((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    const int block = argc > 4 ? std::atoi(argv[4]) : 1;
    const size_t base = size_t(block) * 8192;
    if (card.size() < base + 8192) { std::fprintf(stderr, "card too small\n"); return 2; }
    const uint8_t* save = card.data() + base;
    int shown = 0;
    for (size_t off = 0; off + 16 <= 8192 && shown < 60;) {
        bool nonzero = false;
        for (size_t k = 0; k < 16; ++k) nonzero |= save[off + k] != 0;
        size_t bestLen = 0, bestAddr = 0;
        if (nonzero) {
            for (size_t a = 0; a + 16 <= p.ram.size(); ++a) {
                if (std::memcmp(&p.ram[a], save + off, 16) != 0) continue;
                size_t len = 16;
                while (off + len < 8192 && a + len < p.ram.size() && p.ram[a + len] == save[off + len]) ++len;
                if (len > bestLen) { bestLen = len; bestAddr = a; }
            }
        }
        if (bestLen >= 16) {
            std::printf("card +%04zx .. +%04zx (%zu bytes) = RAM %08zx\n", off, off + bestLen - 1, bestLen, 0x80000000u + bestAddr);
            ++shown;
            off += bestLen;
        } else {
            ++off;
        }
    }
    if (!shown) std::printf("no run of 16+ bytes of the save was found in RAM\n");
    return 0;
}

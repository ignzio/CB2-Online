// Dev tool: load a save state, run to two frames (relative to the state) and list the words of main RAM that equal `value` at the first
// frame and differ at the second (finding the game's variables, e.g. the current level id).
// Usage: cb2_statediff <game folder> <state file> <frameA> <frameB> <value> [from=0x80040000] [to=0x80090000]
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>

#include "savestate.hpp"

int main(int argc, char** argv) {
    if (argc < 6) { std::fprintf(stderr, "usage: cb2_statediff <game folder> <state> <frameA> <frameB> <value> [from] [to] [watch address]\n"); return 2; }
    const std::string folder = argv[1];
    const uint64_t a = std::strtoull(argv[3], nullptr, 10), b = std::strtoull(argv[4], nullptr, 10);
    const uint32_t value = uint32_t(std::strtoul(argv[5], nullptr, 0));
    const uint32_t from = argc > 6 ? uint32_t(std::strtoul(argv[6], nullptr, 0)) : 0x80040000u, to = argc > 7 ? uint32_t(std::strtoul(argv[7], nullptr, 0)) : 0x80090000u;
    psx::Platform p;
    p.echoPrintf = false;
    p.card.path.clear();
    if (!p.loadExe(folder + "\\SCUS_941.54") || !p.mountDisc(folder)) { std::fprintf(stderr, "cannot start the game\n"); return 2; }
    std::vector<std::array<uint64_t, 3>> presses;   // CB2_PRESS=from-to:hexmask,...  (frames relative to the state)
    if (const char* ps = std::getenv("CB2_PRESS")) {
        std::string s = ps;
        for (size_t pos = 0; pos < s.size();) {
            const size_t comma = s.find(',', pos);
            const std::string item = s.substr(pos, comma == std::string::npos ? std::string::npos : comma - pos);
            presses.push_back({std::strtoull(item.c_str(), nullptr, 10), std::strtoull(item.c_str() + item.find('-') + 1, nullptr, 10), std::strtoull(item.c_str() + item.find(':') + 1, nullptr, 16)});
            if (comma == std::string::npos) break;
            pos = comma + 1;
        }
    }
    uint64_t originFrame = 0;
    p.onVblank = [&] {
        uint16_t pad = 0xFFFF;
        for (const auto& pr : presses) if (p.frames - originFrame >= pr[0] && p.frames - originFrame <= pr[1]) pad &= uint16_t(~pr[2]);
        p.padButtons = pad;
    };
    // CB2_HOOKS=addr,addr,...: print every time execution reaches one of these addresses (a0..a3, ra)
    if (const char* hs = std::getenv("CB2_HOOKS")) {
        std::string s = hs;
        for (size_t pos = 0; pos < s.size();) {
            const size_t comma = s.find(',', pos);
            const uint32_t addr = uint32_t(std::strtoul(s.substr(pos, comma == std::string::npos ? std::string::npos : comma - pos).c_str(), nullptr, 16));
            p.addHook(addr, [addr](psx::Platform& pl) {
                std::printf("frame %llu: reached %08x a0=%08x a1=%08x a2=%08x a3=%08x ra=%08x v0=%08x s1=%08x s2=%08x\n", (unsigned long long)pl.frames, addr, pl.cpu.r[4], pl.cpu.r[5], pl.cpu.r[6], pl.cpu.r[7], pl.cpu.r[31], pl.cpu.r[2], pl.cpu.r[17], pl.cpu.r[18]);
            });
            if (comma == std::string::npos) break;
            pos = comma + 1;
        }
    }
    std::vector<uint8_t> extra;
    std::string err;
    std::string lastFile;
    if (std::getenv("CB2_CDLOG")) p.cd.onSectorRead = [&](uint32_t lba) {
        const std::string f = p.disc.fileAt(lba);
        if (f != lastFile) { lastFile = f; std::printf("frame +%llu: disc read of %s (lba %u)\n", (unsigned long long)p.frames, f.c_str(), lba); }
    };
    if (!psx::loadState(p, argv[2], extra, err)) { std::fprintf(stderr, "load: %s\n", err.c_str()); return 2; }
    const uint64_t start = p.frames;
    originFrame = start;
    const uint32_t watch = argc > 8 ? uint32_t(std::strtoul(argv[8], nullptr, 0)) : 0;
    uint64_t lastWatch = ~0ull;
    auto runTo = [&](uint64_t f) { while (p.frames < start + f) { if (watch && p.frames / 30 != lastWatch) { lastWatch = p.frames / 30; std::printf("frame +%llu: [%08x] = %08x\n", (unsigned long long)(p.frames - start), watch, p.peek32(watch)); } err = p.run(1 << 14); if (!err.empty()) { std::fprintf(stderr, "stopped: %s\n", err.c_str()); std::exit(1); } } };
    runTo(a);
    std::vector<uint8_t> first = p.ram;
    runTo(b);
    for (uint32_t addr = from; addr < to; addr += 4) {
        uint32_t x, y;
        std::memcpy(&x, &first[addr & 0x1FFFFF], 4);
        std::memcpy(&y, &p.ram[addr & 0x1FFFFF], 4);
        if (x == value && y != value) std::printf("%08x: %08x -> %08x\n", addr, x, y);
    }
    return 0;
}

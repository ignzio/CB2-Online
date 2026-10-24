// Dev tool: play the game headlessly with a scripted pad (or from a save state) and report the 32-bit words of main RAM that change by
// exactly +1 (counters: wumpa fruit, lives, crystals ...) or by any amount, with the frame they changed in. Used to find the game's
// progress variables for the online player list.
// Usage: cb2_scan <game folder> [--load state] [--save-at FRAME:file] [--press FROM-TO:up+cross ...] [--until FRAME]
//                 [--range 0x80060000-0x80080000] [--min N] [--watch addr ...] [--dump-at FRAME addr count]
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "savestate.hpp"

namespace {

struct Press { uint64_t from, to; uint16_t mask; };

uint16_t maskOf(const std::string& names) {
    static const struct { const char* n; uint16_t b; } k[] = {{"select", 1u << 0}, {"start", 1u << 3}, {"up", 1u << 4}, {"right", 1u << 5}, {"down", 1u << 6}, {"left", 1u << 7},
                                                              {"l2", 1u << 8}, {"r2", 1u << 9}, {"l1", 1u << 10}, {"r1", 1u << 11}, {"triangle", 1u << 12}, {"circle", 1u << 13},
                                                              {"cross", 1u << 14}, {"square", 1u << 15}};
    uint16_t m = 0;
    for (size_t pos = 0; pos <= names.size();) {
        const size_t plus = names.find('+', pos);
        const std::string n = names.substr(pos, plus == std::string::npos ? std::string::npos : plus - pos);
        for (const auto& e : k) if (n == e.n) m |= e.b;
        if (plus == std::string::npos) break;
        pos = plus + 1;
    }
    return m;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) { std::fprintf(stderr, "usage: cb2_scan <game folder> [options]\n"); return 2; }
    const std::string folder = argv[1];
    std::string loadPath, savePath;
    uint64_t saveAt = 0, until = 0, dumpAt = 0;
    uint32_t rangeFrom = 0x80060000u, rangeTo = 0x80080000u, dumpAddr = 0, dumpCount = 0;
    int minHits = 1;
    std::vector<Press> presses;
    std::vector<uint32_t> watch;
    for (int i = 2; i < argc; ++i) {
        const std::string a = argv[i];
        const bool more = i + 1 < argc;
        if (a == "--load" && more) loadPath = argv[++i];
        else if (a == "--save-at" && more) { const std::string s = argv[++i]; saveAt = std::strtoull(s.c_str(), nullptr, 10); savePath = s.substr(s.find(':') + 1); }
        else if (a == "--until" && more) until = std::strtoull(argv[++i], nullptr, 10);
        else if (a == "--min" && more) minHits = std::atoi(argv[++i]);
        else if (a == "--range" && more) { const std::string s = argv[++i]; rangeFrom = uint32_t(std::strtoul(s.c_str(), nullptr, 16)); rangeTo = uint32_t(std::strtoul(s.c_str() + s.find('-') + 1, nullptr, 16)); }
        else if (a == "--watch" && more) watch.push_back(uint32_t(std::strtoul(argv[++i], nullptr, 16)));
        else if (a == "--dump-at" && i + 3 < argc) { dumpAt = std::strtoull(argv[++i], nullptr, 10); dumpAddr = uint32_t(std::strtoul(argv[++i], nullptr, 16)); dumpCount = uint32_t(std::strtoul(argv[++i], nullptr, 10)); }
        else if (a == "--press" && more) {
            const std::string s = argv[++i];
            Press p;
            p.from = std::strtoull(s.c_str(), nullptr, 10);
            p.to = std::strtoull(s.c_str() + s.find('-') + 1, nullptr, 10);
            p.mask = maskOf(s.substr(s.find(':') + 1));
            presses.push_back(p);
        }
    }
    psx::Platform p;
    p.echoPrintf = false;
    p.card.path.clear();
    if (!p.loadExe(folder + "\\SCUS_941.54") || !p.mountDisc(folder)) { std::fprintf(stderr, "cannot start the game from %s\n", folder.c_str()); return 2; }
    if (!loadPath.empty()) {
        std::vector<uint8_t> extra;
        std::string err;
        if (!psx::loadState(p, loadPath, extra, err)) { std::fprintf(stderr, "load: %s\n", err.c_str()); return 2; }
        std::printf("loaded %s at frame %llu\n", loadPath.c_str(), (unsigned long long)p.frames);
    }
    const uint64_t origin = p.frames;
    const uint32_t lo = rangeFrom & 0x1FFFFF, hi = rangeTo & 0x1FFFFF;
    std::vector<uint8_t> prev = p.ram;
    struct Hit { int plusOne = 0, other = 0; std::vector<uint64_t> frames; uint32_t last = 0; };
    std::map<uint32_t, Hit> hits;
    bool stopped = false;
    p.onVblank = [&] {
        uint16_t pad = 0xFFFF;
        for (const Press& pr : presses) if (p.frames - origin >= pr.from && p.frames - origin <= pr.to) pad &= uint16_t(~pr.mask);
        p.padButtons = pad;
        if (p.frames - origin >= (until ? until : ~0ull)) stopped = true;
        if (saveAt && p.frames == origin + saveAt) {
            std::string err;
            std::printf(psx::saveState(p, savePath, {}, err) ? "state saved at frame %llu to %s\n" : "state NOT saved at frame %llu to %s\n", (unsigned long long)p.frames, savePath.c_str());
        }
        for (uint32_t w : watch) std::printf("frame %llu: [%08x] = %08x\n", (unsigned long long)p.frames, w, p.peek32(w));
        if (dumpAt && p.frames == origin + dumpAt) {
            std::printf("dump at frame %llu:", (unsigned long long)p.frames);
            for (uint32_t i = 0; i < dumpCount; ++i) std::printf("%s%08x", i % 8 ? " " : "\n  ", p.peek32(dumpAddr + 4 * i));
            std::printf("\n");
        }
        if (until || !hits.empty() || true) {
            for (uint32_t a = lo; a < hi; a += 4) {
                uint32_t x, y;
                std::memcpy(&x, &prev[a], 4);
                std::memcpy(&y, &p.ram[a], 4);
                if (x == y) continue;
                Hit& h = hits[0x80000000u | a];
                if (y == x + 1 && x < 100000) { ++h.plusOne; if (h.frames.size() < 12) h.frames.push_back(p.frames); } else ++h.other;
                h.last = y;
            }
            std::memcpy(prev.data(), p.ram.data(), prev.size());
        }
    };
    while (!stopped) {
        const std::string err = p.run(1 << 14);
        if (!err.empty()) { std::fprintf(stderr, "guest stopped: %s\n", err.c_str()); break; }
    }
    std::printf("ran to frame %llu\n", (unsigned long long)p.frames);
    int shown = 0;
    for (const auto& [addr, h] : hits) {
        if (h.plusOne < minHits || h.plusOne * 3 < h.other) continue;   // mostly "+1" steps: a counter, not noise
        std::printf("%08x: +1 x%d, other changes %d, now %u; frames:", addr, h.plusOne, h.other, h.last);
        for (uint64_t f : h.frames) std::printf(" %llu", (unsigned long long)(f - origin));
        std::printf("\n");
        if (++shown >= 60) break;
    }
    return 0;
}

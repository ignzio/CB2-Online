// cb2_mpspike: experiments for putting more players into the original game (kept as a tool for further research).
//   cb2_mpspike <game folder> --press 2310-2314:start --until 11050 [--trace-step] [--spawn F] [--frames-after N] [--shot out.ppm]
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "psx.hpp"

namespace {
constexpr uint32_t kObjStep = 0x8001C718;   // per-object update (a0 = object)
constexpr uint32_t kSpawn = 0x80019800;     // object creation (parent, type, subtype, argc, [sp+10] args, [sp+14] flag)
uint32_t F(int n) { return uint32_t(0x40 + 4 * n); }
}

int main(int argc, char** argv) {
    if (argc < 2) { std::fprintf(stderr, "usage: cb2_mpspike <game folder> ...\n"); return 2; }
    const std::string folder = argv[1];
    struct Press { uint64_t from, to; uint16_t mask; };
    std::vector<Press> presses;
    static const std::map<std::string, uint16_t> kButtons = {
        {"select", 1u << 0}, {"start", 1u << 3}, {"up", 1u << 4}, {"right", 1u << 5}, {"down", 1u << 6}, {"left", 1u << 7},
        {"l1", 1u << 10}, {"r1", 1u << 11}, {"triangle", 1u << 12}, {"circle", 1u << 13}, {"cross", 1u << 14}, {"square", 1u << 15}};
    uint64_t until = 11050, spawnAt = 0, after = 60;
    bool traceStep = false;
    uint32_t spawnType = 0, spawnSub = 0;
    bool puppet = false, parentRoot = false, dumpDesc = false;
    int puppetDx = 0x40000, puppetDz = 0;
    std::string shot;
    for (int i = 2; i < argc; ++i) {
        const bool more = i + 1 < argc;
        if (!std::strcmp(argv[i], "--press") && more) {
            std::string s = argv[++i];
            const size_t dash = s.find('-'), colon = s.find(':');
            Press p{std::strtoull(s.c_str(), nullptr, 10), std::strtoull(s.c_str() + dash + 1, nullptr, 10), 0};
            std::string names = s.substr(colon + 1);
            for (size_t pos = 0; pos <= names.size();) {
                const size_t plus = names.find('+', pos);
                const std::string n = names.substr(pos, plus == std::string::npos ? std::string::npos : plus - pos);
                if (kButtons.count(n)) p.mask |= kButtons.at(n);
                if (plus == std::string::npos) break;
                pos = plus + 1;
            }
            presses.push_back(p);
        } else if (!std::strcmp(argv[i], "--until") && more) until = std::strtoull(argv[++i], nullptr, 10);
        else if (!std::strcmp(argv[i], "--spawn") && more) spawnAt = std::strtoull(argv[++i], nullptr, 10);
        else if (!std::strcmp(argv[i], "--type") && more) spawnType = uint32_t(std::strtoul(argv[++i], nullptr, 10));
        else if (!std::strcmp(argv[i], "--subtype") && more) spawnSub = uint32_t(std::strtoul(argv[++i], nullptr, 10));
        else if (!std::strcmp(argv[i], "--parent-root")) parentRoot = true;
        else if (!std::strcmp(argv[i], "--dump")) dumpDesc = true;
        else if (!std::strcmp(argv[i], "--puppet")) puppet = true;   // drive the spawned object from the player object every frame
        else if (!std::strcmp(argv[i], "--dx") && more) puppetDx = std::atoi(argv[++i]);
        else if (!std::strcmp(argv[i], "--frames-after") && more) after = std::strtoull(argv[++i], nullptr, 10);
        else if (!std::strcmp(argv[i], "--trace-step")) traceStep = true;
        else if (!std::strcmp(argv[i], "--shot") && more) shot = argv[++i];
    }

    psx::Platform p;
    p.echoPrintf = false;
    if (!p.loadExe(folder + "\\SCUS_941.54") || !p.mountDisc(folder, {})) { std::fprintf(stderr, "cannot load the game from %s\n", folder.c_str()); return 2; }
    p.onVblank = [&] {
        uint16_t pad = 0xFFFF;
        for (const auto& pr : presses) if (p.frames >= pr.from && p.frames <= pr.to) pad &= uint16_t(~pr.mask);
        p.padButtons = pad;
    };

    auto findPlayer = [&]() -> uint32_t {
        for (uint32_t a = 0x80010000; a < 0x80200000 - 0x280; a += 4)
            if (p.peek32(a + F(0)) == a && p.peek32(a + F(5)) == a) return a;
        return 0;
    };
    auto describe = [&](uint32_t a) {
        std::printf("  obj %08x entry %08x id %d sub %d state %08x pos (%d,%d,%d) rotY %d vel (%d,%d,%d) player %08x pc %08x fgrp %08x fnum %d\n", a, p.peek32(a + 0xC),
                    int(p.peek32(a + F(30))), int(p.peek32(a + F(29))), p.peek32(a + F(1) - 0x4 + 0x4), int(p.peek32(a + F(8))), int(p.peek32(a + F(9))),
                    int(p.peek32(a + F(10))), int(p.peek32(a + F(12))), int(p.peek32(a + F(17))), int(p.peek32(a + F(18))), int(p.peek32(a + F(19))),
                    p.peek32(a + F(5)), p.peek32(a + F(32)), p.peek32(a + F(42)), int(p.peek32(a + F(43))));
    };

    uint32_t spawned = 0;
    bool didSpawn = false;
    uint64_t traceFrame = 0;
    int traced = 0;
    p.addHook(kObjStep, [&](psx::Platform& pl) {
        const uint32_t obj = pl.cpu.r[4];
        if (traceStep && pl.frames >= until && traced < 60 && traceFrame != pl.frames + 1) {
            if (traceFrame == 0) traceFrame = pl.frames;
            if (pl.frames == traceFrame) {
                std::printf("[step] frame %llu obj %08x a1 %u ra %08x type %u\n", (unsigned long long)pl.frames, obj, pl.cpu.r[5], pl.cpu.r[31], pl.peek32(obj + 4));
                ++traced;
            }
        }
        if (spawnAt && !didSpawn && pl.frames >= spawnAt) {
            didSpawn = true;
            const uint32_t player = findPlayer();
            std::printf("[spawn] frame %llu: player object %08x, calling the object creation function (type %u sub %u)\n", (unsigned long long)pl.frames, player, spawnType, spawnSub);
            spawned = pl.callFunction(kSpawn, {parentRoot ? 0x8006CB20u : player, spawnType, spawnSub, 0}, {0, 0});
            if (dumpDesc) {
                auto dump = [&](const char* name, uint32_t a) {
                    std::printf("  %s %08x: ", name, a);
                    for (int i = 0; i < 16; ++i) std::printf("%08x ", pl.peek32(a + 4 * i));
                    std::printf("\n");
                    const uint32_t d = pl.peek32(a + 8);
                    std::printf("    descriptor %08x: ", d);
                    for (int i = 0; i < 16; ++i) std::printf("%08x ", pl.peek32(d + 4 * i));
                    std::printf("\n");
                };
                dump("player", player);
                if (spawned) dump("spawned", spawned);
            }
            std::printf("[spawn] returned %08x\n", spawned);
            if (spawned) describe(spawned);
        }
        if (puppet && spawned && spawned != findPlayer() && obj == spawned) {
            // freeze the object's script blocks and copy the player's pose to it, shifted sideways
            const uint32_t pl0 = findPlayer();
            pl.poke32(spawned + 0xE8, 0);   // trans block
            pl.poke32(spawned + 0xEC, 0);   // event block
            pl.poke32(spawned + 0xF0, 0);   // head block
            pl.poke32(spawned + F(8), pl.peek32(pl0 + F(8)) + uint32_t(puppetDx));
            pl.poke32(spawned + F(9), pl.peek32(pl0 + F(9)));
            pl.poke32(spawned + F(10), pl.peek32(pl0 + F(10)) + uint32_t(puppetDz));
            pl.poke32(spawned + F(12), pl.peek32(pl0 + F(12)));
            pl.poke32(spawned + F(42), pl.peek32(pl0 + F(42)));
            pl.poke32(spawned + F(43), pl.peek32(pl0 + F(43)));
            pl.poke32(spawned + F(14), pl.peek32(pl0 + F(14)));
            pl.poke32(spawned + F(15), pl.peek32(pl0 + F(15)));
            pl.poke32(spawned + F(16), pl.peek32(pl0 + F(16)));
        }
    });

    std::string stop;
    const uint64_t last = until + (spawnAt ? after : 0) + (traceStep ? 2 : 0);
    uint64_t lastReport = 0;
    while (p.frames < last && stop.empty()) {
        stop = p.run(1 << 14);
        if (didSpawn && p.frames != lastReport && p.frames % 10 == 0) {
            lastReport = p.frames;
            std::printf("frame %llu\n", (unsigned long long)p.frames);
            describe(findPlayer());
            if (spawned) describe(spawned);
        }
    }
    if (!stop.empty()) std::printf("stopped: %s\n", stop.c_str());
    if (!shot.empty()) {
        FILE* f = std::fopen(shot.c_str(), "wb");
        if (f) {
            const auto& g = p.gpu;
            std::fprintf(f, "P6\n%d %d\n255\n", g.hres, g.vres);
            for (int y = 0; y < g.vres; ++y)
                for (int x = 0; x < g.hres; ++x) {
                    const uint16_t px = g.vram[size_t((g.displayY + y) & 511) * 1024 + size_t((g.displayX + x) & 1023)];
                    const uint8_t c[3] = {uint8_t((px & 31) << 3), uint8_t(((px >> 5) & 31) << 3), uint8_t(((px >> 10) & 31) << 3)};
                    std::fwrite(c, 1, 3, f);
                }
            std::fclose(f);
        }
    }
    return 0;
}

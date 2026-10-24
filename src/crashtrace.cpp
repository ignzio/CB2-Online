// cb2_crashtrace: records the original game's object state (position, velocity, state, animation) frame by frame while a scripted
// pad input is applied. Used to measure Crash's real behaviour so the standalone engine can match it.
//   cb2_crashtrace <game folder> --press 2310-2314:start ... --list FRAME           list game objects at FRAME
//   cb2_crashtrace <game folder> --press ... --follow HEXADDR --from F --to T          one CSV line per frame for that object
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "psx.hpp"

int main(int argc, char** argv) {
    if (argc < 2) { std::fprintf(stderr, "usage: cb2_crashtrace <game folder> [--press a-b:btn+btn]... [--list F] [--follow ADDR --from F --to T]\n"); return 2; }
    const std::string folder = argv[1];
    struct Press { uint64_t from, to; uint16_t mask; };
    std::vector<Press> presses;
    static const std::map<std::string, uint16_t> kButtons = {
        {"select", 1u << 0}, {"start", 1u << 3}, {"up", 1u << 4}, {"right", 1u << 5}, {"down", 1u << 6}, {"left", 1u << 7},
        {"l1", 1u << 10}, {"r1", 1u << 11}, {"triangle", 1u << 12}, {"circle", 1u << 13}, {"cross", 1u << 14}, {"square", 1u << 15}};
    uint64_t listAt = 0, from = 0, to = 0;
    uint32_t follow = 0;
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
        } else if (!std::strcmp(argv[i], "--list") && more) listAt = std::strtoull(argv[++i], nullptr, 10);
        else if (!std::strcmp(argv[i], "--follow") && more) follow = uint32_t(std::strtoul(argv[++i], nullptr, 16));
        else if (!std::strcmp(argv[i], "--from") && more) from = std::strtoull(argv[++i], nullptr, 10);
        else if (!std::strcmp(argv[i], "--to") && more) to = std::strtoull(argv[++i], nullptr, 10);
    }

    psx::Platform p;
    p.echoPrintf = false;
    if (!p.loadExe(folder + "\\SCUS_941.54") || !p.mountDisc(folder, {})) { std::fprintf(stderr, "cannot load the game from %s\n", folder.c_str()); return 2; }

    auto rd = [&](uint32_t a) { return p.cpu.rd32(a); };
    // Crash 2 process layout: field n lives at object + 0x40 + 4n (see src/gool_fields.inc); links point at the object itself.
    auto F = [](int n) { return uint32_t(0x40 + 4 * n); };
    auto listObjects = [&] {
        for (uint32_t a = 0x80010000; a < 0x80200000 - 0x280; a += 4) {
            if (rd(a + F(0)) != a || rd(a + F(5)) != a) continue;   // self link and player link both point at the object: Crash
            std::printf("player object %08x: pos (%d,%d,%d) vel (%d,%d,%d) rotY %d pc %08x framegroup %08x frame %d\n", a, int(rd(a + F(8))), int(rd(a + F(9))),
                        int(rd(a + F(10))), int(rd(a + F(17))), int(rd(a + F(18))), int(rd(a + F(19))), int(rd(a + F(12))), rd(a + F(32)), rd(a + F(42)),
                        int(rd(a + F(43))));
        }
    };
    if (follow) std::printf("frame,x,y,z,vx,vy,vz,rotY,trotY,pc,framegroup,framenum,groundy,statusA,statusB,stateflag,speed,pad\n");
    p.onVblank = [&] {
        uint16_t pad = 0xFFFF;
        for (const auto& pr : presses) if (p.frames >= pr.from && p.frames <= pr.to) pad &= uint16_t(~pr.mask);
        p.padButtons = pad;
        if (listAt && p.frames == listAt) listObjects();
        if (follow && p.frames >= from && p.frames <= to) {
            const uint32_t a = follow;
            std::printf("%llu,%d,%d,%d,%d,%d,%d,%d,%d,%08x,%08x,%d,%d,%08x,%08x,%d,%d,%04x\n", (unsigned long long)p.frames, int(rd(a + F(8))), int(rd(a + F(9))),
                        int(rd(a + F(10))), int(rd(a + F(17))), int(rd(a + F(18))), int(rd(a + F(19))), int(rd(a + F(12))), int(rd(a + F(21))), rd(a + F(32)),
                        rd(a + F(42)), int(rd(a + F(43))), int(rd(a + F(47))), rd(a + F(26)), rd(a + F(27)), int(rd(a + F(48))), int(rd(a + F(49))), pad);
        }
    };
    const uint64_t last = follow ? to : listAt;
    std::string stop;
    while (p.frames <= last && stop.empty()) stop = p.run(1 << 14);
    if (!stop.empty()) std::fprintf(stderr, "stopped: %s\n", stop.c_str());
    return 0;
}

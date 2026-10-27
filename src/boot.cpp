// Boot harness: runs the game executable on the platform layer and reports what it asked of the system.
//   cb2_boot <game folder> [instructions] [--trace] [--vram out.ppm] [--cd]
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "psx.hpp"
#include "savestate.hpp"

int main(int argc, char** argv) {
    if (argc < 2) { std::fprintf(stderr, "usage: cb2_boot <game folder> [instructions] [--trace] [--vram out.ppm]\n"); return 2; }
    const std::string folder = argv[1];
    uint64_t limit = 50'000'000ull, frameLimit = 0;
    bool trace = false, irq = false, cdTrace = false;
    std::string vramOut, displayOut, wavOut, ramOut;
    uint32_t ramMb = 2;
    std::vector<std::string> mods;
    std::vector<int16_t> audioCapture;
    std::vector<uint32_t> peeks;
    struct Press { uint64_t from, to; uint16_t mask; };
    std::vector<Press> presses;
    std::vector<std::pair<uint64_t, std::string>> shots;
    static const std::map<std::string, uint16_t> kButtons = {
        {"select", 1u << 0}, {"start", 1u << 3}, {"up", 1u << 4}, {"right", 1u << 5}, {"down", 1u << 6}, {"left", 1u << 7},
        {"l2", 1u << 8}, {"r2", 1u << 9}, {"l1", 1u << 10}, {"r1", 1u << 11}, {"triangle", 1u << 12}, {"circle", 1u << 13},
        {"cross", 1u << 14}, {"square", 1u << 15}};
    bool useNative = false;
    std::string hwSitesOut, statePath;   // --load-state: start from a save state (a game that froze) instead of the boot
    for (int i = 2; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--native")) useNative = true;
        else if (!std::strcmp(argv[i], "--load-state") && i + 1 < argc) statePath = argv[++i];
        else if (!std::strcmp(argv[i], "--hwsites") && i + 1 < argc) hwSitesOut = argv[++i];
        else if (!std::strcmp(argv[i], "--trace")) trace = true;
        else if (!std::strcmp(argv[i], "--irq")) irq = true;
        else if (!std::strcmp(argv[i], "--cd")) cdTrace = true;
        else if (!std::strcmp(argv[i], "--frames") && i + 1 < argc) frameLimit = std::strtoull(argv[++i], nullptr, 0);
        else if (!std::strcmp(argv[i], "--press") && i + 1 < argc) {  // --press 2200-2210:start+cross
            const std::string s = argv[++i];
            const size_t dash = s.find('-'), colon = s.find(':');
            Press pr{std::strtoull(s.c_str(), nullptr, 10), std::strtoull(s.c_str() + dash + 1, nullptr, 10), 0};
            for (size_t b = colon + 1; b < s.size();) {
                const size_t plus = std::min(s.find('+', b), s.size());
                auto it = kButtons.find(s.substr(b, plus - b));
                if (it != kButtons.end()) pr.mask |= it->second;
                b = plus + 1;
            }
            presses.push_back(pr);
        }
        else if (!std::strcmp(argv[i], "--shot") && i + 1 < argc) {  // --shot 2400:out.ppm
            const std::string s = argv[++i];
            shots.push_back({std::strtoull(s.c_str(), nullptr, 10), s.substr(s.find(':') + 1)});
        }
        else if (!std::strcmp(argv[i], "--ram-mb") && i + 1 < argc) ramMb = uint32_t(std::strtoul(argv[++i], nullptr, 10));
        else if (!std::strcmp(argv[i], "--ram") && i + 1 < argc) ramOut = argv[++i];
        else if (!std::strcmp(argv[i], "--mod") && i + 1 < argc) mods.push_back(argv[++i]);
        else if (!std::strcmp(argv[i], "--wav") && i + 1 < argc) wavOut = argv[++i];
        else if (!std::strcmp(argv[i], "--display") && i + 1 < argc) displayOut = argv[++i];
        else if (!std::strcmp(argv[i], "--vram") && i + 1 < argc) vramOut = argv[++i];
        else if (!std::strcmp(argv[i], "--peek") && i + 1 < argc) peeks.push_back(uint32_t(std::strtoul(argv[++i], nullptr, 16)));
        else limit = std::strtoull(argv[i], nullptr, 0);
    }
    psx::Platform p;
    const char* exeName = std::filesystem::exists(folder + "\\SCES_009.67") ? "SCES_009.67" : "SCUS_941.54";   // the European or the American release
    std::string exe = folder + "\\" + exeName;
    for (const auto& m : mods) if (std::filesystem::exists(m + "\\" + exeName)) exe = m + "\\" + exeName;
    p.traceBios = trace;
    p.traceIrq = irq;
    p.cd.trace = cdTrace;
    if (std::getenv("CB2_NOREVERB")) p.spu.reverb = false;
    p.recordHwSites = !hwSitesOut.empty();
    if (ramMb > 2) p.setRamMegabytes(ramMb);
    if (!wavOut.empty()) p.spu.sink = [&](int16_t l, int16_t r) { audioCapture.push_back(l); audioCapture.push_back(r); };
    if (!p.loadExe(exe)) { std::fprintf(stderr, "cannot load %s\n", exe.c_str()); return 2; }
    if (!p.mountDisc(folder, mods)) { std::fprintf(stderr, "cannot mount disc folder\n"); return 2; }
    std::printf("virtual disc: %u sectors\n", p.disc.sectorCount());
    if (useNative) std::printf("native engine code: %s\n", p.enableNative() ? "enabled" : "not available (run tools/recomp.py)");

    auto writeDisplay = [&](const std::string& path) {
        FILE* f = std::fopen(path.c_str(), "wb");
        if (!f) return;
        const auto& g = p.gpu;
        std::fprintf(f, "P6\n%d %d\n255\n", g.hres, g.vres);
        for (int y = 0; y < g.vres; ++y)
            for (int x = 0; x < g.hres; ++x) {
                const uint16_t px = g.vram[size_t((g.displayY + y) & 511) * 1024 + size_t((g.displayX + x) & 1023)];
                const uint8_t c[3] = {uint8_t((px & 31) << 3), uint8_t(((px >> 5) & 31) << 3), uint8_t(((px >> 10) & 31) << 3)};
                std::fwrite(c, 1, 3, f);
            }
        std::fclose(f);
    };
    p.onVblank = [&] {
        uint16_t pad = 0xFFFF;
        for (const auto& pr : presses) if (p.frames >= pr.from && p.frames <= pr.to) pad &= uint16_t(~pr.mask);
        p.padButtons = pad;
        for (const auto& [fr, path] : shots) if (fr == p.frames) writeDisplay(path);
    };

    if (!statePath.empty()) {
        std::vector<uint8_t> extra;
        std::string err;
        if (!psx::loadState(p, statePath, extra, err)) { std::fprintf(stderr, "cannot load the state: %s\n", err.c_str()); return 2; }
        std::printf("loaded state %s (frame %llu, pc %08x)\n", statePath.c_str(), (unsigned long long)p.frames, p.cpu.pc);
    }
    if (const char* hp = std::getenv("CB2_HOOKPC")) {   // tool: CB2_HOOKPC=addr[,addr...] print the registers and the words a0 points at the first times the CPU reaches each pc
        std::string list = hp;
        for (size_t pos = 0; pos < list.size();) {
            const size_t comma = list.find(',', pos);
            const uint32_t at = uint32_t(std::strtoul(list.substr(pos, comma == std::string::npos ? std::string::npos : comma - pos).c_str(), nullptr, 16));
            pos = comma == std::string::npos ? list.size() : comma + 1;
            auto hits = std::make_shared<int>(0);
            const int cap = std::getenv("CB2_HOOKMAX") ? std::atoi(std::getenv("CB2_HOOKMAX")) : 6;
            p.addHook(at, [hits, cap](psx::Platform& pl) {
                if (++*hits > cap) return;
                std::printf("[hook] pc %08x a0 %08x a1 %08x a2 %08x a3 %08x ra %08x sp %08x; *a0 = %08x %08x %08x %08x\n", pl.cpu.pc, pl.cpu.r[4], pl.cpu.r[5], pl.cpu.r[6], pl.cpu.r[7], pl.cpu.r[31], pl.cpu.r[29], pl.peek32(pl.cpu.r[4]), pl.peek32(pl.cpu.r[4] + 4), pl.peek32(pl.cpu.r[4] + 8), pl.peek32(pl.cpu.r[4] + 12));
            });
        }
    }
    if (const char* kill = std::getenv("CB2_KILL")) {   // tool: CB2_KILL=object:destroyRoutine - run the game's own destroy routine on an object (does removing it free a stuck game?)
        const uint32_t obj = uint32_t(std::strtoul(kill, nullptr, 16));
        const char* colon = std::strchr(kill, ':');
        const uint32_t fn = colon ? uint32_t(std::strtoul(colon + 1, nullptr, 16)) : 0;
        try { p.callFunction(fn, {obj, 0}, {0, 0}); std::printf("[kill] destroyed %08x with %08x\n", obj, fn); }
        catch (const std::exception& e) { std::printf("[kill] failed: %s\n", e.what()); }
    }
    std::map<uint32_t, uint64_t> hist;
    std::string stop;
    uint64_t done = 0;
    while (done < limit && (!frameLimit || p.frames < frameLimit)) {
        const uint64_t chunk = std::min<uint64_t>(16381, limit - done);
        stop = p.run(chunk);
        done += chunk;
        ++hist[p.cpu.pc];
        if (!stop.empty()) break;
    }
    if (std::getenv("CB2_SAMPLE")) {
        std::printf("[sample] %s; istat %08x imask %08x\n", p.cd.describe().c_str(), p.istat, p.imask);
        int marked = 0;   // objects the online code created (field 63 holds the marker 0x4D504C59)
        for (uint32_t a = 0x80010000u; a < 0x80200000u; a += 4)
            if (p.peek32(a) == 0x4D504C59u) { std::printf("[sample] online puppet object at %08x\n", a - 0x13Cu); ++marked; }
        std::printf("[sample] %d online puppet object(s) in RAM\n", marked);
    }
    if (std::getenv("CB2_SAMPLE")) {   // tool: where does the CPU spend its time, and who called it? (a short sample of the program counter and the return address)
        std::map<uint32_t, uint64_t> pcs, ras;
        const uint64_t before = p.cpu.instructions;
        for (int i = 0; i < 4000; ++i) { p.run(997); ++pcs[p.cpu.pc]; ++ras[p.cpu.r[31]]; }
        std::printf("[sample] %llu instructions, frames %llu\n", (unsigned long long)(p.cpu.instructions - before), (unsigned long long)p.frames);
        auto top = [](const std::map<uint32_t, uint64_t>& m, const char* tag) {
            std::vector<std::pair<uint64_t, uint32_t>> v;
            for (auto& [k, n] : m) v.push_back({n, k});
            std::sort(v.rbegin(), v.rend());
            for (size_t i = 0; i < v.size() && i < 14; ++i) std::printf("[sample] %s %08x %llu\n", tag, v[i].second, (unsigned long long)v[i].first);
        };
        top(pcs, "pc"); top(ras, "ra");
    }
    if (std::getenv("CB2_STEPDUMP")) {   // tool: the next instructions the CPU executes (where is it stuck?)
        for (int i = 0; i < 40; ++i) {
            std::printf("[step] pc %08x next %08x ins %08x r2 %08x r3 %08x r4 %08x ra %08x sr %08x istat %08x imask %08x\n", p.cpu.pc, p.cpu.nextPc, p.cpu.rd32(p.cpu.pc), p.cpu.r[2], p.cpu.r[3], p.cpu.r[4], p.cpu.r[31], p.cpu.cop0[12], p.istat, p.imask);
            try { p.cpu.step(); } catch (const std::exception& e) { std::printf("[step] exception: %s\n", e.what()); break; }
            std::printf("[step]   after: pc %08x next %08x ra %08x instructions %llu fault %d\n", p.cpu.pc, p.cpu.nextPc, p.cpu.r[31], (unsigned long long)p.cpu.instructions, int(p.cpu.fault));
        }
    }
    if (!hwSitesOut.empty()) {
        FILE* f = std::fopen(hwSitesOut.c_str(), "w");
        if (f) {
            for (auto& [pc, devs] : p.hwSites) {
                std::fprintf(f, "%08x", pc);
                for (auto& [d, n] : devs) std::fprintf(f, " %s:%llu", d.c_str(), (unsigned long long)n);
                std::fprintf(f, "\n");
            }
            for (auto& [ra, n] : p.biosSites) std::fprintf(f, "bios %08x %llu\n", ra, (unsigned long long)n);
            std::fclose(f);
        }
    }
    std::printf("stopped after %llu instructions: %s\n", (unsigned long long)done, stop.empty() ? "instruction limit" : stop.c_str());
    std::printf("frames: %llu  gpu: %u draw commands, %u primitives, %u unknown\n", (unsigned long long)p.frames, p.gpu.drawCalls, p.gpu.primitives, p.gpu.unknownCommands);
    std::printf("display: %dx%d at (%d,%d) enabled=%d\n", p.gpu.hres, p.gpu.vres, p.gpu.displayX, p.gpu.displayY, p.gpu.displayEnabled);
    if (useNative) {
        std::printf("native: %llu translated calls, %llu interpreted calls, %llu interpreted instructions of %llu total\n", (unsigned long long)p.nativeCalls,
                    (unsigned long long)p.interpCalls, (unsigned long long)p.interpSteps, (unsigned long long)p.cpu.instructions);
        FILE* hf = std::fopen("analysis\\interp_hist.txt", "w");
        if (hf) { for (auto& [pc, n] : p.interpHist) std::fprintf(hf, "%08x %llu\n", pc, (unsigned long long)n); std::fclose(hf); }
    }

    std::printf("\nBIOS calls:\n");
    for (auto& [k, n] : p.biosCalls)
        std::printf("  %02x(%02x) %-26s x%llu%s\n", k >> 8, k & 0xFF, psx::Platform::biosName(k >> 8, k & 0xFF), (unsigned long long)n,
                    p.unknownBios.count(k) ? "   [NOT IMPLEMENTED]" : "");
    for (auto& [k, c] : p.unknownFirstCall)
        std::printf("  unimplemented %02x(%02x): first call ra=%08x a0=%08x a1=%08x a2=%08x\n", k >> 8, k & 0xFF, c[0], c[1], c[2], c[3]);
    std::printf("\nirqs raised:");
    for (int i = 0; i < 11; ++i) if (p.irqCount[i]) std::printf(" bit%d:x%llu", i, (unsigned long long)p.irqCount[i]);
    std::printf("  I_STAT=%x I_MASK=%x\n", p.istat, p.imask);
    p.printTimers();
    std::printf("\nCD commands:");
    for (int i = 0; i < 0x20; ++i) if (p.cd.commandsSeen[i]) std::printf(" %02x:x%llu", i, (unsigned long long)p.cd.commandsSeen[i]);
    std::printf("\n\nHardware registers touched (reads/writes):\n");
    std::map<uint32_t, std::pair<uint64_t, uint64_t>> io;
    for (auto& [a, n] : p.ioReads) io[a].first = n;
    for (auto& [a, n] : p.ioWrites) io[a].second = n;
    for (auto& [a, rw] : io) {
        if (a >= 0x1F801C00 && a < 0x1F802000 && rw.first < 1000 && rw.second < 1000) continue;
        std::printf("  %08x r=%llu w=%llu\n", a, (unsigned long long)rw.first, (unsigned long long)rw.second);
    }
    std::vector<std::pair<uint64_t, uint32_t>> hot;
    for (auto& [pc, n] : hist) hot.push_back({n, pc});
    std::sort(hot.rbegin(), hot.rend());
    std::printf("\nHottest sampled pcs:");
    for (size_t i = 0; i < hot.size() && i < 8; ++i) std::printf(" %08x(%llu)", hot[i].second, (unsigned long long)hot[i].first);
    std::printf("\n");

    if (!ramOut.empty()) {
        FILE* f = std::fopen(ramOut.c_str(), "wb");
        if (f) { std::fwrite(p.ram.data(), 1, p.ram.size(), f); std::fclose(f); }
    }
    for (uint32_t a : peeks) {
        std::printf("peek %08x:", a);
        for (uint32_t i = 0; i < 8; ++i) std::printf(" %08x", p.cpu.rd32(a + 4 * i));
        std::printf("\n");
    }
    if (!wavOut.empty()) {
        FILE* f = std::fopen(wavOut.c_str(), "wb");
        if (f) {
            const uint32_t bytes = uint32_t(audioCapture.size() * 2), rate = 44100;
            const uint16_t ch = 2, bits = 16, align = 4, fmt = 1;
            const uint32_t riff = 36 + bytes, fmtLen = 16, byteRate = rate * 4;
            std::fwrite("RIFF", 1, 4, f); std::fwrite(&riff, 4, 1, f); std::fwrite("WAVEfmt ", 1, 8, f);
            std::fwrite(&fmtLen, 4, 1, f); std::fwrite(&fmt, 2, 1, f); std::fwrite(&ch, 2, 1, f); std::fwrite(&rate, 4, 1, f);
            std::fwrite(&byteRate, 4, 1, f); std::fwrite(&align, 2, 1, f); std::fwrite(&bits, 2, 1, f);
            std::fwrite("data", 1, 4, f); std::fwrite(&bytes, 4, 1, f);
            std::fwrite(audioCapture.data(), 2, audioCapture.size(), f);
            std::fclose(f);
        }
        int peak = 0; uint64_t nonzero = 0;
        for (int16_t s : audioCapture) { peak = std::max(peak, std::abs(int(s))); nonzero += s != 0; }
        std::printf("audio: %zu frames, peak %d, %llu non-zero samples\n", audioCapture.size() / 2, peak, (unsigned long long)nonzero);
    }
    if (!displayOut.empty()) {
        FILE* f = std::fopen(displayOut.c_str(), "wb");
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
    if (!vramOut.empty()) {
        FILE* f = std::fopen(vramOut.c_str(), "wb");
        if (f) {
            std::fprintf(f, "P6\n1024 512\n255\n");
            for (uint16_t px : p.gpu.vram) {
                const uint8_t c[3] = {uint8_t((px & 31) << 3), uint8_t(((px >> 5) & 31) << 3), uint8_t(((px >> 10) & 31) << 3)};
                std::fwrite(c, 1, 3, f);
            }
            std::fclose(f);
        }
    }
    return stop.empty() ? 0 : 1;
}

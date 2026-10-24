// Benchmark for the virtual disc: reads whole level files sector by sector (the way the game's loader does) and a random scatter of
// sectors, and reports the time. Usage: cb2_discbench <game folder> [file=S0\S000000E.NSF]
#include <chrono>
#include <cstring>
#include <fstream>
#include <vector>
#include <cstdio>
#include <cstdlib>
#include <algorithm>
#include <iterator>
#include <random>
#include <string>

#include "disc.hpp"

int main(int argc, char** argv) {
    if (argc < 2) { std::fprintf(stderr, "usage: cb2_discbench <game folder> [file]\n"); return 2; }
    const std::string name = argc > 2 ? argv[2] : "S0\\S000000E.NSF";
    psx::VirtualDisc disc;
    if (!disc.build(argv[1])) { std::fprintf(stderr, "cannot build the disc\n"); return 2; }
    const uint32_t first = disc.fileLba(name);
    if (!first) { std::fprintf(stderr, "%s not on the disc\n", name.c_str()); return 2; }
    uint32_t last = first;
    uint8_t raw[2352];
    while (disc.readSector(last + 1, raw) && disc.fileAt(last + 1) == disc.fileAt(first)) ++last;
    const uint32_t sectors = last - first + 1;
    using clock = std::chrono::steady_clock;
    auto ms = [](clock::time_point a) { return std::chrono::duration<double, std::milli>(clock::now() - a).count(); };

    uint64_t sum = 0;
    auto t0 = clock::now();
    for (uint32_t s = first; s <= last; ++s) { disc.readSector(s, raw); sum += raw[24] + raw[2000]; }
    const double seq = ms(t0);
    std::printf("%s: %u sectors (%.1f MB)\n", name.c_str(), sectors, sectors * 2048.0 / 1048576.0);
    std::printf("  sequential: %.1f ms  (%.1f MB/s, %.1f us per sector)\n", seq, sectors * 2048.0 / 1048576.0 / (seq / 1000.0), seq * 1000.0 / sectors);

    std::mt19937 rng{12345};
    const int n = 4000;
    t0 = clock::now();
    for (int i = 0; i < n; ++i) { disc.readSector(first + rng() % sectors, raw); sum += raw[24]; }
    const double rnd = ms(t0);
    std::printf("  random:     %.1f ms for %d reads (%.1f us per sector)\n", rnd, n, rnd * 1000.0 / n);

    // the pattern of the engine: bursts of 32 sectors (a 64 KB page) at scattered places
    t0 = clock::now();
    for (int i = 0; i < 40; ++i) { const uint32_t at = first + rng() % (sectors - 32); for (uint32_t s = 0; s < 32; ++s) { disc.readSector(at + s, raw); sum += raw[24]; } }
    const double page = ms(t0);
    std::printf("  40 page loads of 64 KB: %.1f ms (%.2f ms per page)\n", page, page / 40.0);
    // every byte of the file, read through the disc, must equal the file itself (the last sector is zero padded); also a scrambled order
    {
        const std::string host = disc.fileAt(first);
        std::ifstream f(host, std::ios::binary);
        std::vector<uint8_t> direct((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        std::vector<uint32_t> order(sectors);
        for (uint32_t i = 0; i < sectors; ++i) order[i] = i;
        std::shuffle(order.begin(), order.end(), rng);
        size_t bad = 0;
        for (uint32_t s : order) {
            disc.readSector(first + s, raw);
            uint8_t expect[2048] = {};
            const size_t at = size_t(s) * 2048;
            if (at < direct.size()) std::memcpy(expect, direct.data() + at, std::min<size_t>(2048, direct.size() - at));
            if (std::memcmp(raw + 24, expect, 2048) != 0) ++bad;
        }
        std::printf("  verified against the file (%zu bytes, shuffled order): %s\n", direct.size(), bad ? "MISMATCH" : "identical");
        if (bad) { std::printf("  %zu sectors differ\n", bad); return 1; }
    }
    std::printf("  checksum %llu\n", (unsigned long long)sum);
    return 0;
}

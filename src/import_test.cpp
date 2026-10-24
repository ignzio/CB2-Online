// Test for the disc image importer: builds disc images from a game folder in the layouts real images use (raw 2352 mode 2, raw 2352
// mode 1, plain 2048 ISO, with and without a .cue), imports each into a temporary folder and compares every file.
// Usage: cb2_importtest <game folder> <work folder>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>

#include "discimport.hpp"
#include "disc.hpp"

namespace fs = std::filesystem;

static std::vector<char> slurp(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    return std::vector<char>((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

static int compareTrees(const fs::path& gameDir, const fs::path& outDir, int expectFiles) {
    int bad = 0, count = 0;
    for (const auto& e : fs::recursive_directory_iterator(outDir)) {
        if (!e.is_regular_file()) continue;
        ++count;
        const fs::path rel = fs::relative(e.path(), outDir);
        const fs::path src = gameDir / rel;
        if (!fs::exists(src) || slurp(src) != slurp(e.path())) { std::printf("  MISMATCH %s\n", rel.string().c_str()); ++bad; }
        if (e.path().extension() == ".part") { std::printf("  LEFTOVER %s\n", rel.string().c_str()); ++bad; }
    }
    if (count != expectFiles) { std::printf("  expected %d files, found %d\n", expectFiles, count); ++bad; }
    return bad;
}

int main(int argc, char** argv) {
    if (argc < 3) { std::fprintf(stderr, "usage: cb2_importtest <game folder> <work folder>\n"); return 2; }
    const fs::path game = argv[1], work = argv[2];
    fs::create_directories(work);
    psx::VirtualDisc disc;
    if (!disc.build(game.string())) { std::fprintf(stderr, "cannot build the disc from %s\n", argv[1]); return 2; }
    int expect = 0;
    for (const auto& e : fs::recursive_directory_iterator(game)) {
        if (!e.is_regular_file()) continue;
        const std::string rel = fs::relative(e.path(), game).string();
        if (rel == "SCUS_941.54" || rel == "SYSTEM.CNF" || (rel.size() > 3 && rel[0] == 'S' && rel[1] >= '0' && rel[1] <= '3' && rel[2] == '\\')) ++expect;
    }

    struct Variant { const char* name; uint32_t stride; uint32_t offset; bool mode1; const char* ext; bool cue; };
    const Variant variants[] = {
        {"raw2352-mode2+cue", 2352, 24, false, ".bin", true},
        {"raw2352-mode1+cue", 2352, 16, true, ".bin", true},
        {"iso2048", 2048, 0, false, ".iso", false},
    };
    int failures = 0;
    for (const Variant& v : variants) {
        const fs::path dir = work / v.name;
        fs::remove_all(dir);
        fs::create_directories(dir);
        const fs::path image = dir / (std::string("Crash Bandicoot 2 (USA)") + v.ext);
        {
            std::ofstream out(image, std::ios::binary);
            uint8_t raw[2352];
            for (uint32_t lba = 0; disc.readSector(lba, raw); ++lba) {
                if (v.stride == 2048) { out.write(reinterpret_cast<char*>(raw) + 24, 2048); continue; }
                if (v.mode1) {
                    uint8_t s[2352] = {};
                    std::memcpy(s, raw, 12);       // sync
                    std::memcpy(s + 12, raw + 12, 3);
                    s[15] = 1;
                    std::memcpy(s + 16, raw + 24, 2048);
                    out.write(reinterpret_cast<char*>(s), 2352);
                } else out.write(reinterpret_cast<char*>(raw), 2352);
            }
        }
        fs::path open = image;
        if (v.cue) {
            open = dir / "Crash Bandicoot 2 (USA).cue";
            std::ofstream cue(open);
            cue << "FILE \"Crash Bandicoot 2 (USA).bin\" BINARY\r\n  TRACK 01 MODE2/2352\r\n    INDEX 01 00:00:00\r\n";
        }
        const fs::path outDir = dir / "data";
        uint64_t last = 0;
        const cb2import::Result r = cb2import::importDisc(open.string(), outDir.string(), [&](uint64_t d, uint64_t) { last = d; });
        std::printf("%s: %s, %d files, %llu bytes\n", v.name, r.ok ? "imported" : ("FAILED: " + r.error).c_str(), r.files, (unsigned long long)r.bytes);
        if (!r.ok) { ++failures; continue; }
        if (last != r.bytes) { std::printf("  progress ended at %llu\n", (unsigned long long)last); ++failures; }
        failures += compareTrees(game, outDir, expect) ? 1 : 0;
    }

    // things that must be refused
    {
        const fs::path junk = work / "junk.bin";
        { std::ofstream o(junk, std::ios::binary); std::vector<char> z(2352 * 40, 7); o.write(z.data(), std::streamsize(z.size())); }
        const cb2import::Result r = cb2import::importDisc(junk.string(), (work / "junkout").string());
        std::printf("junk image: %s (%s)\n", r.ok ? "ACCEPTED (bad)" : "refused", r.error.c_str());
        if (r.ok) ++failures;
        const cb2import::Result m = cb2import::importDisc((work / "nothere.cue").string(), (work / "junkout").string());
        std::printf("missing cue: %s (%s)\n", m.ok ? "ACCEPTED (bad)" : "refused", m.error.c_str());
        if (m.ok) ++failures;
        const fs::path trunc = work / "trunc.iso";
        {
            std::ifstream in(work / "iso2048" / "Crash Bandicoot 2 (USA).iso", std::ios::binary);
            std::vector<char> part(60u << 20);
            in.read(part.data(), std::streamsize(part.size()));
            std::ofstream o(trunc, std::ios::binary);
            o.write(part.data(), in.gcount());
        }
        const cb2import::Result t = cb2import::importDisc(trunc.string(), (work / "truncout").string());
        std::printf("truncated image: %s (%s)\n", t.ok ? "ACCEPTED (bad)" : "refused", t.error.c_str());
        if (t.ok) ++failures;
    }
    std::printf(failures ? "FAILED: %d\n" : "all passed\n", failures);
    return failures ? 1 : 0;
}

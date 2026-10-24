// cb2_pack: builds a single, self-contained game executable. The game data (SCUS_941.54, SYSTEM.CNF, S0..S3) is appended to a copy
// of the player, which then reads its own file as the CD. The result contains your game files: keep it for yourself.
//   cb2_pack <player.exe> <game folder> <out.exe>
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

#include "bundle.hpp"

namespace fs = std::filesystem;

int main(int argc, char** argv) {
    if (argc < 4) { std::fprintf(stderr, "usage: cb2_pack <player.exe> <game folder> <out.exe>\n"); return 2; }
    const fs::path stub = argv[1], game = argv[2], out = argv[3];
    if (!fs::exists(stub)) { std::fprintf(stderr, "no such file: %s\n", stub.string().c_str()); return 2; }
    if (!fs::exists(game / "SCUS_941.54")) { std::fprintf(stderr, "SCUS_941.54 not found in %s\n", game.string().c_str()); return 2; }
    auto upper = [](std::string s) { for (auto& c : s) c = char(std::toupper(static_cast<unsigned char>(c))); return s; };

    std::vector<std::pair<std::string, std::string>> files;
    for (const char* name : {"SCUS_941.54", "SYSTEM.CNF"})
        if (fs::exists(game / name)) files.push_back({upper(name), (game / name).string()});
    for (const char* dir : {"S0", "S1", "S2", "S3"}) {
        if (!fs::is_directory(game / dir)) { std::fprintf(stderr, "warning: folder %s is missing\n", dir); continue; }
        for (auto& e : fs::recursive_directory_iterator(game / dir))
            if (e.is_regular_file()) files.push_back({upper(fs::relative(e.path(), game).string()), e.path().string()});
    }
    std::sort(files.begin(), files.end());
    uint64_t bytes = 0;
    for (auto& f : files) bytes += fs::file_size(f.second);
    std::printf("embedding %zu files, %.1f MB\n", files.size(), double(bytes) / 1048576.0);

    const std::string err = psx::writeBundle(stub.string(), out.string(), files);
    if (!err.empty()) { std::fprintf(stderr, "error: %s\n", err.c_str()); return 1; }
    std::printf("wrote %s (%.1f MB)\n", out.string().c_str(), double(fs::file_size(out)) / 1048576.0);
    return 0;
}

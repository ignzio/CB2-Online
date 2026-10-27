#include "config.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstdint>
#include <filesystem>
#include <fstream>

namespace cb2 {
namespace {

std::string trim(const std::string& s) {
    const size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return {};
    return s.substr(a, s.find_last_not_of(" \t\r\n") - a + 1);
}

}  // namespace

bool Config::load(const std::string& exeDir) {
    namespace fs = std::filesystem;
    for (const fs::path& p : {fs::path("cb2.cfg"), fs::path(exeDir) / "cb2.cfg"}) {
        std::ifstream in(p);
        if (!in) continue;
        for (std::string line; std::getline(in, line);) {
            const size_t hash = line.find('#');
            if (hash != std::string::npos) line.resize(hash);
            const size_t eq = line.find('=');
            if (eq == std::string::npos) continue;
            const std::string k = trim(line.substr(0, eq)), v = trim(line.substr(eq + 1));
            if (k == "game") game = v;
            else if (k == "game_pal") gamePal = v;
            else if (k == "region") { region = v; for (char& c : region) c = char(std::tolower(static_cast<unsigned char>(c))); }
            else if (k == "mod" && !v.empty()) mods.push_back(v);
            else if (k == "card") card = v;
            else if (k == "scale") scale = std::atoi(v.c_str());
            else if (k == "ram_mb") ramMb = std::atoi(v.c_str());
            else if (k == "volume") volume = std::max(0, std::min(100, std::atoi(v.c_str())));
            else if (k == "filter") filter = std::max(0, std::min(2, std::atoi(v.c_str())));
            else if (k == "opacity") opacity = std::max(10, std::min(100, std::atoi(v.c_str())));
            else if (k == "timer") timer = !(v == "off" || v == "0" || v == "no" || v == "false");
            else if (k == "textures") textures = (v == "smooth" || v == "1") ? 1 : 0;
            else if (k == "smooth") smooth = (v == "on" || v == "1" || v == "yes" || v == "true");
            else if (k == "disc_speed") discSpeed = std::max(1, std::min(24, std::atoi(v.c_str())));
            else if (k == "reverb") reverb = !(v == "off" || v == "0" || v == "no" || v == "false");
            else if (k == "refresh_rate") refreshRate = std::max(0, std::min(1000, std::atoi(v.c_str())));
            else if (k == "perspective") perspective = (v == "on" || v == "1" || v == "yes" || v == "true");
            else if (k == "native_world") nativeWorld = (v == "on" || v == "1" || v == "yes" || v == "true");
            else if (k == "widescreen") widescreen = (v == "on" || v == "1" || v == "yes" || v == "true");
            else if (k == "vsync") vsync = !(v == "off" || v == "0" || v == "no" || v == "false");
            else if (k == "roster") roster = !(v == "off" || v == "0" || v == "no" || v == "false");
            else if (k.rfind("pad_", 0) == 0) padNames[k.substr(4)] = v;
            else if (k == "nametags") nameTags = !(v == "off" || v == "0" || v == "no" || v == "false");
            else if (k.rfind("key_", 0) == 0) keyNames[k.substr(4)] = v;
            else if (k == "name") name = v;
            else if (k == "room") room = v;
            else if (k == "rendezvous") rendezvous = v;
            else if (k == "directory") directory = v;
            else if (k == "update") update = !(v == "off" || v == "0" || v == "no" || v == "false");
            else if (k == "lobby") lobby = !(v == "off" || v == "0" || v == "no" || v == "false");
        }
        loadedFrom = p.string();
        return true;
    }
    return false;
}

bool Config::save(const std::string& path) const {
    std::ofstream out(path);
    if (!out) return false;
    out << "# Crash Bandicoot 2 native port settings (see src/config.hpp)\n";
    out << "game = " << game << "\n";
    for (const auto& m : mods) out << "mod = " << m << "\n";
    out << "card = " << card << "\nscale = " << scale << "\nram_mb = " << ramMb << "\n";
    return bool(out);
}

bool Config::remember(const std::string& exeDir, const std::string& key, const std::string& value) const {
    namespace fs = std::filesystem;
    const fs::path path = loadedFrom.empty() ? fs::path(exeDir) / "cb2.cfg" : fs::path(loadedFrom);
    std::vector<std::string> lines;
    {
        std::ifstream in(path);
        for (std::string line; std::getline(in, line);) lines.push_back(line);
    }
    auto keyOf = [&](const std::string& line, bool commented) {
        std::string s = trim(line);
        if (commented) { if (s.empty() || s[0] != '#') return false; s = trim(s.substr(1)); }
        return s.compare(0, key.size(), key) == 0 && trim(s.substr(key.size())).rfind('=', 0) == 0;
    };
    bool done = false;
    for (std::string& line : lines) if (keyOf(line, false)) { line = key + " = " + value; done = true; break; }
    if (!done) for (std::string& line : lines) if (keyOf(line, true)) { line = key + " = " + value; done = true; break; }
    if (!done) lines.push_back(key + " = " + value);
    std::ofstream out(path);
    if (!out) return false;
    for (const std::string& line : lines) out << line << "\n";
    return bool(out);
}

void Config::resolvePaths(const std::string& exeDir) {
    namespace fs = std::filesystem;
    const fs::path exe(exeDir);
    auto holds = [](const fs::path& p, const cb2game::Profile& g) { std::error_code ec; return fs::exists(p / g.exeName, ec); };
    auto holdsAny = [&](const fs::path& p) { return holds(p, cb2game::ntsc()) || holds(p, cb2game::pal()); };
    // `game`: as written, or next to the program, or <exe>\data (the layout of the packaged game folder)
    if (game.empty()) {
        if (holdsAny(exe / "data")) game = (exe / "data").string();
        else if (holdsAny(fs::path("data"))) game = "data";
    } else if (fs::path(game).is_relative() && !holdsAny(game) && holdsAny(exe / game)) {
        game = (exe / game).string();
    }
    if (gameFromCommandLine) gamePal.clear();   // --game <folder> means exactly that folder, whichever release it holds
    else if (gamePal.empty()) {
        if (holdsAny(exe / "data-pal")) gamePal = (exe / "data-pal").string();
        else if (holdsAny(fs::path("data-pal"))) gamePal = "data-pal";
    } else if (fs::path(gamePal).is_relative() && !holdsAny(gamePal) && holdsAny(exe / gamePal)) {
        gamePal = (exe / gamePal).string();
    }
    for (std::string& m : mods)
        if (fs::path(m).is_relative() && !fs::exists(m) && fs::exists(exe / m)) m = (exe / m).string();

    // what is installed: a folder holds the release whose executable is in it (`game` may hold either; `game_pal` is meant for PAL)
    haveNtsc = havePal = false;
    ntscFolder.clear();
    palFolder.clear();
    for (const std::string& f : {game, gamePal}) {
        if (f.empty()) continue;
        if (!haveNtsc && holds(f, cb2game::ntsc())) { haveNtsc = true; ntscFolder = f; }
        if (!havePal && holds(f, cb2game::pal())) { havePal = true; palFolder = f; }
    }
    const bool wantPal = region == "pal" || (region != "ntsc" && havePal);
    profile = nullptr;
    if (wantPal && havePal) profile = &cb2game::pal();
    else if (haveNtsc) profile = &cb2game::ntsc();
    else if (havePal) profile = &cb2game::pal();
    if (profile) game = profile->region == cb2game::Region::Pal ? palFolder : ntscFolder;
}

std::string Config::validate() const {
    static const char* kHelp = "\n\nThis program does not include the game. Copy the files of your own Crash Bandicoot 2 disc (SCUS_941.54 for the American release, or SCES_009.67 for the European one, "
                               "SYSTEM.CNF and the S0, S1, S2, S3 folders) into the 'data' (American) or 'data-pal' (European) folder next to Crash2.exe, or start the game and choose the disc image; then start it again.";
    if (!profile) return "No game files found (looked for SCUS_941.54 and SCES_009.67)." + std::string(kHelp);
    return {};
}

}  // namespace cb2
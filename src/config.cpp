#include "config.hpp"

#include <algorithm>
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
    auto holdsGame = [](const fs::path& p) { return fs::exists(p / "SCUS_941.54"); };
    if (game.empty()) {
        if (holdsGame(exe / "data")) game = (exe / "data").string();
        else if (holdsGame(fs::path("data"))) game = "data";
    } else if (fs::path(game).is_relative() && !holdsGame(game) && holdsGame(exe / game)) {
        game = (exe / game).string();
    }
    for (std::string& m : mods)
        if (fs::path(m).is_relative() && !fs::exists(m) && fs::exists(exe / m)) m = (exe / m).string();
}

std::string Config::validate() const {
    namespace fs = std::filesystem;
    static const char* kHelp = "\n\nThis program does not include the game. Copy the files of your own Crash Bandicoot 2 disc "
                               "(SCUS_941.54, SYSTEM.CNF and the S0, S1, S2, S3 folders) into the 'data' folder next to Crash2.exe, then start it again.";
    if (game.empty()) return std::string("Game files not found.") + kHelp;
    if (!fs::exists(fs::path(game) / "SCUS_941.54")) return "SCUS_941.54 not found in " + game + kHelp;
    return {};
}

}  // namespace cb2

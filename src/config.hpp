#pragma once
// Settings file (cb2.cfg): where the user's game data lives and how to run. Lines are `key = value`, `#` starts a comment.
//   game    = D:\Games\Crash2          folder with SCUS_941.54 and the S0..S3 level folders (read-only)
//   mod     = mods\myhack              overlay folder, may be repeated; later ones win
//   card    = saves\memcard0.mcr       memory card image
//   scale   = 4                        internal resolution multiplier of the OpenGL renderer
//   ram_mb  = 8                        engine heap size (2..16)
//   name    = Player                   name shown above your Crash in online play (cb2_play / Crash2.exe)
//   room    = PUBLIC                   online room: players who type the same name meet
//   rendezvous = my.server.com:7777    rendezvous server (cb2_server) that introduces players to each other; empty = this PC / LAN only
//   directory = https://cb2-lobby.example.workers.dev   lobby service (docs/LOBBY_PROTOCOL.md): lists open servers and rooms; built in, this only overrides it
//   lobby   = off                      do not show the online lobby before the game
// Command-line options override the file. The file is searched in the current folder, then next to the executable.
#include <string>
#include <cstdint>
#include <vector>

namespace cb2 {

struct Config {
    std::string game;
    std::vector<std::string> mods;
    std::string card = "memcard0.mcr";
    int scale = 4;
    int ramMb = 2;
    bool lobby = true;         // show the online lobby before the game starts (lobby = off disables it)
#ifndef CB2_DEFAULT_DIRECTORY
#define CB2_DEFAULT_DIRECTORY ""
#endif
#ifndef CB2_DEFAULT_RENDEZVOUS
#define CB2_DEFAULT_RENDEZVOUS ""
#endif
    // online play: player name (default: the Windows user name), room, the optional relay server and the lobby service. The last two come
    // built into the game (see CMakeLists.txt) and only need to be set to override them: `directory =` (empty) turns the lobby service off.
    std::string name, room = "PUBLIC", rendezvous = CB2_DEFAULT_RENDEZVOUS, directory = CB2_DEFAULT_DIRECTORY;   // online play: player name (default: the Windows user name), room, rendezvous server
    std::string loadedFrom;   // empty when no file was found

    // Returns true when a file was read. `exeDir` is searched after the working directory.
    bool load(const std::string& exeDir);
    bool save(const std::string& path) const;
    // Checks that the game folder holds the executable; returns an error text or an empty string.
    std::string validate() const;
    // Finds the game folder: a relative `game` is also tried next to the executable, and an empty one falls back to
    // <exeDir>\data (the layout of the packaged game folder).
    void resolvePaths(const std::string& exeDir);
    // Writes one setting into the file it was loaded from (or `exeDir`\\cb2.cfg), keeping the rest of the file as it is.
    bool remember(const std::string& exeDir, const std::string& key, const std::string& value) const;
};

}  // namespace cb2

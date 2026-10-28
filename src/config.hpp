#pragma once
// Settings file (cb2.cfg): where the user's game data lives and how to run. Lines are `key = value`, `#` starts a comment.
//   game    = D:\Games\Crash2          folder with SCUS_941.54 (NTSC-U) and the S0..S3 level folders (read-only); default: data
//   game_pal = D:\Games\Crash2-pal     the same for the European (PAL) release: folder with SCES_009.67 and S0..S3; default: data-pal
//   region  = auto                     which release to play when both are installed: auto (PAL first), pal or ntsc. Online rooms are per release (a PAL and an NTSC player never share one)
//   mod     = mods\myhack              overlay folder, may be repeated; later ones win
//   card    = saves\memcard0.mcr       memory card image
//   scale   = 4                        internal resolution multiplier of the OpenGL renderer
//   ram_mb  = 8                        engine heap size (2..16)
//   volume  = 100                      0..100 (in-game menu: Esc)
//   filter  = 2                        0 nearest, 1 bilinear, 2 sharp bilinear
//   nametags = on                      names over the other players
//   pad_cross = A                      game pad bindings, same ids; buttons A B X Y LB RB BACK START GUIDE LEFT STICK RIGHT STICK DPAD UP/RIGHT/DOWN/LEFT, triggers LT RT, NONE
//   opacity = 100                      how solid the other players' Crashes are (10..100)
//   timer   = on                       the level timer in the corner (F4 toggles it)
//   smooth  = off                      60 fps: draw the picture between two frames of the 30 fps game (Esc menu: Sound and picture)
//   textures = sharp                   sharp: texels as the console shows them; smooth: bilinear texture filtering (Esc menu: Sound and picture)
//   disc_speed = 16                    how many times faster than the console's drive the game's disc reads are (1 = authentic, loads take seconds; max 24)
//   reverb  = on                       the console's sound reverb (off: dry sound)
//   refresh_rate = 0                   your monitor's refresh rate for 60 fps smoothing (0 = ask the monitor). From about 90 Hz up, every refresh shows a picture
//                                      interpolated for that exact moment (160 Hz monitors: 160 distinct pictures a second, no judder); the cost is one more frame of delay
//   perspective = off                  perspective-correct texturing: textures stop swimming on floors and walls (uses the GTE depth, like DuckStation's PGXP)
//   skip_intro = on                    a new game starts in the warp room (the intro and the title are skipped); only while the memory card holds no save
//   props = off                        things that are not in the game but live in it: a bouncy crate in the warp room; experimental
//   native_world = off                 draw the level scenery with OpenGL from the level files (true widescreen, camera-exact in-between pictures) instead of the game's own polygons; experimental
//   widescreen = off                   show the picture at the window's aspect ratio (16:9 ...) instead of 4:3: 3D is squeezed and stretched back, the HUD keeps its shape
//   vsync   = on                       wait for the monitor's refresh when showing a picture (no tearing)
//   roster  = on                       the list of players at the right edge (F3 toggles it in game)
//   key_cross = Z                      keyboard bindings: key_up/down/left/right/cross/square/circle/triangle/l1/r1/l2/r2/start/select
//   name    = Player                   name shown above your Crash in online play (cb2_play / Crash2.exe)
//   room    = PUBLIC                   online room: players who type the same name meet
//   rendezvous = my.server.com:7777    rendezvous server (cb2_server) that introduces players to each other; empty = this PC / LAN only
//   directory = https://cb2-lobby.example.workers.dev   lobby service (docs/LOBBY_PROTOCOL.md): lists open servers and rooms; built in, this only overrides it
//   lobby   = off                      do not show the online lobby before the game
//   update  = off                      do not look for a newer release on GitHub when the lobby opens
// Command-line options override the file. The file is searched in the current folder, then next to the executable.
#include <map>
#include <string>
#include <cstdint>
#include <vector>

#include "game_profile.hpp"

namespace cb2 {

struct Config {
    std::string game, gamePal;   // the game folders of the NTSC-U and the PAL release; after resolvePaths(): `game` is the folder that will be played
    std::string region = "auto";   // auto / pal / ntsc
    bool gameFromCommandLine = false;   // --game was given: only that folder is used
    const cb2game::Profile* profile = nullptr;   // the release that resolvePaths() chose (nullptr: none installed)
    bool havePal = false, haveNtsc = false;     // which releases are installed (after resolvePaths)
    std::string ntscFolder, palFolder;           // where they are
    std::vector<std::string> mods;
    std::string card = "memcard0.mcr";
    int scale = 4;
    int ramMb = 2;
    // Set in the in-game menu (Esc) and saved here: volume 0..100, display filter 0 nearest / 1 bilinear / 2 sharp bilinear, name tags over
    // the other players, and the keyboard bindings (`key_cross = Z` ... see src/keymap.hpp; id -> key name).
    int volume = 100, filter = 2;
    bool nameTags = true;
    std::map<std::string, std::string> keyNames, padNames;   // `key_cross = Z`, `pad_cross = A` (id -> name)
    int opacity = 100;         // how solid the other players' Crashes are drawn, 10..100 percent
    bool roster = true;        // the player list at the right edge (online)
    bool timer = true;         // the level timer in the corner
    bool smooth = false;       // draw an in-between picture every other vertical blank (60 fps from the 30 fps game), see interp.hpp
    int textures = 0;          // 0 sharp (the console's own look), 1 smooth (bilinear)
    int discSpeed = 16;        // emulated disc drive speed-up: area and level loads take a fraction of a second (1 = as slow as the console)
    bool reverb = true;        // SPU reverb in the mix
    int refreshRate = 0;       // 0 = the monitor's own
    bool perspective = false;  // perspective-correct texturing
    bool nativeWorld = false;  // native world renderer
    bool skipIntro = true;     // start in the warp room instead of playing the intro (not when the card has a save)
    bool props = false;        // props: a bouncy crate in the warp room (experimental)
    bool widescreen = false;   // wide picture
    bool vsync = true;         // swap with the monitor's refresh
    bool update = true;        // look for a newer release (see updater.hpp); update = off disables it
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
    // Checks that a game folder with an executable was found; returns an error text or an empty string.
    std::string validate() const;
    // Finds the game folder: a relative `game` is also tried next to the executable, and an empty one falls back to
    // <exeDir>\data (the layout of the packaged game folder).
    void resolvePaths(const std::string& exeDir);
    // Writes one setting into the file it was loaded from (or `exeDir`\\cb2.cfg), keeping the rest of the file as it is.
    bool remember(const std::string& exeDir, const std::string& key, const std::string& value) const;
};

}  // namespace cb2

#pragma once
// First-run setup inside the game's own window (no Windows file dialog): the player says which release of Crash Bandicoot 2 they own (European PAL or American NTSC-U), browses to
// the disc image (.cue/.bin/.iso) in the usual Windows file window (opened on top of the game's window) - or drops the file on the window - and the files the game needs are copied out of it with a progress bar.
// Also started by `Crash2.exe --setup` to add the other release later.
#include <string>
#include <vector>

#include "game_profile.hpp"

namespace cb2setup {

// Scripted input for tests (no Windows file window is opened then): up, down, enter, esc, wait:<frames>, shot, drop:<file> (= choosing that file), exit.
struct Script {
    std::vector<std::string> tokens;
    std::string shotPrefix;
};

struct Result {
    bool imported = false;
    const cb2game::Profile* profile = nullptr;   // the release that was copied
    std::string folder;                          // where
};

// Opens its own window (and closes it again). `firstRun`: nothing is installed yet, so Esc leaves the program instead of going back. `hidden`: tests.
Result run(const std::string& exeDir, bool firstRun, bool hidden, const Script& script);

}  // namespace cb2setup

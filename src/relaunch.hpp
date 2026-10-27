// Starts a fresh copy of the game at the lobby (the pause menu's "Main menu"). The emulated console cannot be rewound to
// its boot screen, so the new process takes over and the old one exits.
#pragma once

namespace cb2 {

// Keeps the game/mod/card options of this run, drops the options that would skip the lobby or only exist for tests, adds --lobby.
// Returns false when the new process could not be started.
// `extraArg` (optional) is added to the new process's command line (for example "--setup").
bool relaunchToLobby(int argc, char** argv, const char* extraArg = nullptr);

}   // namespace cb2

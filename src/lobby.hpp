#pragma once
// The screen shown before the game starts: play alone, host a game, or join a host or a dedicated server (cb2_server). Once hosting or
// joined, a waiting room lists the players who are already there; Enter starts the game. Everybody starts their own copy of the game
// from there (there is no shared start: each player chooses New Game or Load Game as usual, and meets the others in the same level).
#include <string>
#include <vector>

struct GLFWwindow;

namespace cb2mp { class Multiplayer; struct Options; }
namespace cb2update { class Updater; }

namespace cb2lobby {

enum class Outcome {
    Offline,   // play alone
    Online,    // the Multiplayer object has been started (hosting or joined); the caller installs its hooks
    Quit,
    Update,    // the new version has been downloaded: the caller installs it and restarts
};

// Scripted input for automated tests (--lobby-script "down,enter,wait:60,shot"): up, down, enter, esc, back (backspace),
// text:abc, wait:<frames>, shot (save a picture as <shotPrefix>_<n>.png).
struct Script {
    std::vector<std::string> tokens;
    std::string shotPrefix;
};

// `options` supplies the starting name, server and port; the name and server typed here are written back into it.
// `updater` (may be null) is checking for a newer release; when it finds one the main screen gets an UPDATE row. `version` is shown in the corner.
Outcome run(GLFWwindow* window, cb2mp::Multiplayer& mp, cb2mp::Options& options, const Script& script, cb2update::Updater* updater, const std::string& version);

}  // namespace cb2lobby

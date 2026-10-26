#pragma once
// Crash and bug reports. A crash writes crash.log next to the executable (what failed, where, the game's own state at that moment); a session that ended without a clean exit
// (the process was killed, the graphics driver reset, anything that gives no exception) leaves a lock file that the next start finds. Either way the next start offers to open a
// prefilled GitHub issue, and Esc -> Report a bug does the same at any time. Nothing is sent anywhere: the browser opens a page the player submits themselves.
#include <atomic>
#include <cstdint>
#include <string>

namespace cb2crash {

// What the game was doing, kept up to date by the main loop (plain fields: it is read from the crash handler).
struct Live {
    std::atomic<uint64_t> frame{0};
    std::atomic<uint32_t> level{0};
    std::atomic<uint32_t> guestPc{0};
    char phase[48] = "starting";    // "lobby", "playing", "menu", "loading"
    char settings[256] = "";       // the options in force, one line
    char renderer[160] = "";       // OpenGL renderer string
    char room[64] = "";            // online room, or empty
};
extern Live live;

// What the game is doing now (shown in reports).
void setPhase(const char* phase);
// Installs the handlers (exception filter, terminate, abort, invalid parameter). `dir` is where crash.log is written.
void install(const std::string& dir, const std::string& version);
// Marks the session as running (lock file); call only for sessions a player is playing.
void sessionStart();
// Refreshes the lock file with the game's current state (at most every two seconds), so a session that vanishes leaves a hint of where it was.
void heartbeat();
// The session ended normally: removes the lock file.
void sessionEnd();
// True when the last session ended in a crash or without a clean exit; `details` receives the crash log (or a note when there is none). Archives it so it is offered only once.
bool previousSessionFailed(std::string& details);
// The text of a bug report: version, system, graphics, settings, current state and, when given, the crash details.
std::string reportText(const std::string& crashDetails);
// Opens the new-issue page of `repo` ("owner/name") in the browser with the title and the report in the body. Returns false when it could not.
bool openIssue(const std::string& repo, const std::string& title, const std::string& body);
// A Yes / No question box. False elsewhere than Windows.
bool ask(const std::string& title, const std::string& text);

}  // namespace cb2crash

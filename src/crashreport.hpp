#pragma once
// Crash and bug reports. Every session keeps an event log (cb2-session.log, the previous one as cb2-session-previous.log) that is written line by line, so it survives a process that is
// killed. A crash writes crash.log (what failed, where, the loaded modules, the last events) and crash.dmp (a minidump) next to the executable (the game's own state at that moment); a session that ended without a clean exit
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
// Installs the handlers (exception filter, terminate, abort, invalid parameter) and opens the session log. `dir` is where crash.log and the logs are written.
void install(const std::string& dir, const std::string& version);
// One line in the session log (printf style, timestamped). Cheap enough for events (a level opens, a player joins, a state is loaded), not for every frame.
void log(const char* fmt, ...);
// The main loop calls beat() every iteration and status() about once a second; a watchdog thread (started by watchMainThread()) notices when the loop stops beating for 20 s
// (a hang: the window says "not responding") and writes where the main thread is stuck into the log and into hang.dmp. status() also logs memory use every 30 s.
void beat();
void status();
void watchMainThread();
// The last `maxBytes` of this session's log (whole lines).
std::string recentEvents(size_t maxBytes);
// Opens the folder of the game in Explorer with the crash files selected.
void showCrashFiles();
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

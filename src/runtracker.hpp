#pragma once
// Times the player's runs through the levels of the real game: watches the game (level opens, level requests, player updates), keeps the
// personal records, and tells the rest of the program what happened. Works offline and online. See docs/SPEEDRUN.md.
#include <cstdint>

#include "game_profile.hpp"
#include <string>
#include <vector>

#include "psx.hpp"
#include "records.hpp"
#include "runclock.hpp"

namespace cb2run {

struct Event {
    Run run;
    bool newBest = false;
    uint32_t previousBest = 0;   // the record before this run (0 = none)
};

class Tracker {
public:
    Tracker(psx::Platform& platform, std::string recordsFile) : p(platform), records(std::move(recordsFile)) { records.load(); }
    void install();                       // hooks the game; call once, after the game is loaded
    // A saved state was loaded: `extra` is what saveExtra() stored with it.
    void stateLoaded(const std::vector<uint8_t>& extra);
    std::vector<uint8_t> saveExtra() const;
    std::vector<Event> takeEvents() { std::vector<Event> e; e.swap(events); return e; }

    // Tests: the game asks for `target` now, exactly as the GOOL instruction would (--fake-request).
    void debugRequest(int target) { request(target); }

    int level() const { return clock.level(); }
    int cameFrom() const { return clock.cameFrom(); }
    bool timing() const { return clock.running(); }
    uint32_t levelMs() const { return clock.elapsedMs(p.frames); }   // game time since Crash could move; stands still while the menu pauses the game
    const Records& stored() const { return records; }
    // The latest finished or failed run (for showing to the others).
    const Run& lastRun() const { return last; }
    uint32_t bestMsFor(int level) const { const Best* b = records.find(level); return b ? b->bestMs : 0; }

    // engine addresses: game_profile.hpp, cb2game::current()

private:
    psx::Platform& p;
    Records records;
    RunClock clock;
    Run last;
    std::vector<Event> events;
    void finish(const Run& r);
    void request(int target);
};

}  // namespace cb2run

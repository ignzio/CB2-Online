#pragma once
// Level timing, independent of the emulator so that it can be tested: feed it what the game does (a level was opened, Crash was updated, the
// game asked for another level) and it says when a run started and how it ended.
//
// What is known about the game (see docs/SPEEDRUN.md):
//   - opening a level (function 0x80012170) names the level id;
//   - the first frame in which the player object is updated is the first frame Crash can be controlled (loading is over);
//   - a script asks for another level with the GOOL "next level" instruction (0x80037DC8): the portal asks for the level behind it, the end of a
//     level asks for the level it was entered from (the warp room), and running out of lives asks for level 0x3B (the "Continue?" screen);
//   - dying while lives are left is not a level change at all, so the clock keeps running (as it does in a speedrun).
#include <cstdint>
#include <vector>

#include "game_profile.hpp"

namespace cb2run {

constexpr int kNoLevel = 0xFF;
constexpr int kGameOverLevel = 0x3B;

struct Run {
    int level = kNoLevel;
    uint32_t ms = 0;
    bool finished = false;     // reached the end of the level; false = ran out of lives
};

// Frames to milliseconds: the American release runs at 59.94 Hz, the European one at 50 Hz (their times are not comparable, so records are kept apart).
inline uint32_t framesToMs(uint64_t frames) {
    return cb2game::current().region == cb2game::Region::Pal ? uint32_t(frames * 20ull) : uint32_t(frames * 1001ull / 60ull);
}

class RunClock {
public:
    void levelOpened(int id, uint64_t frame);
    void playerStepped(uint64_t frame);
    // The game asked for level `target`; a finished or failed run is appended to `out`.
    void levelRequested(int target, uint64_t frame, std::vector<Run>& out);
    // After a saved state was loaded: where we are and where we came from (the run clock starts again at the next player update).
    void restore(int level, int cameFrom);

    int level() const { return cur; }
    int cameFrom() const { return from; }
    bool running() const { return started; }
    uint32_t elapsedMs(uint64_t frame) const { return started && frame >= playStart ? framesToMs(frame - playStart) : 0; }

private:
    int cur = kNoLevel, from = kNoLevel;
    bool started = false;
    uint64_t playStart = 0;
};

}  // namespace cb2run

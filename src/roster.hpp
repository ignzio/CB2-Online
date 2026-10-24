#pragma once
// Who is in the room and what they are doing: joins and leaves, entering and leaving levels, and how long somebody has been in their current
// level. Pure bookkeeping on the list of remote players (net.hpp) with a clock, so it can be tested without the game. The overlay that shows
// it is in hud.hpp.
#include <cstdint>
#include <string>
#include <vector>

#include "netproto.hpp"

namespace cb2roster {

// Name of a level id as the game numbers them ("LEVEL 0F" when it is not in the table yet).
std::string levelName(int id);
// The colour of a player's name tag and list entry (0xRRGGBB), by the player's number in the room.
uint32_t playerRgb(int id);
// 83.4 -> "1:23.4"
std::string formatTime(double seconds);

enum class EventKind { Joined, Left, EnteredLevel, LeftLevel, Finished, Failed };

struct Event {
    EventKind kind = EventKind::Joined;
    std::string name;
    int level = 0xFF;      // the level entered or left
    double seconds = 0;    // LeftLevel: how long they were in it; Finished / Failed: the time of the run
};

struct Member {
    uint8_t id = 0;
    std::string name;
    int level = 0xFF;          // 0xFF: not in a level (menus, loading)
    double levelSince = 0;     // clock value when they entered `level`
    double joinedAt = 0;
    int lastLevel = 0xFF;      // the level they were in before, and how long they spent there
    double lastTime = 0;
    bool seen = false;
    // from their speedrun data (netproto.hpp)
    uint32_t bestMs = 0;       // their personal best for the level they are in
    int ranLevel = 0xFF;       // their latest run ...
    uint32_t ranMs = 0;
    bool ranFinished = false;
    int runCount = 0;          // ... and the counter that tells a new run from the old one
};

class Roster {
public:
    // Call whenever the list of remote players may have changed (every frame is fine). `now` is a clock in seconds.
    // New events are appended to `events`. A player seen for the first time at the very start (the list just appeared) still counts as joined.
    void update(const std::vector<cb2net::PlayerState>& others, double now, std::vector<Event>& events);
    void clear() { members.clear(); }
    const std::vector<Member>& list() const { return members; }
    double levelTime(const Member& m, double now) const { return m.level == 0xFF ? 0.0 : now - m.levelSince; }

private:
    std::vector<Member> members;
};

}  // namespace cb2roster

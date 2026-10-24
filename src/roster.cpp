#include "roster.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace cb2roster {

std::string levelName(int id) {
    switch (id) {
    case 0x02: return "WARP ROOM";
    case 0x0E: return "SNOW GO";
    case 0x1E: return "TURTLE WOODS";
    case 0x18: return "CRASH DASH";
    default: break;
    }
    char b[24];
    std::snprintf(b, sizeof b, "LEVEL %02X", id & 0xFF);
    return b;
}

uint32_t playerRgb(int id) {
    static const uint32_t k[] = {0x60C0FF, 0x60E060, 0xF070E0, 0xF0E850, 0xF0F0F0};   // blue, green, pink, yellow, white
    return k[(id % 5 + 5) % 5];
}

std::string formatTime(double s) {
    if (s < 0) s = 0;
    const int tenths = int(s * 10.0 + 0.5);
    const int total = tenths / 10;
    char b[32];
    if (total >= 3600) std::snprintf(b, sizeof b, "%d:%02d:%02d", total / 3600, total / 60 % 60, total % 60);
    else std::snprintf(b, sizeof b, "%d:%02d.%d", total / 60, total % 60, tenths % 10);
    return b;
}

void Roster::update(const std::vector<cb2net::PlayerState>& others, double now, std::vector<Event>& events) {
    for (Member& m : members) m.seen = false;
    for (const cb2net::PlayerState& s : others) {
        Member* m = nullptr;
        for (Member& x : members) if (x.id == s.id) { m = &x; break; }
        const int level = s.level;
        if (!m) {
            members.emplace_back();
            m = &members.back();
            m->id = s.id;
            m->name = s.name;
            m->joinedAt = now;
            m->levelSince = now;
            m->level = level;
            m->runCount = s.runCount;   // runs made before we met are not news
            m->ranLevel = s.lastLevel; m->ranMs = s.lastMs; m->ranFinished = s.lastFinished != 0;
            Event e;
            e.kind = EventKind::Joined;
            e.name = m->name;
            events.push_back(e);
            if (level != 0xFF) { e.kind = EventKind::EnteredLevel; e.level = level; events.push_back(e); }
        } else {
            m->name = s.name;
            if (s.runCount != m->runCount) {   // they finished (or failed) a run
                m->runCount = s.runCount;
                m->ranLevel = s.lastLevel; m->ranMs = s.lastMs; m->ranFinished = s.lastFinished != 0;
                Event e;
                e.kind = m->ranFinished ? EventKind::Finished : EventKind::Failed;
                e.name = m->name;
                e.level = m->ranLevel;
                e.seconds = double(m->ranMs) / 1000.0;
                events.push_back(e);
            }
            if (level != m->level) {
                if (m->level != 0xFF) {
                    Event e;
                    e.kind = EventKind::LeftLevel;
                    e.name = m->name;
                    e.level = m->level;
                    e.seconds = now - m->levelSince;
                    events.push_back(e);
                    m->lastLevel = m->level;
                    m->lastTime = e.seconds;
                }
                m->level = level;
                m->levelSince = now;
                if (level != 0xFF) {
                    Event e;
                    e.kind = EventKind::EnteredLevel;
                    e.name = m->name;
                    e.level = level;
                    events.push_back(e);
                }
            }
        }
        m->bestMs = s.bestMs;
        m->seen = true;
    }
    for (auto it = members.begin(); it != members.end();) {
        if (it->seen) { ++it; continue; }
        Event e;
        e.kind = EventKind::Left;
        e.name = it->name;
        e.level = it->level;
        events.push_back(e);
        it = members.erase(it);
    }
    std::sort(members.begin(), members.end(), [](const Member& a, const Member& b) { return a.id < b.id; });
}

}  // namespace cb2roster

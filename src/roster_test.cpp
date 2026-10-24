// Test for the roster bookkeeping (src/roster.cpp): joins, leaves, level changes and the times measured, with a fake clock.
#include <cstdio>
#include <cstring>

#include "roster.hpp"

using namespace cb2roster;

static int failures = 0;
#define CHECK(cond) do { if (!(cond)) { std::printf("FAIL line %d: %s\n", __LINE__, #cond); ++failures; } } while (0)

static cb2net::PlayerState player(uint8_t id, const char* name, int level) {
    cb2net::PlayerState s;
    s.id = id;
    s.level = uint8_t(level);
    std::strncpy(s.name, name, cb2net::kNameLength - 1);
    return s;
}

int main() {
    Roster r;
    std::vector<Event> ev;

    r.update({}, 0.0, ev);
    CHECK(ev.empty());

    // a player appears already in a level: joined + entered
    r.update({player(1, "ALICE", 0x0E)}, 1.0, ev);
    CHECK(ev.size() == 2 && ev[0].kind == EventKind::Joined && ev[1].kind == EventKind::EnteredLevel && ev[1].level == 0x0E);
    CHECK(r.list().size() == 1);
    CHECK(std::abs(r.levelTime(r.list()[0], 11.5) - 10.5) < 1e-9);

    // no change: no events
    ev.clear();
    r.update({player(1, "ALICE", 0x0E)}, 2.0, ev);
    CHECK(ev.empty());

    // a second player joins at a menu (not in a level): joined only
    r.update({player(1, "ALICE", 0x0E), player(2, "BOB", 0xFF)}, 3.0, ev);
    CHECK(ev.size() == 1 && ev[0].kind == EventKind::Joined && ev[0].name == "BOB");
    CHECK(r.levelTime(r.list()[1], 9.0) == 0.0);

    // Alice leaves the level for the warp room: left (with the time she spent) + entered
    ev.clear();
    r.update({player(1, "ALICE", 0x02), player(2, "BOB", 0xFF)}, 71.4, ev);
    CHECK(ev.size() == 2 && ev[0].kind == EventKind::LeftLevel && ev[0].level == 0x0E && std::abs(ev[0].seconds - 70.4) < 1e-9);
    CHECK(ev[1].kind == EventKind::EnteredLevel && ev[1].level == 0x02);
    CHECK(r.list()[0].lastLevel == 0x0E && std::abs(r.list()[0].lastTime - 70.4) < 1e-9);

    // Bob enters a level
    ev.clear();
    r.update({player(1, "ALICE", 0x02), player(2, "BOB", 0x18)}, 80.0, ev);
    CHECK(ev.size() == 1 && ev[0].kind == EventKind::EnteredLevel && ev[0].level == 0x18 && ev[0].name == "BOB");

    // Bob goes to a menu: left level only
    ev.clear();
    r.update({player(1, "ALICE", 0x02), player(2, "BOB", 0xFF)}, 95.0, ev);
    CHECK(ev.size() == 1 && ev[0].kind == EventKind::LeftLevel && std::abs(ev[0].seconds - 15.0) < 1e-9);

    // Alice disconnects
    ev.clear();
    r.update({player(2, "BOB", 0xFF)}, 100.0, ev);
    CHECK(ev.size() == 1 && ev[0].kind == EventKind::Left && ev[0].name == "ALICE");
    CHECK(r.list().size() == 1 && r.list()[0].id == 2);

    // she reconnects (same id again) as a new member
    ev.clear();
    r.update({player(1, "ALICE", 0xFF), player(2, "BOB", 0xFF)}, 110.0, ev);
    CHECK(ev.size() == 1 && ev[0].kind == EventKind::Joined);
    CHECK(r.list()[0].id == 1 && r.list()[1].id == 2);   // kept in id order

    // a run finished: only the counter tells a new run from the old one
    {
        Roster q;
        std::vector<Event> e2;
        auto withRun = [&](int count, int lvl, uint32_t ms, bool fin) { cb2net::PlayerState s = player(1, "CARA", 0x02); s.runCount = uint8_t(count); s.lastLevel = uint8_t(lvl); s.lastMs = ms; s.lastFinished = fin; s.bestMs = 61000; return s; };
        q.update({withRun(3, 0x1E, 90000, true)}, 1.0, e2);          // she had runs before we met: not news
        CHECK(e2.size() == 2 && e2[0].kind == EventKind::Joined && e2[1].kind == EventKind::EnteredLevel);   // she is in the warp room already
        CHECK(q.list()[0].bestMs == 61000);
        e2.clear();
        q.update({withRun(3, 0x1E, 90000, true)}, 2.0, e2);
        CHECK(e2.empty());
        q.update({withRun(4, 0x0E, 83400, true)}, 3.0, e2);           // a new finished run
        CHECK(e2.size() == 1 && e2[0].kind == EventKind::Finished && e2[0].level == 0x0E && std::abs(e2[0].seconds - 83.4) < 1e-9 && e2[0].name == "CARA");
        e2.clear();
        q.update({withRun(5, 0x0E, 40000, false)}, 4.0, e2);          // a failed one (game over)
        CHECK(e2.size() == 1 && e2[0].kind == EventKind::Failed);
        e2.clear();
        q.update({withRun(5, 0x0E, 40000, false)}, 5.0, e2);
        CHECK(e2.empty());
        // the counter wraps around 255 -> 0: still a new run
        q.update({withRun(255, 0x0E, 40000, true)}, 6.0, e2);
        e2.clear();
        q.update({withRun(0, 0x0E, 39000, true)}, 7.0, e2);
        CHECK(e2.size() == 1 && e2[0].kind == EventKind::Finished);
    }
    // times
    CHECK(formatTime(83.4) == "1:23.4");
    CHECK(formatTime(0) == "0:00.0");
    CHECK(formatTime(59.96) == "1:00.0");
    CHECK(formatTime(3725) == "1:02:05");
    CHECK(formatTime(-5) == "0:00.0");
    CHECK(levelName(0x1E) == "TURTLE WOODS");
    CHECK(levelName(0x0E) == "SNOW GO" && levelName(0x18) == "CRASH DASH" && levelName(0x2B) == "LEVEL 2B");

    std::printf(failures ? "FAILED (%d)\n" : "all passed\n", failures);
    return failures ? 1 : 0;
}

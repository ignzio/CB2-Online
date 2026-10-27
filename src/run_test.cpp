// Test for the run clock (src/runclock.cpp) and the records file (src/records.cpp).
#include <cstdio>
#include <filesystem>
#include <fstream>

#include "game_profile.hpp"
#include "records.hpp"
#include "runclock.hpp"

using namespace cb2run;

static int failures = 0;
#define CHECK(cond) do { if (!(cond)) { std::printf("FAIL line %d: %s\n", __LINE__, #cond); ++failures; } } while (0)

int main() {
    // --- the European release has one warp room per language (levels 0x2D-0x30 are copies of 0x02): for the other players they are all the warp room
    {
        using cb2game::Region;
        for (uint32_t id : {0x02u, 0x2Du, 0x2Eu, 0x2Fu, 0x30u}) CHECK(cb2game::canonicalLevel(Region::Pal, id) == 0x02u);
        CHECK(cb2game::canonicalLevel(Region::Pal, 0x1E) == 0x1Eu && cb2game::canonicalLevel(Region::Pal, 0x2C) == 0x2Cu && cb2game::canonicalLevel(Region::Pal, 0x31) == 0x31u);
        CHECK(cb2game::canonicalLevel(Region::Ntsc, 0x30) == 0x30u && cb2game::canonicalLevel(Region::Ntsc, 0x2D) == 0x2Du);   // the American release has no such levels
    }
    // --- the usual story: hub, portal, level, finish back to the hub
    {
        RunClock c;
        std::vector<Run> out;
        c.levelOpened(0x3C, 0);          // boot
        c.levelOpened(0x02, 100);        // the warp room
        c.playerStepped(150);
        c.levelRequested(0x1E, 1000, out);   // the portal: not a finish
        CHECK(out.empty());
        c.levelOpened(0x1E, 1100);       // Turtle Woods; loading takes a while
        CHECK(!c.running());
        c.playerStepped(1300);           // the first frame Crash can move: the clock starts here
        CHECK(c.running() && c.elapsedMs(1300) == 0);
        CHECK(c.elapsedMs(1300 + 600) == framesToMs(600));
        c.levelRequested(0x02, 1300 + 5000, out);   // the end of the level: back to the warp room
        CHECK(out.size() == 1 && out[0].level == 0x1E && out[0].finished && out[0].ms == framesToMs(5000));
        CHECK(!c.running());
        c.levelRequested(0x02, 1300 + 5001, out);   // nothing is counted twice
        CHECK(out.size() == 1);
        c.levelOpened(0x02, 7000);       // arrives in the warp room, cameFrom is now Turtle Woods
        CHECK(c.cameFrom() == 0x1E);
        c.playerStepped(7050);
        c.levelRequested(0x0E, 8000, out);    // the portal to Snow Go is not a finish
        CHECK(out.size() == 1);
    }
    // --- dying with lives left changes nothing; running out of lives is a failed run
    {
        RunClock c;
        std::vector<Run> out;
        c.levelOpened(0x02, 0);
        c.levelOpened(0x18, 10);
        c.playerStepped(60);
        c.levelOpened(0x18, 400);        // a restart opens the same level again: the clock keeps running
        CHECK(c.running() && c.elapsedMs(660) == framesToMs(600));
        c.levelRequested(0x3B, 900, out);
        CHECK(out.size() == 1 && out[0].level == 0x18 && !out[0].finished && out[0].ms == framesToMs(840));
    }
    // --- a request before the player existed, or with an unknown origin, is ignored
    {
        RunClock c;
        std::vector<Run> out;
        c.levelRequested(0x02, 10, out);
        CHECK(out.empty());
        c.restore(0x0E, 0x02);           // after loading a saved state in Snow Go
        c.levelRequested(0x02, 20, out);
        CHECK(out.empty());              // the clock only starts at the next player update
        c.playerStepped(30);
        c.levelRequested(0x02, 330, out);
        CHECK(out.size() == 1 && out[0].finished && out[0].ms == framesToMs(300));
    }
    // --- a level cannot "finish" into itself
    {
        RunClock c;
        std::vector<Run> out;
        c.levelOpened(0x02, 0);
        c.playerStepped(5);
        c.levelRequested(0x02, 100, out);   // the hub asking for the hub (from == 0xFF anyway)
        CHECK(out.empty());
    }
    // --- records
    {
        const std::filesystem::path f = std::filesystem::temp_directory_path() / "cb2_records_test.txt";
        std::filesystem::remove(f);
        Records r(f.string());
        CHECK(!r.load());                                  // no file yet
        CHECK(r.add(0x1E, 90000, true, "2026-10-24"));     // first finish is a best
        CHECK(!r.add(0x1E, 95000, true, "2026-10-25"));    // slower
        CHECK(r.add(0x1E, 80100, true, "2026-10-26"));     // faster
        CHECK(!r.add(0x1E, 1000, false, "2026-10-27"));    // a failed run never counts, however short
        CHECK(r.add(0x0E, 123456, true, "2026-10-26"));
        CHECK(r.find(0x1E) && r.find(0x1E)->bestMs == 80100 && r.find(0x1E)->finishes == 3 && r.find(0x1E)->attempts == 4 && r.find(0x1E)->lastMs == 80100);
        CHECK(r.find(0x1E)->bestDate == "2026-10-26");
        CHECK(!r.find(0x18));
        CHECK(r.save());
        Records again(f.string());
        CHECK(again.load());
        CHECK(again.all().size() == 2);
        CHECK(again.find(0x1E) && again.find(0x1E)->bestMs == 80100 && again.find(0x1E)->attempts == 4 && again.find(0x0E)->bestMs == 123456);
        // a damaged file loses only the damaged lines
        { std::ofstream o(f, std::ios::app); o << "R\tZZ\t1\t1\t1\t1\n\ngarbage\nR\t05\t77\n"; }
        Records damaged(f.string());
        CHECK(damaged.load() && damaged.find(0x1E) && damaged.find(0x0E));
        CHECK(!damaged.find(0x05));
        std::filesystem::remove(f);
    }
    CHECK(framesToMs(60) == 1001);
    std::printf(failures ? "FAILED (%d)\n" : "all passed\n", failures);
    return failures ? 1 : 0;
}

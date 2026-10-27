#include "runtracker.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace cb2run {

namespace {
bool debug() { static const bool d = std::getenv("CB2_RUNLOG") != nullptr; return d; }
}

void Tracker::install() {
    p.addHook(cb2game::current().openLevel, [this](psx::Platform& pl) {
        if (pl.cpu.r[4] != 0) return;   // the second pass of the same load
        const int id = int(pl.cpu.r[5] & 0xFF);
        clock.levelOpened(id, pl.frames);
        if (debug()) { std::printf("[run] frame %llu: level %02X opened (came from %02X)\n", (unsigned long long)pl.frames, id, clock.cameFrom()); std::fflush(stdout); }
    });
    p.addHook(cb2game::current().objectStep, [this](psx::Platform& pl) {
        const uint32_t obj = pl.cpu.r[4];
        if (obj && obj == pl.peek32(cb2game::current().playerGlobal) && !clock.running() && clock.level() != kNoLevel) {
            clock.playerStepped(pl.frames);
            if (debug()) { std::printf("[run] frame %llu: Crash can move in level %02X, the clock starts\n", (unsigned long long)pl.frames, clock.level()); std::fflush(stdout); }
        }
    });
    // The GOOL "next level" instruction: the value is already shifted down to the level id when execution reaches the store.
    p.addHook(cb2game::current().nextLevel, [this](psx::Platform& pl) { request(int(pl.cpu.r[2] & 0xFF)); });
}

void Tracker::request(int target) {
    if (debug()) { std::printf("[run] frame %llu: the game asks for level %02X (in %02X, from %02X, clock %s %u ms)\n", (unsigned long long)p.frames, target, clock.level(), clock.cameFrom(), clock.running() ? "running" : "stopped", levelMs()); std::fflush(stdout); }
    std::vector<Run> done;
    clock.levelRequested(target, p.frames, done);
    for (const Run& r : done) finish(r);
}

void Tracker::finish(const Run& r) {
    Event e;
    e.run = r;
    if (const Best* b = records.find(r.level)) e.previousBest = b->bestMs;
    e.newBest = records.add(r.level, r.ms, r.finished, todayText());
    records.save();
    last = r;
    events.push_back(e);
    if (debug()) { std::printf("[run] %s level %02X in %u ms%s\n", r.finished ? "finished" : "failed", r.level, r.ms, e.newBest ? " (new best)" : ""); std::fflush(stdout); }
}

std::vector<uint8_t> Tracker::saveExtra() const {
    std::vector<uint8_t> x(8, 0);
    const uint32_t lvl = uint32_t(clock.level()), from = uint32_t(clock.cameFrom());
    std::memcpy(x.data(), &lvl, 4);
    std::memcpy(x.data() + 4, &from, 4);
    return x;
}

void Tracker::stateLoaded(const std::vector<uint8_t>& extra) {
    uint32_t lvl = kNoLevel, from = kNoLevel;
    if (extra.size() >= 8) { std::memcpy(&lvl, extra.data(), 4); std::memcpy(&from, extra.data() + 4, 4); }
    // the level that is really loaded is in the game's own variable (older states did not store it)
    const uint32_t loaded = p.peek32(cb2game::current().currentLevel) & 0xFF;
    if (loaded != 0 && loaded <= 0x3F) lvl = loaded;
    clock.restore(int(lvl & 0xFF), int(from & 0xFF));
    if (debug()) { std::printf("[run] state loaded in level %02X (came from %02X)\n", int(lvl & 0xFF), int(from & 0xFF)); std::fflush(stdout); }
}

}  // namespace cb2run

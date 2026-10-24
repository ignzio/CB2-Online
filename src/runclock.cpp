#include "runclock.hpp"

namespace cb2run {

void RunClock::levelOpened(int id, uint64_t) {
    if (id == cur) return;   // the same level opened again (a restart): the clock keeps running
    from = cur;
    cur = id;
    started = false;
}

void RunClock::playerStepped(uint64_t frame) {
    if (cur == kNoLevel || started) return;
    started = true;
    playStart = frame;
}

void RunClock::levelRequested(int target, uint64_t frame, std::vector<Run>& out) {
    if (!started || cur == kNoLevel) return;
    Run r;
    r.level = cur;
    r.ms = framesToMs(frame - playStart);
    if (target == kGameOverLevel) r.finished = false;
    else if (target == from && target != cur) r.finished = true;
    else return;
    out.push_back(r);
    started = false;   // one result per run; the next level's clock starts when it opens
}

void RunClock::restore(int level, int cameFrom) {
    cur = level;
    from = cameFrom;
    started = false;
}

}  // namespace cb2run

#pragma once
// Runtime interface of the translated (native) engine code produced by tools/recomp.py.
#include <cstdint>

namespace psx { class Platform; }

namespace recomp {

using Fn = void (*)(psx::Platform&);

struct Entry {
    uint32_t addr;
    Fn fn;
};

// Generated (recomp_table.cpp). Absent (empty) when the translator has not been run.
extern const Entry kFunctions[];
extern const unsigned kFunctionCount;
// Every basic-block start of the translated code (sorted); used by the differential checker.
extern const uint32_t kLeaders[];
extern const unsigned kLeaderCount;

}  // namespace recomp

// Generated code: advance the instruction counter by a basic block and let the platform run devices and interrupts.
#define TICK(a, n)                                                 \
    do {                                                           \
        c.instructions += (n);                                     \
        if (P.blockTrace) P.blockTrace->push_back(a);              \
        if (c.instructions >= P.pollAt) P.pollNative();            \
    } while (0)

// `jr ra`: return to the caller. A function that returns somewhere else continues there (tail transfer).
#define CB2_RETURN(t)                                              \
    do {                                                           \
        if ((t) != entryRa) P.tail(t);                             \
        return;                                                    \
    } while (0)

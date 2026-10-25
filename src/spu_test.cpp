// Test for the SPU reverb (src/spu.cpp, Spu::reverbStep): an impulse comes back after exactly the programmed delay, nothing is written while the reverb is
// switched off, silence stays silent, and a feedback setup stays bounded.
#include <cmath>
#include <cstdio>
#include <cstdlib>

#include "spu.hpp"

using namespace psx;

static int failures = 0;
#define CHECK(cond) do { if (!(cond)) { std::printf("FAIL line %d: %s\n", __LINE__, #cond); ++failures; } } while (0)

// Register offsets (bytes from 0x1F801C00)
enum : uint32_t {
    dAPF1 = 0x1C0, dAPF2 = 0x1C2, vIIR = 0x1C4, vCOMB1 = 0x1C6, vCOMB2 = 0x1C8, vCOMB3 = 0x1CA, vCOMB4 = 0x1CC, vWALL = 0x1CE, vAPF1 = 0x1D0, vAPF2 = 0x1D2,
    mLSAME = 0x1D4, mRSAME = 0x1D6, mLCOMB1 = 0x1D8, mRCOMB1 = 0x1DA, mLCOMB2 = 0x1DC, mRCOMB2 = 0x1DE, dLSAME = 0x1E0, dRSAME = 0x1E2, mLDIFF = 0x1E4, mRDIFF = 0x1E6,
    mLCOMB3 = 0x1E8, mRCOMB3 = 0x1EA, mLCOMB4 = 0x1EC, mRCOMB4 = 0x1EE, dLDIFF = 0x1F0, dRDIFF = 0x1F2, mLAPF1 = 0x1F4, mRAPF1 = 0x1F6, mLAPF2 = 0x1F8, mRAPF2 = 0x1FA,
    vLIN = 0x1FC, vRIN = 0x1FE, vLOUT = 0x184, vROUT = 0x186, mBASE = 0x1A2, SPUCNT = 0x1AA
};

// A reverb whose left output is the left input delayed by `delay` steps: the input goes into [mLSAME] unchanged (vIIR = 1, vWALL = 0) and one comb tap reads
// the buffer `delay` steps back.
static void delayLine(Spu& s, uint32_t base, int delay, bool enabled) {
    const uint32_t area = 0x80000 - base;
    s.write(mBASE, uint16_t(base >> 3));
    for (uint32_t r = dAPF1; r <= vRIN; r += 2) s.write(r, 0);
    s.write(vIIR, 0x7FFF);
    s.write(vLIN, 0x7FFF);
    s.write(vCOMB1, 0x7FFF);
    // every register the formula writes through gets its own place, or the writes of one would land on the cell another reads
    s.write(mLSAME, 0);
    s.write(mRSAME, 0x200); s.write(mLDIFF, 0x300); s.write(mRDIFF, 0x400); s.write(mRCOMB1, 0x500);
    s.write(mLAPF1, 0x600); s.write(mRAPF1, 0x700); s.write(mLAPF2, 0x800); s.write(mRAPF2, 0x900);
    s.write(mLCOMB1, uint16_t((area - uint32_t(delay) * 2) >> 3));
    s.write(vLOUT, 0x7FFF);
    s.write(vROUT, 0x7FFF);
    s.write(SPUCNT, uint16_t(0xC000 | (enabled ? 0x80 : 0)));
}

int main() {
    // an impulse returns after exactly `delay` steps, once
    {
        Spu s;
        delayLine(s, 0x70000, 400, true);
        int firstAt = -1, count = 0;
        int32_t peak = 0;
        for (int i = 0; i < 2000; ++i) {
            int32_t l = 0, r = 0;
            s.reverbStep(i == 0 ? 20000 : 0, 0, l, r);
            if (std::abs(l) > 4) { if (firstAt < 0) firstAt = i; ++count; peak = std::max(peak, std::abs(l)); }
            if (i < 1000) CHECK(r == 0);   // (later the right comb tap reads a cell the left all-pass wrote: the registers' places overlap by design of this test)
        }
        CHECK(firstAt == 400);
        CHECK(count == 1);
        CHECK(std::abs(peak - 20000) <= 4);
    }
    // while the reverb is switched off (SPUCNT bit 7 clear) the buffer is not written, so nothing comes back
    {
        Spu s;
        delayLine(s, 0x70000, 400, false);
        int count = 0;
        for (int i = 0; i < 2000; ++i) {
            int32_t l = 0, r = 0;
            s.reverbStep(i == 0 ? 20000 : 0, 0, l, r);
            if (std::abs(l) > 0) ++count;
        }
        CHECK(count == 0);
    }
    // silence in, silence out; the work area is not touched
    {
        Spu s;
        delayLine(s, 0x70000, 400, true);
        for (int i = 0; i < 5000; ++i) {
            int32_t l = 1, r = 1;
            s.reverbStep(0, 0, l, r);
            CHECK(l == 0 && r == 0);
        }
        for (uint32_t a = 0x6FFF0; a < 0x70040; ++a) CHECK(s.ram[a] == 0);
    }
    // the work area is a ring: the write position wraps from the end of the area back to its start, and no echo appears from the second lap
    {
        Spu s;
        delayLine(s, 0x78000, 300, true);   // an area of 0x8000 bytes = 16384 steps
        int hits = 0;
        for (int i = 0; i < 40000; ++i) {
            int32_t l = 0, r = 0;
            s.reverbStep(i == 0 ? 10000 : 0, 0, l, r);
            if (std::abs(l) > 4) { ++hits; CHECK(i == 300); }
        }
        CHECK(hits == 1);
        // and a second impulse after the wrap is delayed by the same amount
        int second = -1;
        for (int i = 40000; i < 41000; ++i) {
            int32_t l = 0, r = 0;
            s.reverbStep(i == 40000 ? 10000 : 0, 0, l, r);
            if (std::abs(l) > 4 && second < 0) second = i;
        }
        CHECK(second == 40300);
    }

    // feedback (a wall that returns the reflection, with the all-pass filters on) stays bounded and decays
    {
        Spu s;
        delayLine(s, 0x70000, 400, true);
        s.write(vWALL, 0x6000);
        s.write(dLSAME, 0x0100);   // [dLSAME] is 0x800 bytes ahead of the write position: the ring's far side, i.e. old data
        s.write(vAPF1, 0x4000); s.write(dAPF1, 0x0040);
        s.write(vAPF2, 0x4000); s.write(dAPF2, 0x0080);
        int32_t late = 0;
        for (int i = 0; i < 200000; ++i) {
            int32_t l = 0, r = 0;
            s.reverbStep(i == 0 ? 30000 : 0, 0, l, r);
            CHECK(std::abs(l) <= 32768 && std::abs(r) <= 32768);
            if (i > 190000) late = std::max(late, std::abs(l));
        }
        CHECK(late < 30000);
    }

    std::printf(failures ? "FAILED: %d\n" : "all passed\n", failures);
    return failures ? 1 : 0;
}

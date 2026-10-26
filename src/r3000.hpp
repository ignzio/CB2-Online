#pragma once
// MIPS R3000 (PlayStation CPU) interpreter core and the runtime shared with recompiled code.
// Exact behaviours implemented: branch delay slots, load delay slots (including the lwl/lwr bypass and a write to
// the load target cancelling the load), unaligned lwl/lwr/swl/swr, R3000 divide results, isolated-cache stores.
#include <climits>
#include <cstdint>
#include <cstring>
#include <functional>
#include <stdexcept>
#include <array>
#include <unordered_map>
#include <vector>

namespace r3000 {

struct OutOfBudget {};

// Unrounded screen positions of GTE-projected vertices, keyed by the packed (x | y << 16) value the GTE pushed. Used by
// renderers that draw at a higher resolution than the PS1 grid ("precise geometry"); null when not needed.
using PgxpMap = std::unordered_map<uint32_t, std::array<float, 3>>;   // x, y and the depth (SZ) the GTE projected the vertex with

constexpr uint32_t kRamSize = 0x200000;        // 2 MiB main RAM, mirrored through the first 8 MiB
constexpr uint32_t kScratchBase = 0x1F800000;  // 1 KiB scratchpad (4 KiB are modelled so tests can map whole pages)
constexpr uint32_t kScratchSize = 0x1000;

// One projected vertex with the transform that produced it (for telling objects apart in the scene).
struct GteSample {
    uint32_t rt[5];     // control registers 0-4: rotation matrix
    int32_t tr[3];      // control registers 5-7: translation
    float sx, sy, sz;
    int16_t vx, vy, vz;   // the vertex as it was loaded into the GTE (before the matrix)
    uint32_t pc;          // address of the RTPS/RTPT instruction
    int32_t ofx, ofy;     // screen offsets (16.16) and projection distance H at that moment
    uint16_t h;
};

struct Cpu {
    uint32_t r[32] = {};
    uint32_t hi = 0, lo = 0;
    uint32_t pc = 0, nextPc = 4;
    uint32_t cop0[32] = {};                    // SR = 12, CAUSE = 13, EPC = 14
    uint32_t cop2d[32] = {}, cop2c[32] = {};   // GTE data and control registers
    uint8_t* ram = nullptr;                    // ramMask + 1 bytes, owned by the caller
    uint32_t ramMask = kRamSize - 1;           // 2 MiB retail console; a power of two up to 8 MiB for extended-memory builds
    uint8_t scratch[kScratchSize] = {};
    PgxpMap* pgxp = nullptr;
    // When set, every vertex the GTE projects (RTPS/RTPT) is appended as {screen x, screen y, depth}. Used to find where an object
    // was drawn on screen (name tags of other players).
    std::vector<std::array<float, 3>>* sxyCapture = nullptr;
    std::vector<GteSample>* gteSamples = nullptr;   // when set, every projected vertex is recorded here
    std::vector<GteSample>* gteSamples2 = nullptr;  // a second consumer of the same record (the native world renderer)
    uint64_t instructions = 0;
    int64_t budget = INT64_MAX;                // decremented by CB2_TICK in recompiled code
    bool fault = false;                        // set when an unmapped address was touched
    uint32_t faultAddr = 0;

    // Hardware and BIOS hooks. Unset hooks fault, which is what tests rely on.
    std::function<uint32_t(Cpu&, uint32_t addr, int size)> ioRead;
    std::function<void(Cpu&, uint32_t addr, uint32_t value, int size)> ioWrite;
    // Called when execution reaches the BIOS call vectors 0xA0 / 0xB0 / 0xC0 (physical address passed).
    // Return true if handled; the hook must then set pc/nextPc (normally to r[31]).
    std::function<bool(Cpu&, uint32_t vector)> bios;
    // Called before every instruction with its address. Return true if the hook handled it (it must then
    // set pc/nextPc itself, normally returning to r[31]). Used to replace engine routines with test stubs.
    std::function<bool(Cpu&, uint32_t pc)> intercept;
    std::function<void(Cpu&, uint32_t addr)> indirectCall;  // recompiled code: call to an address with no translation

    // Load delay slot state: a load commits after the following instruction executes.
    uint32_t pendReg = 0, pendVal = 0;

    uint8_t* map(uint32_t addr, uint32_t size) {
        const uint32_t p = addr & 0x1FFFFFFFu;
        const uint32_t lim = ramMask < 0x7FFFFFu ? 0x00800000u : ramMask + 1;  // the retail 2 MiB is mirrored through 8 MiB
        if (p < lim) return ram + (p & ramMask);
        if (p >= kScratchBase && p - kScratchBase + size <= kScratchSize) return scratch + (p - kScratchBase);
        return nullptr;
    }
    static bool isIo(uint32_t addr) {
        const uint32_t p = addr & 0x1FFFFFFFu;
        return p >= 0x1F801000u && p < 0x1F803000u;
    }
    void trap(uint32_t addr) { if (!fault) { fault = true; faultAddr = addr; } }
    bool cacheIsolated() const { return (cop0[12] & 0x10000u) != 0; }

    uint32_t rd8(uint32_t a) { if (auto* p = map(a, 1)) return *p; if (isIo(a) && ioRead) return ioRead(*this, a, 1); trap(a); return 0; }
    uint32_t rd16(uint32_t a) {
        if (auto* p = map(a, 2)) { uint16_t v; std::memcpy(&v, p, 2); return v; }
        if (isIo(a) && ioRead) return ioRead(*this, a, 2);
        trap(a); return 0;
    }
    uint32_t rd32(uint32_t a) {
        if (auto* p = map(a, 4)) { uint32_t v; std::memcpy(&v, p, 4); return v; }
        if (isIo(a) && ioRead) return ioRead(*this, a, 4);
        trap(a); return 0;
    }
    void wr8(uint32_t a, uint32_t v) {
        if (cacheIsolated()) return;
        if (auto* p = map(a, 1)) *p = uint8_t(v); else if (isIo(a) && ioWrite) ioWrite(*this, a, v, 1); else trap(a);
    }
    void wr16(uint32_t a, uint32_t v) {
        if (cacheIsolated()) return;
        if (auto* p = map(a, 2)) { const uint16_t x = uint16_t(v); std::memcpy(p, &x, 2); }
        else if (isIo(a) && ioWrite) ioWrite(*this, a, v, 2);
        else trap(a);
    }
    void wr32(uint32_t a, uint32_t v) {
        if (cacheIsolated()) return;
        if (auto* p = map(a, 4)) std::memcpy(p, &v, 4);
        else if (isIo(a) && ioWrite) ioWrite(*this, a, v, 4);
        else trap(a);
    }

    // Unaligned access (little endian). `old` is the register value the load merges into.
    uint32_t lwl(uint32_t a, uint32_t old) {
        const uint32_t w = rd32(a & ~3u), s = (a & 3u) * 8;
        return (old & (0x00FFFFFFu >> s)) | (w << (24 - s));
    }
    uint32_t lwr(uint32_t a, uint32_t old) {
        const uint32_t w = rd32(a & ~3u), s = (a & 3u) * 8;
        return (old & (0xFFFFFF00u << (24 - s))) | (w >> s);
    }
    void swl(uint32_t a, uint32_t v) {
        const uint32_t w = rd32(a & ~3u), s = (a & 3u) * 8;
        wr32(a & ~3u, (w & (0xFFFFFF00u << s)) | (v >> (24 - s)));
    }
    void swr(uint32_t a, uint32_t v) {
        const uint32_t w = rd32(a & ~3u), s = (a & 3u) * 8;
        wr32(a & ~3u, (w & (0x00FFFFFFu >> (24 - s))) | (v << s));
    }

    void mult(uint32_t a, uint32_t b) {
        const int64_t p = int64_t(int32_t(a)) * int64_t(int32_t(b));
        lo = uint32_t(p); hi = uint32_t(uint64_t(p) >> 32);
    }
    void multu(uint32_t a, uint32_t b) {
        const uint64_t p = uint64_t(a) * uint64_t(b);
        lo = uint32_t(p); hi = uint32_t(p >> 32);
    }
    // R3000 results for the cases the manual leaves open: divide by zero and INT_MIN / -1.
    void div(uint32_t a, uint32_t b) {
        const int32_t x = int32_t(a), y = int32_t(b);
        if (y == 0) { lo = x < 0 ? 1u : 0xFFFFFFFFu; hi = a; }
        else if (x == INT32_MIN && y == -1) { lo = 0x80000000u; hi = 0; }
        else { lo = uint32_t(x / y); hi = uint32_t(x % y); }
    }
    void divu(uint32_t a, uint32_t b) {
        if (b == 0) { lo = 0xFFFFFFFFu; hi = a; }
        else { lo = a / b; hi = a % b; }
    }

    // ---- interpreter ----
    void reset(uint32_t entry) { pc = entry; nextPc = entry + 4; pendReg = 0; fault = false; }
    void step();                 // execute one instruction; throws std::runtime_error on faults and unsupported ops
    void run(uint64_t count) { for (uint64_t i = 0; i < count; ++i) step(); }

    [[noreturn]] void brk(uint32_t code);
    [[noreturn]] void syscall(uint32_t code);
    [[noreturn]] void invalid(uint32_t pc, uint32_t word);
    void unresolved(uint32_t addr);

    // GTE (COP2). Implemented in gte.cpp.
    uint32_t gteReadData(uint32_t reg);
    void gteWriteData(uint32_t reg, uint32_t v);
    uint32_t gteReadCtrl(uint32_t reg);
    void gteWriteCtrl(uint32_t reg, uint32_t v);
    void gteCommand(uint32_t cmd);
    uint32_t gteInsnPc = 0;   // address of the COP2 instruction being executed (for tools)
    std::vector<uint32_t> dbgLog;   // tool: the registers at every execution of dbgPc, 32 words each
    uint32_t dbgPc = 0, dbgRegs[32] = {}, dbgHits = 0;   // tools: registers at the first execution of the GTE instruction at dbgPc (dbgHits counts them)
};

#ifdef CB2_BUDGET
#define CB2_TICK(c) do { if (--(c).budget < 0) throw ::r3000::OutOfBudget{}; } while (0)
#else
#define CB2_TICK(c) do { } while (0)
#endif

}  // namespace r3000

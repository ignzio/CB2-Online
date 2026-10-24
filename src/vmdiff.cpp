// Differential fuzzer for the C++ GOOL VM against the game's own interpreter.
//   cb2_vmdiff <SCUS_941.54> [cases=1000] [seed=1] [-v]
// Each case builds one memory image (process, pools, links, stack, scratchpad, arrays), writes a random GOOL program
// (with embedded native MIPS blocks), and runs it twice: through the original interpreter at 0x8003A014 on the R3000
// core, and through cb2::goolRun. Native engine routines are replaced by one deterministic stub in both runs (it also
// edits process state, so the register-reload paths are exercised). The runs must agree on the return value, the
// service-call log, the scratchpad, and every byte of the memory the VM can touch.
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <random>
#include <string>
#include <vector>

#include "gool_decode.hpp"
#include "gool_native.hpp"
#include "gool_vm.hpp"
#include "nsf.hpp"
#include "r3000.hpp"

namespace {

using Rng = std::mt19937;

constexpr uint32_t kProc0 = 0x80100000;
constexpr uint32_t kOtherProc[4] = {0x80110000, 0x80111000, 0x80112000, 0x80113000};
constexpr uint32_t kOwnEntry = 0x80101000, kLinkEntry = 0x80101100;
constexpr uint32_t kOwnPool = 0x80102000, kLinkPool = 0x80102400;
constexpr uint32_t kCode = 0x80103000;
constexpr uint32_t kOut = 0x80104000;
constexpr uint32_t kArrays = 0x80121000;  // random data region is 0x80120000..0x80124000
constexpr uint32_t kAnimTable = 0x80125000;
constexpr uint32_t kFrame = kProc0 + 0x40 + 0x300;
constexpr uint32_t kSentinel = 0x80005000;
constexpr uint32_t kOpTable = 0x8005C514;
constexpr uint32_t kRandState = 0x8005F29C;
constexpr uint32_t kMipsMarker = 0x49BE0BE0;  // the word the compiler emits for opcode 73
constexpr uint32_t kBlockExit = 0x03E0A809;   // jalr s5, ra

uint32_t rnd(Rng& g, uint32_t n) { return n ? uint32_t(g() % n) : 0; }
bool chance(Rng& g, int percent) { return int(g() % 100) < percent; }

uint32_t mix(uint64_t a, uint64_t b, uint64_t c) {
    uint64_t h = a * 0x9E3779B97F4A7C15ull ^ (b + 0x7F4A7C15) * 0xC2B2AE3D27D4EB4Full ^ (c + 1) * 0x165667B19E3779F9ull;
    h ^= h >> 29;
    h *= 0xBF58476D1CE4E5B9ull;
    h ^= h >> 32;
    return uint32_t(h);
}

// ---- operand generators (12-bit refs, see docs/GOOL.md) ----

uint32_t imm9(Rng& g, int lo, int hi) { return 0x800u | (uint32_t(lo + int(rnd(g, uint32_t(hi - lo + 1)))) & 0x1FF); }

// Frame slots 0-2 hold the saved frame header (flags, return ip, packed stack/frame offsets). Scripts keep locals at
// slot 3 and above and arguments at negative slots, so the generator does the same.
int slotIndex(Rng& g) {
    int s = int(rnd(g, 16)) - 8;
    if (s >= 0 && s <= 2) s += 3;
    return s;
}

uint32_t srcRef(Rng& g, bool ptrSafe = false) {
    const uint32_t k = rnd(g, 100);
    if (k < 20) return rnd(g, 16);                                       // own pool
    if (k < 32) return 0x400u | rnd(g, 16);                              // linked pool
    if (k < 44) return 0x800u | rnd(g, 0x200);                           // imm9
    if (k < 54) return 0xA00u | rnd(g, 0x100);                           // imm8
    if (k < 68) return 0xB00u | (uint32_t(slotIndex(g)) & 0x7F);         // stack or argument slot
    if (k < 72) return ptrSafe ? (0xB00u | (uint32_t(slotIndex(g)) & 0x7F)) : (0xB80u | (chance(g, 50) ? 0x10u : 0u));  // constant 0 or 1
    if (k < 82) return 0xC00u | (rnd(g, 8) << 6) | rnd(g, 64);           // linked field
    if (k < 96) return 0xE00u | (8 + rnd(g, 119));                       // own field
    return 0xE1Fu;                                                       // pop the operand stack
}

// Fields 0x20 and 0x21 hold the saved instruction pointer and frame pointer; scripts never write them.
uint32_t safeField(uint32_t n) { return (n == 0x20 || n == 0x21) ? n + 2 : n; }

uint32_t dstRef(Rng& g) {
    uint32_t r;
    const uint32_t k = rnd(g, 100);
    if (k < 35) r = uint32_t(slotIndex(g)) & 0x7F;                       // stack slot
    else if (k < 75) r = 0x600u | (chance(g, 12) ? 0x1Fu : safeField(8 + rnd(g, 119)));
    else r = 0x400u | ((1 + rnd(g, 7)) << 6) | safeField(rnd(g, 64));    // linked field
    return r | (chance(g, 50) ? 0x800u : 0u);                            // bit 11 is not examined for destinations
}

// A reference whose address will be written through (LOOP): never a pool word, which holds array pointers and code
// addresses that later instructions rely on.
uint32_t rwRef(Rng& g) {
    const uint32_t k = rnd(g, 100);
    if (k < 35) return 0xB00u | (uint32_t(slotIndex(g)) & 0x7F);         // stack or argument slot
    if (k < 75) return 0xE00u | safeField(8 + rnd(g, 119));              // own field
    return 0xC00u | ((1 + rnd(g, 7)) << 6) | safeField(rnd(g, 64));      // linked field
}

uint32_t word(uint32_t op, uint32_t hi, uint32_t lo) { return (op << 24) | ((hi & 0xFFF) << 12) | (lo & 0xFFF); }

// ---- native MIPS blocks ----
// A block touches only the interpreter's registers (s0-s7, fp, sp, ra) plus temporaries it writes before reading,
// which is the property the shipped blocks have (tools/block_livein.py). It ends with `jalr s5, ra` and a delay slot.

uint32_t rType(uint32_t rs, uint32_t rt, uint32_t rd, uint32_t sa, uint32_t fn) { return (rs << 21) | (rt << 16) | (rd << 11) | (sa << 6) | fn; }
uint32_t iType(uint32_t op, uint32_t rs, uint32_t rt, uint32_t imm) { return (op << 26) | (rs << 21) | (rt << 16) | (imm & 0xFFFF); }

std::vector<uint32_t> genBlock(Rng& g) {
    enum : uint32_t { S0 = 16, S1 = 17, S5 = 21, S6 = 22, S7 = 23 };
    static const uint32_t temps[] = {2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 24, 25};
    static const uint32_t readable[] = {16, 17, 18, 19, 20, 22, 23};
    std::vector<uint32_t> w, defined;
    bool inSkip = false;

    auto pickDefined = [&]() -> uint32_t {
        const uint32_t k = rnd(g, 10);
        if (defined.empty() || k < 3) return k < 2 ? 0u : readable[rnd(g, 7)];
        return defined[rnd(g, uint32_t(defined.size()))];
    };
    auto pickDest = [&]() -> uint32_t {
        if (inSkip && !defined.empty()) return defined[rnd(g, uint32_t(defined.size()))];  // a skipped write must not create a new temp
        if (inSkip) return 0;
        return temps[rnd(g, 16)];
    };
    auto define = [&](uint32_t r) {
        if (r == 0) return;
        for (uint32_t d : defined) if (d == r) return;
        defined.push_back(r);
    };
    // Memory operand: base and an aligned offset that stays inside the process, the stack or the frame.
    auto mem = [&](uint32_t align, uint32_t& base) -> uint32_t {
        const uint32_t k = rnd(g, 3);
        int off;
        if (k == 0) {
            base = S0;
            do { off = 0x60 + int(rnd(g, 0x2A0)); off &= ~int(align - 1); } while (off >= 0xBC && off < 0xC8);  // skip the link fields (0x40-0x5F)
        } else {
            base = k == 1 ? S6 : S7;
            off = slotIndex(g) * 4;
        }
        return uint32_t(off) & 0xFFFF;
    };

    const int body = 2 + int(rnd(g, 12));
    for (int i = 0; i < body; ++i) {
        const uint32_t k = rnd(g, 100);
        if (k < 14) {  // constants
            const uint32_t d = pickDest();
            const uint32_t kind = rnd(g, 3);
            w.push_back(kind == 0 ? iType(0x0F, 0, d, rnd(g, 0x10000)) : kind == 1 ? iType(0x0D, 0, d, rnd(g, 0x10000)) : iType(0x09, 0, d, rnd(g, 0x10000)));
            define(d);
        } else if (k < 38) {  // register ALU
            static const uint32_t fns[] = {0x21, 0x23, 0x24, 0x25, 0x26, 0x27, 0x2A, 0x2B, 0x04, 0x06, 0x07};
            const uint32_t d = pickDest();
            w.push_back(rType(pickDefined(), pickDefined(), d, 0, fns[rnd(g, 11)]));
            define(d);
        } else if (k < 48) {  // immediate shifts
            static const uint32_t fns[] = {0x00, 0x02, 0x03};
            const uint32_t d = pickDest();
            w.push_back(rType(0, pickDefined(), d, rnd(g, 32), fns[rnd(g, 3)]));
            define(d);
        } else if (k < 60) {  // immediate ALU
            static const uint32_t ops[] = {0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E};
            const uint32_t d = pickDest();
            w.push_back(iType(ops[rnd(g, 6)], pickDefined(), d, rnd(g, 0x10000)));
            define(d);
        } else if (k < 72) {  // loads
            static const uint32_t ops[] = {0x23, 0x21, 0x25, 0x20, 0x24};  // lw lh lhu lb lbu
            const uint32_t op = ops[rnd(g, 5)];
            const uint32_t align = op == 0x23 ? 4 : (op == 0x21 || op == 0x25) ? 2 : 1;
            uint32_t base;
            const uint32_t off = mem(align, base);
            const uint32_t d = pickDest();
            w.push_back(iType(op, base, d, off));
            w.push_back(0);  // load delay slot: compiled code never reads the loaded register in the next instruction
            define(d);
        } else if (k < 82) {  // stores
            static const uint32_t ops[] = {0x2B, 0x29, 0x28};  // sw sh sb
            const uint32_t op = ops[rnd(g, 3)];
            uint32_t base;
            const uint32_t off = mem(op == 0x2B ? 4 : op == 0x29 ? 2 : 1, base);
            w.push_back(iType(op, base, pickDefined(), off));
        } else if (k < 88) {  // push or pop on the operand stack
            if (chance(g, 50)) {
                w.push_back(iType(0x2B, S6, pickDefined(), 0));
                w.push_back(iType(0x09, S6, S6, 4));
            } else if (!inSkip) {
                const uint32_t d = temps[rnd(g, 16)];
                w.push_back(iType(0x09, S6, S6, 0xFFFC));
                w.push_back(iType(0x23, S6, d, 0));
                w.push_back(0);  // load delay slot
                define(d);
            }
        } else if (k < 92) {  // flags
            w.push_back(iType(chance(g, 50) ? 0x0D : 0x0C, S1, S1, rnd(g, 0x100) | (chance(g, 30) ? 0x1B : 0)));
        } else if (k < 95) {  // multiply or divide, then read HI/LO
            const uint32_t d = pickDest();
            w.push_back(rType(pickDefined(), pickDefined(), 0, 0, rnd(g, 4) + 0x18));
            w.push_back(rType(0, 0, d, 0, chance(g, 50) ? 0x10 : 0x12));
            define(d);
        } else if (!inSkip && i + 5 < body + 4) {  // a conditional forward branch over a few ops
            const uint32_t skip = 1 + rnd(g, 3);
            const size_t at = w.size();
            w.push_back(0);  // patched below
            w.push_back(0);  // delay slot
            inSkip = true;
            const size_t before = w.size();
            for (uint32_t j = 0; j < skip; ++j) {
                const uint32_t d = defined.empty() ? 0 : defined[rnd(g, uint32_t(defined.size()))];
                uint32_t base;
                const uint32_t off = mem(4, base);
                if (chance(g, 50) && d) w.push_back(rType(pickDefined(), pickDefined(), d, 0, 0x21));
                else w.push_back(iType(0x2B, base, pickDefined(), off));
            }
            inSkip = false;
            const uint32_t count = uint32_t(w.size() - before);
            w[at] = iType(chance(g, 50) ? 0x04 : 0x05, pickDefined(), pickDefined(), count + 1);
        }
    }
    w.push_back(kBlockExit);
    w.push_back(0);
    return w;
}

// ---- programs ----

struct Program {
    std::vector<uint32_t> code;
    std::vector<std::pair<uint32_t, uint32_t>> poolFixups;  // (pool index, value)
    int nativeBlocks = 0;
};

Program genProgram(Rng& g, int n) {
    Program p;
    // Layout first: which items are bytecode and which are native blocks, so jump targets never land inside a block.
    std::vector<std::vector<uint32_t>> blocks(static_cast<size_t>(n));
    std::vector<uint32_t> pos(size_t(n) + 1);
    uint32_t cursor = 0;
    for (int i = 0; i < n; ++i) {
        pos[size_t(i)] = cursor;
        if (chance(g, 9)) { blocks[size_t(i)] = genBlock(g); cursor += 1 + uint32_t(blocks[size_t(i)].size()); ++p.nativeBlocks; }
        else cursor += 1;
    }
    pos[size_t(n)] = cursor;  // the first padding RET

    auto target = [&](int i) -> uint32_t {  // an instruction start after item i, close enough for a 10-bit branch offset
        for (int tries = 0; tries < 8; ++tries) {
            const int j = i + 1 + int(rnd(g, uint32_t(n - i)));
            if (pos[size_t(j)] - pos[size_t(i)] < 400) return pos[size_t(j)];
        }
        return pos[size_t(i) + 1];
    };

    int calrPool = 4;
    static const uint32_t svcA[] = {40, 41, 48, 58, 63, 64, 65, 66, 67, 70, 72, 74, 75, 76, 77};
    static const uint32_t svcB[] = {53, 56, 57, 60, 68, 69};

    for (int i = 0; i < n; ++i) {
        if (!blocks[size_t(i)].empty()) {
            p.code.push_back(kMipsMarker);
            for (uint32_t w : blocks[size_t(i)]) p.code.push_back(w);
            continue;
        }
        const bool hasLater = i < n - 1;
        const uint32_t here = pos[size_t(i)];
        const uint32_t k = rnd(g, 100);
        uint32_t w;
        if (k < 28) {
            w = word(rnd(g, 17), srcRef(g), srcRef(g));  // ADD .. RAND
        } else if (k < 38) {
            static const uint32_t ops[] = {17, 18, 23, 25, 39, 43, 44};
            w = word(ops[rnd(g, 7)], srcRef(g), dstRef(g));
        } else if (k < 42) {
            w = word(chance(g, 70) ? 20 : 46, srcRef(g, true), dstRef(g));
        } else if (k < 46) {
            w = chance(g, 60) ? word(22, srcRef(g, true), srcRef(g)) : word(38, srcRef(g, true), srcRef(g, true));
        } else if (k < 50) {
            static const uint32_t ops[] = {19, 34, 37};
            const uint32_t op = ops[rnd(g, 3)];
            w = word(op, chance(g, 12) ? 0xBF0u : srcRef(g), op == 19 ? rwRef(g) : srcRef(g));
        } else if (k < 54) {
            static const uint32_t ops[] = {21, 27, 29, 30, 33};
            w = word(ops[rnd(g, 5)], srcRef(g), srcRef(g));
        } else if (k < 57) {
            w = chance(g, 50) ? word(31, rnd(g, 4096), imm9(g, -60, 60)) : word(32, srcRef(g), imm9(g, -60, 60));
        } else if (k < 60) {
            w = word(chance(g, 50) ? 42 : 78, imm9(g, 0, 200), rnd(g, 4));  // base: array pointer in the own pool
        } else if (k < 63) {  // collision halfword access through a link (never link 0, the object itself): fields at bits 12-14 and 15-20
            const uint32_t bits = ((1 + rnd(g, 7)) << 12) | (rnd(g, 63) << 15);
            w = chance(g, 50) ? ((35u << 24) | bits) : ((36u << 24) | bits | (srcRef(g) & 0xFFF));
        } else if (k < 66) {
            w = word(45, srcRef(g), srcRef(g, true));  // ATAN
        } else if (k < 68) {
            w = (24u << 24) | (safeField(8 + rnd(g, 56)) << 18) | (chance(g, 50) ? 0x4000u : 0u) | (chance(g, 10) ? 0x3FFFu : rnd(g, 64));  // SETC
        } else if (k < 71) {
            w = word(26, rnd(g, 4096), rnd(g, 4096));  // CPAD
        } else if (k < 80 && hasLater) {  // forward branches
            const uint32_t op = 50 + rnd(g, 3);
            const uint32_t off = target(i) - (here + 1);
            w = (op << 24) | (rnd(g, 64) << 14) | (rnd(g, 4) << 10) | off;
        } else if (k < 84) {  // service calls
            const uint32_t sub = rnd(g, 6);
            if (sub == 0) w = (svcA[rnd(g, 15)] << 24) | rnd(g, 0x1000000);
            else if (sub == 1) w = (svcB[rnd(g, 6)] << 24) | rnd(g, 0x1000000);
            else if (sub == 2 && hasLater) w = ((54u + rnd(g, 2)) << 24) | (rnd(g, 64) << 14) | (rnd(g, 4) << 10);  // GNEZ / GEQZ
            else if (sub == 3) w = ((61u + rnd(g, 2)) << 24) | rnd(g, 0x1000000);  // RJEV / ACEV
            else if (sub == 4) w = (28u << 24) | rnd(g, 0x1000000);                // MISC
            else w = (svcA[rnd(g, 15)] << 24) | rnd(g, 0x1000000);
        } else if (k < 90 && hasLater) {  // CALL a later instruction of this program
            w = (59u << 24) | (rnd(g, 4) << 20) | (chance(g, 25) ? 0x4000u : 0u) | target(i);
        } else if (k < 93 && hasLater && calrPool < 10) {  // CALR through a code pointer kept in the own pool
            p.poolFixups.push_back({uint32_t(calrPool), kCode + 4 * target(i)});
            w = word(71, imm9(g, 0, 3), uint32_t(calrPool));
            ++calrPool;
        } else if (k < 95) {
            w = 47u << 24;  // NOP
        } else if (k < 96) {
            w = 49u << 24;  // RET before the end
        } else {
            w = word(rnd(g, 17), srcRef(g), srcRef(g));
        }
        p.code.push_back(w);
    }
    for (int i = 0; i < 4; ++i) p.code.push_back(49u << 24);  // RET, padded so a skipped instruction cannot run off the end
    return p;
}

// ---- memory image ----

using Writes = std::vector<std::pair<uint32_t, uint32_t>>;

Writes buildImage(Rng& g, const Program& prog) {
    Writes w;
    auto put = [&](uint32_t a, uint32_t v) { w.push_back({a, v}); };
    auto fill = [&](uint32_t a, uint32_t bytes) { for (uint32_t o = 0; o < bytes; o += 4) put(a + o, g()); };

    fill(kProc0 + 0x40, 0x800);
    for (uint32_t p : kOtherProc) fill(p + 0x40, 0x800);
    fill(0x80120000, 0x4000);
    fill(kOwnPool, 0x400);
    fill(kLinkPool, 0x400);

    put(kProc0 + 0x0C, kOwnEntry);
    put(kProc0 + 0x10, kOwnEntry);
    put(kProc0 + 0x14, kLinkEntry);
    for (uint32_t e : {kOwnEntry, kLinkEntry}) {
        put(e + 0x14, kCode);
        put(e + 0x24, kAnimTable);
    }
    put(kOwnEntry + 0x18, kOwnPool);
    put(kLinkEntry + 0x18, kLinkPool);

    // Pool words 0..3 hold array pointers; the program may add code pointers at 4..9.
    for (uint32_t i = 0; i < 4; ++i) { put(kOwnPool + 4 * i, kArrays + 0x400 * i); put(kLinkPool + 4 * i, kArrays + 0x400 * i); }
    for (auto [idx, v] : prog.poolFixups) put(kOwnPool + 4 * idx, v);

    // links: self, parent, brother, child, creator, player, collider, interrupter
    put(kProc0 + 0x40, kProc0);
    for (uint32_t l = 1; l < 8; ++l) put(kProc0 + 0x40 + 4 * l, chance(g, 20) ? 0u : kOtherProc[rnd(g, 4)]);

    put(kProc0 + 0xBC, kFrame + 0xC);  // field 0x1F: operand stack pointer
    put(kProc0 + 0xC0, kCode);         // field 0x20: instruction pointer
    put(kProc0 + 0xC4, kFrame);        // field 0x21: frame pointer
    put(kFrame + 0, 0);
    put(kFrame + 4, 0);
    put(kFrame + 8, 0);                // an empty saved-frame word marks the outermost frame

    for (uint32_t i = 0; i < prog.code.size(); ++i) put(kCode + 4 * i, prog.code[i]);

    // scratchpad: engine variables used by VEL (0x54), SYNC (0x50), GVAR/GVAW (0x58) and the dispatch table (0x5C)
    for (uint32_t o = 0; o < 0x40; o += 4) put(0x1F800000 + o, g());
    put(0x1F800048, g() & 0xFFFF);
    put(0x1F800050, g() % 0x2000);
    put(0x1F800054, 0x100 + g() % 0x2000);
    put(0x1F800058, kArrays + 0x1000);
    put(0x1F80005C, kOpTable);
    put(kRandState, g());
    put(kRandState + 4, g());
    return w;
}

// ---- service stub, shared by both runs ----

int arity(cb2::Svc s) {
    switch (s) {
        case cb2::Svc::Go: case cb2::Svc::Anim: case cb2::Svc::Anif: case cb2::Svc::Sevt: case cb2::Svc::Cevt:
        case cb2::Svc::Bevt: case cb2::Svc::Rjev: return 4;
        default: return 2;
    }
}

struct Call { int svc; uint32_t a[4]; uint32_t result; bool operator==(const Call& o) const { return svc == o.svc && result == o.result && !memcmp(a, o.a, sizeof a); } };

// State right after a native block returns to the interpreter: s1..s7 and a hash of the memory the block can touch.
struct BlockState {
    uint32_t s[7];
    uint32_t memHash;
    bool operator==(const BlockState& o) const { return !memcmp(s, o.s, sizeof s) && memHash == o.memHash; }
};

uint32_t hashState(const uint8_t* ram, const uint8_t* scratch) {
    uint32_t h = 2166136261u;
    for (uint32_t i = 0x100000; i < 0x101000; ++i) h = (h ^ ram[i]) * 16777619u;
    for (uint32_t i = 0; i < 0x100; ++i) h = (h ^ scratch[i]) * 16777619u;
    return h;
}

struct TraceLine { uint32_t pc; uint32_t ins; uint32_t r[32]; };

// Bit mask of the general registers an instruction reads (integer ISA subset the blocks use).
uint32_t readMask(uint32_t w) {
    const uint32_t op = w >> 26, rs = (w >> 21) & 31, rt = (w >> 16) & 31, fn = w & 63;
    auto bit = [](uint32_t r) { return r ? (1u << r) : 0u; };
    switch (op) {
        case 0x00:
            if (fn == 0x00 || fn == 0x02 || fn == 0x03) return bit(rt);
            if (fn == 0x08 || fn == 0x09 || fn == 0x11 || fn == 0x13) return bit(rs);
            if (fn == 0x10 || fn == 0x12) return 0;
            return bit(rs) | bit(rt);
        case 0x04: case 0x05: case 0x28: case 0x29: case 0x2A: case 0x2B: case 0x2E: return bit(rs) | bit(rt);
        case 0x22: case 0x26: return bit(rs) | bit(rt);
        case 0x0F: case 0x02: case 0x03: return 0;
        default: return bit(rs);
    }
}

struct Stub : cb2::Services {
    cb2::Bus* bus = nullptr;
    uint64_t seed = 0;
    std::vector<Call> log;
    std::vector<BlockState> blocks;
    int traceBlock = -1;                 // native block index to trace instruction by instruction, or -1
    std::vector<TraceLine>* trace = nullptr;

    uint32_t call(cb2::Svc s, uint32_t a0, uint32_t a1, uint32_t a2, uint32_t a3) override {
        Call c{int(s), {a0, a1, 0, 0}, 0};
        if (arity(s) == 4) { c.a[2] = a2; c.a[3] = a3; }
        const uint32_t h = mix(seed, log.size(), uint32_t(s));
        const bool procService = s != cb2::Svc::Cpad && s != cb2::Svc::Atan && s != cb2::Svc::Op46;
        if (procService) {
            const uint32_t proc = a0;
            switch (h & 3) {
                case 1: bus->wr32(proc + 0xBC, bus->rd32(proc + 0xBC) - 4); break;
                case 2: bus->wr32(proc + 0xBC, bus->rd32(proc + 0xBC) + 4); break;
                case 3: bus->wr32(proc + 0xBC, bus->rd32(proc + 0xBC) - 8); break;
                default: break;
            }
            if (((h >> 4) % 5) == 0) bus->wr32(proc + 0xC0, bus->rd32(proc + 0xC0) + 4);
            if (s == cb2::Svc::Misc && ((h >> 8) % 3) == 0) bus->wr32(proc + 0x10, kLinkEntry);
        }
        if (s == cb2::Svc::Rjev) bus->wr32(0x1F800048, a2 ^ (h & 0xFFFF));
        switch (s) {
            case cb2::Svc::Cpad: case cb2::Svc::Atan: c.result = h; break;
            case cb2::Svc::Go: case cb2::Svc::Anim: case cb2::Svc::Anif: case cb2::Svc::Sevt: case cb2::Svc::Cevt:
            case cb2::Svc::Bevt: case cb2::Svc::Rjev: c.result = ((h >> 12) % 8 == 0) ? (h & 0xFF) + 1 : 0; break;
            default: c.result = 0; break;
        }
        log.push_back(c);
        return c.result;
    }

    bool runNative(cb2::Bus& b, cb2::VmRegs& regs, uint32_t) override {
        auto& mem = dynamic_cast<cb2::MemBus&>(b);
        cb2::NativeStepHook hook = [this, &mem](const uint32_t* r, uint32_t pc) {
            TraceLine t;
            t.pc = pc;
            t.ins = mem.rd32(pc);
            std::memcpy(t.r, r, sizeof t.r);
            trace->push_back(t);
        };
        const bool traced = trace && traceBlock == int(blocks.size());
        cb2::runNativeBlockR3000(mem, regs, 2000000, traced ? &hook : nullptr);
        BlockState st{{regs.s1, regs.s2, regs.s3, regs.s4, regs.s5, regs.s6, regs.s7}, hashState(mem.ram(), mem.scratch())};
        blocks.push_back(st);
        return true;
    }
};

struct CpuBus : cb2::Bus {
    r3000::Cpu& c;
    explicit CpuBus(r3000::Cpu& cpu) : c(cpu) {}
    uint32_t rd32(uint32_t a) override { return c.rd32(a); }
    void wr32(uint32_t a, uint32_t v) override { c.wr32(a, v); }
    uint32_t rd16(uint32_t a) override { return c.rd16(a); }
    void wr16(uint32_t a, uint32_t v) override { c.wr16(a, v); }
};

enum class Outcome { Ok, Skip };

struct Run {
    Outcome outcome = Outcome::Ok;
    int32_t result = 0;
    std::vector<Call> log;
    std::vector<BlockState> blocks;
    std::string why;
    uint8_t scratch[0x100] = {};
    cb2::RunStats stats;
};

constexpr int kServiceCount = int(cb2::Svc::Mips);

void loadExe(const cb2::Bytes& exe, uint8_t* ram) {
    uint32_t textAddr, textSize;
    std::memcpy(&textAddr, &exe[0x18], 4);
    std::memcpy(&textSize, &exe[0x1C], 4);
    std::memcpy(ram + (textAddr & 0x1FFFFF), &exe[0x800], textSize);
}

struct StepState { uint32_t ip, hash; };

// Debug aid: reports the first write into the program's code region and the bytecode instruction that made it.
struct WatchBus : cb2::MemBus {
    using cb2::MemBus::MemBus;
    uint32_t ip = 0;
    bool armed = false;  // only after the memory image is loaded, and only in the debug re-run
    bool hit = false, hitLink = false;
    void note(const char* what, uint32_t a, uint32_t v) {
        const uint32_t p = a & 0x1FFFFFFF;
        const uint32_t links = (kProc0 + 0x40) & 0x1FFFFFFF;
        if (armed && !hitLink && p >= links && p < links + 0x20) {
            hitLink = true;
            std::printf("  watch: %s of %08x to link field %u (%08x) while executing the instruction at %08x (word %u)\n", what, v, (p - links) / 4, a, ip, (ip - kCode) / 4);
        }
        if (!armed || hit || p < (kCode & 0x1FFFFFFF) || p >= (kCode & 0x1FFFFFFF) + 0x400) return;
        hit = true;
        std::printf("  watch: %s of %08x to %08x while executing the instruction at %08x (word %u)\n", what, v, a, ip, (ip - kCode) / 4);
    }
    void wr32(uint32_t a, uint32_t v) override { note("write", a, v); cb2::MemBus::wr32(a, v); }
    void wr16(uint32_t a, uint32_t v) override { note("halfword write", a, v); cb2::MemBus::wr16(a, v); }
};

Run runOracle(const cb2::Bytes& exe, std::vector<uint8_t>& ram, const Writes& image, uint32_t flags, uint64_t seed,
              int traceBlock = -1, std::vector<TraceLine>* trace = nullptr, std::vector<StepState>* steps = nullptr) {
    Run run;
    r3000::Cpu cpu;
    cpu.ram = ram.data();
    loadExe(exe, ram.data());
    for (auto [a, v] : image) cpu.wr32(a, v);

    CpuBus bus(cpu);
    Stub stub;
    stub.bus = &bus;
    stub.seed = seed;
    std::map<uint32_t, cb2::Svc> targets;
    for (int i = 0; i < kServiceCount; ++i) targets[cb2::svcAddress(cb2::Svc(i))] = cb2::Svc(i);
    std::vector<BlockState> blockTrace;
    bool armed = false, tracing = false;
    cpu.intercept = [&](r3000::Cpu& c, uint32_t pc) {
        if (pc == 0x8003A06C && steps) steps->push_back({c.r[21], hashState(c.ram, c.scratch)});  // the interpreter's dispatch loop head
        if (pc == 0x8003AAD8) armed = true;  // the delay slot of `jalr s5`: the next instruction is the block's first
        else if (armed) { armed = false; tracing = traceBlock >= 0 && int(blockTrace.size()) == traceBlock; }
        if (tracing && trace && pc != cb2::kNativeReturnAddress) {
            TraceLine t;
            t.pc = pc;
            t.ins = c.rd32(pc);
            std::memcpy(t.r, c.r, sizeof t.r);
            trace->push_back(t);
        }
        if (pc == cb2::kNativeReturnAddress) {  // a native block just returned to the interpreter
            tracing = false;
            blockTrace.push_back({{c.r[17], c.r[18], c.r[19], c.r[20], c.r[21], c.r[22], c.r[23]}, hashState(c.ram, c.scratch)});
            return false;
        }
        const auto it = targets.find(pc);
        if (it == targets.end()) return false;
        c.r[2] = stub.call(it->second, c.r[4], c.r[5], c.r[6], c.r[7]);
        c.pc = c.r[31];
        c.nextPc = c.pc + 4;
        return true;
    };

    cpu.r[4] = kProc0;
    cpu.r[5] = flags;
    cpu.r[6] = kOut;
    cpu.r[29] = 0x801FFFF0;
    cpu.r[31] = kSentinel;
    cpu.reset(0x8003A014);
    try {
        for (uint64_t n = 0; cpu.pc != kSentinel; ++n) {
            if (n > 3000000) { run.outcome = Outcome::Skip; run.why = "oracle timeout"; return run; }
            cpu.step();
        }
    } catch (const std::exception& ex) {
        run.outcome = Outcome::Skip;
        run.why = std::string("oracle fault: ") + ex.what();
        return run;
    }
    run.result = int32_t(cpu.r[2]);
    run.log = stub.log;
    run.blocks = blockTrace;
    std::memcpy(run.scratch, cpu.scratch, sizeof run.scratch);
    return run;
}

Run runVm(const cb2::Bytes& exe, std::vector<uint8_t>& ram, const Writes& image, uint32_t flags, uint64_t seed,
          int traceBlock = -1, std::vector<TraceLine>* trace = nullptr, std::vector<StepState>* steps = nullptr) {
    Run run;
    loadExe(exe, ram.data());
    WatchBus bus(ram.data());
    for (auto [a, v] : image) bus.wr32(a, v);
    bus.armed = steps != nullptr;
    Stub stub;
    stub.bus = &bus;
    stub.seed = seed;
    stub.traceBlock = traceBlock;
    stub.trace = trace;
    if (steps) run.stats.onStep = [&](uint32_t ip) { bus.ip = ip; steps->push_back({ip, hashState(bus.ram(), bus.scratch())}); };
    try {
        run.result = cb2::goolRun(bus, stub, kProc0, flags, kOut, &run.stats, 2000000);
    } catch (const std::exception& ex) {
        run.outcome = Outcome::Skip;
        run.why = std::string("vm exception: ") + ex.what();
    }
    run.log = stub.log;
    run.blocks = stub.blocks;
    std::memcpy(run.scratch, bus.scratch(), sizeof run.scratch);
    return run;
}

struct Region { uint32_t phys, size; };

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) { std::fprintf(stderr, "usage: cb2_vmdiff <SCUS_941.54> [cases] [seed] [-v]\n"); return 2; }
    const int cases = argc > 2 ? std::atoi(argv[2]) : 1000;
    const uint64_t seed = argc > 3 ? std::strtoull(argv[3], nullptr, 10) : 1;
    bool verbose = false;
    for (int i = 4; i < argc; ++i) if (!std::strcmp(argv[i], "-v")) verbose = true;

    const cb2::Bytes exe = cb2::loadFile(argv[1]);
    if (exe.size() < 0x800 || std::memcmp(exe.data(), "PS-X EXE", 8) != 0) { std::fprintf(stderr, "not a PS-X EXE\n"); return 2; }

    int compared = 0, skippedOracle = 0, skippedVm = 0, skippedSelfModified = 0, mismatches = 0, withNative = 0;
    std::map<std::string, int> skipReasons;
    std::map<int32_t, int> returns;
    uint64_t executed[128] = {};
    size_t serviceCalls = 0;

    for (int c = 0; c < cases; ++c) {
        Rng g(uint32_t(mix(seed, uint64_t(c), 7)));
        const Program prog = genProgram(g, 5 + int(rnd(g, 36)));
        const Writes image = buildImage(g, prog);
        const uint32_t flags = chance(g, 30) ? 0u : (g() & 0xFFFF003Bu);
        const uint64_t stubSeed = mix(seed, uint64_t(c), 99);

        std::vector<uint8_t> ramA(r3000::kRamSize, 0), ramB(r3000::kRamSize, 0);
        const Run a = runOracle(exe, ramA, image, flags, stubSeed);
        const Run b = runVm(exe, ramB, image, flags, stubSeed);

        if (a.outcome == Outcome::Skip) { ++skippedOracle; ++skipReasons[a.why.substr(0, 40)]; continue; }
        if (b.outcome == Outcome::Skip) {
            ++skippedVm;
            std::printf("case %d: oracle ran but the VM failed: %s\n", c, b.why.c_str());
            ++mismatches;
            continue;
        }
        {   // A program that overwrote its own code (through a wild pointer) is not a meaningful comparison:
            // the blocks it then runs read stale registers that differ between the two cores by construction.
            bool modified = false;
            for (size_t i = 0; i < prog.code.size() && !modified; ++i) {
                uint32_t x, y;
                std::memcpy(&x, ramA.data() + 0x103000 + 4 * i, 4);
                std::memcpy(&y, ramB.data() + 0x103000 + 4 * i, 4);
                modified = x != prog.code[i] || y != prog.code[i];
            }
            if (modified) { ++skippedSelfModified; continue; }
        }
        ++compared;
        if (prog.nativeBlocks) ++withNative;
        for (int o = 0; o < 128; ++o) executed[o] += b.stats.opCounts[o];
        ++returns[a.result];
        serviceCalls += a.log.size();

        std::string problem;
        int firstBlock = -1;
        {
            const size_t common = std::min(a.blocks.size(), b.blocks.size());
            for (size_t i = 0; i < common; ++i) {
                if (a.blocks[i] == b.blocks[i]) continue;
                firstBlock = int(i);
                char buf[256];
                std::snprintf(buf, sizeof buf, " first native block to differ: #%zu (s1 %08x/%08x s5 %08x/%08x s6 %08x/%08x s7 %08x/%08x mem %08x/%08x);", i,
                              a.blocks[i].s[0], b.blocks[i].s[0], a.blocks[i].s[4], b.blocks[i].s[4], a.blocks[i].s[5], b.blocks[i].s[5],
                              a.blocks[i].s[6], b.blocks[i].s[6], a.blocks[i].memHash, b.blocks[i].memHash);
                problem += buf;
                break;
            }
            if (a.blocks.size() != b.blocks.size()) problem += " native block count " + std::to_string(a.blocks.size()) + " vs " + std::to_string(b.blocks.size()) + ";";
        }
        if (a.result != b.result) problem += " return " + std::to_string(a.result) + " vs " + std::to_string(b.result) + ";";
        if (a.log.size() != b.log.size()) problem += " service log length " + std::to_string(a.log.size()) + " vs " + std::to_string(b.log.size()) + ";";
        else for (size_t i = 0; i < a.log.size(); ++i) if (!(a.log[i] == b.log[i])) {
            char buf[160];
            std::snprintf(buf, sizeof buf, " service call %zu differs (%s: %08x %08x %08x %08x -> %08x vs %s: %08x %08x %08x %08x -> %08x);", i,
                          cb2::svcName(cb2::Svc(a.log[i].svc)), a.log[i].a[0], a.log[i].a[1], a.log[i].a[2], a.log[i].a[3], a.log[i].result,
                          cb2::svcName(cb2::Svc(b.log[i].svc)), b.log[i].a[0], b.log[i].a[1], b.log[i].a[2], b.log[i].a[3], b.log[i].result);
            problem += buf;
            break;
        }
        const Region regions[] = {{0x0, 0x1000}, {0x100000, 0x40000}, {0x5F29C, 8}};
        for (const Region& r : regions) {
            if (!std::memcmp(ramA.data() + r.phys, ramB.data() + r.phys, r.size)) continue;
            char buf[128];
            int shown = 0;
            for (uint32_t off = 0; off < r.size && shown < 6; off += 4) {
                uint32_t x, y;
                std::memcpy(&x, ramA.data() + r.phys + off, 4);
                std::memcpy(&y, ramB.data() + r.phys + off, 4);
                if (x == y) continue;
                std::snprintf(buf, sizeof buf, " memory %08x: oracle %08x, vm %08x;", 0x80000000u + r.phys + off, x, y);
                problem += buf;
                ++shown;
            }
            break;
        }
        if (std::memcmp(a.scratch, b.scratch, sizeof a.scratch)) {
            uint32_t off = 0;
            while (a.scratch[off] == b.scratch[off]) ++off;
            char buf[128];
            std::snprintf(buf, sizeof buf, " scratchpad differs at %08x (oracle %02x, vm %02x);", 0x1F800000u + off, a.scratch[off], b.scratch[off]);
            problem += buf;
        }

        if (!problem.empty()) {
            ++mismatches;
            if (mismatches <= 6 || verbose) {
                std::printf("case %d (seed %llu):%s\n", c, (unsigned long long)seed, problem.c_str());
                for (size_t i = 0; i + 4 < prog.code.size(); ++i)
                    std::printf("    %02zu: %08x  %s\n", i, prog.code[i], cb2::disassemble(prog.code[i]).c_str());
                {   // lockstep: the first bytecode instruction after which memory (or the next ip) differs
                    std::vector<StepState> sa, sb;
                    std::vector<uint8_t> ra(r3000::kRamSize, 0), rb(r3000::kRamSize, 0);
                    runOracle(exe, ra, image, flags, stubSeed, -1, nullptr, &sa);
                    runVm(exe, rb, image, flags, stubSeed, -1, nullptr, &sb);
                    for (size_t i = 0; i < std::min(sa.size(), sb.size()); ++i) {
                        if (sa[i].ip == sb[i].ip && sa[i].hash == sb[i].hash) continue;
                        const size_t prev = i ? i - 1 : 0;
                        const uint32_t pip = sb[prev].ip;
                        const uint32_t idx = (pip - kCode) / 4;
                        std::printf("  lockstep: step %zu differs (ip %08x/%08x); the instruction before it ran at %08x (word %02u): %08x  %s\n", i, sa[i].ip, sb[i].ip,
                                    pip, idx, idx < prog.code.size() ? prog.code[idx] : 0, idx < prog.code.size() ? cb2::disassemble(prog.code[idx]).c_str() : "?");
                        break;
                    }
                }
                if (firstBlock >= 0 && mismatches <= 2) {
                    std::vector<TraceLine> ta, tb;
                    std::vector<uint8_t> ra(r3000::kRamSize, 0), rb(r3000::kRamSize, 0);
                    runOracle(exe, ra, image, flags, stubSeed, firstBlock, &ta);
                    runVm(exe, rb, image, flags, stubSeed, firstBlock, &tb);
                    std::printf("  trace of native block #%d: oracle %zu instructions, runner %zu\n", firstBlock, ta.size(), tb.size());
                    for (size_t i = 0; i < std::min<size_t>(std::min(ta.size(), tb.size()), 12); ++i) {
                        uint32_t wa = 0, wb = 0;
                        const uint32_t addr = (ta[i].r[22] - 4) & 0x1FFFFFFF;
                        std::memcpy(&wa, ra.data() + (addr & 0x1FFFFF), 4);
                        std::memcpy(&wb, rb.data() + (addr & 0x1FFFFF), 4);
                        std::printf("    #%zu pc %08x/%08x word %08x/%08x s6 %08x/%08x t9 %08x/%08x t1 %08x/%08x\n", i, ta[i].pc, tb[i].pc, ta[i].ins, tb[i].ins,
                                    ta[i].r[22], tb[i].r[22], ta[i].r[25], tb[i].r[25], ta[i].r[9], tb[i].r[9]);
                        (void)wa; (void)wb;
                    }
                    for (size_t i = 0; i < std::min(ta.size(), tb.size()); ++i) {
                        const uint32_t mask = readMask(ta[i].ins);
                        bool differs = ta[i].pc != tb[i].pc;
                        for (int r = 1; r < 32; ++r) if ((mask >> r & 1) && ta[i].r[r] != tb[i].r[r]) differs = true;
                        if (!differs) continue;
                        std::printf("  first instruction that reads a differing register: #%zu at %08x, word %08x\n", i, ta[i].pc, ta[i].ins);
                        for (int r = 1; r < 32; ++r)
                            if ((mask >> r & 1) && ta[i].r[r] != tb[i].r[r]) std::printf("    r%-2d oracle %08x runner %08x\n", r, ta[i].r[r], tb[i].r[r]);
                        break;
                    }
                }
            }
        }
    }

    std::printf("%d cases: %d compared (%d with native blocks), %d skipped (oracle fault or timeout), %d skipped (program overwrote its own code), %d VM failures, mismatches: %d\n", cases,
                compared, withNative, skippedOracle, skippedSelfModified, skippedVm, mismatches);
    std::printf("service calls compared: %zu; distinct return values: %zu\n", serviceCalls, returns.size());
    std::string never;
    int executedKinds = 0;
    for (int o = 0; o < cb2::kGoolOpcodeCount; ++o) {
        if (executed[o]) ++executedKinds;
        else never += " " + std::to_string(o);
    }
    std::printf("opcodes executed at least once: %d of %d; never executed:%s\n", executedKinds, cb2::kGoolOpcodeCount, never.empty() ? " none" : never.c_str());
    if (verbose || skippedOracle) for (auto& [why, n] : skipReasons) std::printf("  skipped x%d: %s\n", n, why.c_str());
    return mismatches ? 1 : 0;
}

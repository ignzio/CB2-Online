#include "gool_vm.hpp"

#include <cstdio>
#include <string>

namespace cb2 {
namespace {

constexpr uint32_t kScratch = 0x1F800000;
constexpr uint32_t kSinTable = 0x8005BCB0;  // 1025 halfwords; read from the executable's data
constexpr uint32_t kRandState = 0x8005F29C;

int32_t sx(uint32_t v, int bits) {
    const uint32_t m = 1u << (bits - 1);
    return int32_t(((v & ((1u << bits) - 1)) ^ m) - m);
}

// R3000 divide results, including the cases the manual leaves open.
void divS(uint32_t a, uint32_t b, uint32_t& lo, uint32_t& hi) {
    const int32_t x = int32_t(a), y = int32_t(b);
    if (y == 0) { lo = x < 0 ? 1u : 0xFFFFFFFFu; hi = a; }
    else if (x == INT32_MIN && y == -1) { lo = 0x80000000u; hi = 0; }
    else { lo = uint32_t(x / y); hi = uint32_t(x % y); }
}
void divU(uint32_t a, uint32_t b, uint32_t& lo, uint32_t& hi) {
    if (b == 0) { lo = 0xFFFFFFFFu; hi = a; }
    else { lo = a / b; hi = a % b; }
}

struct Vm {
    Bus& bus;
    Services& svc;
    uint32_t s0, s1, s2, s3 = 0, s4 = 0, s5 = 0, s6 = 0, s7 = 0;

    Vm(Bus& b, Services& s, uint32_t proc, uint32_t flags, uint32_t out) : bus(b), svc(s), s0(proc), s1(flags), s2(out) {}

    uint32_t rd(uint32_t a) { return bus.rd32(a); }
    void wr(uint32_t a, uint32_t v) { bus.wr32(a, v); }
    uint32_t field(uint32_t n) const { return s0 + kProcFieldBase + 4 * n; }

    void loadPools() {
        s3 = rd(rd(s0 + 0x14) + 0x18);
        s4 = rd(rd(s0 + 0x10) + 0x18);
    }

    // ---- operand resolvers (0x80039E80 value, 0x80039D2C address, 0x80039E10 destination) ----

    uint32_t V(uint32_t ref) {
        ref &= 0xFFF;
        if (!(ref & 0x800)) return rd(((ref & 0x400) ? s3 : s4) + (ref & 0x3FF) * 4);
        if (!(ref & 0x400)) {
            if (!(ref & 0x200)) return uint32_t(int32_t(ref << 23) >> 15);
            if (!(ref & 0x100)) return uint32_t(int32_t(ref << 24) >> 20);
            if (!(ref & 0x80)) return rd(s7 + uint32_t(int32_t(ref << 25) >> 23));
            return (ref >> 4) & 1;
        }
        if (ref & 0x200) {
            if ((ref & 0x1FF) == 0x1F) { s6 -= 4; return rd(s6); }
            return rd(field(ref & 0x1FF));
        }
        const uint32_t link = rd(field((ref >> 6) & 7));
        return link ? rd(link + kProcFieldBase + 4 * (ref & 0x3F)) : 0;
    }

    // Address of a ref. Immediates are materialised in the scratchpad at `off`. A constant ref yields the
    // value 0 or 1 in place of an address, and a null link yields 0, exactly as the original does.
    uint32_t P(uint32_t ref, uint32_t off) {
        ref &= 0xFFF;
        if (!(ref & 0x800)) return ((ref & 0x400) ? s3 : s4) + (ref & 0x3FF) * 4;
        if (!(ref & 0x400)) {
            if (!(ref & 0x200)) { wr(kScratch + off, uint32_t(int32_t(ref << 23) >> 15)); return kScratch + off; }
            if (!(ref & 0x100)) { wr(kScratch + off, uint32_t(int32_t(ref << 24) >> 20)); return kScratch + off; }
            if (!(ref & 0x80)) return s7 + uint32_t(int32_t(ref << 25) >> 23);
            return (ref >> 4) & 1;
        }
        if (ref & 0x200) {
            if ((ref & 0x1FF) == 0x1F) { s6 -= 4; return s6; }
            return field(ref & 0x1FF);
        }
        const uint32_t link = rd(field((ref >> 6) & 7));
        return link ? link + kProcFieldBase + 4 * (ref & 0x3F) : 0;
    }

    // Store target. Only the low 11 bits of the operand are examined. Pushing is a destination of field 0x1F.
    uint32_t D(uint32_t ins) {
        const uint32_t ref = ins & 0x7FF;
        if (!(ref & 0x400)) return s7 + uint32_t(int32_t(ref << 25) >> 23);
        if (ref & 0x200) {
            if ((ref & 0x1FF) == 0x1F) { const uint32_t p = s6; s6 += 4; return p; }
            return field(ref & 0x1FF);
        }
        const uint32_t link = rd(field((ref >> 6) & 7));
        return link ? link + kProcFieldBase + 4 * (ref & 0x3F) : 0;
    }

    // Process field by number (0x80039F74 source, 0x80039F9C destination): field 0x1F is the operand stack.
    uint32_t Fsrc(uint32_t n) {
        if (n == 0x1F) { s6 -= 4; return s6; }
        return field(n);
    }
    uint32_t Fdst(uint32_t n) {
        if (n == 0x1F) { const uint32_t p = s6; s6 += 4; return p; }
        return field(n);
    }

    void push(uint32_t v) { s6 += 4; wr(s6 - 4, v); }

    uint32_t sincos(uint32_t a0) {  // 0x8003A9B8
        uint32_t a3 = a0 << 20;
        const bool neg = int32_t(a3) < 0;
        a3 <<= 1;
        const bool mirror = int32_t(a3) < 0;
        a3 >>= 20;
        int32_t v;
        if (mirror) v = int16_t(bus.rd16(kSinTable - a3 + 0x1000));
        else v = int16_t(bus.rd16(kSinTable + a3));
        return uint32_t(neg ? -v : v);
    }

    // ---- exits and service calls ----

    void saveRegs() { wr(s0 + 0xBC, s6); wr(s0 + 0xC4, s7); }
    void reloadRegs() { s5 = rd(s0 + 0xC0); s6 = rd(s0 + 0xBC); s7 = rd(s0 + 0xC4); }
    int32_t leave(int32_t v0) { wr(s0 + 0xC0, s5); wr(s0 + 0xBC, s6); wr(s0 + 0xC4, s7); return v0; }

    // Trampoline 0x80039FC4: call and continue.
    void callA(Svc id, uint32_t ins) {
        saveRegs();
        svc.call(id, s0, ins, 0, 0);
        reloadRegs();
    }
    // Trampoline 0x80039FE4: a non-zero result ends the run.
    bool callB(Svc id, uint32_t ins) {
        saveRegs();
        const uint32_t r = svc.call(id, s0, ins, s1, s2);
        reloadRegs();
        retval = int32_t(r);
        return r != 0;
    }
    int32_t retval = 0;

    // ---- CALL machinery ----

    void pushFrame(uint32_t argc) {
        wr(s6, s1);
        uint32_t a2 = s6 - (argc << 2) - s0 - 0x40;
        uint32_t a3 = (s7 - s0 - 0x40) << 16;
        wr(s6 + 4, s5);
        a2 |= a3;
        s7 = s6;
        wr(s6 + 8, a2);
        s6 += 0xC;
    }

    // RET (0x8003ABE8). Returns true when the run ends, with the interpreter's return value in `result`.
    bool doRet(int32_t& result) {
        uint32_t v1 = 0;  // the original reads a stale v1 on the outermost frame, but that path always returns before using it
        int32_t v0 = kRetOutermost;
        const uint32_t a2 = rd(s7 + 8);
        const uint32_t a3 = a2 >> 16;
        if (a3 != 0) {
            s6 = (a2 & 0xFFFF) + 0x40 + s0;
            s5 = rd(s7 + 4);
            const uint32_t saved = bus.rd16(s7);
            s7 = a3 + 0x40 + s0;
            v1 = (s1 & 0xFFFF0000u) | saved;
            v0 = -0xFF;
        }
        if (s1 & 8) {
            if (s1 & 0x20) { wr(s2, 0xFF); result = -0xFF; return true; }
            result = -0x1A;
            return true;
        }
        const uint32_t a0 = (s1 & 1) | ((-0xFF < v0) ? 1u : 0u);
        if (a0 != 0) {
            result = v0;
            if (!(s1 & 0x10)) return true;
            if (!(v0 < -0xFE)) return true;
            push(0);
            return true;
        }
        if ((v1 & 0xFFFF) != 0xFFFF) s1 = v1;
        return false;
    }

    int32_t run(uint64_t maxInstructions, RunStats* stats) {
        s5 = rd(s0 + 0xC0);
        s6 = rd(s0 + 0xBC);
        s7 = rd(s0 + 0xC4);
        loadPools();

        for (uint64_t n = 0;; ++n) {
            if (n >= maxInstructions) throw std::runtime_error("GOOL VM: instruction budget exceeded");
            if (stats) stats->instructions = n + 1;
            if (stats && stats->onStep) stats->onStep(s5);
            const uint32_t ins = rd(s5);
            s5 += 4;
            wr(s0 + 0xC0, s5);
            const uint32_t op = ins >> 24;
            if (stats) ++stats->opCounts[op & 127];
            const uint32_t hiRef = ins >> 12, loRef = ins;

            switch (op) {
                // ---- arithmetic: the high operand is resolved first ----
                case 0: { const uint32_t r = V(hiRef), l = V(loRef); push(l + r); break; }
                case 1: { const uint32_t r = V(hiRef), l = V(loRef); push(l - r); break; }
                case 2: { const uint32_t r = V(hiRef), l = V(loRef); push(uint32_t(int32_t(l)) * uint32_t(int32_t(r))); break; }
                case 3: { const uint32_t r = V(hiRef), l = V(loRef); uint32_t lo, hi; divS(l, r, lo, hi); push(lo); break; }
                case 4: { const uint32_t r = V(hiRef), l = V(loRef); push((l ^ r) == 0 ? 1u : 0u); break; }
                case 5: { const uint32_t r = V(hiRef), l = V(loRef); push(r != 0 ? l : r); break; }
                case 6:
                case 8: { const uint32_t r = V(hiRef), l = V(loRef); push(l | r); break; }
                case 7: { const uint32_t r = V(hiRef), l = V(loRef); push(l & r); break; }
                case 9: { const uint32_t r = V(hiRef), l = V(loRef); push(int32_t(l) < int32_t(r) ? 1u : 0u); break; }
                case 10: { const uint32_t r = V(hiRef), l = V(loRef); push(int32_t(r) < int32_t(l) ? 0u : 1u); break; }
                case 11: { const uint32_t r = V(hiRef), l = V(loRef); push(int32_t(r) < int32_t(l) ? 1u : 0u); break; }
                case 12: { const uint32_t r = V(hiRef), l = V(loRef); push(int32_t(l) < int32_t(r) ? 0u : 1u); break; }
                case 13: { const uint32_t r = V(hiRef), l = V(loRef); uint32_t lo, hi; divS(l, r, lo, hi); push(hi); break; }
                case 14: { const uint32_t r = V(hiRef), l = V(loRef); push(l ^ r); break; }
                case 15: { const uint32_t r = V(hiRef), l = V(loRef); push(((l & r) ^ r) == 0 ? 1u : 0u); break; }
                case 16: {  // RAND
                    const uint32_t r = V(hiRef), l = V(loRef);
                    uint32_t a1 = rd(kRandState + 4), a0 = rd(kRandState);
                    uint32_t a2 = a1 << 24;
                    a1 >>= 8;
                    uint32_t a3 = a0 + a1;
                    a0 >>= 8;
                    a0 |= a2;
                    a2 = a0 >> 8;
                    a3 += a2;
                    a3 <<= 24;
                    a1 |= a3;
                    a2 = r - l;
                    uint32_t lo, hi;
                    divU(a1, a2, lo, hi);
                    wr(kRandState + 4, a1);
                    wr(kRandState, a0);
                    push(hi + l);
                    break;
                }
                case 17: { const uint32_t r = V(hiRef); const uint32_t d = D(ins); wr(d, r); break; }
                case 18: { const uint32_t r = V(hiRef); const uint32_t d = D(ins); wr(d, r == 0 ? 1u : 0u); break; }
                case 23: { const uint32_t r = V(hiRef); const uint32_t d = D(ins); wr(d, ~r); break; }
                case 19: {  // LOOP: step a value toward/around a limit (0x8003A7CC)
                    uint32_t v1l, a3;
                    if ((hiRef & 0xFFF) == 0xBF0) { a3 = rd(s6 - 4); s6 -= 4; v1l = rd(s6 - 4); s6 -= 4; }
                    else { v1l = V(hiRef); a3 = 0x100; }
                    const uint32_t ptr = P(loRef, 0x18);
                    uint32_t a2 = rd(ptr), a0 = a2 - v1l, ret;
                    if (int32_t(v1l) >= 0) {
                        a0 += a3;
                        const uint32_t keep = a2 + a3;
                        if (int32_t(a0) < 0) a2 = keep; else a2 = (a3 << 1) - v1l;
                        ret = int32_t(a2) >= 0 ? a2 : uint32_t(-int32_t(a2));
                    } else {
                        a0 -= a3;
                        a2 -= a3;
                        if (!(int32_t(a0) > 0)) a2 = uint32_t(-int32_t(v1l)) - a3 - a3;
                        ret = int32_t(a2) <= 0 ? a2 : uint32_t(-int32_t(a2));
                    }
                    wr(ptr, a2);
                    push(ret);
                    break;
                }
                case 20: { const uint32_t p = P(hiRef, 0x1C); const uint32_t d = D(ins); wr(d, p); break; }
                case 21: {
                    const uint32_t r = V(hiRef), l = V(loRef);
                    const uint32_t out = int32_t(r) >= 0 ? (l << (r & 31)) : uint32_t(int32_t(l) >> ((0u - r) & 31));
                    push(out);
                    break;
                }
                case 22: {  // PUSH value, then the word the address points at
                    const uint32_t p = P(hiRef, 0x1C);
                    const uint32_t l = V(loRef);
                    uint32_t extra = 0;
                    if (p != 0) extra = rd(p);
                    push(l);
                    if (p != 0) push(extra);
                    break;
                }
                case 38: {  // PSHP
                    const uint32_t p = P(hiRef, 0x1C);
                    const uint32_t q = P(loRef, 0x18);
                    push(q);
                    if (p != 0) push(p);
                    break;
                }
                case 24: {  // SETC: pointer into the object's or linked entry's table
                    const uint32_t d = Fdst((ins >> 18) & 0x3F);
                    const uint32_t idx = ins & 0x3FFF;
                    uint32_t a0 = 0;
                    if (idx != 0x3FFF) {
                        a0 = rd(s0 + ((ins & 0x4000) ? 0x14 : 0x0C));
                        a0 = rd(a0 + 0x14) + idx * 4;
                    }
                    wr(d, a0);
                    break;
                }
                case 25: {
                    int32_t r = int32_t(V(hiRef));
                    const uint32_t d = D(ins);
                    wr(d, uint32_t(r));
                    if (r < 0) wr(d, uint32_t(-r));
                    break;
                }
                case 26: push(svc.call(Svc::Cpad, ins & 0x1FFFFF, 0, 0, 0)); break;
                case 27: {  // VEL
                    const uint32_t a3 = rd(kScratch + 0x54);
                    const uint32_t r = V(hiRef), l = V(loRef);
                    push(uint32_t(int32_t(r * a3) >> 10) + l);
                    break;
                }
                case 28: {  // MISC
                    saveRegs();
                    svc.call(Svc::Misc, s0, ins, 0, 0);
                    loadPools();
                    s6 = rd(s0 + 0xBC);
                    break;
                }
                case 29: {  // PSIN
                    const uint32_t r = V(hiRef);
                    uint32_t l = V(loRef);
                    l <<= 11;
                    uint32_t lo, hi;
                    divS(l, r, lo, hi);
                    uint32_t at = int32_t(0xFFFF) < int32_t(r) ? 1u : 0u;
                    uint32_t a0 = sincos(lo - 0x400);
                    at <<= 1;
                    a0 += 0x1000;
                    a0 = uint32_t(int32_t(a0) >> (at & 31));
                    const uint32_t prod = a0 * r;
                    at = 0xD - at;
                    push(uint32_t(int32_t(prod) >> (at & 31)));
                    break;
                }
                case 30: {  // SYNC
                    const uint32_t a3 = rd(kScratch + 0x50);
                    const uint32_t r = V(hiRef), l = V(loRef);
                    uint32_t lo, hi;
                    divS(r + a3, l, lo, hi);
                    push(hi);
                    break;
                }
                case 31: {  // GVAR
                    uint32_t v0 = V(loRef);
                    const uint32_t base = rd(kScratch + 0x58);
                    v0 = uint32_t(int32_t(v0) >> 8) << 2;
                    push(rd(v0 + base));
                    break;
                }
                case 32: {  // GVAW
                    const uint32_t r = V(hiRef), l = V(loRef);
                    const uint32_t base = rd(kScratch + 0x58);
                    wr((uint32_t(int32_t(l) >> 8) << 2) + base, r);
                    break;
                }
                case 33: {  // DEGD: signed 12-bit angle difference
                    const uint32_t r = V(hiRef), l = V(loRef);
                    push(uint32_t(int32_t((r << 20) - (l << 20)) >> 20));
                    break;
                }
                case 34: {  // SEEK
                    uint32_t a3, r;
                    if ((hiRef & 0xFFF) == 0xBF0) {
                        s6 -= 4; a3 = rd(s6);
                        s6 -= 4; r = rd(s6);
                        if (int32_t(a3) < 0) a3 = uint32_t(-int32_t(a3));
                    } else { r = V(hiRef); a3 = 0x100; }
                    const uint32_t l = V(loRef);
                    const uint32_t a2 = r - l;
                    uint32_t a0;
                    if (int32_t(a2) >= 0) { a0 = a2 - a3; a0 = int32_t(a0) <= 0 ? r : l + a3; }
                    else { a0 = a2 + a3; a0 = int32_t(a0) >= 0 ? r : l - a3; }
                    push(a0);
                    break;
                }
                case 37: {  // DSEK: SEEK on 12-bit angles, scaled by the frame step
                    uint32_t a3, r;
                    if ((hiRef & 0xFFF) == 0xBF0) { s6 -= 4; a3 = rd(s6); s6 -= 4; r = rd(s6); }
                    else { r = V(hiRef); a3 = 0x100; }
                    const uint32_t l = V(loRef);
                    const uint32_t step = rd(kScratch + 0x54);
                    const uint32_t scaled = uint32_t(int32_t(step * a3) >> 10);
                    int32_t a2 = int32_t((r << 20) - (l << 20)) >> 20;
                    int32_t mag = a2 < 0 ? -a2 : a2;
                    uint32_t a0;
                    if (int32_t(uint32_t(mag) - scaled) <= 0) a0 = r;
                    else if (a2 >= 0) a0 = (l + scaled) & 0xFFF;
                    else a0 = (l - scaled) & 0xFFF;
                    push(a0);
                    break;
                }
                case 35: {  // COLR
                    uint32_t a0 = rd(s0 + ((ins >> 10) & 0x1C) + 0x40);
                    const uint32_t v = uint32_t(int32_t(int16_t(bus.rd16(a0 + ((ins >> 14) & 0x7E) + 0x20))));
                    push(v);
                    break;
                }
                case 36: {  // COLW
                    const uint32_t v0 = V(loRef);
                    const uint32_t a0 = rd(s0 + ((ins >> 10) & 0x1C) + 0x40);
                    bus.wr16(a0 + ((ins >> 14) & 0x7E) + 0x20, v0);
                    break;
                }
                case 39: {  // ANIS: address of an animation table entry
                    const uint32_t r = V(hiRef);
                    const uint32_t d = D(ins);
                    const uint32_t a2 = rd(rd(s0 + 0x0C) + 0x24);
                    wr(d, ((r >> 8) << 2) + a2);
                    break;
                }
                case 42: {  // ARRL
                    const uint32_t r = V(hiRef), l = V(loRef);
                    push(rd(l + ((r >> 8) << 2)));
                    break;
                }
                case 78: {  // ARRS
                    const uint32_t r = V(hiRef), l = V(loRef);
                    const uint32_t dst = l + ((r >> 8) << 2);
                    s6 -= 4;
                    wr(dst, rd(s6));
                    break;
                }
                case 43: { const uint32_t r = V(hiRef); const uint32_t d = D(ins); wr(d, sincos(r)); break; }
                case 44: { const uint32_t r = V(hiRef); const uint32_t d = D(ins); wr(d, sincos(r + 0x400)); break; }
                case 45: {  // ATAN
                    const uint32_t r = V(hiRef);
                    const uint32_t p = P(loRef, 0x18);
                    const uint32_t l = rd(p);
                    push(svc.call(Svc::Atan, l, r, 0, 0));
                    break;
                }
                case 46: {  // opcode 46
                    const uint32_t src = P(hiRef, 0x1C);
                    const uint32_t dst = D(ins);
                    const uint32_t w0 = rd(src), w1 = rd(src + 4), w2 = rd(src + 8);  // all three loads precede the stores
                    wr(kScratch + 0x20, w0);
                    wr(kScratch + 0x24, w1);
                    wr(kScratch + 0x28, w2);
                    svc.call(Svc::Op46, src, dst, 0, 0);
                    break;
                }
                case 47: break;  // NOP

                // ---- control flow ----
                case 50: {  // BRA
                    s5 += uint32_t(sx(ins, 10) * 4);
                    s6 -= ((ins >> 8) & 0x3C);
                    break;
                }
                case 51:
                case 52: {  // BNEZ, BEQZ
                    const uint32_t v = rd(Fsrc((ins >> 14) & 0x3F));
                    const bool take = (op == 51) ? v != 0 : v == 0;
                    if (take) {
                        s5 += uint32_t(sx(ins, 10) * 4);
                        s6 -= ((ins >> 8) & 0x3C);
                    }
                    break;
                }
                case 53: if (callB(Svc::Go, ins)) return leave(retval); break;
                case 54: {  // GNEZ
                    const uint32_t v = rd(Fsrc((ins >> 14) & 0x3F));
                    if (v != 0) { if (callB(Svc::Go, ins)) return leave(retval); }
                    else s6 -= ((ins >> 8) & 0x3C);
                    break;
                }
                case 55: {  // GEQZ
                    const uint32_t v = rd(Fsrc((ins >> 14) & 0x3F));
                    if (v == 0) { if (callB(Svc::Go, ins)) return leave(retval); }
                    else s6 -= ((ins >> 8) & 0x3C);
                    break;
                }
                case 56: if (callB(Svc::Anim, ins)) return leave(retval); break;
                case 57: if (callB(Svc::Anif, ins)) return leave(retval); break;
                case 60: if (callB(Svc::Sevt, ins)) return leave(retval); break;
                case 68: if (callB(Svc::Bevt, ins)) return leave(retval); break;
                case 69: if (callB(Svc::Cevt, ins)) return leave(retval); break;
                case 61:
                case 62: {  // RJEV, ACEV
                    saveRegs();
                    const uint32_t r = svc.call(Svc::Rjev, s0, ins, s1, s2);
                    s1 = rd(kScratch + 0x48);
                    reloadRegs();
                    if (r != 0) return leave(int32_t(r));
                    break;
                }
                case 40: callA(Svc::Eflr, ins); break;
                case 41: callA(Svc::Eflv, ins); break;
                case 48: callA(Svc::Dbg, ins); break;
                case 58: callA(Svc::Vec, ins); break;
                case 63:
                case 70: callA(Svc::Spwn, ins); break;
                case 64: callA(Svc::Chnk, ins); break;
                case 65: callA(Svc::Sndp, ins); break;
                case 66: callA(Svc::Snda, ins); break;
                case 67: callA(Svc::Coll, ins); break;
                case 72: callA(Svc::Op72, ins); break;
                case 74: callA(Svc::Op74, ins); break;
                case 75: callA(Svc::Op75, ins); break;
                case 76: callA(Svc::Op76, ins); break;
                case 77: callA(Svc::Op77, ins); break;

                case 59: {  // CALL
                    const uint32_t idx = ins & 0x3FFF;
                    pushFrame((ins >> 20) & 0xF);
                    s5 = idx - 0x3FFF;
                    if (s5 != 0) {
                        const uint32_t entry = rd(s0 + ((ins & 0x4000) ? 0x14 : 0x0C));
                        s5 = rd(entry + 0x14) + idx * 4;
                    }
                    s1 &= ~9u;
                    break;
                }
                case 71: {  // CALR: call through a code pointer
                    const uint32_t r = V(hiRef);
                    const uint32_t p = P(loRef, 0x18);
                    const uint32_t argc = uint32_t(int32_t(r) >> 8);
                    const uint32_t target = p ? rd(p) : 0;
                    if (p == 0 || target == 0) { s6 -= (r << 2); break; }
                    pushFrame(argc);
                    s5 = target;
                    s1 &= ~9u;
                    break;
                }
                case 49: {  // RET
                    int32_t result = 0;
                    if (doRet(result)) return leave(result);
                    break;
                }
                case 73: {  // MIPS: native code stored in the script, running on the interpreter's registers
                    VmRegs regs{s0, s1, s2, s3, s4, s5, s6, s7};
                    if (!svc.runNative(bus, regs, ins)) throw std::runtime_error("GOOL VM: native blocks are not supported by this Services object");
                    s1 = regs.s1; s2 = regs.s2; s3 = regs.s3; s4 = regs.s4; s5 = regs.s5; s6 = regs.s6; s7 = regs.s7;
                    if (s5 != 0) break;  // `bnez s5` back into the loop; otherwise fall into the RET handler
                    int32_t result = 0;
                    if (doRet(result)) return leave(result);
                    break;
                }
                default:
                    throw std::runtime_error("GOOL VM: unsupported opcode " + std::to_string(op));
            }
        }
    }
};

}  // namespace

const char* svcName(Svc s) {
    static const char* names[] = {"GO", "ANIM", "ANIF", "SEVT", "CEVT", "BEVT", "RJEV/ACEV", "DBG", "EFLR", "EFLV", "VEC",
                                  "COLL", "SPWN/SPWF", "SNDP", "SNDA", "CHNK", "OP72", "OP74", "OP75", "OP76", "OP77",
                                  "MISC", "CPAD", "ATAN", "OP46", "MIPS"};
    return names[int(s)];
}

uint32_t svcAddress(Svc s) {
    static const uint32_t addr[] = {0x8003900C, 0x80039104, 0x800391D8, 0x80039310, 0x800394C4, 0x80039684, 0x8003981C,
                                    0x80037440, 0x80037448, 0x8003756C, 0x8003817C, 0x80038700, 0x80038AFC, 0x80038C14,
                                    0x80038D08, 0x80038F00, 0x80038E14, 0x80039A14, 0x80039AA4, 0x80039B14, 0x80039C08,
                                    0x80037698, 0x80036FAC, 0x8004F078, 0x8001EAD8, 0};
    return addr[int(s)];
}

int32_t goolRun(Bus& bus, Services& svc, uint32_t proc, uint32_t flags, uint32_t out, RunStats* stats,
                uint64_t maxInstructions) {
    Vm vm(bus, svc, proc, flags, out);
    return vm.run(maxInstructions, stats);
}

}  // namespace cb2

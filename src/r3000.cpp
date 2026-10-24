#include "r3000.hpp"

#include <cstdio>
#include <string>

namespace r3000 {
namespace {

std::string hex(uint32_t v) {
    char b[16];
    std::snprintf(b, sizeof b, "%08x", v);
    return b;
}

int32_t sext16(uint32_t v) { return int32_t(int16_t(v & 0xFFFF)); }

}  // namespace

void Cpu::brk(uint32_t code) { throw std::runtime_error("break " + hex(code) + " at " + hex(pc)); }
void Cpu::syscall(uint32_t code) { throw std::runtime_error("syscall " + hex(code) + " at " + hex(pc)); }
void Cpu::invalid(uint32_t at, uint32_t word) { throw std::runtime_error("invalid instruction " + hex(word) + " at " + hex(at)); }

void Cpu::unresolved(uint32_t addr) {
    if (indirectCall) indirectCall(*this, addr);
    else trap(addr);
}

void Cpu::step() {
    const uint32_t cur = pc;
    const uint32_t phys = cur & 0x1FFFFFFFu;
    if (intercept && intercept(*this, cur)) {
        ++instructions;
        return;
    }
    if (bios && (phys == 0xA0 || phys == 0xB0 || phys == 0xC0) && bios(*this, phys)) {
        ++instructions;
        return;
    }
    if (cur & 3u) throw std::runtime_error("unaligned pc " + hex(cur));
    const uint32_t ins = rd32(cur);
    if (fault) throw std::runtime_error("instruction fetch from unmapped address " + hex(cur));
    pc = nextPc;
    nextPc = pc + 4;
    ++instructions;

    // The load issued by the previous instruction commits after this one, unless this one overwrites its target.
    uint32_t ldR = pendReg, ldV = pendVal;
    pendReg = 0;

    const uint32_t op = ins >> 26, rs = (ins >> 21) & 31, rt = (ins >> 16) & 31, rd = (ins >> 11) & 31;
    const uint32_t sa = (ins >> 6) & 31, fn = ins & 63, imm = ins & 0xFFFF;
    const uint32_t target = (cur + 4 + uint32_t(sext16(imm) * 4));

    auto W = [&](uint32_t reg, uint32_t v) {
        if (reg == ldR) ldR = 0;
        r[reg] = v;
    };
    auto L = [&](uint32_t reg, uint32_t v) {
        if (reg == ldR) ldR = 0;
        pendReg = reg;
        pendVal = v;
    };
    auto branch = [&](bool taken) { if (taken) nextPc = target; };
    // lwl/lwr see the value of a load still in flight to the same register.
    auto mergeBase = [&](uint32_t reg) { return (reg == ldR) ? ldV : r[reg]; };
    auto checkFault = [&]() {
        if (fault) throw std::runtime_error("access to unmapped address " + hex(faultAddr) + " at " + hex(cur));
    };
    auto align = [&](uint32_t addr, uint32_t mask) {
        if (addr & mask) throw std::runtime_error("unaligned access " + hex(addr) + " at " + hex(cur));
    };

    switch (op) {
        case 0x00:
            switch (fn) {
                case 0x00: W(rd, r[rt] << sa); break;
                case 0x02: W(rd, r[rt] >> sa); break;
                case 0x03: W(rd, uint32_t(int32_t(r[rt]) >> sa)); break;
                case 0x04: W(rd, r[rt] << (r[rs] & 31)); break;
                case 0x06: W(rd, r[rt] >> (r[rs] & 31)); break;
                case 0x07: W(rd, uint32_t(int32_t(r[rt]) >> (r[rs] & 31))); break;
                case 0x08: nextPc = r[rs]; break;
                case 0x09: { const uint32_t t = r[rs]; W(rd, cur + 8); nextPc = t; break; }
                case 0x0C: syscall((ins >> 6) & 0xFFFFF); break;
                case 0x0D: brk((ins >> 6) & 0xFFFFF); break;
                case 0x10: W(rd, hi); break;
                case 0x11: hi = r[rs]; break;
                case 0x12: W(rd, lo); break;
                case 0x13: lo = r[rs]; break;
                case 0x18: mult(r[rs], r[rt]); break;
                case 0x19: multu(r[rs], r[rt]); break;
                case 0x1A: div(r[rs], r[rt]); break;
                case 0x1B: divu(r[rs], r[rt]); break;
                case 0x20: {
                    const int64_t s = int64_t(int32_t(r[rs])) + int64_t(int32_t(r[rt]));
                    if (s != int32_t(s)) throw std::runtime_error("add overflow at " + hex(cur));
                    W(rd, uint32_t(s));
                    break;
                }
                case 0x21: W(rd, r[rs] + r[rt]); break;
                case 0x22: {
                    const int64_t s = int64_t(int32_t(r[rs])) - int64_t(int32_t(r[rt]));
                    if (s != int32_t(s)) throw std::runtime_error("sub overflow at " + hex(cur));
                    W(rd, uint32_t(s));
                    break;
                }
                case 0x23: W(rd, r[rs] - r[rt]); break;
                case 0x24: W(rd, r[rs] & r[rt]); break;
                case 0x25: W(rd, r[rs] | r[rt]); break;
                case 0x26: W(rd, r[rs] ^ r[rt]); break;
                case 0x27: W(rd, ~(r[rs] | r[rt])); break;
                case 0x2A: W(rd, int32_t(r[rs]) < int32_t(r[rt]) ? 1u : 0u); break;
                case 0x2B: W(rd, r[rs] < r[rt] ? 1u : 0u); break;
                default: invalid(cur, ins);
            }
            break;
        case 0x01: {
            const bool ge = (rt & 1) != 0;
            const bool link = (rt & 0x1E) == 0x10;
            const bool taken = ge ? int32_t(r[rs]) >= 0 : int32_t(r[rs]) < 0;
            if (link) W(31, cur + 8);  // linking happens whether or not the branch is taken
            branch(taken);
            break;
        }
        case 0x02: nextPc = ((cur + 4) & 0xF0000000u) | ((ins & 0x03FFFFFFu) << 2); break;
        case 0x03: W(31, cur + 8); nextPc = ((cur + 4) & 0xF0000000u) | ((ins & 0x03FFFFFFu) << 2); break;
        case 0x04: branch(r[rs] == r[rt]); break;
        case 0x05: branch(r[rs] != r[rt]); break;
        case 0x06: branch(int32_t(r[rs]) <= 0); break;
        case 0x07: branch(int32_t(r[rs]) > 0); break;
        case 0x08: {
            const int64_t s = int64_t(int32_t(r[rs])) + sext16(imm);
            if (s != int32_t(s)) throw std::runtime_error("addi overflow at " + hex(cur));
            W(rt, uint32_t(s));
            break;
        }
        case 0x09: W(rt, r[rs] + uint32_t(sext16(imm))); break;
        case 0x0A: W(rt, int32_t(r[rs]) < sext16(imm) ? 1u : 0u); break;
        case 0x0B: W(rt, r[rs] < uint32_t(sext16(imm)) ? 1u : 0u); break;
        case 0x0C: W(rt, r[rs] & imm); break;
        case 0x0D: W(rt, r[rs] | imm); break;
        case 0x0E: W(rt, r[rs] ^ imm); break;
        case 0x0F: W(rt, imm << 16); break;
        case 0x10:  // COP0
            if (rs == 0x00) L(rt, cop0[rd]);
            else if (rs == 0x04) cop0[rd] = r[rt];
            else if (rs == 0x10 && fn == 0x10) cop0[12] = (cop0[12] & ~0xFu) | ((cop0[12] >> 2) & 0xFu);  // rfe
            else invalid(cur, ins);
            break;
        case 0x12:  // COP2 (GTE)
            if (ins & 0x02000000u) gteCommand(ins & 0x1FFFFFFu);
            else if (rs == 0x00) L(rt, gteReadData(rd));
            else if (rs == 0x02) L(rt, gteReadCtrl(rd));
            else if (rs == 0x04) gteWriteData(rd, r[rt]);
            else if (rs == 0x06) gteWriteCtrl(rd, r[rt]);
            else invalid(cur, ins);
            break;
        case 0x20: { const uint32_t a = r[rs] + uint32_t(sext16(imm)); const uint32_t v = rd8(a); checkFault(); L(rt, uint32_t(int32_t(int8_t(v)))); break; }
        case 0x21: { const uint32_t a = r[rs] + uint32_t(sext16(imm)); align(a, 1); const uint32_t v = rd16(a); checkFault(); L(rt, uint32_t(int32_t(int16_t(v)))); break; }
        case 0x22: { const uint32_t a = r[rs] + uint32_t(sext16(imm)); const uint32_t v = lwl(a, mergeBase(rt)); checkFault(); L(rt, v); break; }
        case 0x23: { const uint32_t a = r[rs] + uint32_t(sext16(imm)); align(a, 3); const uint32_t v = rd32(a); checkFault(); L(rt, v); break; }
        case 0x24: { const uint32_t a = r[rs] + uint32_t(sext16(imm)); const uint32_t v = rd8(a); checkFault(); L(rt, v); break; }
        case 0x25: { const uint32_t a = r[rs] + uint32_t(sext16(imm)); align(a, 1); const uint32_t v = rd16(a); checkFault(); L(rt, v); break; }
        case 0x26: { const uint32_t a = r[rs] + uint32_t(sext16(imm)); const uint32_t v = lwr(a, mergeBase(rt)); checkFault(); L(rt, v); break; }
        case 0x28: wr8(r[rs] + uint32_t(sext16(imm)), r[rt]); checkFault(); break;
        case 0x29: { const uint32_t a = r[rs] + uint32_t(sext16(imm)); align(a, 1); wr16(a, r[rt]); checkFault(); break; }
        case 0x2A: swl(r[rs] + uint32_t(sext16(imm)), r[rt]); checkFault(); break;
        case 0x2B: { const uint32_t a = r[rs] + uint32_t(sext16(imm)); align(a, 3); wr32(a, r[rt]); checkFault(); break; }
        case 0x2E: swr(r[rs] + uint32_t(sext16(imm)), r[rt]); checkFault(); break;
        case 0x32: { const uint32_t a = r[rs] + uint32_t(sext16(imm)); align(a, 3); const uint32_t v = rd32(a); checkFault(); gteWriteData(rt, v); break; }
        case 0x3A: { const uint32_t a = r[rs] + uint32_t(sext16(imm)); align(a, 3); wr32(a, gteReadData(rt)); checkFault(); break; }
        default: invalid(cur, ins);
    }

    if (ldR) r[ldR] = ldV;
    r[0] = 0;
}

}  // namespace r3000

// Geometry Transformation Engine (COP2) of the PlayStation CPU.
// Verified against 1,150 register/opcode captures taken from real hardware (tools/gte_tests_convert.py, cb2_gtetest).
#include <algorithm>
#include <cstdint>
#include <cstring>

#include "r3000.hpp"

namespace r3000 {
namespace {

constexpr uint32_t kErrorMask = 0x7F87E000u;

uint8_t unrEntry(uint32_t i) {
    static uint8_t table[0x101];
    static bool ready = false;
    if (!ready) {
        for (uint32_t k = 0; k <= 0x100; ++k) {
            const int32_t v = (0x40000 / int32_t(k + 0x100) + 1) / 2 - 0x101;
            table[k] = uint8_t(v < 0 ? 0 : v);
        }
        ready = true;
    }
    return table[i];
}

struct Exec {
    uint32_t* d;
    uint32_t* c;
    uint32_t flag = 0;
    int sf = 0;
    bool lm = false;

    int shift() const { return sf ? 12 : 0; }

    // 44-bit MAC1..3 accumulator with overflow flags.
    int64_t A(int i, int64_t v) {
        if (v > 0x7FFFFFFFFFFLL) flag |= 1u << (31 - i);
        else if (v < -0x80000000000LL) flag |= 1u << (28 - i);
        return int64_t(uint64_t(v) << 20) >> 20;
    }
    int64_t F(int64_t v) {
        if (v > 0x7FFFFFFFLL) flag |= 1u << 16;
        else if (v < -0x80000000LL) flag |= 1u << 15;
        return v;
    }
    int32_t lim(int i, int64_t v, bool lowZero, bool setFlag = true) {
        const int64_t lo = lowZero ? 0 : -0x8000;
        if (v > 0x7FFF) { if (setFlag) flag |= 1u << (25 - i); return 0x7FFF; }
        if (v < lo) { if (setFlag) flag |= 1u << (25 - i); return int32_t(lo); }
        return int32_t(v);
    }
    int32_t ir(int i) const { return int16_t(d[8 + i]); }
    void setIR(int i, int32_t v) { d[8 + i] = uint32_t(v); }
    void setMac(int i, int64_t acc) { d[24 + i] = uint32_t(acc >> shift()); }
    int32_t mac(int i) const { return int32_t(d[24 + i]); }

    int16_t mat(int base, int r, int col) const {
        const int k = r * 3 + col;
        const uint32_t w = c[base + k / 2];
        return int16_t((k & 1) ? w >> 16 : w);
    }
    void vec(int n, int32_t out[3]) const {
        out[0] = int16_t(d[2 * n]);
        out[1] = int16_t(d[2 * n] >> 16);
        out[2] = int16_t(d[2 * n + 1]);
    }

    // MVMVA core: MAC = (T << 12) + M * V (with the far-colour bug for T = FC), then IR from MAC.
    // `rtpsQuirk` reproduces the IR3 flag being derived from MAC3 >> 12 whatever `sf` is.
    void mulMatVec(const int32_t m[3][3], const int32_t v[3], int tx, bool rtpsQuirk = false) {
        static const int tBase[4] = {5, 13, 21, 0};
        int64_t acc[3];
        for (int i = 0; i < 3; ++i) {
            const int64_t t = tx == 3 ? 0 : int32_t(c[tBase[tx] + i]);
            int64_t x;
            if (tx != 2) {
                x = A(i + 1, t * 0x1000);
                for (int k = 0; k < 3; ++k) x = A(i + 1, x + int64_t(m[i][k]) * v[k]);
            } else {
                x = A(i + 1, t * 0x1000 + int64_t(m[i][0]) * v[0]);
                lim(i + 1, x >> shift(), false);
                x = A(i + 1, int64_t(m[i][1]) * v[1]);
                x = A(i + 1, x + int64_t(m[i][2]) * v[2]);
            }
            acc[i] = x;
            setMac(i + 1, x);
        }
        lastAcc3 = acc[2];
        for (int i = 0; i < 3; ++i) {
            if (i == 2 && rtpsQuirk) {
                lim(3, acc[2] >> 12, false);
                setIR(3, lim(3, mac(3), lm, false));
            } else {
                setIR(i + 1, lim(i + 1, mac(i + 1), lm));
            }
        }
    }
    int64_t lastAcc3 = 0;
    PgxpMap* pgxp = nullptr;
    std::vector<std::array<float, 3>>* capture = nullptr;
    std::vector<GteSample>* samples = nullptr;
    std::vector<GteSample>* samples2 = nullptr;
    uint32_t insnPc = 0;

    void matrix(int base, int32_t m[3][3]) const {
        for (int r = 0; r < 3; ++r)
            for (int k = 0; k < 3; ++k) m[r][k] = mat(base, r, k);
    }

    void pushSZ(int64_t v) {
        if (v < 0) { flag |= 1u << 18; v = 0; }
        else if (v > 0xFFFF) { flag |= 1u << 18; v = 0xFFFF; }
        d[16] = d[17]; d[17] = d[18]; d[18] = d[19];
        d[19] = uint32_t(v);
    }
    void pushSXY(int64_t x, int64_t y) {
        if (x < -0x400) { flag |= 1u << 14; x = -0x400; } else if (x > 0x3FF) { flag |= 1u << 14; x = 0x3FF; }
        if (y < -0x400) { flag |= 1u << 13; y = -0x400; } else if (y > 0x3FF) { flag |= 1u << 13; y = 0x3FF; }
        d[12] = d[13]; d[13] = d[14];
        d[14] = uint32_t(uint16_t(x)) | uint32_t(uint16_t(y)) << 16;
        d[15] = d[14];
    }
    void pushColor() {
        uint32_t out = d[6] & 0xFF000000u;
        for (int i = 0; i < 3; ++i) {
            int32_t v = mac(i + 1) >> 4;
            if (v < 0) { flag |= 1u << (22 - (i + 1)); v = 0; }
            else if (v > 255) { flag |= 1u << (22 - (i + 1)); v = 255; }
            out |= uint32_t(v) << (8 * i);
        }
        d[20] = d[21]; d[21] = d[22]; d[22] = out;
    }

    uint32_t unrDivide(uint32_t lhs, uint32_t rhs) {
        if (lhs >= rhs * 2) { flag |= 1u << 17; return 0x1FFFF; }
        uint32_t z = 0;
        while (z < 16 && !((rhs << z) & 0x8000)) ++z;
        const uint64_t n = uint64_t(lhs) << z;
        uint64_t dd = uint64_t(rhs) << z;
        const uint64_t u = unrEntry(uint32_t((dd - 0x7FC0) >> 7)) + 0x101;
        dd = (0x2000080 - dd * u) >> 8;
        dd = (0x80 + dd * u) >> 8;
        return uint32_t(std::min<uint64_t>(0x1FFFF, (n * dd + 0x8000) >> 16));
    }

    void rtp(int n, bool last) {
        int32_t m[3][3], v[3];
        matrix(0, m);
        vec(n, v);
        mulMatVec(m, v, 0, true);
        pushSZ(lastAcc3 >> 12);
        const uint32_t div = unrDivide(c[26] & 0xFFFF, d[19] & 0xFFFF);
        const int64_t x = F(int64_t(int32_t(c[24])) + int64_t(ir(1)) * div);
        const int64_t y = F(int64_t(int32_t(c[25])) + int64_t(ir(2)) * div);
        pushSXY(x >> 16, y >> 16);
        if (pgxp) {
            auto clampf = [](double v) { return float(v < -1024.0 ? -1024.0 : (v > 1023.99998 ? 1023.99998 : v)); };
            if (pgxp->size() > (1u << 18)) pgxp->clear();
            (*pgxp)[d[14]] = {clampf(double(x) / 65536.0), clampf(double(y) / 65536.0), float(d[19] & 0xFFFF)};
        }
        if (samples || samples2) {
            GteSample s{{c[0], c[1], c[2], c[3], c[4]}, {int32_t(c[5]), int32_t(c[6]), int32_t(c[7])}, float(double(x) / 65536.0), float(double(y) / 65536.0), float(d[19] & 0xFFFF),
                        int16_t(v[0]), int16_t(v[1]), int16_t(v[2]), insnPc, int32_t(c[24]), int32_t(c[25]), uint16_t(c[26])};
            if (samples) samples->push_back(s);
            if (samples2) samples2->push_back(s);
        }
        if (capture) capture->push_back({float(double(x) / 65536.0), float(double(y) / 65536.0), float(d[19] & 0xFFFF)});
        if (!last) return;
        const int64_t m0 = F(int64_t(int32_t(c[28])) + int64_t(int16_t(c[27])) * div);
        d[24] = uint32_t(m0);
        const int64_t i0 = m0 >> 12;
        int64_t r = i0;
        if (r < 0) { flag |= 1u << 12; r = 0; } else if (r > 0x1000) { flag |= 1u << 12; r = 0x1000; }
        d[8] = uint32_t(r);
    }

    // Colour interpolation towards the far colour: MAC = in + (FC - in) * IR0.
    void interpolate(const int64_t in[3]) {
        int32_t t[3];
        for (int i = 0; i < 3; ++i) {
            const int64_t x = A(i + 1, (int64_t(int32_t(c[21 + i])) << 12) - in[i]);
            setMac(i + 1, x);
            t[i] = lim(i + 1, mac(i + 1), false);
        }
        for (int i = 0; i < 3; ++i) {
            const int64_t x = A(i + 1, int64_t(t[i]) * ir(0) + in[i]);
            setMac(i + 1, x);
            setIR(i + 1, lim(i + 1, mac(i + 1), lm));
        }
    }

    void modulate(int64_t out[3]) const {
        for (int i = 0; i < 3; ++i) out[i] = (int64_t((d[6] >> (8 * i)) & 0xFF) * ir(i + 1)) << 4;
    }
    void modulateStore() {
        int64_t in[3];
        modulate(in);
        for (int i = 0; i < 3; ++i) {
            const int64_t x = A(i + 1, in[i]);
            setMac(i + 1, x);
            setIR(i + 1, lim(i + 1, mac(i + 1), lm));
        }
    }

    void lightStep1(int n) {
        int32_t m[3][3], v[3];
        matrix(8, m);
        vec(n, v);
        mulMatVec(m, v, 3);
    }
    void lightStep2() {
        int32_t m[3][3], v[3] = {ir(1), ir(2), ir(3)};
        matrix(16, m);
        mulMatVec(m, v, 1);
    }

    void run(uint32_t op) {
        sf = (op >> 19) & 1;
        lm = (op >> 10) & 1;
        const int mx = (op >> 17) & 3, vx = (op >> 15) & 3, tx = (op >> 13) & 3;
        switch (op & 0x3F) {
        case 0x01: rtp(0, true); break;
        case 0x30: rtp(0, false); rtp(1, false); rtp(2, true); break;
        case 0x06: {
            const int32_t sx0 = int16_t(d[12]), sy0 = int16_t(d[12] >> 16), sx1 = int16_t(d[13]), sy1 = int16_t(d[13] >> 16);
            const int32_t sx2 = int16_t(d[14]), sy2 = int16_t(d[14] >> 16);
            d[24] = uint32_t(F(int64_t(sx0) * sy1 + int64_t(sx1) * sy2 + int64_t(sx2) * sy0 - int64_t(sx0) * sy2 - int64_t(sx1) * sy0 - int64_t(sx2) * sy1));
            break;
        }
        case 0x0C: {
            const int32_t d1 = int16_t(c[0]), d2 = int16_t(c[2]), d3 = int16_t(c[4]);
            const int64_t a[3] = {int64_t(ir(3)) * d2 - int64_t(ir(2)) * d3, int64_t(ir(1)) * d3 - int64_t(ir(3)) * d1,
                                  int64_t(ir(2)) * d1 - int64_t(ir(1)) * d2};
            int64_t x[3];
            for (int i = 0; i < 3; ++i) { x[i] = A(i + 1, a[i]); setMac(i + 1, x[i]); }
            for (int i = 0; i < 3; ++i) setIR(i + 1, lim(i + 1, mac(i + 1), lm));
            break;
        }
        case 0x10: {
            const int64_t in[3] = {int64_t(d[6] & 0xFF) << 16, int64_t((d[6] >> 8) & 0xFF) << 16, int64_t((d[6] >> 16) & 0xFF) << 16};
            interpolate(in);
            pushColor();
            break;
        }
        case 0x2A:
            for (int k = 0; k < 3; ++k) {
                const int64_t in[3] = {int64_t(d[20] & 0xFF) << 16, int64_t((d[20] >> 8) & 0xFF) << 16, int64_t((d[20] >> 16) & 0xFF) << 16};
                interpolate(in);
                pushColor();
            }
            break;
        case 0x11: {
            const int64_t in[3] = {int64_t(ir(1)) << 12, int64_t(ir(2)) << 12, int64_t(ir(3)) << 12};
            interpolate(in);
            pushColor();
            break;
        }
        case 0x12: {
            int32_t m[3][3], v[3];
            if (mx == 3) {
                const int32_t rr = int32_t(d[6] & 0xFF);
                const int32_t rt13 = mat(0, 0, 2), rt22 = mat(0, 1, 1);
                const int32_t g[3][3] = {{-rr * 16, rr * 16, ir(0)}, {rt13, rt13, rt13}, {rt22, rt22, rt22}};
                std::copy(&g[0][0], &g[0][0] + 9, &m[0][0]);
            } else {
                matrix(mx * 8, m);
            }
            if (vx == 3) { v[0] = ir(1); v[1] = ir(2); v[2] = ir(3); }
            else vec(vx, v);
            mulMatVec(m, v, tx);
            break;
        }
        case 0x13: lightStep1(0); lightStep2(); { int64_t in[3]; modulate(in); interpolate(in); pushColor(); } break;
        case 0x16:
            for (int n = 0; n < 3; ++n) { lightStep1(n); lightStep2(); int64_t in[3]; modulate(in); interpolate(in); pushColor(); }
            break;
        case 0x14: lightStep2(); { int64_t in[3]; modulate(in); interpolate(in); pushColor(); } break;
        case 0x1B: lightStep1(0); lightStep2(); modulateStore(); pushColor(); break;
        case 0x3F:
            for (int n = 0; n < 3; ++n) { lightStep1(n); lightStep2(); modulateStore(); pushColor(); }
            break;
        case 0x1C: lightStep2(); modulateStore(); pushColor(); break;
        case 0x1E: lightStep1(0); lightStep2(); pushColor(); break;
        case 0x20:
            for (int n = 0; n < 3; ++n) { lightStep1(n); lightStep2(); pushColor(); }
            break;
        case 0x28:
            for (int i = 1; i <= 3; ++i) {
                const int64_t x = A(i, int64_t(ir(i)) * ir(i));
                setMac(i, x);
            }
            for (int i = 1; i <= 3; ++i) setIR(i, lim(i, mac(i), lm));
            break;
        case 0x29: { int64_t in[3]; modulate(in); interpolate(in); pushColor(); break; }
        case 0x2D: case 0x2E: {
            const bool four = (op & 0x3F) == 0x2E;
            const int64_t sum = int64_t(d[17] & 0xFFFF) + (d[18] & 0xFFFF) + (d[19] & 0xFFFF) + (four ? (d[16] & 0xFFFF) : 0);
            const int64_t zsf = int16_t(c[four ? 30 : 29]);
            const int64_t m0 = F(zsf * sum);
            d[24] = uint32_t(m0);
            int64_t o = m0 >> 12;
            if (o < 0) { flag |= 1u << 18; o = 0; } else if (o > 0xFFFF) { flag |= 1u << 18; o = 0xFFFF; }
            d[7] = uint32_t(o);
            break;
        }
        case 0x3D:
            for (int i = 1; i <= 3; ++i) { const int64_t x = A(i, int64_t(ir(i)) * ir(0)); setMac(i, x); }
            for (int i = 1; i <= 3; ++i) setIR(i, lim(i, mac(i), lm));
            pushColor();
            break;
        case 0x3E:
            for (int i = 1; i <= 3; ++i) {
                const int64_t x = A(i, (int64_t(mac(i)) << shift()) + int64_t(ir(i)) * ir(0));
                setMac(i, x);
            }
            for (int i = 1; i <= 3; ++i) setIR(i, lim(i, mac(i), lm));
            pushColor();
            break;
        default: break;
        }
        if (flag & kErrorMask) flag |= 0x80000000u;
        c[31] = flag;
    }
};

uint32_t countLeading(uint32_t v) {
    if (int32_t(v) < 0) v = ~v;
    uint32_t n = 0;
    for (uint32_t bit = 0x80000000u; bit && !(v & bit); bit >>= 1) ++n;
    return n;
}

}  // namespace

uint32_t Cpu::gteReadData(uint32_t reg) {
    switch (reg & 31) {
    case 1: case 3: case 5: case 8: case 9: case 10: case 11: return uint32_t(int32_t(int16_t(cop2d[reg & 31])));
    case 7: case 16: case 17: case 18: case 19: return cop2d[reg & 31] & 0xFFFF;
    case 15: return cop2d[14];
    case 28: case 29: {
        uint32_t v = 0;
        for (int i = 0; i < 3; ++i) {
            const int32_t x = int16_t(cop2d[9 + i]) >> 7;
            v |= uint32_t(std::min(std::max(x, 0), 0x1F)) << (5 * i);
        }
        return v;
    }
    default: return cop2d[reg & 31];
    }
}

void Cpu::gteWriteData(uint32_t reg, uint32_t v) {
    reg &= 31;
    switch (reg) {
    case 1: case 3: case 5: case 8: case 9: case 10: case 11: cop2d[reg] = uint32_t(int32_t(int16_t(v))); break;
    case 7: case 16: case 17: case 18: case 19: cop2d[reg] = v & 0xFFFF; break;
    case 15: cop2d[12] = cop2d[13]; cop2d[13] = cop2d[14]; cop2d[14] = v; cop2d[15] = v; break;
    case 28:
        cop2d[28] = v & 0x7FFF;
        for (int i = 0; i < 3; ++i) cop2d[9 + i] = ((v >> (5 * i)) & 0x1F) * 0x80;
        break;
    case 29: break;
    case 30: cop2d[30] = v; cop2d[31] = countLeading(v) + ((int32_t(v) < 0) ? 0 : 0); if (v == 0) cop2d[31] = 32; break;
    case 31: break;
    default: cop2d[reg] = v; break;
    }
}

uint32_t Cpu::gteReadCtrl(uint32_t reg) {
    reg &= 31;
    switch (reg) {
    case 4: case 12: case 20: case 26: case 27: case 29: case 30: return uint32_t(int32_t(int16_t(cop2c[reg])));
    case 31: {
        const uint32_t f = cop2c[31] & 0x7FFFF000u;
        return f | ((f & 0x7F87E000u) ? 0x80000000u : 0u);
    }
    default: return cop2c[reg];
    }
}

void Cpu::gteWriteCtrl(uint32_t reg, uint32_t v) {
    reg &= 31;
    if (reg == 31) { cop2c[31] = v & 0x7FFFF000u; return; }
    cop2c[reg] = v;
}

void Cpu::gteCommand(uint32_t cmd) {
    Exec e{cop2d, cop2c};
    e.pgxp = pgxp;
    e.capture = sxyCapture;
    e.samples = gteSamples;
    e.samples2 = gteSamples2;
    e.insnPc = gteInsnPc;
    if (dbgPc && gteInsnPc == dbgPc) {
        if (dbgHits++ == 0) std::memcpy(dbgRegs, r, sizeof r);
        if (dbgLog.size() < 32u * 6000u) dbgLog.insert(dbgLog.end(), r, r + 32);
    }
    e.run(cmd);
}

}  // namespace r3000

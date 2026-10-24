#include "spu.hpp"

#include <algorithm>
#include <cstring>

namespace psx {
namespace {
const int kPos[5] = {0, 60, 115, 98, 122};
const int kNeg[5] = {0, 0, -52, -55, -60};
inline int16_t clamp16(int v) { return int16_t(v < -32768 ? -32768 : (v > 32767 ? 32767 : v)); }
}  // namespace

int Spu::volume(int16_t raw) {
    const uint16_t u = uint16_t(raw);
    if (u & 0x8000) return int((u & 0x7F) << 8);  // sweep volumes are treated as their target level
    return int(int16_t(u << 1)) >> 1;
}

uint16_t Spu::read(uint32_t off) {
    off &= 0x3FF;
    if (off < 0x180) {
        const Voice& vc = v[off >> 4];
        switch ((off >> 1) & 7) {
        case 0: return uint16_t(vc.volL);
        case 1: return uint16_t(vc.volR);
        case 2: return vc.pitch;
        case 3: return vc.startAddr;
        case 4: return vc.adsr1;
        case 5: return vc.adsr2;
        case 6: return uint16_t(vc.env);
        default: return vc.repeatAddr;
        }
    }
    switch (off) {
    case 0x19C: return uint16_t(endx);
    case 0x19E: return uint16_t(endx >> 16);
    case 0x1A6: return uint16_t(transferAddr >> 3);
    case 0x1AE: return uint16_t((regs[0xD5] & 0x3F) | (irq ? 0x40 : 0));
    case 0x1B8: case 0x1BA: return 0;
    default: break;
    }
    if (off >= 0x200 && off < 0x260) {
        const Voice& vc = v[(off - 0x200) >> 2];
        return uint16_t((off & 2) ? vc.volR : vc.volL);
    }
    return regs[(off >> 1) & 0x1FF];
}

void Spu::write(uint32_t off, uint16_t val) {
    off &= 0x3FF;
    regs[(off >> 1) & 0x1FF] = val;
    if (off < 0x180) {
        Voice& vc = v[off >> 4];
        switch ((off >> 1) & 7) {
        case 0: vc.volL = int16_t(val); break;
        case 1: vc.volR = int16_t(val); break;
        case 2: vc.pitch = val; break;
        case 3: vc.startAddr = val; break;
        case 4: vc.adsr1 = val; break;
        case 5: vc.adsr2 = val; break;
        case 6: break;
        default: vc.repeatAddr = val; vc.repeatSet = true; break;
        }
        return;
    }
    switch (off) {
    case 0x188: keyOn(val); break;
    case 0x18A: keyOn(uint32_t(val) << 16); break;
    case 0x18C: keyOff(val); break;
    case 0x18E: keyOff(uint32_t(val) << 16); break;
    case 0x1A4: irqAddr = uint32_t(val) << 3; break;
    case 0x1A6: transferAddr = uint32_t(val) << 3; break;
    case 0x1A8:
        if (transferAddr + 2 <= ram.size()) std::memcpy(&ram[transferAddr], &val, 2);
        transferAddr = (transferAddr + 2) % uint32_t(ram.size());
        break;
    case 0x1AA: if (!(val & 0x40)) irq = false; break;
    default: break;
    }
}

void Spu::dmaWrite(const uint8_t* data, uint32_t bytes) {
    for (uint32_t i = 0; i < bytes; ++i) ram[(transferAddr + i) % ram.size()] = data[i];
    transferAddr = uint32_t((transferAddr + bytes) % ram.size());
}

void Spu::dmaRead(uint8_t* data, uint32_t bytes) {
    for (uint32_t i = 0; i < bytes; ++i) data[i] = ram[(transferAddr + i) % ram.size()];
    transferAddr = uint32_t((transferAddr + bytes) % ram.size());
}

void Spu::decodeBlock(Voice& vc) {
    const uint32_t a = vc.cur % uint32_t(ram.size());
    if ((regs[0xD5] & 0x40) && (irqAddr & ~15u) == (a & ~15u)) irq = true;
    const uint8_t head = ram[a], flags = ram[a + 1];
    const int shift = (head & 0xF) > 12 ? 9 : (head & 0xF);
    const int filter = std::min((head >> 4) & 7, 4);
    if (flags & 4) { vc.repeatAddr = uint16_t(vc.cur >> 3); vc.repeatSet = true; }
    int16_t old = vc.old, older = vc.older;
    std::memmove(vc.samples, vc.samples + 28, 3 * sizeof(int16_t));
    for (int i = 0; i < 28; ++i) {
        const uint8_t byte = ram[(a + 2 + uint32_t(i / 2)) % ram.size()];
        const int nib = (i & 1) ? (byte >> 4) : (byte & 0xF);
        int s = int(int16_t(nib << 12)) >> shift;
        s += (old * kPos[filter] + older * kNeg[filter] + 32) >> 6;
        const int16_t out = clamp16(s);
        older = old; old = out;
        vc.samples[3 + i] = out;
    }
    vc.old = old; vc.older = older;
}

void Spu::keyOn(uint32_t mask) {
    for (int i = 0; i < 24; ++i) {
        if (!(mask >> i & 1)) continue;
        Voice& vc = v[i];
        vc.cur = uint32_t(vc.startAddr) << 3;
        vc.counter = 0;
        vc.env = 0;
        vc.envCounter = 0;
        vc.phase = Phase::Attack;
        vc.old = vc.older = 0;
        vc.repeatAddr = vc.startAddr;
        vc.repeatSet = false;
        std::memset(vc.samples, 0, sizeof vc.samples);
        endx &= ~(1u << i);
        decodeBlock(vc);
    }
}

void Spu::keyOff(uint32_t mask) {
    for (int i = 0; i < 24; ++i)
        if ((mask >> i & 1) && v[i].phase != Phase::Off) { v[i].phase = Phase::Release; v[i].envCounter = 0; }
}

void Spu::stepEnvelope(Voice& vc) {
    bool exp = false, dec = false;
    int shift = 0, step = 0;
    switch (vc.phase) {
    case Phase::Attack: exp = vc.adsr1 & 0x8000; shift = (vc.adsr1 >> 10) & 0x1F; step = 7 - ((vc.adsr1 >> 8) & 3); break;
    case Phase::Decay: exp = true; dec = true; shift = (vc.adsr1 >> 4) & 0xF; step = -8; break;
    case Phase::Sustain:
        exp = vc.adsr2 & 0x8000; dec = vc.adsr2 & 0x4000; shift = (vc.adsr2 >> 8) & 0x1F;
        step = dec ? -8 + ((vc.adsr2 >> 6) & 3) : 7 - ((vc.adsr2 >> 6) & 3);
        break;
    case Phase::Release: exp = vc.adsr2 & 0x20; dec = true; shift = vc.adsr2 & 0x1F; step = -8; break;
    default: return;
    }
    uint32_t inc = 0x8000u >> std::max(0, shift - 11);
    if (shift < 11) step <<= (11 - shift);
    if (exp && !dec && vc.env > 0x6000) inc = std::max(1u, inc >> 2);
    if (exp && dec) step = (step * vc.env) >> 15;
    vc.envCounter += std::max(1u, inc);
    if (!(vc.envCounter & 0x8000)) return;
    vc.envCounter = 0;
    vc.env = std::clamp(vc.env + step, 0, 0x7FFF);
    switch (vc.phase) {
    case Phase::Attack: if (vc.env >= 0x7FFF) vc.phase = Phase::Decay; break;
    case Phase::Decay: if (vc.env <= int(((vc.adsr1 & 0xF) + 1) * 0x800)) vc.phase = Phase::Sustain; break;
    case Phase::Release: if (vc.env == 0) vc.phase = Phase::Off; break;
    default: break;
    }
}

void Spu::tick(uint32_t frames) {
    for (uint32_t f = 0; f < frames; ++f) {
        int64_t l = 0, r = 0;
        for (int i = 0; i < 24; ++i) {
            Voice& vc = v[i];
            if (vc.phase == Phase::Off) continue;
            const uint32_t idx = vc.counter >> 12;
            const int frac = int(vc.counter & 0xFFF);
            const int p0 = vc.samples[idx + 1], p1 = vc.samples[idx + 2], p2 = vc.samples[idx + 3], p3 = vc.samples[std::min<uint32_t>(idx + 4, 30)];
            const double t = double(frac) / 4096.0;
            double sd = 0.5 * (2.0 * p1 + (p2 - p0) * t + (2.0 * p0 - 5.0 * p1 + 4.0 * p2 - p3) * t * t + (3.0 * p1 - p0 - 3.0 * p2 + p3) * t * t * t);
            int64_t s = std::clamp<int64_t>(int64_t(sd), -32768, 32767);
            s = (s * vc.env) >> 15;
            l += (s * volume(vc.volL)) >> 14;
            r += (s * volume(vc.volR)) >> 14;
            stepEnvelope(vc);
            uint32_t pitch = std::min<uint32_t>(vc.pitch, 0x3FFF);
            vc.counter += pitch;
            if ((vc.counter >> 12) >= 28) {
                vc.counter -= 28u << 12;
                const uint8_t flags = ram[(vc.cur + 1) % ram.size()];
                if (flags & 1) {
                    endx |= 1u << i;
                    if (flags & 2) vc.cur = uint32_t(vc.repeatAddr) << 3;
                    else { vc.env = 0; vc.phase = Phase::Off; continue; }
                } else {
                    vc.cur = (vc.cur + 16) % uint32_t(ram.size());
                }
                decodeBlock(vc);
            }
        }
        const int mainL = volume(int16_t(regs[0xC0])), mainR = volume(int16_t(regs[0xC1]));
        int16_t outL = 0, outR = 0;
        if (spuEnabled() && (regs[0xD5] & 0x4000)) {
            outL = clamp16(int((l * mainL) >> 14));
            outR = clamp16(int((r * mainR) >> 14));
        }
        ++framesProduced;
        if (sink) sink(outL, outR);
    }
}

}  // namespace psx

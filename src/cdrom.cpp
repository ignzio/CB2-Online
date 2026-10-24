#include "cdrom.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace psx {
namespace {
constexpr uint64_t kCyclesPerSector = 33868800 / 150;  // double speed
uint32_t fromBcd(uint8_t v) { return (v >> 4) * 10u + (v & 15u); }
}  // namespace

void CdRom::queue(uint64_t delay, uint8_t irq, std::vector<uint8_t> resp, int64_t sectorLba) {
    pending.push_back({now + delay, irq, std::move(resp), sectorLba});
}

void CdRom::deliver() {
    // A new response can only be posted once the previous interrupt has been acknowledged.
    while (!pending.empty() && pending.front().due <= now && (flags & 7) == 0) {
        Pending p = std::move(pending.front());
        pending.pop_front();
        if (p.sectorLba >= 0) loadSector(uint32_t(p.sectorLba));
        response.assign(p.resp.begin(), p.resp.end());
        flags = uint8_t((flags & ~7) | p.irq);
        if (trace) std::printf("[cd] INT%d resp=%02x.. lba=%lld\n", p.irq, p.resp.empty() ? 0 : p.resp[0], (long long)p.sectorLba);
        updateIrq();
    }
}

void CdRom::loadSector(uint32_t at) {
    uint8_t raw[2352];
    if (!disc || !disc->readSector(at, raw)) { sectorBuf.assign(2048, 0); return; }
    if (mode & 0x20) sectorBuf.assign(raw + 12, raw + 12 + 2340);
    else sectorBuf.assign(raw + 24, raw + 24 + 2048);
}

void CdRom::tick(uint64_t cycles) {
    now += cycles;
    if (reading && now >= nextSector) {
        nextSector = now + (mode & 0x80 ? kCyclesPerSector : kCyclesPerSector * 2);
        queue(0, 1, {status}, lba);
        ++lba;
    }
    deliver();
}

void CdRom::command(uint8_t cmd) {
    ++commandsSeen[cmd & 0x1F];
    std::vector<uint8_t> p(params.begin(), params.end());
    if (trace) { std::printf("[cd] command %02x params:", cmd); for (uint8_t b : p) std::printf(" %02x", b); std::printf("\n"); }
    params.clear();
    const uint64_t kFast = 20000, kSlow = 400000;
    switch (cmd) {
    case 0x01: queue(kFast, 3, {status}); break;                       // Getstat
    case 0x02:                                                         // Setloc
        if (p.size() >= 3) seekLba = (fromBcd(p[0]) * 60 + fromBcd(p[1])) * 75 + fromBcd(p[2]) - 150;
        if (trace && disc) std::printf("[cd] seek lba %u (%s)\n", seekLba, disc->fileAt(seekLba).c_str());
        queue(kFast, 3, {status});
        break;
    case 0x06: case 0x1B:                                              // ReadN / ReadS
        lba = seekLba; reading = true; status = 0x42; nextSector = now + kSlow;
        queue(kFast, 3, {status});
        break;
    case 0x09: reading = false; status = 0x02; queue(kFast, 3, {0x42}); queue(kSlow, 2, {status}); break;  // Pause
    case 0x0A: reading = false; status = 0x02; mode = 0; queue(kFast, 3, {status}); queue(kSlow, 2, {status}); break;  // Init
    case 0x0B: case 0x0C: queue(kFast, 3, {status}); break;            // Mute / Demute
    case 0x0D: if (p.size() >= 2) { filterFile = p[0]; filterChannel = p[1]; } queue(kFast, 3, {status}); break;
    case 0x0E: if (!p.empty()) mode = p[0]; queue(kFast, 3, {status}); break;  // Setmode
    case 0x13: queue(kFast, 3, {status, 0x01, 0x01}); break;           // GetTN
    case 0x14: queue(kFast, 3, {status, 0x00, 0x02}); break;           // GetTD
    case 0x15: case 0x16: lba = seekLba; reading = false; queue(kFast, 3, {status}); queue(kSlow, 2, {status}); break;  // SeekL / SeekP
    case 0x19:                                                         // Test
        if (!p.empty() && p[0] == 0x20) queue(kFast, 3, {0x94, 0x09, 0x19, 0xC0});
        else queue(kFast, 3, {status});
        break;
    case 0x1A: queue(kFast, 3, {status}); queue(kSlow, 2, {0x02, 0x00, 0x20, 0x00, 'S', 'C', 'E', 'A'}); break;  // GetID
    case 0x08: reading = false; status = 0x00; queue(kFast, 3, {0x02}); queue(kSlow, 2, {status}); break;      // Stop
    case 0x07: status = 0x02; queue(kFast, 3, {status}); queue(kSlow, 2, {status}); break;                       // MotorOn
    case 0x10: case 0x11: queue(kFast, 3, {0, 0, 0, 0, 0, 0, 0, 0}); break;
    default: queue(kFast, 5, {uint8_t(status | 1), 0x40}); break;      // unsupported: error
    }
}

uint8_t CdRom::read(uint32_t reg) {
    switch (reg & 3) {
    case 0: {
        uint8_t s = uint8_t(index);
        if (params.empty()) s |= 0x08;
        if (params.size() < 16) s |= 0x10;
        if (!response.empty()) s |= 0x20;
        if (dataPos < data.size()) s |= 0x40;
        return s;
    }
    case 1: { if (response.empty()) return 0; const uint8_t v = response.front(); response.pop_front(); return v; }
    case 2: { return dataPos < data.size() ? data[dataPos++] : 0; }
    default: return index & 1 ? uint8_t(0xE0 | flags) : uint8_t(0xE0 | enable);
    }
}

void CdRom::write(uint32_t reg, uint8_t v) {
    const uint32_t r = reg & 3;
    if (r == 0) { index = v & 3; return; }
    const int key = index * 4 + int(r);
    switch (key) {
    case 1: command(v); break;                                    // 1801.0 command
    case 2: params.push_back(v); break;                           // 1802.0 parameter
    case 3: request = v; if (trace) std::printf("[cd] request %02x sector=%zu bytes\n", v, sectorBuf.size()); if (v & 0x80) { if (dataPos >= data.size()) { data = sectorBuf; dataPos = 0; } } else { data.clear(); dataPos = 0; } break;  // 1803.0 request
    case 6: enable = v & 0x1F; updateIrq(); break;                // 1802.1 interrupt enable
    case 7:                                                       // 1803.1 acknowledge
        if (trace) std::printf("[cd] ack %02x (flags %02x)\n", v, flags);
        flags &= uint8_t(~(v & 0x1F));
        if (v & 0x40) params.clear();
        updateIrq();
        deliver();
        break;
    default: break;
    }
}

uint32_t CdRom::readDataWord() {
    uint32_t w = 0;
    for (int i = 0; i < 4; ++i) w |= uint32_t(dataPos < data.size() ? data[dataPos++] : 0) << (8 * i);
    return w;
}

}  // namespace psx

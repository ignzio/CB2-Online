#include "psx.hpp"

#include "recomp_rt.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <stdexcept>

namespace psx {

namespace {
constexpr uint32_t kSentinel = 0x7FFFFFF0u;
constexpr uint64_t kCyclesPerInstruction = 2;
}  // namespace

Platform::Platform() {
    card.format();
    cpu.ram = ram.data();
    cpu.cop0[12] = 0x00000400u;  // IM2 set; IEc stays clear until the game leaves its critical section
    cd.disc = &disc;
    nextVblank = kFrameCycles;
    cpu.ioRead = [this](r3000::Cpu&, uint32_t a, int s) { return ioRead(a, s); };
    cpu.ioWrite = [this](r3000::Cpu&, uint32_t a, uint32_t v, int s) { ioWrite(a, v, s); };
    cpu.bios = [this](r3000::Cpu&, uint32_t vec) { return bios(vec); };
    cpu.intercept = [this](r3000::Cpu&, uint32_t pc) { return syscallHook(pc); };
    // Kernel function tables hold pointers to stub addresses; the game's libraries read these tables.
    const uint32_t tables[3] = {0x200, 0x874, 0x674};
    for (uint32_t t = 0; t < 3; ++t)
        for (uint32_t n = 0; n < 0x100; ++n) {
            const uint32_t v = 0x80000000u | (kStubBase + t * 0x400 + n * 4);
            std::memcpy(&ram[tables[t] + 4 * n], &v, 4);
        }
}

void Platform::setRamMegabytes(uint32_t mb) {
    if (mb < 2) mb = 2;
    if (mb > 16) mb = 16;  // GPU ordering-table links are 24-bit: the original engine cannot address more than 16 MiB
    uint32_t pow2 = 2;
    while (pow2 < mb) pow2 <<= 1;
    mb = pow2;
    ram.assign(size_t(mb) << 20, 0);
    cpu.ram = ram.data();
    cpu.ramMask = (mb << 20) - 1;
    // New RAM above the retail 2 MiB is not mirrored, so the stack moves to the top of the larger memory.
    initialSp = 0x80000000u + (mb << 20) - 0x10;
}

bool Platform::loadExe(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    return loadExeImage(std::vector<uint8_t>((std::istreambuf_iterator<char>(f)), {}));
}

bool Platform::loadExeImage(const std::vector<uint8_t>& b) {
    if (b.size() < 0x800 || std::memcmp(b.data(), "PS-X EXE", 8) != 0) return false;
    auto w = [&](size_t o) { uint32_t v; std::memcpy(&v, &b[o], 4); return v; };
    uint32_t pc = w(0x10), gp = w(0x14), dest = w(0x18), size = w(0x1C), sp = w(0x30) ? w(0x30) + w(0x34) : 0x801FFFF0u;
    if (initialSp) sp = initialSp;
    const size_t n = std::min<size_t>(size, b.size() - 0x800);
    std::memcpy(ram.data() + (dest & ramMask()), b.data() + 0x800, n);
    if (ramMask() + 1 > r3000::kRamSize) {
        // The Psy-Q start-up code derives the stack and the heap end from this "RAM size" word of the data segment,
        // so changing it hands the extra memory to the engine's heap (verified: heap end moves to the new top).
        const uint32_t sizeWord = 0x8005F188u & ramMask();
        const uint32_t v = ramMask() + 1;
        std::memcpy(ram.data() + sizeWord, &v, 4);
    }
    cpu.r[28] = gp;
    cpu.r[29] = cpu.r[30] = sp;
    cpu.reset(pc);
    return true;
}

bool Platform::mountDisc(const std::string& folder, const std::vector<std::string>& overlays) { return disc.build(folder, overlays); }

bool Platform::mountBundle(const std::string& file, const std::vector<BundledFile>& files, const std::vector<std::string>& overlays) {
    return disc.buildFromBundle(file, files, overlays);
}

// ---------------------------------------------------------------------------------------------------------------
// Time, interrupts, vblank
// ---------------------------------------------------------------------------------------------------------------

void Platform::tickTimers(uint64_t delta) {
    for (int i = 0; i < 3; ++i) {
        Timer& t = timers[i];
        uint64_t div = 1;
        if (i == 0 && (t.mode & 0x100)) div = 6;
        if (i == 1 && (t.mode & 0x100)) div = 2146;
        if (i == 2 && (t.mode & 0x200)) div = 8;
        t.rem += delta;
        const uint64_t units = t.rem / div;
        t.rem %= div;
        if (!units) continue;
        uint64_t v = t.value + units;
        const bool resetOnTarget = (t.mode & 8) != 0;
        const uint32_t limit = resetOnTarget ? t.target : 0xFFFF;
        auto fire = [&]() {
            if (t.irqFired && !(t.mode & 0x40)) return;  // one-shot mode: a single interrupt per mode write
            t.irqFired = true;
            raiseIrq(0x10u << i);
        };
        if (t.target && t.value < t.target && v >= t.target) {
            t.mode |= 0x800;
            if (t.mode & 0x10) fire();
        }
        if (v > limit) {
            if (!resetOnTarget || t.target == 0) { t.mode |= 0x1000; if (t.mode & 0x20) fire(); }
            const uint64_t span = uint64_t(limit) + 1;
            v %= span;
        }
        t.value = uint32_t(v);
    }
}

void Platform::updatePads() {
    if (!padActive || !padBuf1) return;
    uint8_t pad[34] = {};
    pad[0] = 0x00; pad[1] = 0x41;
    pad[2] = uint8_t(padButtons); pad[3] = uint8_t(padButtons >> 8);
    for (uint32_t i = 0; i < 34 && i < padLen1; ++i) cpu.wr8(padBuf1 + i, pad[i]);
    if (padBuf2) {
        cpu.wr8(padBuf2, 0xFF);
        for (uint32_t i = 1; i < 34 && i < padLen2; ++i) cpu.wr8(padBuf2 + i, 0xFF);
    }
}

void Platform::service() {
    const uint64_t delta = cycles - lastService;
    lastService = cycles;
    tickTimers(delta);
    cd.tick(delta);
    spuCycles += delta;
    if (const uint64_t frames = spuCycles / 768) { spuCycles %= 768; spu.tick(uint32_t(frames)); }
    if (spu.irq && !spuIrqSeen) raiseIrq(0x200);
    spuIrqSeen = spu.irq;
    if (cd.irqLine && !cdPrevIrq) raiseIrq(4);
    cdPrevIrq = cd.irqLine;
    if (cycles >= nextVblank) {
        nextVblank += kFrameCycles;
        raiseIrq(1);
        gpu.vblank();
        ++frames;
        updatePads();
        if (onVblank) onVblank();
        if (yieldOnVblank) vblankYield = true;
    }
    if (istat & imask) cpu.cop0[13] |= 0x400u; else cpu.cop0[13] &= ~0x400u;
}

void Platform::addHook(uint32_t pc, std::function<void(Platform&)> fn) {
    if (hookMark.empty()) hookMark.assign((cpu.ramMask + 1) >> 2, 0);
    auto& slot = hooks[pc];
    if (slot) { auto earlier = std::move(slot); slot = [earlier, later = std::move(fn)](Platform& pl) { earlier(pl); later(pl); }; }   // several users of one address all run
    else slot = std::move(fn);
    hookMark[(pc & 0x1FFFFFFFu & cpu.ramMask) >> 2] = 1;
}

uint32_t Platform::callFunction(uint32_t fn, std::initializer_list<uint32_t> regArgs, std::initializer_list<uint32_t> stackArgs) {
    uint32_t r[32];
    std::memcpy(r, cpu.r, sizeof r);
    const uint32_t hi = cpu.hi, lo = cpu.lo, pc = cpu.pc, nextPc = cpu.nextPc, pendReg = cpu.pendReg, pendVal = cpu.pendVal, sr = cpu.cop0[12];
    const uint64_t startInstr = cpu.instructions;
    cpu.pendReg = 0;
    cpu.cop0[12] &= ~1u;   // no interrupts while the call runs
    uint32_t sp = (r[29] - 0x100) & ~7u;
    size_t i = 0;
    for (uint32_t a : regArgs) if (i < 4) cpu.r[4 + i++] = a;
    uint32_t off = 0x10;
    for (uint32_t a : stackArgs) { cpu.wr32(sp + off, a); off += 4; }
    cpu.r[29] = sp;
    cpu.r[31] = kSentinel;
    cpu.pc = fn; cpu.nextPc = fn + 4;
    auto restore = [&] {
        std::memcpy(cpu.r, r, sizeof r);
        cpu.hi = hi; cpu.lo = lo; cpu.pc = pc; cpu.nextPc = nextPc; cpu.pendReg = pendReg; cpu.pendVal = pendVal; cpu.cop0[12] = sr;
        cpu.instructions = startInstr;
        cpu.fault = false;
    };
    try {
        for (uint64_t guard = 0; cpu.pc != kSentinel; ++guard) {
            if (guard > callBudget) throw std::runtime_error("guest function called from a hook did not return");
            cpu.step();
        }
    } catch (...) {
        restore();   // the caller may survive a fault in the called function
        throw;
    }
    const uint32_t result = cpu.r[2];
    lastCallInstructions = cpu.instructions - startInstr;
    std::memcpy(cpu.r, r, sizeof r);
    cpu.hi = hi; cpu.lo = lo; cpu.pc = pc; cpu.nextPc = nextPc; cpu.pendReg = pendReg; cpu.pendVal = pendVal; cpu.cop0[12] = sr;
    cpu.instructions = startInstr;   // the call happens "between" two instructions: guest time does not move
    syncTime();
    return result;
}

uint32_t Platform::callGuest(uint32_t fn, uint32_t sp, uint32_t a0) {
    cpu.r[29] = sp & ~7u;
    cpu.r[31] = kSentinel;
    cpu.r[4] = a0;
    cpu.pc = fn; cpu.nextPc = fn + 4;
    for (uint64_t guard = 0; cpu.pc != kSentinel; ++guard) {
        if (guard > 4'000'000) throw std::runtime_error("event handler did not return");
        stepHybrid();
        syncTime();
    }
    return cpu.r[2];
}

void Platform::deliverEvent(uint32_t cls, uint32_t spec, uint32_t sp) {
    for (auto& e : events) {
        if (!e.used || e.cls != cls || e.spec != spec || e.status != 0x2000) continue;
        if (e.mode == 0x1000 && e.handler) callGuest(e.handler, sp);
        else e.status = 0x4000;
    }
}

// The part of the BIOS exception handler that runs before the library's hook: root counter interrupts become
// events (class 0xF2000000 + counter, spec 2), and the vertical blank is delivered as counter 3.
void Platform::kernelInterrupts(uint32_t sp) {
    for (int t = 0; t < 3; ++t) {
        const uint32_t bit = 0x10u << t;
        if (!(istat & imask & bit)) continue;
        istat &= ~bit;
        deliverEvent(0xF2000000u + uint32_t(t), 2, sp);
    }
    if (istat & imask & 1) deliverEvent(0xF2000003u, 2, sp);
}

void Platform::enterInterrupt() {
    if (cpu.pendReg) { cpu.r[cpu.pendReg] = cpu.pendVal; cpu.pendReg = 0; }
    std::memcpy(saved.r, cpu.r, sizeof saved.r);
    saved.hi = cpu.hi; saved.lo = cpu.lo; saved.epc = cpu.pc; saved.sr = cpu.cop0[12]; saved.valid = true;
    cpu.cop0[12] &= ~1u;
    // The BIOS exception handler resumes a longjmp-style buffer registered with HookEntryInt.
    uint32_t b[12];
    for (int i = 0; i < 12; ++i) b[i] = rd32(hookBuf + 4u * uint32_t(i));
    kernelInterrupts(b[1]);
    cpu.r[31] = b[0]; cpu.r[29] = b[1]; cpu.r[30] = b[2];
    for (int i = 0; i < 8; ++i) cpu.r[16 + i] = b[3 + i];
    cpu.r[28] = b[11];
    cpu.r[2] = 1;
    cpu.pc = b[0]; cpu.nextPc = b[0] + 4;
}

void Platform::returnFromException() {
    if (!saved.valid) { cpu.pc = cpu.r[31]; cpu.nextPc = cpu.pc + 4; return; }
    std::memcpy(cpu.r, saved.r, sizeof saved.r);
    cpu.hi = saved.hi; cpu.lo = saved.lo;
    cpu.cop0[12] = saved.sr;
    cpu.pc = saved.epc; cpu.nextPc = saved.epc + 4;
    saved.valid = false;
}

namespace {
// PsyQ VSync(): the loop that waits for the vertical-blank counter (0x8005DC98) to reach the target in $a0, with its time-out counter at 0x10($sp).
constexpr uint32_t kVsyncSpin = 0x8004A5F4, kVsyncCounter = 0x8005DC98;
constexpr uint32_t kVsyncCode[][2] = {{0x8004A5F4, 0x8FA20010}, {0x8004A5F8, 0}, {0x8004A5FC, 0x2442FFFF}, {0x8004A600, 0xAFA20010}, {0x8004A604, 0x8FA20010}, {0x8004A608, 0},
                                       {0x8004A60C, 0x1443000C}, {0x8004A610, 0}, {0x8004A640, 0x3C028006}, {0x8004A644, 0x8C42DC98}, {0x8004A648, 0}, {0x8004A64C, 0x0044102A},
                                       {0x8004A650, 0x1440FFE8}, {0x8004A654, 0}};
constexpr uint64_t kVsyncLoopInstructions = 14;   // one pass: 8 instructions up to the branch and its delay slot, 6 more to the loop-back branch and its delay slot
}  // namespace

// Cycles from now until something other than the CPU can change the machine: the vertical blank, the CD controller, a timer interrupt, the SPU interrupt.
uint64_t Platform::cyclesToNextEvent() const {
    if (spu.irqEnabled()) return 0;
    const uint64_t behind = cycles - lastService;   // the devices have been told the time as of the last service()
    uint64_t horizon = nextVblank > cycles ? nextVblank - cycles : 0;
    auto soonest = [&](uint64_t in) { if (in != UINT64_MAX) horizon = std::min(horizon, in > behind ? in - behind : 0); };
    soonest(cd.cyclesToEvent());
    for (int i = 0; i < 3; ++i) {
        const Timer& t = timers[i];
        if (!(t.mode & 0x30) || (t.irqFired && !(t.mode & 0x40))) continue;
        uint64_t div = 1;
        if (i == 0 && (t.mode & 0x100)) div = 6;
        if (i == 1 && (t.mode & 0x100)) div = 2146;
        if (i == 2 && (t.mode & 0x200)) div = 8;
        const uint32_t limit = (t.mode & 8) ? t.target : 0xFFFF;
        uint64_t units = UINT64_MAX;
        if ((t.mode & 0x10) && t.target && t.value < t.target) units = t.target - t.value;
        if (t.mode & 0x20) units = std::min<uint64_t>(units, t.value <= limit ? uint64_t(limit) + 1 - t.value : 0);
        if (units == UINT64_MAX) continue;
        const uint64_t need = units * div;
        soonest(need > t.rem ? need - t.rem : 0);
    }
    return horizon;
}

uint64_t Platform::skipVsyncSpin(uint64_t room) {
    constexpr uint64_t kMargin = 256;   // stay this far from the next event: it is then reached by running the loop normally
    if (cpu.pendReg != 0 || cpu.r[3] != 0xFFFFFFFFu) return 0;
    const uint64_t horizon = cyclesToNextEvent();
    if (horizon <= kMargin + 2 * kVsyncLoopInstructions * 2) return 0;
    uint64_t passes = (horizon - kMargin) / (kVsyncLoopInstructions * 2);
    const uint32_t timeoutAt = cpu.r[29] + 0x10;
    const int32_t timeout = int32_t(cpu.rd32(timeoutAt));
    if (timeout <= 2 || int32_t(cpu.rd32(kVsyncCounter)) >= int32_t(cpu.r[4])) return 0;
    passes = std::min<uint64_t>(passes, uint64_t(timeout - 2));   // never skip the time-out itself
    passes = std::min(passes, room / kVsyncLoopInstructions);   // run() hands control back after a fixed number of instructions: skipped ones count, so it stays where it always did
    if (passes == 0) return 0;
    for (const auto& w : kVsyncCode) if (cpu.rd32(w[0]) != w[1]) return 0;
    cpu.wr32(timeoutAt, uint32_t(timeout - int32_t(passes)));
    cpu.r[2] = 1;   // the result of the loop's last comparison
    cpu.instructions += passes * kVsyncLoopInstructions;
    idleSkippedInstructions += passes * kVsyncLoopInstructions;
    syncTime();
    return passes * kVsyncLoopInstructions;
}

std::string Platform::run(uint64_t count) {
    if (useNative) {
        std::unique_lock<std::mutex> lk(gameMutex);
        if (gameState == GameState::Finished) return lastError;
        runLimit = cpu.instructions + count;
        if (gameState == GameState::NotStarted) {
            gameState = GameState::Running;
            gameThread = std::thread([this] { gameMain(); });
        } else {
            gameState = GameState::Running;
            gameCv.notify_all();
        }
        gameCv.wait(lk, [&] { return gameState != GameState::Running; });
        return gameState == GameState::Finished ? lastError : std::string();
    }
    try {
        if (serviceAt > cycles + 64) serviceAt = cycles;   // stale: a saved state was loaded
        for (uint64_t i = 0; i < count; ++i) {
            if (cycles >= serviceAt) { service(); serviceAt += 64 * ((cycles - serviceAt) / 64 + 1); }
            if ((istat & imask) && hookBuf && !saved.valid && (cpu.cop0[12] & 1) && cpu.nextPc == cpu.pc + 4) enterInterrupt();
            if (entryHook && cpu.nextPc == cpu.pc + 4 && lookupNative(cpu.pc)) entryHook(cpu.pc);
            if (!hookMark.empty() && cpu.nextPc == cpu.pc + 4 && !inHook) {
                const uint32_t p = cpu.pc & 0x1FFFFFFFu;
                if (p < hookMark.size() * 4 && hookMark[p >> 2]) {
                    const auto it = hooks.find(cpu.pc);
                    if (it != hooks.end()) {
                        inHook = true;
                        try { it->second(*this); } catch (...) { inHook = false; throw; }
                        inHook = false;
                    }
                }
            }
            if (idleSkip && cpu.pc == kVsyncSpin && cpu.nextPc == kVsyncSpin + 4 && !(istat & imask) && !inHook) i += skipVsyncSpin(count - i - 1);
            cpu.step();
            syncTime();
            if (pcHist && (cpu.instructions & 255) == 0) ++(*pcHist)[cpu.pc];
        }
    } catch (const std::exception& e) {
        char b[160];
        std::snprintf(b, sizeof b, " (pc=%08x ra=%08x a0=%08x a2=%08x v0=%08x)", cpu.pc, cpu.r[31], cpu.r[4], cpu.r[6], cpu.r[2]);
        lastError = std::string(e.what()) + b;
        return lastError;
    }
    return {};
}

bool Platform::syscallHook(uint32_t pc) {
    const uint32_t phys = pc & 0x1FFFFFFF;
    if (phys < 0x10000 && phys >= kStubBase + 0xC00) {
        char b[96];
        std::snprintf(b, sizeof b, "execution entered the BIOS kernel area at %08x (ra=%08x)", pc, cpu.r[31]);
        throw std::runtime_error(b);
    }
    auto ov = overrides.find(pc);
    if (ov != overrides.end()) {
        cpu.r[2] = ov->second;
        cpu.pc = cpu.r[31]; cpu.nextPc = cpu.pc + 4;
        return true;
    }
    if (phys >= kStubBase && phys < kStubBase + 0xC00 && (phys & 3) == 0) {
        const uint32_t off = phys - kStubBase;
        return biosCall(0xA0 + (off >> 10) * 0x10, (off & 0x3FF) >> 2);
    }
    const uint32_t w = cpu.rd32(pc);
    if ((w >> 26) != 0 || (w & 0x3F) != 0x0C) return false;
    ++syscalls[cpu.r[4]];
    if (cpu.r[4] == 1) { cpu.r[2] = (cpu.cop0[12] & 1) ? 1 : 0; cpu.cop0[12] &= ~1u; }
    else if (cpu.r[4] == 2) cpu.cop0[12] |= 1;
    cpu.pc = cpu.nextPc; cpu.nextPc += 4;
    return true;
}

// ---------------------------------------------------------------------------------------------------------------
// Hardware registers
// ---------------------------------------------------------------------------------------------------------------

static const char* deviceOf(uint32_t p) {
    if (p >= 0x1F801810 && p < 0x1F801818) return "GPU";
    if (p >= 0x1F801800 && p < 0x1F801804) return "CD";
    if (p >= 0x1F801C00 && p < 0x1F802000) return "SPU";
    if (p >= 0x1F801080 && p < 0x1F801100) return "DMA";
    if (p >= 0x1F8010F0 && p < 0x1F8010F8) return "DMA";
    if (p >= 0x1F801100 && p < 0x1F801130) return "TIMER";
    if (p >= 0x1F801040 && p < 0x1F801050) return "PAD";
    if (p >= 0x1F801070 && p < 0x1F801078) return "IRQ";
    return "OTHER";
}

uint32_t Platform::ioRead(uint32_t addr, int size) {
    const uint32_t a = addr & 0x1FFFFFFF;
    ++ioReads[a];
    if (recordHwSites) ++hwSites[cpu.pc - 4][deviceOf(a)];
    if (a >= 0x1F801800 && a < 0x1F801804) return cd.read(a);
    if (a == 0x1F801810) return gpu.read();
    if (a == 0x1F801814) return gpu.stat();
    if (a == 0x1F801070) return istat;
    if (a == 0x1F801074) return imask;
    if (a >= 0x1F801080 && a < 0x1F8010F0) {
        const uint32_t ch = (a - 0x1F801080) >> 4, reg = (a >> 2) & 3;
        return reg == 0 ? dmaMadr[ch] : reg == 1 ? dmaBcr[ch] : dmaChcr[ch];
    }
    if (a == 0x1F8010F0) return dpcr;
    if (a == 0x1F8010F4) {
        const uint32_t flags = (dicr >> 24) & 0x7F, en = (dicr >> 16) & 0x7F;
        const bool master = (dicr >> 15 & 1) || ((dicr >> 23 & 1) && (flags & en));
        return (dicr & 0x7FFFFFFF) | (master ? 0x80000000u : 0);
    }
    if (a >= 0x1F801100 && a < 0x1F801130) {
        Timer& t = timers[(a - 0x1F801100) >> 4];
        switch ((a >> 2) & 3) {
        case 0: return t.value & 0xFFFF;
        case 1: { const uint32_t m = t.mode; t.mode &= ~0x1800u; return m; }
        case 2: return t.target;
        default: return 0;
        }
    }
    if (a >= 0x1F801C00 && a < 0x1F802000) {
        const uint32_t o = a - 0x1F801C00;
        uint32_t v = spu.read(o);
        if (size == 4) v |= uint32_t(spu.read(o + 2)) << 16;
        return v;
    }
    return 0;
}

void Platform::dmaDone(int ch) {
    dmaChcr[ch] &= ~(1u << 24 | 1u << 28);
    if ((dicr >> (16 + ch)) & 1) {
        const bool before = ((dicr >> 24) & 0x7F) & ((dicr >> 16) & 0x7F);
        dicr |= 1u << (24 + ch);
        (void)before;
        if ((dicr >> 23) & 1) raiseIrq(8);
    }
}

void Platform::dmaStart(int ch) {
    uint8_t* mem = ram.data();
    const uint32_t ramWordMask = ramMask() & ~3u;
    const uint32_t chcr = dmaChcr[ch], bcr = dmaBcr[ch];
    const uint32_t sync = (chcr >> 9) & 3;
    uint32_t addr = dmaMadr[ch] & ramWordMask;
    uint32_t words = sync == 0 ? (bcr & 0xFFFF ? bcr & 0xFFFF : 0x10000) : (bcr & 0xFFFF) * (bcr >> 16);
    const bool fromRam = (chcr & 1) != 0;
    switch (ch) {
    case 2:
        if (sync == 2) {
            for (uint32_t guard = 0; guard < (1u << 20); ++guard) {
                uint32_t hdr;
                std::memcpy(&hdr, mem + (addr & ramWordMask), 4);
                const uint32_t n = hdr >> 24;
                for (uint32_t i = 0; i < n; ++i) {
                    uint32_t w;
                    std::memcpy(&w, mem + ((addr + 4 + 4 * i) & ramWordMask), 4);
                    gpu.gp0(w);
                }
                if (hdr & 0x800000) break;
                addr = hdr & ramWordMask;
            }
            if (onDisplayList) onDisplayList();
        } else if (fromRam) {
            for (uint32_t i = 0; i < words; ++i) { uint32_t w; std::memcpy(&w, mem + ((addr + 4 * i) & ramWordMask), 4); gpu.gp0(w); }
        } else {
            for (uint32_t i = 0; i < words; ++i) { const uint32_t w = gpu.read(); std::memcpy(mem + ((addr + 4 * i) & ramWordMask), &w, 4); }
        }
        break;
    case 3:
        if (traceIrq) std::printf("[dma3] -> %08x words=%u (cd data left=%zu)\n", addr, words, cd.dataLeft());
        for (uint32_t i = 0; i < words; ++i) { const uint32_t w = cd.readDataWord(); std::memcpy(mem + ((addr + 4 * i) & ramWordMask), &w, 4); }
        break;
    case 4:
        if (fromRam) spu.dmaWrite(mem + (addr & ramWordMask), words * 4 > ram.size() - (addr & ramWordMask) ? uint32_t(ram.size()) - (addr & ramWordMask) : words * 4);
        else spu.dmaRead(mem + (addr & ramWordMask), words * 4);
        break;
    case 6: {
        const uint32_t n = bcr & 0xFFFF ? bcr & 0xFFFF : 0x10000;
        for (uint32_t i = 0; i < n; ++i) {
            const uint32_t w = i == n - 1 ? 0xFFFFFFu : (addr - 4) & 0xFFFFFF;
            std::memcpy(mem + (addr & ramWordMask), &w, 4);
            addr = (addr - 4) & ramWordMask;
        }
        break;
    }
    default: break;
    }
    dmaDone(ch);
}

void Platform::ioWrite(uint32_t addr, uint32_t v, int size) {
    const uint32_t a = addr & 0x1FFFFFFF;
    ++ioWrites[a];
    if (recordHwSites) ++hwSites[cpu.pc - 4][deviceOf(a)];
    if (a >= 0x1F801800 && a < 0x1F801804) { cd.write(a, uint8_t(v)); return; }
    if (a == 0x1F801810) { gpu.gp0(v); return; }
    if (a == 0x1F801814) { gpu.gp1(v); return; }
    if (a == 0x1F801070) { if (traceIrq) std::printf("[irq] write I_STAT %04x (was %04x) pc=%08x\n", v, istat, cpu.pc); istat &= v; return; }
    if (a == 0x1F801074) { if (traceIrq) std::printf("[irq] write I_MASK %04x pc=%08x ra=%08x cyc=%llu\n", v, cpu.pc, cpu.r[31], (unsigned long long)cycles); imask = v & 0x7FF; return; }
    if (a >= 0x1F801080 && a < 0x1F8010F0) {
        const uint32_t ch = (a - 0x1F801080) >> 4, reg = (a >> 2) & 3;
        if (reg == 0) {
            dmaMadr[ch] = v & (ramMask() | 0xFFFFFFu);
            if (ch == 3 && traceIrq && (v & 0xFFFFFF) > 0xA0000 && !stackDumped) {
                stackDumped = true;
                std::printf("[dma3 madr %06x] pc=%08x ra=%08x stack:", v & 0xFFFFFF, cpu.pc, cpu.r[31]);
                for (uint32_t i = 0; i < 64; ++i) {
                    const uint32_t w = rd32(cpu.r[29] + 4 * i);
                    if (w >= 0x80010000u && w < 0x8005F800u) std::printf(" %08x", w);
                }
                std::printf("\n");
            }
        }
        else if (reg == 1) dmaBcr[ch] = v;
        else {
            dmaChcr[ch] = v;
            const bool trigger = (v & (1u << 24)) && (((v >> 9) & 3) != 0 || (v & (1u << 28)) || ch == 6 || ch == 3 || ch == 4);
            if (trigger) dmaStart(int(ch));
        }
        return;
    }
    if (a == 0x1F8010F0) { dpcr = v; return; }
    if (a == 0x1F8010F4) { dicr = (v & 0x00FFFFFFu) | (dicr & 0x7F000000u & ~(v & 0x7F000000u)); return; }
    if (a >= 0x1F801100 && a < 0x1F801130) {
        Timer& t = timers[(a - 0x1F801100) >> 4];
        switch ((a >> 2) & 3) {
        case 0: t.value = v & 0xFFFF; break;
        case 1: if (traceIrq) std::printf("[timer%u] mode=%04x pc=%08x\n", (a - 0x1F801100) >> 4, v, cpu.pc); t.mode = (v & 0x3FF) | 0x400; t.value = 0; t.rem = 0; t.irqFired = false; break;
        case 2: if (traceIrq) std::printf("[timer%u] target=%04x pc=%08x\n", (a - 0x1F801100) >> 4, v, cpu.pc); t.target = v & 0xFFFF; break;
        default: break;
        }
        return;
    }
    if (a >= 0x1F801C00 && a < 0x1F802000) {
        const uint32_t o = a - 0x1F801C00;
        if (size == 1) return;
        spu.write(o & ~1u, uint16_t(v));
        if (size == 4) spu.write((o & ~1u) + 2, uint16_t(v >> 16));
        return;
    }
}

// ---------------------------------------------------------------------------------------------------------------
// BIOS kernel replacement
// ---------------------------------------------------------------------------------------------------------------

const char* Platform::biosName(uint32_t vec, uint32_t fn) {
    static const std::map<uint32_t, const char*> names = {
        {0xA013, "setjmp"}, {0xA014, "longjmp"}, {0xA017, "strcmp"}, {0xA018, "strncmp"}, {0xA019, "strcpy"}, {0xA01A, "strncpy"},
        {0xA01B, "strlen"}, {0xA02A, "memcpy"}, {0xA02B, "memset"}, {0xA02C, "memmove"}, {0xA02D, "memcmp"}, {0xA02E, "memchr"},
        {0xA02F, "rand"}, {0xA030, "srand"}, {0xA033, "malloc"}, {0xA034, "free"}, {0xA039, "InitHeap"}, {0xA03C, "putchar"},
        {0xA03E, "puts"}, {0xA03F, "printf"}, {0xA044, "FlushCache"}, {0xA04A, "InitCARD"}, {0xA04B, "StartCARD"}, {0xA070, "_bu_init"},
        {0xA071, "_96_init"}, {0xA072, "_96_remove"},
        {0xB007, "DeliverEvent"}, {0xB008, "OpenEvent"}, {0xB009, "CloseEvent"}, {0xB00A, "WaitEvent"}, {0xB00B, "TestEvent"},
        {0xB00C, "EnableEvent"}, {0xB00D, "DisableEvent"}, {0xB012, "InitPad"}, {0xB013, "StartPad"}, {0xB014, "StopPad"},
        {0xB017, "ReturnFromException"}, {0xB018, "ResetEntryInt"}, {0xB019, "HookEntryInt"}, {0xB020, "UnDeliverEvent"},
        {0xB03D, "putchar"}, {0xB03F, "puts"}, {0xB04A, "InitCARD"}, {0xB04B, "StartCARD"}, {0xB04C, "StopCARD"}, {0xB056, "GetC0Table"},
        {0xB057, "GetB0Table"}, {0xB05B, "ChangeClearPad"},
        {0xC000, "EnqueueTimerAndVblankIrqs"}, {0xC001, "EnqueueSyscallHandler"}, {0xC002, "SysEnqIntRP"}, {0xC003, "SysDeqIntRP"},
        {0xC007, "InstallExceptionHandlers"}, {0xC008, "SysInitMemory"}, {0xC00A, "ChangeClearRCnt"}, {0xC012, "InstallDevices"},
    };
    auto it = names.find(vec << 8 | fn);
    return it == names.end() ? "?" : it->second;
}

std::string Platform::readString(uint32_t a) {
    std::string s;
    for (int i = 0; i < 4096; ++i) { const char c = char(cpu.rd8(a + uint32_t(i))); if (!c) break; s += c; }
    return s;
}

std::string Platform::formatPrintf(uint32_t fmtAddr, int firstArg) {
    const std::string fmt = readString(fmtAddr);
    std::string out;
    int arg = firstArg;
    auto next = [&]() -> uint32_t {
        const uint32_t v = arg < 4 ? cpu.r[4 + arg] : rd32(cpu.r[29] + 16 + 4u * uint32_t(arg - 4));
        ++arg;
        return v;
    };
    for (size_t i = 0; i < fmt.size(); ++i) {
        if (fmt[i] != '%') { out += fmt[i]; continue; }
        std::string spec = "%";
        ++i;
        while (i < fmt.size() && std::strchr("-+ #0123456789.", fmt[i])) spec += fmt[i++];
        while (i < fmt.size() && (fmt[i] == 'l' || fmt[i] == 'h')) ++i;
        if (i >= fmt.size()) break;
        char buf[512];
        switch (fmt[i]) {
        case '%': out += '%'; break;
        case 'd': case 'i': std::snprintf(buf, sizeof buf, (spec + "d").c_str(), int(next())); out += buf; break;
        case 'u': std::snprintf(buf, sizeof buf, (spec + "u").c_str(), next()); out += buf; break;
        case 'x': case 'X': case 'o': std::snprintf(buf, sizeof buf, (spec + fmt[i]).c_str(), next()); out += buf; break;
        case 'c': out += char(next()); break;
        case 'p': std::snprintf(buf, sizeof buf, "%08x", next()); out += buf; break;
        case 's': std::snprintf(buf, sizeof buf, (spec + "s").c_str(), readString(next()).c_str()); out += buf; break;
        default: out += spec; out += fmt[i]; break;
        }
    }
    return out;
}

int Platform::cardPort(const std::string& p) {
    if (p.size() >= 5 && p.compare(0, 2, "bu") == 0 && p[4] == ':') return p[2] == '0' && p[3] == '0' ? 0 : (p[2] == '1' && p[3] == '0' ? 1 : -1);
    return -1;
}

void Platform::cardEvents(uint32_t hwSpec, uint32_t swSpec) {
    uint32_t keep[32];
    std::memcpy(keep, cpu.r, sizeof keep);
    const uint32_t hi = cpu.hi, lo = cpu.lo;
    deliverEvent(0xF0000011u, hwSpec, keep[29] - 64);
    deliverEvent(0xF4000001u, swSpec, keep[29] - 64);
    std::memcpy(cpu.r, keep, sizeof keep);
    cpu.hi = hi; cpu.lo = lo;
}

bool Platform::bios(uint32_t vec) { return biosCall(vec, cpu.r[9] & 0xFF); }

bool Platform::biosCall(uint32_t vec, uint32_t fn) {
    ++biosCalls[vec << 8 | fn];
    if (recordHwSites) ++biosSites[cpu.r[31]];
    auto& r = cpu.r;
    const uint32_t a0 = r[4], a1 = r[5], a2 = r[6], a3 = r[7];
    uint32_t ret = 0;
    bool jump = false;
    const uint32_t key = vec << 8 | fn;
    if (traceBios) std::printf("[bios] %s(%02x) a0=%08x a1=%08x a2=%08x ra=%08x\n", biosName(vec, fn), fn, a0, a1, a2, r[31]);

    switch (key) {
    // ---- libc (A0)
    case 0xA013:  // setjmp
        wr32(a0 + 0, r[31]); wr32(a0 + 4, r[29]); wr32(a0 + 8, r[30]);
        for (int i = 0; i < 8; ++i) wr32(a0 + 12 + 4u * uint32_t(i), r[16 + i]);
        wr32(a0 + 44, r[28]);
        ret = 0;
        break;
    case 0xA014: {  // longjmp
        r[31] = rd32(a0); r[29] = rd32(a0 + 4); r[30] = rd32(a0 + 8);
        for (int i = 0; i < 8; ++i) r[16 + i] = rd32(a0 + 12 + 4u * uint32_t(i));
        r[28] = rd32(a0 + 44);
        ret = a1 ? a1 : 1;
        break;
    }
    case 0xA017: case 0xA018: {  // strcmp / strncmp
        const uint32_t lim = key == 0xA018 ? a2 : 0xFFFFFFFFu;
        int res = 0;
        for (uint32_t i = 0; i < lim; ++i) {
            const int x = cpu.rd8(a0 + i), y = cpu.rd8(a1 + i);
            if (x != y) { res = x - y; break; }
            if (!x) break;
        }
        ret = uint32_t(res);
        break;
    }
    case 0xA019: case 0xA01A: {  // strcpy / strncpy
        uint32_t i = 0;
        const uint32_t lim = key == 0xA01A ? a2 : 0xFFFFFFFFu;
        for (; i < lim; ++i) { const uint8_t c = uint8_t(cpu.rd8(a1 + i)); cpu.wr8(a0 + i, c); if (!c) { ++i; break; } }
        if (key == 0xA01A) for (; i < lim; ++i) cpu.wr8(a0 + i, 0);
        ret = a0;
        break;
    }
    case 0xA01B: { uint32_t n = 0; while (cpu.rd8(a0 + n)) ++n; ret = n; break; }
    case 0xA00E: ret = uint32_t(int32_t(a0) < 0 ? -int32_t(a0) : int32_t(a0)); break;  // abs
    case 0xA00F: ret = uint32_t(int32_t(a0) < 0 ? -int32_t(a0) : int32_t(a0)); break;  // labs
    case 0xA010: case 0xA011: case 0xA00D: case 0xA00C: {                                 // atoi / atol / strtol / strtoul
        uint32_t i = a0;
        while (cpu.rd8(i) == ' ' || (cpu.rd8(i) >= 9 && cpu.rd8(i) <= 13)) ++i;
        bool neg = false;
        if (cpu.rd8(i) == '-') { neg = true; ++i; } else if (cpu.rd8(i) == '+') ++i;
        uint32_t base = (key == 0xA00D || key == 0xA00C) ? a2 : 10;
        if ((base == 0 || base == 16) && cpu.rd8(i) == '0' && (cpu.rd8(i + 1) | 0x20) == 'x') { i += 2; base = 16; }
        else if (base == 0) base = cpu.rd8(i) == '0' ? 8 : 10;
        uint32_t v = 0;
        for (;; ++i) {
            const uint32_t c = cpu.rd8(i);
            uint32_t d = c >= '0' && c <= '9' ? c - '0' : (c | 0x20) >= 'a' && (c | 0x20) <= 'z' ? (c | 0x20) - 'a' + 10 : 99;
            if (d >= base) break;
            v = v * base + d;
        }
        if ((key == 0xA00D || key == 0xA00C) && a1) wr32(a1, i);
        ret = neg ? 0u - v : v;
        break;
    }
    case 0xA015: case 0xA016: {  // strcat / strncat
        uint32_t d = a0;
        while (cpu.rd8(d)) ++d;
        const uint32_t lim = key == 0xA016 ? a2 : 0xFFFFFFFFu;
        for (uint32_t i = 0; i < lim; ++i) { const uint8_t c = uint8_t(cpu.rd8(a1 + i)); if (!c) break; cpu.wr8(d++, c); }
        cpu.wr8(d, 0);
        ret = a0;
        break;
    }
    case 0xA01C: case 0xA01E: {  // index / strchr
        ret = 0;
        for (uint32_t i = a0;; ++i) { const uint32_t c = cpu.rd8(i); if (c == (a1 & 0xFF)) { ret = i; break; } if (!c) break; }
        break;
    }
    case 0xA01D: case 0xA01F: {  // rindex / strrchr
        ret = 0;
        for (uint32_t i = a0;; ++i) { const uint32_t c = cpu.rd8(i); if (c == (a1 & 0xFF)) ret = i; if (!c) break; }
        break;
    }
    case 0xA021: case 0xA022: {  // strspn / strcspn
        uint32_t n = 0;
        for (;; ++n) {
            const uint32_t c = cpu.rd8(a0 + n);
            if (!c) break;
            bool in = false;
            for (uint32_t j = 0; cpu.rd8(a1 + j); ++j) if (cpu.rd8(a1 + j) == c) { in = true; break; }
            if (in != (key == 0xA021)) break;
        }
        ret = n;
        break;
    }
    case 0xA024: {  // strstr
        ret = 0;
        const std::string needle = readString(a1);
        for (uint32_t i = a0;; ++i) {
            bool ok = true;
            for (size_t j = 0; j < needle.size() && ok; ++j) ok = cpu.rd8(i + uint32_t(j)) == uint8_t(needle[j]);
            if (ok) { ret = i; break; }
            if (!cpu.rd8(i)) break;
        }
        break;
    }
    case 0xA025: ret = (a0 >= 'a' && a0 <= 'z') ? a0 - 32 : a0; break;  // toupper
    case 0xA026: ret = (a0 >= 'A' && a0 <= 'Z') ? a0 + 32 : a0; break;  // tolower
    case 0xA027: {  // bcopy(src, dst, n)
        std::vector<uint8_t> tmp(a2);
        for (uint32_t i = 0; i < a2; ++i) tmp[i] = uint8_t(cpu.rd8(a0 + i));
        for (uint32_t i = 0; i < a2; ++i) cpu.wr8(a1 + i, tmp[i]);
        break;
    }
    case 0xA028: for (uint32_t i = 0; i < a1; ++i) cpu.wr8(a0 + i, 0); break;  // bzero
    case 0xA029: {  // bcmp
        int res = 0;
        for (uint32_t i = 0; i < a2; ++i) { const int x = cpu.rd8(a0 + i), y = cpu.rd8(a1 + i); if (x != y) { res = x - y; break; } }
        ret = uint32_t(res);
        break;
    }
    case 0xA02E: { ret = 0; for (uint32_t i = 0; i < a2; ++i) if (cpu.rd8(a0 + i) == (a1 & 0xFF)) { ret = a0 + i; break; } break; }  // memchr
    case 0xA037: {  // calloc
        const uint32_t sz = (a0 * a1 + 7) & ~7u;
        if (heapPtr + sz + 8 > heapEnd) { ret = 0; break; }
        wr32(heapPtr, sz);
        ret = heapPtr + 8;
        for (uint32_t i = 0; i < sz; ++i) cpu.wr8(ret + i, 0);
        heapPtr += sz + 8;
        break;
    }
    case 0xA038: {  // realloc
        const uint32_t sz = (a1 + 7) & ~7u;
        if (heapPtr + sz + 8 > heapEnd) { ret = 0; break; }
        wr32(heapPtr, sz);
        ret = heapPtr + 8;
        heapPtr += sz + 8;
        if (a0) { const uint32_t old = rd32(a0 - 8); for (uint32_t i = 0; i < std::min(old, sz); ++i) cpu.wr8(ret + i, cpu.rd8(a0 + i)); }
        break;
    }
    case 0xA02A: case 0xA02C: {  // memcpy / memmove
        std::vector<uint8_t> tmp(a2);
        for (uint32_t i = 0; i < a2; ++i) tmp[i] = uint8_t(cpu.rd8(a1 + i));
        for (uint32_t i = 0; i < a2; ++i) cpu.wr8(a0 + i, tmp[i]);
        ret = a0;
        break;
    }
    case 0xA02B: for (uint32_t i = 0; i < a2; ++i) cpu.wr8(a0 + i, a1); ret = a0; break;
    case 0xA02D: {
        int res = 0;
        for (uint32_t i = 0; i < a2; ++i) { const int x = cpu.rd8(a0 + i), y = cpu.rd8(a1 + i); if (x != y) { res = x - y; break; } }
        ret = uint32_t(res);
        break;
    }
    case 0xA02F: randState = randState * 0x41C64E6Du + 0x3039u; ret = (randState >> 16) & 0x7FFF; break;
    case 0xA030: randState = a0; break;
    case 0xA039: heapStart = heapPtr = (a0 + 7) & ~7u; heapEnd = a0 + a1; break;
    case 0xA033: {
        const uint32_t sz = (a0 + 7) & ~7u;
        if (heapPtr + sz + 8 > heapEnd) { ret = 0; break; }
        wr32(heapPtr, sz);
        ret = heapPtr + 8;
        heapPtr += sz + 8;
        break;
    }
    case 0xA034: break;
    case 0xA03C: case 0xB03D: if (echoPrintf) std::putchar(int(a0 & 0xFF)); break;
    case 0xA03E: case 0xB03F: if (echoPrintf) std::printf("%s\n", readString(a0).c_str()); break;
    case 0xA03F: { const std::string s = formatPrintf(a0, 1); if (echoPrintf) std::fputs(s.c_str(), stdout); ret = uint32_t(s.size()); break; }
    case 0xA044: case 0xA070: case 0xA071: case 0xA072: break;
    // ---- memory card low level (libmcrd): operations complete at once and signal the card events
    case 0xA0AB: {  // _card_info(chan)
        MemCard& c = card;
        if ((a0 >> 4) != 0 || !c.present) { cardEvents(0x0100, 0x0100); }
        else cardEvents(0x0004, c.newCard ? 0x2000 : 0x0004);
        ret = 1;
        break;
    }
    case 0xA0AC:  // _card_load(chan)
        if ((a0 >> 4) != 0 || !card.present) cardEvents(0x0100, 0x0100);
        else cardEvents(0x0004, card.formatted() ? 0x0004 : 0x8000);
        ret = 1;
        break;
    case 0xB04E:  // _card_write(chan, sector, src)
        if ((a0 >> 4) != 0 || !card.present || a1 >= 1024) { cardEvents(0x0100, 0x0100); ret = 0; break; }
        if (a2) for (uint32_t i = 0; i < 128; ++i) card.frame(a1)[i] = uint8_t(cpu.rd8(a2 + i));
        card.newCard = false;
        card.save();
        cardEvents(0x0004, 0x0004);
        ret = 1;
        break;
    case 0xB04F:  // _card_read(chan, sector, dst)
        if ((a0 >> 4) != 0 || !card.present || a1 >= 1024) { cardEvents(0x0100, 0x0100); ret = 0; break; }
        for (uint32_t i = 0; i < 128; ++i) cpu.wr8(a2 + i, card.frame(a1)[i]);
        cardEvents(0x0004, 0x0004);
        ret = 1;
        break;
    case 0xB050: card.newCard = false; ret = 1; break;  // _new_card
    // ---- BIOS file API on "bu00:"
    case 0xB041: ret = cardPort(readString(a0)) == 0 && card.present ? (card.format(), card.save(), 1u) : 0u; break;
    case 0xB042: case 0xB043: {
        MemCard::DirEntry e{};
        int blk = 0;
        if (key == 0xB042) {
            std::string pat = readString(a0);
            if (cardPort(pat) == 0 && card.present) blk = card.firstFile(pat.substr(5), e);
        } else if (card.present) blk = card.nextFile(e);
        if (!blk) { ret = 0; break; }
        const uint32_t ent = key == 0xB042 ? a1 : a0;
        for (uint32_t i = 0; i < 40; ++i) cpu.wr8(ent + i, 0);
        for (uint32_t i = 0; i < 20; ++i) cpu.wr8(ent + i, uint8_t(e.name[i]));
        wr32(ent + 20, 0);
        wr32(ent + 24, e.size);
        wr32(ent + 32, uint32_t(blk - 1));
        ret = ent;
        break;
    }
    case 0xB032: {
        const std::string n = readString(a0);
        ret = uint32_t(cardPort(n) == 0 && card.present ? card.open(n.substr(5), a1) : -1);
        break;
    }
    case 0xB034: {
        std::vector<uint8_t> buf(a2);
        const int got = card.read(int(a0), buf.data(), a2);
        for (int i = 0; i < got; ++i) cpu.wr8(a1 + uint32_t(i), buf[size_t(i)]);
        ret = uint32_t(got);
        break;
    }
    case 0xB035: {
        std::vector<uint8_t> buf(a2);
        for (uint32_t i = 0; i < a2; ++i) buf[i] = uint8_t(cpu.rd8(a1 + i));
        ret = uint32_t(card.write(int(a0), buf.data(), a2));
        break;
    }
    case 0xB036: ret = uint32_t(card.close(int(a0))); break;
    case 0xB045: { const std::string n = readString(a0); ret = cardPort(n) == 0 && card.present && card.erase(n.substr(5)) ? 1u : 0u; break; }
    // ---- events (B0)
    case 0xB008: {
        for (size_t i = 0; i < events.size(); ++i) {
            if (events[i].used) continue;
            events[i] = {a0, a1, a2, a3, 0x1000, true};
            ret = 0xF1000000u | uint32_t(i);
            break;
        }
        break;
    }
    case 0xB009: if ((a0 & 0xFFFFFF) < events.size()) events[a0 & 0xFFFFFF].used = false; ret = 1; break;
    case 0xB00A: {  // WaitEvent: block (re-run this call) until delivered
        Event& e = events[a0 & 0xFFFFFF];
        if (e.status == 0x4000) { e.status = 0x2000; ret = 1; break; }
        return true;  // leave pc on the vector; an interrupt handler will deliver the event
    }
    case 0xB00B: {
        Event& e = events[a0 & 0xFFFFFF];
        if (e.status == 0x4000) { e.status = 0x2000; ret = 1; } else ret = 0;
        break;
    }
    case 0xB00C: events[a0 & 0xFFFFFF].status = 0x2000; ret = 1; break;
    case 0xB00D: events[a0 & 0xFFFFFF].status = 0x1000; ret = 1; break;
    case 0xB007: {
        uint32_t keep[32];
        std::memcpy(keep, cpu.r, sizeof keep);
        const uint32_t hi = cpu.hi, lo = cpu.lo;
        deliverEvent(a0, a1, keep[29] - 64);
        std::memcpy(cpu.r, keep, sizeof keep);
        cpu.hi = hi; cpu.lo = lo;
        break;
    }
    case 0xB020:
        for (auto& e : events)
            if (e.used && e.cls == a0 && e.spec == a1 && e.status == 0x4000) e.status = 0x2000;
        break;
    case 0xB017:
        returnFromException();
        if (nativeIrqDepth) throw ExceptionReturn{};
        jump = true;
        break;
    case 0xB018: hookBuf = 0; break;
    case 0xB019: hookBuf = a0; break;
    case 0xB012: padBuf1 = a0; padLen1 = a1; padBuf2 = a2; padLen2 = a3; ret = 1; break;
    case 0xB013: padActive = true; ret = 1; break;
    case 0xB014: padActive = false; ret = 1; break;
    case 0xB056: ret = 0x200; break;
    case 0xB057: ret = 0x874; break;
    case 0xA049: gpu.gp0(a0); break;  // GPU_cw
    case 0xB05B: case 0xB04A: case 0xB04B: case 0xB04C: case 0xA04A: case 0xA04B: break;
    case 0xC002: case 0xC003: break;  // SysEnqIntRP / SysDeqIntRP: interrupts are dispatched through HookEntryInt
    case 0xC00A: break;
    default:
        if (!unknownBios[key]++) unknownFirstCall[key] = {r[31], a0, a1, a2};
        break;
    }
    if (!jump) {
        r[2] = ret;
        cpu.pc = r[31]; cpu.nextPc = cpu.pc + 4;
    }
    return true;
}

}  // namespace psx

// ---------------------------------------------------------------------------------------------------------------
// Native (translated) execution
// ---------------------------------------------------------------------------------------------------------------

namespace psx {

Platform::~Platform() {
    if (gameThread.joinable()) {
        {
            std::lock_guard<std::mutex> lk(gameMutex);
            quitRequested = true;
        }
        gameCv.notify_all();
        gameThread.join();
    }
}

bool Platform::enableNative() {
    if (!loadNativeTable(true)) return false;
    useNative = true;
    return true;
}

namespace {
// Library routine 8004a5cc(target, timeout): spins until the vblank counter at 8005dc98 reaches `target`.
void hleVsyncWait(Platform& P) {
    while (int32_t(P.cpu.rd32(0x8005DC98u)) < int32_t(P.cpu.r[4])) P.idleToNextEvent();
    P.cpu.r[2] = 0;
}
}  // namespace

// Time advances in poll-sized steps: the CD and SPU models process at most one event per service() call, so a single
// jump to the next vblank starves them and slows the game down.
void Platform::idleToNextEvent() {
    cpu.instructions += 32;
    pollAt = 0;
    pollNative();
}

bool Platform::loadNativeTable(bool hle) {
    if (recomp::kFunctionCount == 0) return false;
    nativeFns.assign(size_t(0x4F800 >> 2), nullptr);
    for (const recomp::Entry* e = recomp::kFunctions; e->fn; ++e) {
        const uint32_t i = (e->addr - 0x80010000u) >> 2;
        if (i < nativeFns.size()) nativeFns[i] = e->fn;
    }
    if (hle) nativeFns[(0x8004A5CCu - 0x80010000u) >> 2] = &hleVsyncWait;
    return true;
}

void Platform::configureShadow(std::vector<uint64_t>* log) {
    shadow = true;
    cpu.ioRead = [log](r3000::Cpu&, uint32_t a, int s) { log->push_back(0x1000000000000ull | uint64_t(s) << 40 | a); return 0u; };
    cpu.ioWrite = [log](r3000::Cpu&, uint32_t a, uint32_t v, int s) { log->push_back(0x2000000000000ull | uint64_t(s) << 40 | a); log->push_back(v); };
    cpu.bios = [log](r3000::Cpu& c, uint32_t vec) {
        log->push_back(0x3000000000000ull | uint64_t(vec) << 8 | (c.r[9] & 0xFF));
        for (int i = 4; i < 8; ++i) log->push_back(c.r[i]);
        c.r[2] = 0; c.pc = c.r[31]; c.nextPc = c.pc + 4;
        return true;
    };
    cpu.intercept = [this, log](r3000::Cpu& c, uint32_t pc) {
        const uint32_t phys = pc & 0x1FFFFFFF;
        if (phys >= kStubBase && phys < kStubBase + 0xC00 && (phys & 3) == 0) {
            log->push_back(0x4000000000000ull | phys);
            c.r[2] = 0; c.pc = c.r[31]; c.nextPc = c.pc + 4;
            return true;
        }
        auto ov = overrides.find(pc);
        if (ov != overrides.end()) { c.r[2] = ov->second; c.pc = c.r[31]; c.nextPc = c.pc + 4; return true; }
        const uint32_t w = c.rd32(pc);
        if ((w >> 26) == 0 && (w & 0x3F) == 0x0C) {
            if (c.r[4] == 1) { c.r[2] = (c.cop0[12] & 1) ? 1 : 0; c.cop0[12] &= ~1u; }
            else if (c.r[4] == 2) c.cop0[12] |= 1;
            c.pc = c.nextPc; c.nextPc += 4;
            return true;
        }
        return false;
    };
}

void Platform::stepHybrid() {
    if (!useNative && !shadow) { cpu.step(); return; }   // interpreter mode never runs translated code
    if (cpu.nextPc == cpu.pc + 4) {
        if (cpu.instructions >= pollAt) pollNative();
        if (NativeFn f = lookupNative(cpu.pc)) {
            if (cpu.pendReg) { cpu.r[cpu.pendReg] = cpu.pendVal; cpu.pendReg = 0; }
            ++nativeCalls;
            f(*this);
            cpu.pc = cpu.r[31];
            cpu.nextPc = cpu.pc + 4;
            return;
        }
        if (blockTrace && traceLeader && traceLeader(cpu.pc)) blockTrace->push_back(cpu.pc);
    }
    ++interpSteps;
    if ((interpSteps & 1023) == 0) ++interpHist[cpu.pc];
    cpu.step();
}

void Platform::interpUntil(uint32_t addr, uint32_t ret) {
    cpu.pc = addr;
    cpu.nextPc = addr + 4;
    cpu.pendReg = 0;
    while (!(cpu.pc == ret && cpu.nextPc == cpu.pc + 4)) stepHybrid();
}

void Platform::call(uint32_t addr) {
    if (NativeFn f = lookupNative(addr)) {
        ++nativeCalls;
        f(*this);
        return;
    }
    ++interpCalls;
    interpUntil(addr, cpu.r[31]);
    // A load in the delay slot of the interpreted code's final `jr ra` is still in flight; translated code continues
    // with the value committed (the next instruction cannot observe the difference unless it was a hazard, which the
    // translator rejects).
    if (cpu.pendReg) { cpu.r[cpu.pendReg] = cpu.pendVal; cpu.pendReg = 0; }
}

void Platform::nativeSyscall(uint32_t code, uint32_t pc) {
    ++syscalls[cpu.r[4]];
    if (cpu.r[4] == 1) { cpu.r[2] = (cpu.cop0[12] & 1) ? 1 : 0; cpu.cop0[12] &= ~1u; }
    else if (cpu.r[4] == 2) cpu.cop0[12] |= 1;
    else {
        char b[96];
        std::snprintf(b, sizeof b, "syscall %x (a0=%x) at %08x", code, cpu.r[4], pc);
        throw std::runtime_error(b);
    }
}

void Platform::nativeBreak(uint32_t code, uint32_t pc) {
    char b[64];
    std::snprintf(b, sizeof b, "break %x at %08x", code, pc);
    throw std::runtime_error(b);
}

void Platform::nativeInvalid(uint32_t pc, uint32_t word) {
    char b[80];
    std::snprintf(b, sizeof b, "invalid instruction %08x at %08x", word, pc);
    throw std::runtime_error(b);
}

void Platform::nativeInterrupt() {
    enterInterrupt();
    ++nativeIrqDepth;
    try {
        for (;;) stepHybrid();
    } catch (const ExceptionReturn&) {
    } catch (...) {
        --nativeIrqDepth;
        throw;
    }
    --nativeIrqDepth;
}

void Platform::pollNative() {
    if (shadow) throw ShadowLimit{};
    syncTime();
    service();
    if (cpu.fault) {
        char b[80];
        std::snprintf(b, sizeof b, "access to unmapped address %08x", cpu.faultAddr);
        throw std::runtime_error(b);
    }
    if ((istat & imask) && hookBuf && !saved.valid && (cpu.cop0[12] & 1)) nativeInterrupt();
    pollAt = cpu.instructions + 32;
    if (cpu.instructions >= runLimit || vblankYield) {
        vblankYield = false;
        parkGameThread();
    }
}

void Platform::parkGameThread() {
    std::unique_lock<std::mutex> lk(gameMutex);
    gameState = GameState::Parked;
    gameCv.notify_all();
    gameCv.wait(lk, [&] { return gameState == GameState::Running || quitRequested; });
    if (quitRequested) throw GameQuit{};
}

void Platform::gameMain() {
    try {
        for (;;) stepHybrid();
    } catch (const GameQuit&) {
    } catch (const std::exception& e) {
        char b[96];
        std::snprintf(b, sizeof b, " (instructions=%llu)", (unsigned long long)cpu.instructions);
        lastError = std::string(e.what()) + b;
    }
    std::lock_guard<std::mutex> lk(gameMutex);
    gameState = GameState::Finished;
    gameCv.notify_all();
}

}  // namespace psx
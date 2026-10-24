#pragma once
// PlayStation platform: the hardware around the R3000 core (interrupt controller, DMA, timers, GPU, CD-ROM, SPU
// registers) plus a high-level replacement for the BIOS kernel. The game's own libc/libgpu/libcd/libspu code runs
// unmodified on top of it.
#include <array>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <initializer_list>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "cdrom.hpp"
#include "disc.hpp"
#include "gpu.hpp"
#include "memcard.hpp"
#include "r3000.hpp"
#include "spu.hpp"

namespace psx {

class Platform {
public:
    Platform();
    ~Platform();

    std::vector<uint8_t> ram = std::vector<uint8_t>(r3000::kRamSize, 0);
    r3000::Cpu cpu;
    Gpu gpu;
    CdRom cd;
    VirtualDisc disc;
    Spu spu;
    MemCard card;   // port 1; created formatted and empty unless an image is loaded
    // Game-library functions that reach into BIOS internals, replaced by returning a fixed value (address -> v0).
    std::map<uint32_t, uint32_t> overrides = {{0x8004a2c4, 0}, {0x8004a2d8, 0}, {0x8004a364, 0}};

    uint32_t istat = 0, imask = 0;
    uint64_t irqCount[11] = {};
    uint64_t cycles = 0;
    uint64_t frames = 0;
    uint16_t padButtons = 0xFFFF;  // active low, PSX digital pad layout
    bool echoPrintf = true;
    bool traceBios = false;
    bool traceIrq = false;
    bool stackDumped = false;
    std::function<void()> onVblank;
    std::function<void()> onDisplayList;     // the frame's polygons are complete and about to be handed to the GPU

    // ---- hooks into the running game (interpreter mode) ----
    // `fn` runs whenever execution reaches `pc` (never inside a branch delay slot). It may read and write guest memory and call
    // guest functions with callFunction(). This is how extra players are put into the original game (see multiplayer.hpp).
    void addHook(uint32_t pc, std::function<void(Platform&)> fn);
    // Calls a guest function from inside a hook: up to four register arguments (a0..a3) and any number of stack arguments.
    // Interrupts are held off and every CPU register is restored afterwards. Returns v0.
    uint32_t callFunction(uint32_t fn, std::initializer_list<uint32_t> regArgs, std::initializer_list<uint32_t> stackArgs = {});
    uint32_t peek32(uint32_t a) { return cpu.rd32(a); }
    void poke32(uint32_t a, uint32_t v) { cpu.wr32(a, v); }

    bool loadExe(const std::string& path);
    bool loadExeImage(const std::vector<uint8_t>& psxExe);   // a PS-X EXE already in memory
    // Extended memory (power of two, 2..16 MiB). The retail console has 2 MiB, mirrored; larger values give the
    // engine heap (and so the level page pool and mods) room the original machine never had. 16 MiB is the limit
    // of the original code (24-bit ordering-table links); verified 2/8/16 MiB play identically, 32 MiB breaks.
    void setRamMegabytes(uint32_t mb);
    uint32_t ramMask() const { return cpu.ramMask; }
    uint32_t initialSp = 0;  // 0 = use the executable header / 0x801FFFF0
    bool mountDisc(const std::string& folder, const std::vector<std::string>& overlays = {});
    // The disc read from files appended to an executable (see bundle.hpp); overlays still replace files.
    bool mountBundle(const std::string& file, const std::vector<BundledFile>& files, const std::vector<std::string>& overlays = {});
    // Runs up to `count` instructions; returns an empty string, or the reason for stopping (guest fault).
    std::string run(uint64_t count);

    // ---- native (translated) execution: the engine code translated ahead of time by tools/recomp.py runs as compiled
    // C++ on its own thread; functions that were not translated, and code that is not part of the executable (such as
    // the native blocks inside GOOL scripts), run on the interpreter. See docs/NATIVE.md.
    using NativeFn = void (*)(Platform&);
    bool enableNative();                     // false when no translated code is linked in
    bool loadNativeTable(bool hle = false);  // fills the translated-function table without switching execution over; hle also installs C++ library replacements
    void idleToNextEvent();                  // skips the CPU ahead to the next vertical blank (used by replaced wait loops)
    bool isTranslated(uint32_t addr) const { return lookupNative(addr) != nullptr; }
    std::function<void(uint32_t)> entryHook; // interpreter mode: called when execution enters a translated function
    // Shadow mode (differential testing, src/nativecheck.cpp): no devices, deterministic stubs, bounded execution.
    struct ShadowLimit {};
    bool shadow = false;
    std::function<bool(uint32_t)> traceLeader;      // with blockTrace: also records interpreted blocks starting at these addresses
    std::vector<uint32_t>* blockTrace = nullptr;   // when set, translated code appends every basic block it enters
    void configureShadow(std::vector<uint64_t>* ioLog);
    bool nativeEnabled() const { return useNative; }
    bool yieldOnVblank = false;              // native mode: run() returns at every vertical blank
    uint64_t pollAt = 0;                     // instruction count at which generated code calls pollNative()
    uint64_t nativeCalls = 0, interpCalls = 0, interpSteps = 0;
    std::map<uint32_t, uint64_t> interpHist;   // every 1024th interpreted instruction, by pc
    void pollNative();
    void call(uint32_t addr);                // call (or tail-jump to) any guest address from generated code
    void tail(uint32_t addr) { call(addr); }
    void nativeSyscall(uint32_t code, uint32_t pc);
    [[noreturn]] void nativeBreak(uint32_t code, uint32_t pc);
    [[noreturn]] void nativeInvalid(uint32_t pc, uint32_t word);

    // Statistics for the boot harness
    std::map<uint32_t, uint64_t> biosCalls;       // vector << 8 | function
    std::map<uint32_t, uint64_t> unknownBios;
    std::map<uint32_t, std::array<uint32_t, 4>> unknownFirstCall;  // ra, a0, a1, a2 of the first unimplemented call
    std::map<uint32_t, uint64_t> ioReads, ioWrites;
    // Which instructions touch which device (filled when recordHwSites is set): site pc -> device name -> accesses.
    bool recordHwSites = false;
    std::map<uint32_t, std::map<std::string, uint64_t>> hwSites;
    std::map<uint32_t, uint64_t> biosSites;   // caller of each BIOS call
    std::map<uint32_t, uint64_t> syscalls;
    std::string lastError;

    void printTimers() const { for (int i = 0; i < 3; ++i) std::printf("timer%d mode=%04x target=%04x value=%04x\n", i, timers[i].mode, timers[i].target, timers[i].value); }
    static const char* biosName(uint32_t vec, uint32_t fn);

private:
    // interrupt controller + exception entry
    struct Context { uint32_t r[32], hi, lo, epc, sr; bool valid = false; } saved;
    uint32_t hookBuf = 0;
    void raiseIrq(uint32_t bit) { if (traceIrq) std::printf("[irq] raise %x cyc=%llu\n", bit, (unsigned long long)cycles); istat |= bit; for (int i = 0; i < 11; ++i) if (bit >> i & 1) ++irqCount[i]; }
    void enterInterrupt();
    void kernelInterrupts(uint32_t sp);
    void deliverEvent(uint32_t cls, uint32_t spec, uint32_t sp);
    uint32_t callGuest(uint32_t fn, uint32_t sp, uint32_t a0 = 0);
    void cardEvents(uint32_t hwSpec, uint32_t swSpec);
    static int cardPort(const std::string& path);  // 0 for "bu00:", 1 for "bu10:", -1 otherwise
    void returnFromException();

    // native execution support
    struct ExceptionReturn {};               // thrown by ReturnFromException while an interrupt runs inside native code
    struct GameQuit {};                      // thrown on the game thread to unwind it at shutdown
    bool useNative = false;
    int nativeIrqDepth = 0;
    std::vector<NativeFn> nativeFns;
    NativeFn lookupNative(uint32_t addr) const {
        const uint32_t i = (addr - 0x80010000u) >> 2;
        return (addr & 3) == 0 && addr >= 0x80010000u && i < nativeFns.size() ? nativeFns[i] : nullptr;
    }
    void stepHybrid();
    void interpUntil(uint32_t addr, uint32_t ret);
    void nativeInterrupt();
    void syncTime() { cycles = cpu.instructions * 2; }
    std::map<uint32_t, std::function<void(Platform&)>> hooks;
    std::vector<uint8_t> hookMark;            // one byte per RAM word: 1 where a hook is set
    bool inHook = false;
    // game thread (native mode)
    std::thread gameThread;
    std::mutex gameMutex;
    std::condition_variable gameCv;
    enum class GameState { NotStarted, Running, Parked, Finished } gameState = GameState::NotStarted;
    bool wantRun = false, quitRequested = false;
    uint64_t runLimit = 0;
    bool vblankYield = false;
    void gameMain();
    void parkGameThread();

    // timers
    struct Timer { uint32_t value = 0, mode = 0, target = 0; uint64_t rem = 0; bool irqFired = false; } timers[3];
    void tickTimers(uint64_t delta);

    // DMA
    uint32_t dmaMadr[7] = {}, dmaBcr[7] = {}, dmaChcr[7] = {}, dpcr = 0x07654321, dicr = 0;
    void dmaStart(int ch);
    void dmaDone(int ch);

    // SPU timing
    uint64_t spuCycles = 0;
    bool spuIrqSeen = false;

    // GPU read latch for GP0 transfer
    uint32_t cdPrevIrq = 0;
    uint64_t lastService = 0, nextVblank = 0;
    static constexpr uint64_t kFrameCycles = 564480;

    // BIOS state
    struct Event { uint32_t cls = 0, spec = 0, mode = 0, handler = 0; uint32_t status = 0; bool used = false; };
    std::vector<Event> events = std::vector<Event>(64);
    uint32_t padBuf1 = 0, padBuf2 = 0, padLen1 = 0, padLen2 = 0;
    bool padActive = false;
    uint32_t heapStart = 0, heapEnd = 0, heapPtr = 0;
    uint32_t randState = 1;

    uint32_t ioRead(uint32_t addr, int size);
    void ioWrite(uint32_t addr, uint32_t value, int size);
    bool bios(uint32_t vec);
    bool biosCall(uint32_t vec, uint32_t fn);
    static constexpr uint32_t kStubBase = 0x1000;  // BIOS table entries point at kStubBase + table * 0x400 + function * 4
    bool syscallHook(uint32_t pc);
    void service();
    void updatePads();
    std::string readString(uint32_t addr);
    std::string formatPrintf(uint32_t fmtAddr, int firstArg);

    uint32_t rd32(uint32_t a) { return cpu.rd32(a); }
    void wr32(uint32_t a, uint32_t v) { cpu.wr32(a, v); }
};

}  // namespace psx

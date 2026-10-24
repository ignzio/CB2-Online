#pragma once
// Reference implementation of the Crash 2 GOOL virtual machine, written from the game's own interpreter
// (SCUS_941.54, function 0x8003A014) and verified against it by differential fuzzing (tools: cb2_vmdiff).
//
// The VM works on the game's own memory layout: a 32-bit address space, process objects with fields at
// +0x40 + 4*n, and the operand stack and call frames stored inside the process object. This keeps pointer
// semantics (LEA, PUSH, array ops) bit-exact. Everything that needs the rest of the engine goes through
// the Services interface, one call per original native routine.
#include <cstdint>
#include <cstring>
#include <functional>
#include <stdexcept>
#include <vector>

namespace cb2 {

struct Bus {
    virtual ~Bus() = default;
    virtual uint32_t rd32(uint32_t addr) = 0;
    virtual void wr32(uint32_t addr, uint32_t v) = 0;
    virtual uint32_t rd16(uint32_t addr) = 0;
    virtual void wr16(uint32_t addr, uint32_t v) = 0;
};

// RAM (2 MiB, mirrored through the first 8 MiB, KSEG0/1 aliases) plus the 1 KiB scratchpad at 0x1F800000.
class MemBus : public Bus {
public:
    static constexpr uint32_t kRamSize = 0x200000;
    static constexpr uint32_t kScratchBase = 0x1F800000;
    static constexpr uint32_t kScratchSize = 0x1000;  // modelled larger than the hardware so tests can map whole pages

    explicit MemBus(uint8_t* ram) : ram_(ram) {}
    uint8_t* scratch() { return scratch_; }
    uint8_t* ram() { return ram_; }

    uint32_t rd32(uint32_t a) override { uint32_t v; copyOut(a, &v, 4); return v; }
    void wr32(uint32_t a, uint32_t v) override { copyIn(a, &v, 4); }
    uint32_t rd16(uint32_t a) override { uint16_t v; copyOut(a, &v, 2); return v; }
    void wr16(uint32_t a, uint32_t v) override { const uint16_t x = uint16_t(v); copyIn(a, &x, 2); }

private:
    uint8_t* at(uint32_t addr, uint32_t size) {
        const uint32_t p = addr & 0x1FFFFFFFu;
        if (p < 0x00800000u) return ram_ + (p & (kRamSize - 1));
        if (p >= kScratchBase && p - kScratchBase + size <= kScratchSize) return scratch_ + (p - kScratchBase);
        throw std::runtime_error("GOOL VM: access to unmapped address");
    }
    void copyOut(uint32_t a, void* dst, uint32_t n) { std::memcpy(dst, at(a, n), n); }
    void copyIn(uint32_t a, const void* src, uint32_t n) { std::memcpy(at(a, n), src, n); }

    uint8_t* ram_;
    uint8_t scratch_[kScratchSize] = {};
};

// Native engine routines that opcodes call. Each value is documented with the routine's address in the
// NTSC-U executable. Arguments are the registers the original passes (a0..a3).
enum class Svc : uint8_t {
    Go,        // 0x8003900C  GO / GNEZ / GEQZ        (proc, ins, flags, out)  nonzero result ends the run
    Anim,      // 0x80039104  ANIM                     (proc, ins, flags, out)
    Anif,      // 0x800391D8  ANIF                     (proc, ins, flags, out)
    Sevt,      // 0x80039310  SEVT                     (proc, ins, flags, out)
    Cevt,      // 0x800394C4  CEVT                     (proc, ins, flags, out)
    Bevt,      // 0x80039684  BEVT                     (proc, ins, flags, out)
    Rjev,      // 0x8003981C  RJEV and ACEV            (proc, ins, flags, out); flags are reloaded from 0x1F800048 afterwards
    Dbg,       // 0x80037440  DBG                      (proc, ins)
    Eflr,      // 0x80037448  EFLR                     (proc, ins)
    Eflv,      // 0x8003756C  EFLV                     (proc, ins)
    Vec,       // 0x8003817C  VEC                      (proc, ins)
    Coll,      // 0x80038700  COLL                     (proc, ins)
    Spwn,      // 0x80038AFC  SPWN and SPWF            (proc, ins)
    Sndp,      // 0x80038C14  SNDP                     (proc, ins)
    Snda,      // 0x80038D08  SNDA                     (proc, ins)
    Chnk,      // 0x80038F00  CHNK                     (proc, ins)
    Op72,      // 0x80038E14  opcode 72                (proc, ins)
    Op74,      // 0x80039A14                           (proc, ins)
    Op75,      // 0x80039AA4                           (proc, ins)
    Op76,      // 0x80039B14                           (proc, ins)
    Op77,      // 0x80039C08                           (proc, ins)
    Misc,      // 0x80037698  MISC                     (proc, ins); pool bases and the stack pointer are reloaded afterwards
    Cpad,      // 0x80036FAC  CPAD                     (ins & 0x1FFFFF, 0) -> value pushed
    Atan,      // 0x8004F078  ATAN                     (left value, right value) -> angle pushed
    Op46,      // 0x8001EAD8  opcode 46                (source pointer, destination pointer); 12 bytes are copied to 0x1F800020 first
    Mips,      // native block embedded in the script  (proc, ip) -> new ip; see docs/GOOL.md
    Count
};

const char* svcName(Svc s);
uint32_t svcAddress(Svc s);  // 0 for Svc::Mips

// Interpreter registers a native block (opcode 73) can see and change.
struct VmRegs {
    uint32_t s0, s1, s2, s3, s4, s5, s6, s7;  // process, flags, out pointer, linked pool, own pool, ip, stack pointer, frame
};

struct Services {
    virtual ~Services() = default;
    virtual uint32_t call(Svc s, uint32_t a0, uint32_t a1, uint32_t a2, uint32_t a3) = 0;
    // Runs the native MIPS code at regs.s5 (the word after the MIPS marker) until it returns to the interpreter,
    // updating `regs` (s5 becomes the address after the block). Return false if native blocks are unsupported.
    virtual bool runNative(Bus& bus, VmRegs& regs, uint32_t ins) { (void)bus; (void)regs; (void)ins; return false; }
};

constexpr uint32_t kProcFieldBase = 0x40;  // fields live at proc + 0x40 + 4 * n

// Run result codes returned by the original interpreter in v0.
constexpr int32_t kRetOutermost = -0x19;

struct RunStats {
    uint64_t instructions = 0;
    uint64_t opCounts[128] = {};  // executed instructions per opcode
    std::function<void(uint32_t ip)> onStep;  // debugging: called before each instruction is fetched
};

// Runs process `proc` until the interpreter returns. `flags` and `out` are the original's a1 and a2.
// Returns the interpreter's return value. Throws std::runtime_error on an invalid opcode or when
// `maxInstructions` is exceeded.
int32_t goolRun(Bus& bus, Services& svc, uint32_t proc, uint32_t flags, uint32_t out,
                RunStats* stats = nullptr, uint64_t maxInstructions = 1000000);

}  // namespace cb2

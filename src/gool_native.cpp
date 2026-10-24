#include "gool_native.hpp"

#include <cstring>
#include <stdexcept>

#include "r3000.hpp"

namespace cb2 {

void runNativeBlockR3000(MemBus& bus, VmRegs& regs, uint64_t maxInstructions, const NativeStepHook* hook) {
    static_assert(r3000::kRamSize == MemBus::kRamSize, "RAM sizes must agree");
    static_assert(r3000::kScratchSize == MemBus::kScratchSize, "scratchpad sizes must agree");

    r3000::Cpu cpu;
    cpu.ram = bus.ram();
    std::memcpy(cpu.scratch, bus.scratch(), MemBus::kScratchSize);

    cpu.r[16] = regs.s0; cpu.r[17] = regs.s1; cpu.r[18] = regs.s2; cpu.r[19] = regs.s3;
    cpu.r[20] = regs.s4; cpu.r[21] = regs.s5; cpu.r[22] = regs.s6; cpu.r[23] = regs.s7;
    cpu.r[30] = kNativeFramePointer;
    cpu.r[29] = kNativeStackPointer;
    cpu.r[31] = kNativeReturnAddress;
    cpu.reset(regs.s5);
    if (hook) cpu.intercept = [hook](r3000::Cpu& c, uint32_t pc) { (*hook)(c.r, pc); return false; };

    for (uint64_t n = 0; cpu.pc != kNativeReturnAddress; ++n) {
        if (n >= maxInstructions) throw std::runtime_error("native block: instruction budget exceeded");
        cpu.step();
    }

    // The block may have written the scratchpad; hand it back to the VM's memory.
    std::memcpy(bus.scratch(), cpu.scratch, MemBus::kScratchSize);
    regs.s1 = cpu.r[17]; regs.s2 = cpu.r[18]; regs.s3 = cpu.r[19]; regs.s4 = cpu.r[20];
    regs.s5 = cpu.r[21]; regs.s6 = cpu.r[22]; regs.s7 = cpu.r[23];
}

}  // namespace cb2

#pragma once
// Runs the native MIPS blocks embedded in GOOL scripts (opcode 73) on the R3000 core.
//
// The original interpreter enters a block with `jalr s5` (0x8003AAD4) and the block hands control back with
// `jalr s5, ra`, which jumps to the interpreter and leaves the address after the block in s5. All 817 distinct blocks
// in the shipped Crash 2 levels read only the interpreter's own registers (s0-s7, fp, sp, ra), verified by
// tools/block_livein.py, so those registers are all that has to be reproduced.
//
// This runner executes the block's machine code against the VM's own memory (RAM and scratchpad), which makes it
// exact for any block, including blocks added by mods. It is also the reference that translated blocks are tested against.
#include <functional>

#include "gool_vm.hpp"

namespace cb2 {

constexpr uint32_t kNativeReturnAddress = 0x8003AADC;  // the interpreter's `bnez s5` after `jalr s5`
constexpr uint32_t kNativeFramePointer = 0x1F800000;   // fp: scratchpad base, as set by the interpreter prologue
constexpr uint32_t kNativeStackPointer = 0x801FFF00;

// Optional per-instruction observer for debugging: called before each instruction with the 32 registers and the pc.
using NativeStepHook = std::function<void(const uint32_t* regs, uint32_t pc)>;

// Executes the block at regs.s5. Updates s1-s7 in `regs`; s5 holds the address after the block (or 0).
// Throws std::runtime_error on a CPU fault or when `maxInstructions` is exceeded.
void runNativeBlockR3000(MemBus& bus, VmRegs& regs, uint64_t maxInstructions = 2000000, const NativeStepHook* hook = nullptr);

}  // namespace cb2

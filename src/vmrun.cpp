// Oracle harness: runs the game's own GOOL interpreter (SCUS_941.54, function 0x8003A014) on the R3000 core.
//   cb2_vmrun <SCUS_941.54>
// Builds a synthetic process in RAM, runs a small hand-assembled program, and prints the resulting state.
#include <cstdio>
#include <cstring>
#include <vector>

#include "nsf.hpp"
#include "r3000.hpp"

namespace {

constexpr uint32_t kProc = 0x80100000;       // s0: the process object; fields live at +0x40 + 4*n
constexpr uint32_t kOwnEntry = 0x80101000;   // s4 = *(entry + 0x18): own constant pool base
constexpr uint32_t kLinkEntry = 0x80101100;  // s3 = *(entry + 0x18): linked entry's pool base
constexpr uint32_t kOwnPool = 0x80102000;
constexpr uint32_t kLinkPool = 0x80102400;
constexpr uint32_t kCode = 0x80103000;
constexpr uint32_t kOut = 0x80104000;        // a2 / s2: result word written by some returns
constexpr uint32_t kSentinel = 0x80005000;   // return address that ends the run
constexpr uint32_t kVmEntry = 0x8003A014;
constexpr uint32_t kOpTable = 0x8005C514;

uint32_t imm8(int v) { return 0xA00u | (uint32_t(v) & 0xFFu); }        // value = signext8 << 4
uint32_t dstField(uint32_t n) { return 0x600u | n; }                    // destination: own process field n
uint32_t srcField(uint32_t n) { return 0xE00u | n; }                    // source: own process field n
uint32_t word(uint32_t op, uint32_t hiRef, uint32_t loRef) { return (op << 24) | (hiRef << 12) | loRef; }

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) { std::fprintf(stderr, "usage: cb2_vmrun <SCUS_941.54>\n"); return 1; }
    const cb2::Bytes exe = cb2::loadFile(argv[1]);
    if (exe.size() < 0x800 || std::memcmp(exe.data(), "PS-X EXE", 8) != 0) { std::fprintf(stderr, "not a PS-X EXE\n"); return 1; }

    static uint8_t ram[r3000::kRamSize];
    r3000::Cpu cpu;
    cpu.ram = ram;
    uint32_t textAddr, textSize;
    std::memcpy(&textAddr, &exe[0x18], 4);
    std::memcpy(&textSize, &exe[0x1C], 4);
    std::memcpy(ram + (textAddr & 0x1FFFFF), &exe[0x800], textSize);

    auto put = [&](uint32_t addr, uint32_t v) { cpu.wr32(addr, v); };
    auto get = [&](uint32_t addr) { return cpu.rd32(addr); };

    put(kProc + 0x0C, kOwnEntry);
    put(kProc + 0x10, kOwnEntry);
    put(kProc + 0x14, kLinkEntry);
    put(kOwnEntry + 0x18, kOwnPool);
    put(kLinkEntry + 0x18, kLinkPool);

    const uint32_t stackBase = kProc + 0x40 + 0x300;  // frame area; [s7 + 8] == 0 marks the outermost frame
    put(kProc + 0xC0, kCode);
    put(kProc + 0xC4, stackBase);
    put(kProc + 0xBC, stackBase + 0xC);
    put(0x1F80005C, kOpTable);

    // Program: f5 = 5 << 4; push (f5 + (3 << 4)); f6 = pop; RET   (value ref 0xE1F pops the operand stack)
    const uint32_t code[] = {
        word(17, imm8(5), dstField(5)),   // SETF f5 <- #80
        word(0, imm8(3), srcField(5)),    // ADD  push(L = f5, R = #48)
        word(17, 0xE1Fu, dstField(6)),    // SETF f6 <- pop
        word(49, 0, 0),                   // RET
    };
    for (size_t i = 0; i < sizeof code / sizeof code[0]; ++i) put(kCode + uint32_t(i) * 4, code[i]);

    cpu.r[4] = kProc;
    cpu.r[5] = 0;
    cpu.r[6] = kOut;
    cpu.r[29] = 0x801FFFF0;
    cpu.r[31] = kSentinel;
    cpu.reset(kVmEntry);

    try {
        for (uint64_t n = 0; cpu.pc != kSentinel; ++n) {
            if (n > 100000) { std::fprintf(stderr, "timeout\n"); return 2; }
            cpu.step();
        }
    } catch (const std::exception& ex) {
        std::fprintf(stderr, "stopped: %s (pc=%08x)\n", ex.what(), cpu.pc);
        return 2;
    }

    std::printf("VM returned v0=%d after %llu MIPS instructions\n", int32_t(cpu.r[2]), (unsigned long long)cpu.instructions);
    for (uint32_t f = 4; f < 9; ++f) std::printf("  field %u = %08x\n", f, get(kProc + 0x40 + 4 * f));
    std::printf("  ip=%08x sp=%08x\n", get(kProc + 0xC0), get(kProc + 0xBC));
    return 0;
}

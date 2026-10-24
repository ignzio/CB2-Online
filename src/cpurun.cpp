// Batch test runner for the R3000 interpreter.
//   input:  u32 ncases, then per case: u32 regs[32], u32 hi, u32 lo, u32 n, u32 code[n] (at 0x10000), u32 data[1024] (at 0x20000)
//   output: per case: u32 regs[32], u32 hi, u32 lo, u32 data[1024], u32 status (0 = ok, 1 = exception or timeout)
#include <cstdio>
#include <cstring>
#include <vector>

#include "r3000.hpp"

int main(int argc, char** argv) {
    if (argc < 3) return 2;
    FILE* in = std::fopen(argv[1], "rb");
    if (!in) return 2;
    std::vector<uint32_t> file;
    uint32_t w;
    while (std::fread(&w, 4, 1, in) == 1) file.push_back(w);
    std::fclose(in);
    if (file.empty()) return 2;

    FILE* out = std::fopen(argv[2], "wb");
    if (!out) return 2;
    static uint8_t ram[r3000::kRamSize];
    size_t pos = 1;
    for (uint32_t c = 0; c < file[0]; ++c) {
        if (pos + 35 > file.size()) return 2;
        std::memset(ram, 0, sizeof ram);
        r3000::Cpu cpu;
        cpu.ram = ram;
        for (int i = 0; i < 32; ++i) cpu.r[i] = file[pos + size_t(i)];
        cpu.hi = file[pos + 32];
        cpu.lo = file[pos + 33];
        const uint32_t n = file[pos + 34];
        if (pos + 35 + n + 1024 > file.size()) return 2;
        std::memcpy(ram + 0x10000, &file[pos + 35], n * 4);
        std::memcpy(ram + 0x20000, &file[pos + 35 + n], 4096);
        pos += 35 + n + 1024;

        const uint32_t end = 0x10000 + n * 4;
        cpu.reset(0x10000);
        uint32_t status = 0;
        try {
            for (uint32_t steps = 0; cpu.pc != end; ++steps) {
                if (steps > 100000) { status = 1; break; }
                cpu.step();
            }
            if (cpu.pendReg) cpu.r[cpu.pendReg] = cpu.pendVal;  // commit a load left in flight at the end
        } catch (const std::exception&) {
            status = 1;
        }
        std::fwrite(cpu.r, 4, 32, out);
        std::fwrite(&cpu.hi, 4, 1, out);
        std::fwrite(&cpu.lo, 4, 1, out);
        std::fwrite(ram + 0x20000, 4, 1024, out);
        std::fwrite(&status, 4, 1, out);
    }
    std::fclose(out);
    return 0;
}

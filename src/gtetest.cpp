// Checks the GTE against real-hardware captures: cb2_gtetest <analysis/gte/tests.bin>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <map>
#include <vector>

#include "r3000.hpp"

int main(int argc, char** argv) {
    if (argc < 2) return 2;
    std::ifstream f(argv[1], std::ios::binary);
    std::vector<char> b((std::istreambuf_iterator<char>(f)), {});
    if (b.size() < 4) return 2;
    uint32_t n;
    std::memcpy(&n, b.data(), 4);
    const size_t stride = (64 + 1 + 64) * 4;
    int failed = 0;
    std::map<uint32_t, std::pair<int, int>> perCmd;
    std::map<uint32_t, int> badRegs;
    for (uint32_t t = 0; t < n; ++t) {
        const uint32_t* p = reinterpret_cast<const uint32_t*>(b.data() + 4 + t * stride);
        r3000::Cpu cpu;
        for (int r = 0; r < 64; ++r) { if (r < 32) cpu.gteWriteData(uint32_t(r), 0); else cpu.gteWriteCtrl(uint32_t(r - 32), 0); }
        for (int r = 0; r < 64; ++r) { if (r < 32) cpu.gteWriteData(uint32_t(r), p[r]); else cpu.gteWriteCtrl(uint32_t(r - 32), p[r]); }
        const uint32_t op = p[64];
        if (op != 0xFFFFFFFFu) cpu.gteCommand(op);
        bool bad = false;
        for (int r = 0; r < 64; ++r) {
            const uint32_t got = r < 32 ? cpu.gteReadData(uint32_t(r)) : cpu.gteReadCtrl(uint32_t(r - 32));
            if (got != p[65 + r]) {
                bad = true;
                ++badRegs[uint32_t(r) | (op & 0x3F) << 8];
                if (failed < 6) std::printf("test %u op %08x reg %d: got %08x expected %08x\n", t + 1, op, r, got, p[65 + r]);
            }
        }
        auto& pc = perCmd[op == 0xFFFFFFFFu ? 0xFF : op & 0x3F];
        (bad ? pc.second : pc.first)++;
        failed += bad;
    }
    for (auto& [cmd, pf] : perCmd) std::printf("cmd %02x: pass %d fail %d\n", cmd, pf.first, pf.second);
    if (failed) for (auto& [k, c] : badRegs) std::printf("  cmd %02x reg %2u wrong in %d tests\n", k >> 8, k & 0xFF, c);
    std::printf("%u tests, %d failed\n", n, failed);
    return failed ? 1 : 0;
}

"""Find the Sony library (libgpu/libcd/libspu/libpad/libcard/libetc/libc) functions inside the game executable.

For every function, report which hardware registers / BIOS vectors it touches. Functions that touch hardware are
library code candidates; functions that call them are game code. Uses the function list from Rizin
(analysis/functions.txt) and Capstone.

  python tools/find_libs.py <SCUS_941.54> <functions.txt>
"""
import re
import struct
import sys
from collections import defaultdict

from capstone import CS_ARCH_MIPS, CS_MODE_LITTLE_ENDIAN, CS_MODE_MIPS32, Cs

sys.path.insert(0, __file__.rsplit("\\", 1)[0])
from psx_xref import PsxExe  # noqa: E402

exe = PsxExe(sys.argv[1])
funcs = []
for line in open(sys.argv[2]):
    m = re.match(r"\s*0x([0-9a-fA-F]+)\s+\d+\s+(\d+)", line)
    if m:
        funcs.append(int(m.group(1), 16))
funcs.sort()
ends = funcs[1:] + [exe.end]
md = Cs(CS_ARCH_MIPS, CS_MODE_MIPS32 | CS_MODE_LITTLE_ENDIAN)

HW = [
    (0x1F801810, 0x1F801818, "GPU"), (0x1F801800, 0x1F801804, "CD"), (0x1F801C00, 0x1F802000, "SPU"),
    (0x1F801080, 0x1F801100, "DMA"), (0x1F801100, 0x1F801130, "TIMER"), (0x1F801040, 0x1F801050, "SIO"),
    (0x1F801070, 0x1F801078, "IRQ"), (0x1F801000, 0x1F801024, "MEMCTRL"), (0x1F801820, 0x1F801830, "MDEC"),
]


def hw_name(a):
    for lo, hi, n in HW:
        if lo <= (a & 0x1FFFFFFF) < hi:
            return n
    return None


info = {}
for s, e in zip(funcs, ends):
    regs = defaultdict(int)
    lui = {}
    bios = set()
    callees = set()
    for a in range(s, e, 4):
        w = exe.u32(a)
        op = w >> 26
        rs, rt, imm = (w >> 21) & 31, (w >> 16) & 31, w & 0xFFFF
        if op == 0x0F:
            lui[rt] = imm << 16
        elif op in (0x20, 0x21, 0x23, 0x24, 0x25, 0x28, 0x29, 0x2B) and rs in lui:
            addr = (lui[rs] + (imm - 0x10000 if imm & 0x8000 else imm)) & 0xFFFFFFFF
            n = hw_name(addr)
            if n:
                regs[n] += 1
        elif op == 0x09 and rt == 0 and False:
            pass
        elif op == 0x09 and rs == 0 and imm in (0xA0, 0xB0, 0xC0):
            bios.add(imm)
        elif op == 3:
            callees.add(((w & 0x3FFFFFF) << 2) | 0x80000000)
        if op not in (0x0F,) and rt in lui and op in (0x09, 0x0D, 0x00, 0x08):
            pass
    info[s] = (e, dict(regs), bios, callees)

hw_funcs = {s: v for s, v in info.items() if v[1]}
print("functions touching hardware registers directly (library code):")
for s, (e, regs, bios, callees) in sorted(hw_funcs.items()):
    print("  %08x size %4d  %s" % (s, e - s, " ".join("%s:%d" % kv for kv in sorted(regs.items()))))
print("%d of %d functions" % (len(hw_funcs), len(funcs)))

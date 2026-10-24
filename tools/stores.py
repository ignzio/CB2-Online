"""Find the stores to a given absolute address in the PS-X EXE (lui + sw/sh/sb pairs): stores.py <exe> <addr>"""
import struct
import sys

sys.path.insert(0, __file__.rsplit("\\", 1)[0])
from psx_xref import PsxExe  # noqa: E402

exe = PsxExe(sys.argv[1])
target = int(sys.argv[2], 16)
hi = (target + 0x8000) >> 16 & 0xFFFF
lo = target & 0xFFFF
words = [struct.unpack_from("<I", exe.text, 4 * i)[0] for i in range(len(exe.text) // 4)]
names = {0x2B: "sw", 0x29: "sh", 0x28: "sb", 0x23: "lw", 0x21: "lh", 0x25: "lhu", 0x20: "lb", 0x24: "lbu"}
for i, w in enumerate(words):
    op = w >> 26
    if op in names and (w & 0xFFFF) == lo:
        base = (w >> 21) & 31
        for j in range(max(0, i - 12), i):
            v = words[j]
            if (v >> 26) == 0x0F and (v & 0xFFFF) == hi and ((v >> 16) & 31) == base:
                print(f"{exe.base + 4 * i:08x}: {names[op]} (lui at {exe.base + 4 * j:08x})")
                break

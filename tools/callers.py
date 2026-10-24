"""Find callers of a function: callers.py <exe> <addr> [context]"""
import struct
import sys

sys.path.insert(0, __file__.rsplit("\\", 1)[0])
from psx_xref import PsxExe  # noqa: E402

exe = PsxExe(sys.argv[1])
target = int(sys.argv[2], 16)
ctx = int(sys.argv[3]) if len(sys.argv) > 3 else 0
for i in range(len(exe.text) // 4):
    w = struct.unpack_from("<I", exe.text, 4 * i)[0]
    if (w >> 26) == 3 and ((w & 0x3FFFFFF) << 2) | 0x80000000 == target:
        print(f"jal at {exe.base + 4 * i:08x}")

"""List the BIOS call stubs linked into the executable: bios_stubs.py <exe>
A stub is `li t2,0xA0/B0/C0 ; jr t2 ; li t1,N` (the two li may be in either order).
"""
import struct
import sys

sys.path.insert(0, __file__.rsplit("\\", 1)[0])
from psx_xref import PsxExe  # noqa: E402

exe = PsxExe(sys.argv[1])
words = [(exe.base + 4 * i, struct.unpack_from("<I", exe.text, 4 * i)[0]) for i in range(len(exe.text) // 4)]
found = {}
for k in range(len(words) - 2):
    a0, w0 = words[k]
    w1, w2 = words[k + 1][1], words[k + 2][1]
    if (w0 >> 16) == 0x240A and (w0 & 0xFFFF) in (0xA0, 0xB0, 0xC0) and w1 == 0x01400008 and (w2 >> 16) == 0x2409:
        found[(w0 & 0xFFFF, w2 & 0xFFFF)] = a0
for (vec, fn), addr in sorted(found.items()):
    print(f"{vec:02X}({fn:02X}) stub at {addr:08x}")
print(len(found), "stubs")

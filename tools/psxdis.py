"""Disassemble a range of the PS-X EXE: dis.py <exe> <addr> [count] [--strings]"""
import sys

from capstone import CS_ARCH_MIPS, CS_MODE_LITTLE_ENDIAN, CS_MODE_MIPS32, Cs

sys.path.insert(0, __file__.rsplit("\\", 1)[0])
from psx_xref import PsxExe  # noqa: E402

exe = PsxExe(sys.argv[1])
addr = int(sys.argv[2], 16)
count = int(sys.argv[3]) if len(sys.argv) > 3 else 40
md = Cs(CS_ARCH_MIPS, CS_MODE_MIPS32 | CS_MODE_LITTLE_ENDIAN)
for i in range(count):
    a = addr + 4 * i
    w = exe.u32(a)
    ins = list(md.disasm(w.to_bytes(4, "little"), a))
    print(f"{a:08x}: {w:08x}  " + (f"{ins[0].mnemonic} {ins[0].op_str}" if ins else "?"))

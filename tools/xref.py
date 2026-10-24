"""Print code that builds a given address: xref.py <exe> <addr> [context]"""
import sys

sys.path.insert(0, __file__.rsplit("\\", 1)[0])
from psx_xref import PsxExe, address_refs, disassemble  # noqa: E402

exe = PsxExe(sys.argv[1])
real = [i for i in disassemble(exe) if i.id != 0]
target = int(sys.argv[2], 16)
ctx = int(sys.argv[3]) if len(sys.argv) > 3 else 8
idx = {i.address: k for k, i in enumerate(real)}
for at, value in address_refs(real):
    if value == target:
        k = idx[at]
        print(f"--- ref at {at:08x}")
        for j in real[max(0, k - ctx): k + ctx]:
            print(f"{j.address:08x}: {j.mnemonic} {j.op_str}")

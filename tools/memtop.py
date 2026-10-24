"""Find memory-size constants in the executable: memtop.py <exe>
Reports lui/ori/addiu pairs that build values near the 2 MiB RAM top or the page-pool bounds.
"""
import sys

sys.path.insert(0, __file__.rsplit("\\", 1)[0])
from psx_xref import PsxExe, address_refs, disassemble  # noqa: E402

exe = PsxExe(sys.argv[1])
real = [i for i in disassemble(exe) if i.id != 0]
idx = {i.address: k for k, i in enumerate(real)}
lo, hi = int(sys.argv[2], 16), int(sys.argv[3], 16)
for at, value in address_refs(real):
    if lo <= value < hi:
        k = idx[at]
        print(f"--- {value:08x} built at {at:08x}")
        for j in real[max(0, k - 3): k + 4]:
            print(f"   {j.address:08x}: {j.mnemonic} {j.op_str}")

"""Lists every instruction that builds the address of a given data label, with surrounding code."""
import sys

from psx_xref import PsxExe, address_refs, containing_function, disassemble, function_starts

exe = PsxExe(sys.argv[1])
insns = disassemble(exe)
real = [i for i in insns if i.id != 0]
starts = function_starts(exe, real, sys.argv[2])
target = int(sys.argv[3], 16)
context = int(sys.argv[4]) if len(sys.argv) > 4 else 6

index = {i.address: k for k, i in enumerate(real)}
for at, value in address_refs(real):
    if value == target:
        k = index[at]
        print(f'--- ref at {at:08x} in fn {containing_function(starts, at):08x}')
        for i in real[max(0, k - 3):k + context]:
            print(f'    {i.address:08x} {i.mnemonic} {i.op_str}')

"""Finds loads/stores of a scratchpad (0x1F80xxxx) variable and prints the surrounding code."""
import sys

from capstone.mips import MIPS_OP_MEM

from psx_xref import PsxExe, containing_function, disassemble, function_starts

exe = PsxExe(sys.argv[1])
real = [i for i in disassemble(exe) if i.id != 0]
starts = function_starts(exe, real, sys.argv[2])
offset = int(sys.argv[3], 16)
context = int(sys.argv[4]) if len(sys.argv) > 4 else 8

lui_1f80 = {}
count = 0
for k, i in enumerate(real):
    if i.mnemonic == 'lui' and i.operands[1].imm == 0x1F80:
        lui_1f80[i.operands[0].reg] = k
    for op in i.operands:
        if op.type == MIPS_OP_MEM and op.mem.base in lui_1f80 and op.mem.disp == offset and k - lui_1f80[op.mem.base] < 12:
            count += 1
            print(f'--- {i.mnemonic} at {i.address:08x} in fn {containing_function(starts, i.address):08x}')
            for j in real[max(0, k - 2):k + context]:
                print(f'    {j.address:08x} {j.mnemonic} {j.op_str}')
print(f'{count} accesses')

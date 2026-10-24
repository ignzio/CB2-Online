"""Finds candidate GOOL dispatch code: bound checks near 79 followed by an indirect jump or jalr."""
import sys

from psx_xref import PsxExe, containing_function, disassemble, function_starts

exe = PsxExe(sys.argv[1])
insns = [i for i in disassemble(exe) if i.id != 0]
starts = function_starts(exe, insns, sys.argv[2] if len(sys.argv) > 2 else None)

lo, hi = int(sys.argv[3], 0), int(sys.argv[4], 0)
for idx, ins in enumerate(insns):
    if ins.mnemonic in ('sltiu', 'slti') and len(ins.operands) == 3 and lo <= ins.operands[2].imm <= hi:
        ahead = insns[idx + 1: idx + 14]
        kind = next((a.mnemonic for a in ahead if a.mnemonic in ('jr', 'jalr') and a.op_str != '$ra'), None)
        if kind:
            fn = containing_function(starts, ins.address)
            print(f'{ins.address:08x} {ins.mnemonic} {ins.op_str:<22} -> {kind}   fn {fn:08x}')

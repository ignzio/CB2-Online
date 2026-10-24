"""Broad search for opcode-dispatch code in the PS-X EXE (opcode extraction + indirect jump)."""
import sys

from psx_xref import PsxExe, containing_function, disassemble, function_starts

exe = PsxExe(sys.argv[1])
insns = [i for i in disassemble(exe) if i.id != 0]
starts = function_starts(exe, insns, sys.argv[2])


def indirect_within(k, window):
    for a in insns[k + 1:k + window]:
        if a.mnemonic in ('jr', 'jalr') and a.op_str != '$ra':
            return a
    return None


print('srl ...,0x18 (opcode extract) followed by an indirect jump/call:')
seen = set()
for k, i in enumerate(insns):
    if i.mnemonic == 'srl' and i.op_str.endswith('0x18'):
        a = indirect_within(k, 30)
        if a:
            fn = containing_function(starts, i.address)
            if (fn, a.address) not in seen:
                seen.add((fn, a.address))
                print(f'  {i.address:08x} {i.op_str:<20} -> {a.mnemonic} {a.address:08x}  fn {fn:08x}')

print('sltiu/slti bound 40..100 followed by an indirect jump/call:')
for k, i in enumerate(insns):
    if i.mnemonic in ('sltiu', 'slti') and 40 <= i.operands[2].imm <= 100:
        a = indirect_within(k, 30)
        if a:
            print(f'  {i.address:08x} {i.op_str:<22} -> {a.mnemonic} {a.address:08x}  fn {containing_function(starts, i.address):08x}')

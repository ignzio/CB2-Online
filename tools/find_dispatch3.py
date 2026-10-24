"""Finds byte-wise opcode fetches (lbu rX, 3(rY)) that feed an indirect jump or call."""
import sys

from psx_xref import PsxExe, containing_function, disassemble, function_starts

exe = PsxExe(sys.argv[1])
insns = [i for i in disassemble(exe) if i.id != 0]
starts = function_starts(exe, insns, sys.argv[2])
window = int(sys.argv[3]) if len(sys.argv) > 3 else 40

hits = {}
for k, i in enumerate(insns):
    if i.mnemonic == 'lbu' and i.op_str.endswith(', 3(' + i.op_str.split('(')[-1]):
        for a in insns[k + 1:k + window]:
            if a.mnemonic in ('jr', 'jalr') and a.op_str != '$ra':
                fn = containing_function(starts, i.address)
                hits.setdefault(fn, []).append((i.address, i.op_str, a.address))
                break

print(f'{len(hits)} functions fetch a byte at +3 and reach an indirect jump')
for fn, lst in sorted(hits.items()):
    a, op, j = lst[0]
    print(f'  fn {fn:08x}: {len(lst)} site(s), first {a:08x} {op} -> jump {j:08x}')

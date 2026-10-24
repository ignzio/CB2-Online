"""Lists functions with indirect `jr` jumps (switch statements) and the bound checks that guard them."""
import sys

from psx_xref import PsxExe, containing_function, disassemble, function_starts

exe = PsxExe(sys.argv[1])
insns = [i for i in disassemble(exe) if i.id != 0]
starts = function_starts(exe, insns, sys.argv[2] if len(sys.argv) > 2 else None)

found = {}
for idx, ins in enumerate(insns):
    if ins.mnemonic == 'jr' and ins.op_str != '$ra':
        window = insns[max(0, idx - 14):idx]
        bound = next((int(w.op_str.split(',')[-1], 0) for w in reversed(window) if w.mnemonic in ('sltiu', 'slti')), None)
        fn = containing_function(starts, ins.address)
        found.setdefault(fn, []).append((ins.address, bound))

print(f'{len(found)} functions with indirect jumps')
for fn, jumps in sorted(found.items(), key=lambda kv: -max((b or 0) for _, b in kv[1])):
    bounds = [b for _, b in jumps if b]
    print(f'  fn {fn:08x}: {len(jumps)} indirect jr, largest switch bound {max(bounds) if bounds else "?"}')

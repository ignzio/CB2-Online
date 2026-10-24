"""Dumps the GOOL opcode -> handler address table from the PS-X EXE and lists handler sizes."""
import json
import struct
import sys

from psx_xref import PsxExe

exe = PsxExe(sys.argv[1])
TABLE = 0x8005C514
COUNT = 79
ops = json.load(open(sys.argv[2])) if len(sys.argv) > 2 else {}

entries = [struct.unpack_from('<I', exe.text, TABLE - exe.base + 4 * i)[0] for i in range(COUNT)]
by_addr = sorted(set(entries))
print(f'{COUNT} entries, {len(by_addr)} distinct handlers')
for opcode, addr in enumerate(entries):
    nxt = next((a for a in by_addr if a > addr), 0x8003AE54)
    name = ops.get(str(opcode), {}).get('name', '?')
    print(f'  op {opcode:2d} {name:<6} -> {addr:08x}  (~{(nxt - addr) // 4:3d} insns)')

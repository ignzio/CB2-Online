"""Survey of R3000 features used by the PS-X EXE, to size the static recompiler.

Counts instruction classes inside the functions found by Rizin (plus jal targets) and checks the two hazards a
recompiler must reproduce exactly: load delay slots and branch delay slots.
"""
import collections
import re
import struct
import sys

from psx_xref import PsxExe

exe = PsxExe(sys.argv[1])
text = exe.text
base = exe.base

# Function ranges from Rizin's list: "0xADDR  nbbs  size ..."
funcs = []
for line in open(sys.argv[2]):
    m = re.match(r'0x([0-9a-f]{8})\s+\d+\s+(\d+)', line)
    if m:
        funcs.append((int(m.group(1), 16), int(m.group(2))))
funcs.sort()
covered = sum(size for _, size in funcs)


def word(addr):
    return struct.unpack_from('<I', text, addr - base)[0]


counts = collections.Counter()
cop2_funcs = set()
load_delay_hazards = []
for start, size in funcs:
    for addr in range(start, start + size, 4):
        w = word(addr)
        op = w >> 26
        if op == 0x12:
            counts['COP2 (GTE)'] += 1
            cop2_funcs.add(start)
        elif op == 0x10:
            counts['COP0'] += 1
        elif op in (0x22, 0x26):
            counts['lwl/lwr'] += 1
        elif op in (0x2A, 0x2E):
            counts['swl/swr'] += 1
        elif op in (0x32, 0x3A):
            counts['lwc2/swc2 (GTE load/store)'] += 1
        elif op == 0 and (w & 0x3F) in (0x08, 0x09):
            counts['jr/jalr'] += 1
        elif op == 0 and (w & 0x3F) in (0x0C, 0x0D):
            counts['syscall/break'] += 1
        elif op in (0x20, 0x21, 0x23, 0x24, 0x25):
            counts['loads'] += 1
            # load delay hazard: next instruction reads the register just loaded
            rt = (w >> 16) & 31
            nxt = word(addr + 4) if addr + 4 < start + size else 0
            nop_ = nxt >> 26
            nrs, nrt = (nxt >> 21) & 31, (nxt >> 16) & 31
            reads = nrs == rt or (nop_ in (0, 0x04, 0x05, 0x28, 0x29, 0x2A, 0x2B, 0x2E) and nrt == rt)
            if rt != 0 and nxt != 0 and reads:
                load_delay_hazards.append(addr)

total = sum(size for _, size in funcs) // 4
print(f'functions={len(funcs)} covering {covered} bytes ({total} words) of {len(text)} text bytes')
for k, v in counts.most_common():
    print(f'  {k:<28} {v}')
print(f'functions using the GTE: {len(cop2_funcs)}')
print(f'load-delay hazards (next insn reads the loaded register): {len(load_delay_hazards)}')
for a in load_delay_hazards[:8]:
    print(f'   {a:08x}: {word(a):08x} / {word(a + 4):08x}')

"""Prints native blocks that may read a stale register, with the offending instruction highlighted.

Usage: python block_livein_show.py <folder> <register> [count]
"""
import struct
import sys

from capstone import CS_ARCH_MIPS, CS_MODE_LITTLE_ENDIAN, CS_MODE_MIPS32, Cs

import block_livein_lib as lib

reg = sys.argv[2]
count = int(sys.argv[3]) if len(sys.argv) > 3 else 3
md = Cs(CS_ARCH_MIPS, CS_MODE_MIPS32 | CS_MODE_LITTLE_ENDIAN)
shown = 0
for b in sorted(lib.unique_blocks(sys.argv[1]), key=len):
    if reg in lib.analyze(b):
        print(f'--- block of {len(b)} words')
        for k, ins in enumerate(md.disasm(struct.pack('<%dI' % len(b), *b), 0)):
            print(f'  {k:3d}: {ins.mnemonic:<6} {ins.op_str}')
        shown += 1
        if shown >= count:
            break

"""Surveys the native MIPS blocks embedded in GOOL scripts.

For static recompilation we need to know what each block may touch: relative branches, absolute jumps into the
executable, indirect jumps, and the GTE.
"""
import collections
import struct
import sys

from capstone import CS_ARCH_MIPS, CS_MODE_LITTLE_ENDIAN, CS_MODE_MIPS32, Cs

from embedded_blocks import all_blocks

EXE_LO, EXE_HI = 0x80010000, 0x8005F800
md = Cs(CS_ARCH_MIPS, CS_MODE_MIPS32 | CS_MODE_LITTLE_ENDIAN)
md.detail = True

blocks = [b for _, _, _, b in all_blocks(sys.argv[1])]
unique = set(blocks)
stats = collections.Counter()
abs_targets = collections.Counter()
for blk in unique:
    raw = struct.pack('<%dI' % len(blk), *blk)
    for ins in md.disasm(raw, 0):
        m = ins.mnemonic
        if m in ('j', 'jal') and ins.operands:
            # j/jal keep the high 4 bits of the (unknown) block address, so compare the low 28 bits
            low = ins.operands[0].imm & 0x0FFFFFFF
            if EXE_LO & 0x0FFFFFFF <= low < EXE_HI & 0x0FFFFFFF:
                stats['j/jal into executable range'] += 1
                abs_targets[low | 0x80000000] += 1
            else:
                stats['j/jal elsewhere'] += 1
        elif m in ('jr', 'jalr') and ins.op_str != '$ra':
            stats['indirect jr/jalr'] += 1
        elif m.startswith('b') or m in ('beq', 'bne'):
            stats['relative branch'] += 1
        elif m in ('mfc2', 'mtc2', 'cfc2', 'ctc2', 'lwc2', 'swc2', 'cop2'):
            stats['GTE'] += 1

print(f'{len(blocks)} blocks, {len(unique)} unique; words in unique blocks: {sum(len(b) for b in unique)}')
for k, v in stats.most_common():
    print(f'  {k:<32} {v}')
print('most-called executable targets:', ', '.join(f'{a:08x}x{n}' for a, n in abs_targets.most_common(12)))
lens = sorted(len(b) for b in unique)
print(f'block length (words): min {lens[0]}, median {lens[len(lens) // 2]}, max {lens[-1]}')

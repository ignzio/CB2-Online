"""Shows how unique embedded MIPS blocks use indirect jumps (jr/jalr to a register other than ra)."""
import struct
import sys

from capstone import CS_ARCH_MIPS, CS_MODE_LITTLE_ENDIAN, CS_MODE_MIPS32, Cs

from embedded_blocks import all_blocks

md = Cs(CS_ARCH_MIPS, CS_MODE_MIPS32 | CS_MODE_LITTLE_ENDIAN)
unique = sorted({b for _, _, _, b in all_blocks(sys.argv[1])}, key=len)
shown = 0
for blk in unique:
    insns = list(md.disasm(struct.pack('<%dI' % len(blk), *blk), 0))
    for k, ins in enumerate(insns):
        if ins.mnemonic in ('jr', 'jalr') and ins.op_str != '$ra':
            print(f'--- block of {len(blk)} words, {ins.mnemonic} {ins.op_str} at word {k}')
            for j in insns[max(0, k - 7):k + 2]:
                print(f'    {j.address // 4:4d}: {j.mnemonic} {j.op_str}')
            shown += 1
            break
    if shown >= 4:
        break

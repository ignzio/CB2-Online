"""Shared helpers: extract the native MIPS blocks embedded in GOOL scripts (after the MIPS marker, opcode 73).

The MIPS handler calls the block with `jalr s5`. The block returns with `jalr s5, ra` (word 0x03E0A809), which
jumps back into the interpreter and links the address after its delay slot into s5, so the interpreter resumes
at the bytecode that follows the block. A block is therefore the words after the marker through that
instruction and its delay slot.
"""
import glob
import struct

from nsf_probe import walk

BLOCK_END = 0x03E0A809
MIPS_MARKER = 73


def gool_code(chunk):
    """Yields (eid, bytecode words) for each GOOL entry (type 0x0B) in a decoded entry chunk."""
    cnt = struct.unpack_from('<I', chunk, 8)[0]
    offs = struct.unpack_from('<%dI' % (cnt + 1), chunk, 0x10)
    for o in offs[:-1]:
        _, eid, ftype, items = struct.unpack_from('<IIII', chunk, o)
        if ftype != 0x0B:
            continue
        io = struct.unpack_from('<%dI' % (items + 1), chunk, o + 16)
        code = chunk[o + io[1]:o + io[2]]
        yield eid, struct.unpack('<%dI' % (len(code) // 4), code[:len(code) // 4 * 4])


def blocks_in(words):
    """Yields (start index of first block word, block words) for each embedded block in one script."""
    i = 0
    while i < len(words):
        if words[i] >> 24 == MIPS_MARKER:
            j = i + 1
            while j < len(words) and words[j] != BLOCK_END:
                j += 1
            if j + 1 < len(words):
                yield i + 1, tuple(words[i + 1:j + 2])
                i = j + 2
                continue
        i += 1


def all_blocks(root):
    """Returns a list of (nsf path, script eid, word index, block words) for every embedded block."""
    out = []
    for path in sorted(glob.glob(root + r'\S?\*.NSF')):
        _, chunks, _ = walk(path)
        for _, _, chunk in chunks:
            if struct.unpack_from('<H', chunk, 2)[0] != 0:
                continue
            for eid, words in gool_code(chunk):
                for index, blk in blocks_in(words):
                    out.append((path, eid, index, blk))
    return out

"""Compares opcode histograms for GOOL items 1 and 2 across all levels, to decide which one is the bytecode."""
import collections
import glob
import struct
import sys

sys.path.insert(0, 'tools')
from nsf_probe import walk  # noqa: E402


def gool_items(d):
    cnt = struct.unpack_from('<I', d, 8)[0]
    offs = struct.unpack_from('<%dI' % (cnt + 1), d, 0x10)
    for o in offs[:-1]:
        _, fn, ft, ic = struct.unpack_from('<IIII', d, o)
        if ft != 0x0B:
            continue
        io = struct.unpack_from('<%dI' % (ic + 1), d, o + 16)
        yield [d[o + a:o + b] for a, b in zip(io, io[1:])]


hist = {1: collections.Counter(), 2: collections.Counter()}
total = {1: 0, 2: 0}
for path in sorted(glob.glob(sys.argv[1] + r'\S?\*.NSF')):
    _, chunks, _ = walk(path)
    for _, _, d in chunks:
        if struct.unpack_from('<H', d, 2)[0] != 0:
            continue
        for items in gool_items(d):
            for k in (1, 2):
                raw = items[k]
                for w in struct.unpack('<%dI' % (len(raw) // 4), raw[:len(raw) // 4 * 4]):
                    hist[k][w >> 24] += 1
                    total[k] += 1

for k in (1, 2):
    valid = sum(n for op, n in hist[k].items() if op < 79)
    print(f'item {k}: {total[k]} words, {valid} with opcode < 79 ({100 * valid / total[k]:.0f}%), '
          f'top opcodes: ' + ', '.join(f'{op}:{n}' for op, n in hist[k].most_common(8)))

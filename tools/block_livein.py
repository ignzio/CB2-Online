"""Reports which registers the native blocks embedded in GOOL scripts may read before writing them.

Usage: python block_livein.py <folder with S0..S3>
"""
import collections
import sys

import block_livein_lib as lib

blocks = lib.unique_blocks(sys.argv[1])
flagged = collections.Counter()
for b in blocks:
    for r in lib.analyze(b):
        flagged[r] += 1
print(f'{len(blocks)} unique blocks analysed')
if not flagged:
    print('no block reads a register it has not written, apart from the interpreter-owned ones (s0-s7, fp, sp, ra)')
else:
    for r, n in flagged.most_common():
        print(f'  {n} block(s) may read {r} before writing it')

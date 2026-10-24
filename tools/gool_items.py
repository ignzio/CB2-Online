"""Prints the item layout of GOOL entries (type 0x0B) in an NSF, to verify which item holds code."""
import struct
import sys

sys.path.insert(0, 'tools')
from nsf_probe import walk

CH = '0123456789abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ_!'


def nm(v):
    c = [CH[(v >> (25 - 6 * i)) & 63] for i in range(4)] + [CH[(v >> 1) & 63]]
    return ''.join(c[:4]) + '.' + c[4]


def entries(d):
    cnt = struct.unpack_from('<I', d, 8)[0]
    offs = struct.unpack_from('<%dI' % (cnt + 1), d, 0x10)
    for o in offs[:-1]:
        _, fn, ft, ic = struct.unpack_from('<IIII', d, o)
        io = struct.unpack_from('<%dI' % (ic + 1), d, o + 16)
        yield nm(fn), ft, [d[o + a:o + b] for a, b in zip(io, io[1:])]


want = sys.argv[2] if len(sys.argv) > 2 else None
buf, chunks, _ = walk(sys.argv[1])
shown = 0
for off, mg, d in chunks:
    if struct.unpack_from('<H', d, 2)[0] != 0:
        continue
    for name, ft, items in entries(d):
        if ft != 0x0B or (want and name != want):
            continue
        hdr = struct.unpack('<6i', items[0])
        print(f'{name}: header(id,class,format,heap,events,entries)={hdr}; item sizes={[len(i) for i in items]}')
        if want:
            code = items[2]
            for k in range(0, min(len(code), 4 * 40), 4):
                w = struct.unpack_from('<I', code, k)[0]
                print(f'  {k // 4:3d}: {w:08x}')
        shown += 1
        if not want and shown >= 12:
            sys.exit()

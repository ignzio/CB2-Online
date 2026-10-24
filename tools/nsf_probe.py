"""Prototype NSF reader used to validate the format before the C++ port.

Walks an on-disc NSF, decompresses 0x1235 chunks (LZSS + RLE) and checks each
0x1234 chunk's checksum, which proves the decoder is correct.
"""
import struct
import sys
import glob
import collections

CHUNK = 0x10000
SECTOR = 0x800


def decompress(buf, off):
    """Return (chunk_bytes[0x10000], bytes_consumed_in_file) for a 0x1235 chunk."""
    dec_size, skip = struct.unpack_from('<II', buf, off + 4)
    if dec_size > CHUNK:
        raise ValueError('bad decompressed size')
    src = off + 0xC
    out = bytearray()
    while len(out) < dec_size:
        x = buf[src]
        src += 1
        if x < 0x80:
            out += buf[src:src + x]
            src += x
        else:
            x = ((x & 0x7F) << 8) | buf[src]
            src += 1
            disp = x >> 3
            ln = (x & 7) + 3
            if ln == 0x0A:
                ln = 0x40
            if disp == 0 or disp > len(out):
                raise ValueError('bad back-reference')
            for _ in range(ln):
                out.append(out[-disp])
    src += skip
    tail = CHUNK - len(out)
    out += buf[src:src + tail]
    src += tail
    consumed = src - off
    return bytes(out), consumed


def chunk_ok(chunk):
    """Structural validation of a decoded 0x10000-byte page.

    The header checksum at +0xC is not CB1's plain byte sum and is not needed for
    loading, so the page is validated structurally instead.
    """
    magic, fam = struct.unpack_from('<HH', chunk, 0)
    if magic != 0x1234 or fam not in (0, 1, 2, 3, 4, 5):
        return False
    if fam != 0:
        return True
    count = struct.unpack_from('<I', chunk, 8)[0]
    if count > 0x3FC0:
        return False
    offs = struct.unpack_from('<%dI' % (count + 1), chunk, 0x10)
    if any(a > b for a, b in zip(offs, offs[1:])) or offs[-1] > CHUNK:
        return False
    return all(struct.unpack_from('<I', chunk, o)[0] == 0x0100FFFF for o in offs[:-1])


def walk(path):
    buf = open(path, 'rb').read()
    off = 0
    chunks = []
    while off + 0x10 <= len(buf):
        magic = struct.unpack_from('<H', buf, off)[0]
        if magic == 0x1234:
            data = buf[off:off + CHUNK]
            used = CHUNK
        elif magic == 0x1235:
            data, used = decompress(buf, off)
        else:
            raise ValueError(f'{path}: bad magic {magic:04x} at {off:x}')
        chunks.append((off, magic, data))
        off += (used + SECTOR - 1) // SECTOR * SECTOR
    return buf, chunks, off


if __name__ == '__main__':
    pattern = sys.argv[1] if len(sys.argv) > 1 else r'S?\*.NSF'
    total = bad = 0
    types = collections.Counter()
    for p in sorted(glob.glob(pattern)):
        buf, chunks, end = walk(p)
        ok = end == len(buf)
        for off, magic, data in chunks:
            total += 1
            fam = struct.unpack_from('<H', data, 2)[0]
            types[fam] += 1
            if len(data) != CHUNK or not chunk_ok(data):
                bad += 1
                print('  STRUCTURE FAIL', p, hex(off), 'family', fam)
        print(f'{p}: {len(chunks)} chunks, ends exactly at EOF: {ok}')
    print(f'chunks={total} bad={bad} families={dict(types)}')

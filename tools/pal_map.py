"""Maps addresses of the NTSC-U executable (SCUS_941.54) to the PAL one (SCES_009.67).
Code: a window of instructions around the address is matched in the other image with jump targets and immediates masked.
Data: an instruction in the NTSC code that references the address (lui + load/store) is mapped, and the same pair is read in the PAL image.
usage: python tools/pal_map.py NTSC_EXE PAL_EXE [hexaddr ...]   (no addresses: the ones the game needs)"""
import struct, sys, collections

BASE = 0x80010000

def image(path):
    d = open(path, 'rb').read()
    n = (len(d) - 0x800) // 4
    return list(struct.unpack('<%dI' % n, d[0x800:0x800 + n * 4]))

def mask(w):
    op = w >> 26
    if op in (2, 3):
        return w & 0xFC000000
    if op == 15 or op in (8, 9, 10, 11, 12, 13, 14) or 0x20 <= op <= 0x2e or op in (0x31, 0x39):
        return w & 0xFFFF0000
    return w

def s16(v):
    return v - 0x10000 if v & 0x8000 else v

class Mapper:
    def __init__(self, ntsc, pal):
        self.n, self.p = image(ntsc), image(pal)
        self.mn = [mask(w) for w in self.n]
        self.mp = [mask(w) for w in self.p]
        self.index = {}

    def _idx(self, k):
        if k not in self.index:
            d = collections.defaultdict(list)
            for i in range(len(self.mp) - k):
                d[tuple(self.mp[i:i + k])].append(i)
            self.index[k] = d
        return self.index[k]

    def code(self, addr, before=0):
        """PAL address of the instruction at NTSC `addr`, or None. Tries windows of growing size around it."""
        i = (addr - BASE) >> 2
        for k in (10, 14, 20, 32):
            for back in sorted({0, 2, 4, 6, 8, 10}, key=lambda b: abs(b - k // 3)):
                s = i - back
                if s < 0 or s + k > len(self.mn):
                    continue
                key = tuple(self.mn[s:s + k])
                if len(set(key)) < 4:
                    continue
                c = self._idx(k).get(key)
                if c and len(c) == 1:
                    return BASE + (c[0] + back) * 4
        return None

    def data(self, daddr):
        """PAL address of the global at NTSC `daddr`: found through a lui/load-store pair in the NTSC code."""
        hi, lo = (daddr + 0x8000) >> 16, daddr & 0xFFFF
        found = collections.Counter()
        for i in range(len(self.n) - 4):
            w = self.n[i]
            if w >> 26 != 15 or (w & 0xFFFF) != hi:
                continue
            reg = (w >> 16) & 31
            for j in range(i + 1, min(i + 4, len(self.n))):
                x = self.n[j]
                op = x >> 26
                if (0x20 <= op <= 0x2e) and ((x >> 21) & 31) == reg and (x & 0xFFFF) == lo:
                    pa = self.code(BASE + i * 4)
                    if pa is None:
                        continue
                    pi = (pa - BASE) >> 2
                    pw = self.p[pi]
                    if pw >> 26 != 15:
                        continue
                    for pj in range(pi + 1, pi + 4):
                        px = self.p[pj]
                        if (0x20 <= (px >> 26) <= 0x2e) and (px >> 26) == op and ((px >> 21) & 31) == ((pw >> 16) & 31):
                            found[((pw & 0xFFFF) << 16) + s16(px & 0xFFFF) & 0xFFFFFFFF] += 1
                            break
        return found.most_common(3)

DEFAULT_CODE = {
    'kObjectStep': 0x8001C718, 'kObjectStepReturn': 0x80018AD4, 'kAfterAnimate': 0x8001DDC0, 'kRunScript': 0x8003A014,
    'kCreateObject': 0x80019800, 'kDestroyObject': 0x80019E08, 'kOpenLevel': 0x80012170, 'kNextLevel': 0x80037DC8,
    'gte scenery A': 0x80042420, 'gte scenery B': 0x800426A0, 'gte scenery C': 0x8004274C, 'gte scenery D': 0x8004513C,
    'vsync spin': 0x8004A5F4, 'vsync wait (hle)': 0x8004A5CC,
    'card tramp 1': 0x8004A2C4, 'card tramp 2': 0x8004A2D8, 'card call': 0x8004A364,
}
DEFAULT_DATA = {
    'kPlayerGlobal': 0x8006CB10, 'kPuppetParent': 0x8006CB20, 'kCurrentLevel': 0x8005F3EC, 'camera position': 0x80060768,
    'vsync counter': 0x8005DC98, 'exe size word': 0x8005F188,
}

if __name__ == '__main__':
    m = Mapper(sys.argv[1], sys.argv[2])
    extra = [int(a, 16) for a in sys.argv[3:]]
    if extra:
        for a in extra:
            print('%08x code -> %s   data -> %s' % (a, ('%08x' % m.code(a)) if m.code(a) else None, [('%08x' % v, c) for v, c in m.data(a)]))
    else:
        for name, a in DEFAULT_CODE.items():
            r = m.code(a)
            print('%-18s %08x -> %s' % (name, a, ('%08x' % r) if r else 'NOT FOUND'))
        for name, a in DEFAULT_DATA.items():
            print('%-18s %08x -> %s' % (name, a, [('%08x' % v, c) for v, c in m.data(a)] or 'NOT FOUND'))

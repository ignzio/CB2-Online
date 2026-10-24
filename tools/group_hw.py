"""Group the recorded hardware-access sites (cb2_boot --hwsites) by function: group_hw.py <hwsites.txt> <functions.txt>"""
import bisect
import re
import sys
from collections import defaultdict

funcs = []
for line in open(sys.argv[2]):
    m = re.match(r"\s*0x([0-9a-fA-F]+)\s+\d+\s+(\d+)", line)
    if m:
        funcs.append(int(m.group(1), 16))
funcs.sort()
dev = defaultdict(lambda: defaultdict(int))
bios = defaultdict(int)
for line in open(sys.argv[1]):
    p = line.split()
    if p[0] == "bios":
        a = int(p[1], 16)
        bios[funcs[bisect.bisect_right(funcs, a - 8) - 1]] += int(p[2])
        continue
    pc = int(p[0], 16)
    f = funcs[bisect.bisect_right(funcs, pc) - 1]
    for kv in p[1:]:
        d, n = kv.split(":")
        dev[f][d] += int(n)
print("functions touching devices:")
for f in sorted(dev):
    print("  %08x  %s" % (f, " ".join("%s:%d" % kv for kv in sorted(dev[f].items()))))
print("%d functions" % len(dev))
print("callers of BIOS calls (functions):", " ".join("%08x" % f for f in sorted(bios)))

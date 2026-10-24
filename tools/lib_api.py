"""List library entry points: functions at/above a boundary that game code (below it) calls via jal, with call counts and
a few example callers.  python tools/lib_api.py <SCUS_941.54> <functions.txt> [boundary=80046000]"""
import bisect
import re
import struct
import sys
from collections import defaultdict

sys.path.insert(0, __file__.rsplit("\\", 1)[0])
from psx_xref import PsxExe  # noqa: E402

exe = PsxExe(sys.argv[1])
bound = int(sys.argv[3], 16) if len(sys.argv) > 3 else 0x80046000
funcs = sorted(int(m.group(1), 16) for m in (re.match(r"\s*0x([0-9a-fA-F]+)\s+\d+\s+(\d+)", l) for l in open(sys.argv[2])) if m)
callers = defaultdict(list)
for a in range(exe.base, bound, 4):
    w = exe.u32(a)
    if w >> 26 == 3:
        t = ((w & 0x3FFFFFF) << 2) | 0x80000000
        if bound <= t < exe.end:
            callers[t].append(a)
print("library functions called from game code (< %08x):" % bound)
for t in sorted(callers):
    f = funcs[bisect.bisect_right(funcs, t) - 1]
    print("  %08x%s  x%-4d e.g. %s" % (t, "" if f == t else " (inside %08x)" % f, len(callers[t]), " ".join("%08x" % c for c in callers[t][:3])))
print(len(callers), "entry points")

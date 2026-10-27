#!/usr/bin/env python3
r"""Turns the addresses of a crash report into function names.

    python tools/resolve_crash.py crash-last.log [Crash2.map]

crash.log (and the hang report in cb2-session.log) lists the call stack as `Crash2.exe+0x1ed36` (module + offset). The linker's map file of the SAME build says which function that is:
every release has one (Crash2-v<version>.map, attached to the release; `tools\make_release.ps1` also keeps it in dist\symbols\<version>\). Without the map argument the script looks for
dist\symbols\<version>\Crash2.map (the version is read from the report) and then build\Release\cb2_game.map. C++ names are demangled with undname.exe when Visual Studio is installed.
"""
import bisect, glob, os, re, subprocess, sys

def load_map(path):
    base = 0
    syms = []   # (rva, name)
    in_publics = False
    for line in open(path, errors="replace"):
        m = re.search(r"Preferred load address is ([0-9a-fA-F]+)", line)
        if m:
            base = int(m.group(1), 16)
            continue
        if "Publics by Value" in line:
            in_publics = True
            continue
        if in_publics:
            m = re.match(r"\s*[0-9a-fA-F]{4}:[0-9a-fA-F]{8}\s+(\S+)\s+([0-9a-fA-F]{8,16})\s", line)
            if m:
                syms.append((int(m.group(2), 16) - base, m.group(1)))
    syms.sort()
    return syms

_undname = None
def undname_path():
    global _undname
    if _undname is None:
        hits = glob.glob(r"C:\Program Files\Microsoft Visual Studio\*\*\VC\Tools\MSVC\*\bin\Hostx64\x64\undname.exe") + glob.glob(r"C:\Program Files (x86)\Microsoft Visual Studio\*\*\VC\Tools\MSVC\*\bin\Hostx64\x64\undname.exe")
        _undname = sorted(hits)[-1] if hits else ""
    return _undname

_cache = {}
def demangle(name):
    if not name.startswith("?") or not undname_path():
        return name
    if name not in _cache:
        try:
            out = subprocess.run([undname_path(), "0x1000", name], capture_output=True, text=True, timeout=10).stdout   # 0x1000: names only
            m = re.search(r'is :- "(.*)"', out)
            _cache[name] = m.group(1) if m else name
        except Exception:
            _cache[name] = name
    return _cache[name]

def main():
    if len(sys.argv) < 2:
        raise SystemExit(__doc__)
    report = open(sys.argv[1], errors="replace").read()
    map_path = sys.argv[2] if len(sys.argv) > 2 else ""
    if not map_path:
        here = os.path.dirname(os.path.abspath(__file__))
        v = re.search(r"^version:\s*(\S+)", report, re.M) or re.search(r"CB2-Online (\S+) started", report)
        cands = []
        if v:
            cands.append(os.path.join(here, "..", "dist", "symbols", "v" + v.group(1), "Crash2.map"))
        cands.append(os.path.join(here, "..", "build", "Release", "cb2_game.map"))
        map_path = next((c for c in cands if os.path.exists(c)), "")
        if not map_path:
            raise SystemExit("no map file found (looked in: " + ", ".join(cands) + "); pass it as the second argument")
    syms = load_map(map_path)
    rvas = [s[0] for s in syms]
    print("; symbols from", os.path.abspath(map_path), f"({len(syms)} functions)")
    def fix(m):
        rva = int(m.group(2), 16)
        i = bisect.bisect_right(rvas, rva) - 1
        if i < 0:
            return m.group(0)
        return f"{m.group(0)}   <- {demangle(syms[i][1])} +0x{rva - syms[i][0]:x}"
    print(re.sub(r"\b(Crash2|cb2_game|cb2_play)\.exe\+0x([0-9a-fA-F]+)", fix, report))

main()

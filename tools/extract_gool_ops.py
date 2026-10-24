"""Extracts the Crash 2 GOOL opcode table (id, name, base class, operand format) from CrashEdit's source.

Usage: python extract_gool_ops.py <GOOLInstructionList.cs> [out.json]
The source file is third-party and is only read, never copied into the project.
"""
import json
import re
import sys

src = open(sys.argv[1], encoding='utf-8').read()
if src.startswith('successfully downloaded'):
    src = src.split('\n', 1)[1] if '\n' in src else src

# Default operand formats declared by the abstract base classes.
base_formats = {}
for m in re.finditer(r'public abstract class (\w+)\s*:\s*(\w+)(.*?)(?=\n    (?:/// |\[GOOLInstruction|public abstract class|public sealed class))', src, re.S):
    name, parent, body = m.groups()
    fm = re.search(r'GetFormat\(\)\s*=>\s*([^;]+);', body)
    base_formats[name] = (parent, fm.group(1).strip() if fm else None)

consts = dict(re.findall(r'public const string (\w+)\s*=\s*"([^"]*)"', open(sys.argv[1], encoding='utf-8').read()))
consts = {}
for line in src.splitlines():
    pass

ops = {}
pattern = re.compile(r'((?:\s*\[GOOLInstruction\([^\]]*\)\]\s*)+)public (?:sealed )?class (\w+)\s*:\s*(\w+)\s*\{(.*?)\n    \}', re.S)
for m in pattern.finditer(src):
    attrs, cls, parent, body = m.groups()
    ids = re.findall(r'GOOLInstruction\((\d+),\s*GameVersion\.Crash2\)', attrs)
    if not ids:
        continue
    nm = re.search(r'GetName\([^)]*\)\s*=>\s*"([^"]+)"', body)
    fmt = re.search(r'GetFormat\(\)\s*=>\s*([^;]+);', body)
    ops[int(ids[0])] = {
        'class': cls,
        'parent': parent,
        'name': nm.group(1) if nm else cls.upper(),
        'format': fmt.group(1).strip() if fmt else None,
    }

# Resolve a missing format by walking up to the nearest base class that declares one.
for op in ops.values():
    p, f = op['parent'], op['format']
    seen = 0
    while f is None and p in base_formats and seen < 10:
        p, f = base_formats[p][0], base_formats[p][1]
        seen += 1
    op['resolved'] = f

print(f'{len(ops)} Crash 2 opcodes; ids {min(ops)}..{max(ops)}')
if len(sys.argv) > 2:
    json.dump({str(k): v for k, v in sorted(ops.items())}, open(sys.argv[2], 'w'), indent=1)
for k, v in sorted(ops.items()):
    print(f"{k:3d} {v['name']:<14} {v['parent']:<22} {v['resolved']}")

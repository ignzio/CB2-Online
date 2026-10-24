"""Scans a PS-X EXE for long runs of 32-bit words that all point into the text segment (jump/handler tables)."""
import struct
import sys

from psx_xref import PsxExe, containing_function, disassemble, function_starts

exe = PsxExe(sys.argv[1])
min_run = int(sys.argv[3]) if len(sys.argv) > 3 else 40
words = struct.unpack('<%dI' % (len(exe.text) // 4), exe.text[:len(exe.text) // 4 * 4])


def is_code_ptr(w):
    return exe.base <= w < exe.end and w % 4 == 0


insns = [i for i in disassemble(exe) if i.id != 0]
starts = function_starts(exe, insns, sys.argv[2])
start_set = set(starts)

runs = []
i = 0
while i < len(words):
    if is_code_ptr(words[i]):
        j = i
        while j < len(words) and is_code_ptr(words[j]):
            j += 1
        if j - i >= min_run:
            runs.append((i, j - i))
        i = j
    else:
        i += 1

for idx, n in runs:
    addr = exe.base + idx * 4
    targets = words[idx:idx + n]
    fn_hits = sum(t in start_set for t in targets)
    owners = sorted({containing_function(starts, t) for t in targets})
    print(f'table at {addr:08x}: {n} entries, {fn_hits} are function starts, targets span {min(targets):08x}..{max(targets):08x}, '
          f'{len(owners)} distinct functions')

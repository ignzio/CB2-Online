"""PS1 (PS-X EXE) analysis helpers built on Capstone, used when REA/Rizin xrefs are not enough.

Resolves MIPS `lui` + `addiu/ori/load/store` address pairs into cross-references and groups them by
function (function starts come from `jal` targets and Rizin's function list when available).
"""
import re
import struct
import sys
from collections import defaultdict

from capstone import CS_ARCH_MIPS, CS_MODE_LITTLE_ENDIAN, CS_MODE_MIPS32, Cs
from capstone.mips import MIPS_OP_IMM, MIPS_OP_MEM, MIPS_OP_REG


class PsxExe:
    def __init__(self, path):
        data = open(path, 'rb').read()
        if data[:8] != b'PS-X EXE':
            raise ValueError('not a PS-X EXE')
        self.pc, self.gp, self.base, self.size, _, _, _, _, self.sp = struct.unpack_from('<9I', data, 0x10)
        self.text = data[0x800:0x800 + self.size]
        self.end = self.base + self.size

    def contains(self, addr):
        return self.base <= addr < self.end

    def u32(self, addr):
        return struct.unpack_from('<I', self.text, addr - self.base)[0]

    def cstring(self, addr, limit=96):
        off = addr - self.base
        end = self.text.find(b'\0', off, off + limit)
        return self.text[off:end if end >= 0 else off + limit].decode('latin1')


def disassemble(exe):
    md = Cs(CS_ARCH_MIPS, CS_MODE_MIPS32 | CS_MODE_LITTLE_ENDIAN)
    md.detail = True
    md.skipdata = True
    return list(md.disasm(exe.text, exe.base))


def address_refs(insns):
    """Yield (instruction address, constant) for every lui-built address constant."""
    last_lui = {}
    for ins in insns:
        if ins.id == 0:  # skipdata pseudo-instruction (data embedded in the text segment)
            continue
        if ins.mnemonic == 'lui' and len(ins.operands) == 2:
            reg, imm = ins.operands
            last_lui[reg.reg] = (imm.imm << 16, ins.address)
            continue
        for op in ins.operands:
            reg = None
            lo = None
            if ins.mnemonic in ('addiu', 'ori') and len(ins.operands) == 3 and ins.operands[1].type == MIPS_OP_REG:
                reg, lo = ins.operands[1].reg, ins.operands[2].imm
            elif op.type == MIPS_OP_MEM:
                reg, lo = op.mem.base, op.mem.disp
            if reg in last_lui and lo is not None:
                hi, at = last_lui[reg]
                if ins.address - at <= 32:
                    value = hi + (lo if ins.mnemonic != 'ori' else lo & 0xFFFF)
                    yield ins.address, value & 0xFFFFFFFF
                break
        if ins.mnemonic.startswith('j') and ins.mnemonic != 'jal':
            last_lui.clear()


def function_starts(exe, insns, rizin_list=None):
    starts = {exe.pc}
    for ins in insns:
        if ins.id == 0:
            continue
        if ins.mnemonic == 'jal' and ins.operands and ins.operands[0].type == MIPS_OP_IMM:
            if exe.contains(ins.operands[0].imm):
                starts.add(ins.operands[0].imm)
    if rizin_list:
        for line in open(rizin_list):
            m = re.match(r'0x([0-9a-f]{8})\s', line)
            if m:
                starts.add(int(m.group(1), 16))
    return sorted(starts)


def containing_function(starts, addr):
    lo, hi = 0, len(starts)
    while lo < hi:
        mid = (lo + hi) // 2
        if starts[mid] <= addr:
            lo = mid + 1
        else:
            hi = mid
    return starts[lo - 1] if lo else None


if __name__ == '__main__':
    exe = PsxExe(sys.argv[1])
    insns = disassemble(exe)
    starts = function_starts(exe, insns, sys.argv[2] if len(sys.argv) > 2 else None)
    refs = defaultdict(set)
    for at, value in address_refs(insns):
        if exe.contains(value):
            refs[value].add(containing_function(starts, at))
    print(f'instructions={len(insns)} function starts={len(starts)} data refs={len(refs)}')
    for text in sys.argv[3:]:
        addr = int(text, 16)
        fns = sorted(f for f in refs.get(addr, ()) if f)
        print(f'{addr:08x} {exe.cstring(addr)!r} -> ' + ', '.join(f'{f:08x}' for f in fns))

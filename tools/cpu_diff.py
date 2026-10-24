"""Differential test: random R3000 programs run on cb2_cpurun and on Unicorn (MIPS32), final state compared.

Unicorn is MIPS32 and has no load delay slots, so the generator never lets a load's result be read by the
very next executed instruction (a NOP follows every load, and delay slots are never loads or branches).
Load-delay behaviour is covered by directed tests in tests/cpu_directed.py.

Usage: python cpu_diff.py <cb2_cpurun.exe> [cases] [seed]
"""
import os
import random
import struct
import subprocess
import sys
import tempfile

from unicorn import UC_ARCH_MIPS, UC_MODE_LITTLE_ENDIAN, UC_MODE_MIPS32, Uc
from unicorn.mips_const import UC_MIPS_REG_0, UC_MIPS_REG_HI, UC_MIPS_REG_LO

CODE, DATA = 0x10000, 0x20000
BASE = 28                      # r28 holds DATA and is never a destination
DEST = [i for i in range(1, 24)] + [26, 27, 29, 30]
SRC = DEST + [0, 24, 28]
M = 0xFFFFFFFF


def R(op, rs, rt, rd, sa, fn):
    return (op << 26) | (rs << 21) | (rt << 16) | (rd << 11) | (sa << 6) | fn


def I(op, rs, rt, imm):
    return (op << 26) | (rs << 21) | (rt << 16) | (imm & 0xFFFF)


NOP = 0


def alu(rng):
    rd, rs, rt = rng.choice(DEST), rng.choice(SRC), rng.choice(SRC)
    kind = rng.randrange(8)
    if kind == 0:
        return [R(0, rs, rt, rd, 0, rng.choice([0x21, 0x23, 0x24, 0x25, 0x26, 0x27, 0x2A, 0x2B]))]
    if kind == 1:
        return [R(0, 0, rt, rd, rng.randrange(32), rng.choice([0x00, 0x02, 0x03]))]
    if kind == 2:
        return [R(0, rs, rt, rd, 0, rng.choice([0x04, 0x06, 0x07]))]
    if kind == 3:
        return [I(rng.choice([0x09, 0x0A, 0x0B]), rs, rd, rng.randrange(0x10000))]
    if kind == 4:
        return [I(rng.choice([0x0C, 0x0D, 0x0E]), rs, rd, rng.randrange(0x10000))]
    if kind == 5:
        return [I(0x0F, 0, rd, rng.randrange(0x10000))]
    if kind == 6:
        return [R(0, rs, rt, 0, 0, rng.choice([0x18, 0x19])), R(0, 0, 0, rng.choice(DEST), 0, rng.choice([0x10, 0x12]))]
    return [R(0, rng.choice(SRC), 0, 0, 0, rng.choice([0x11, 0x13]))]


def memop(rng):
    rt = rng.choice(DEST)
    kind = rng.choice(['lb', 'lbu', 'lh', 'lhu', 'lw', 'lwl', 'lwr', 'sb', 'sh', 'sw', 'swl', 'swr'])
    off = rng.randrange(0, 0xFF0)
    if kind in ('lh', 'lhu', 'sh'):
        off &= ~1
    if kind in ('lw', 'sw'):
        off &= ~3
    op = {'lb': 0x20, 'lh': 0x21, 'lwl': 0x22, 'lw': 0x23, 'lbu': 0x24, 'lhu': 0x25, 'lwr': 0x26,
          'sb': 0x28, 'sh': 0x29, 'swl': 0x2A, 'sw': 0x2B, 'swr': 0x2E}[kind]
    ins = [I(op, BASE, rt, off)]
    if kind.startswith('l'):
        ins.append(NOP)
    return ins


def program(rng, n):
    """Returns a list of words. Control transfers only go forward and never past the end."""
    out = []
    # pending[i] = (kind, ...) is resolved after the body is laid out, so targets can be patched in.
    patches = []
    while len(out) < n:
        r = rng.random()
        if r < 0.50:
            out += alu(rng)
        elif r < 0.75:
            out += memop(rng)
        elif r < 0.90:
            bidx = len(out)
            kind = rng.choice(['beq', 'bne', 'blez', 'bgtz', 'bltz', 'bgez', 'bltzal', 'bgezal'])
            out += [0, rng.choice(alu(rng)[:1])]  # placeholder + delay slot (single-word ALU op only)
            if kind in ('beq', 'bne', 'blez', 'bgtz'):
                patches.append(('b', bidx, kind, rng.choice(SRC), rng.choice(SRC)))
            else:
                patches.append(('r', bidx, kind, rng.choice(SRC), 0))
        elif r < 0.95:
            bidx = len(out)
            out += [0, rng.choice(alu(rng)[:1])]
            patches.append(('j', bidx, rng.choice(['j', 'jal']), 0, 0))
        else:
            bidx = len(out)
            out += [0, 0, 0, rng.choice(alu(rng)[:1])]  # lui, ori, jr/jalr, delay slot
            patches.append(('jr', bidx, rng.choice(['jr', 'jalr']), 0, 0))
    out += [NOP, NOP]
    total = len(out)
    # A branch must not land between the lui/ori that prepares a jump register and the jump itself.
    forbidden = set()
    for kind, bidx, *_ in patches:
        if kind == 'jr':
            forbidden.update((bidx + 1, bidx + 2))
    for kind, bidx, name, a, b in patches:
        span = {'b': 1, 'r': 1, 'j': 1, 'jr': 3}[kind]
        first_target = bidx + span + 1  # skip the delay slot
        if first_target > total:
            first_target = total
        t = rng.randrange(first_target, min(total, first_target + 12) + 1)
        while t in forbidden:
            t += 1
        if kind == 'b':
            imm = t - (bidx + 1)
            op = {'beq': 4, 'bne': 5, 'blez': 6, 'bgtz': 7}[name]
            out[bidx] = I(op, a, b if name in ('beq', 'bne') else 0, imm)
        elif kind == 'r':
            imm = t - (bidx + 1)
            rtf = {'bltz': 0, 'bgez': 1, 'bltzal': 0x10, 'bgezal': 0x11}[name]
            out[bidx] = I(1, a, rtf, imm)
        elif kind == 'j':
            addr = CODE + 4 * t
            out[bidx] = ((2 if name == 'j' else 3) << 26) | ((addr >> 2) & 0x3FFFFFF)
        else:
            addr = CODE + 4 * t
            out[bidx] = I(0x0F, 0, 25, addr >> 16)
            out[bidx + 1] = I(0x0D, 25, 25, addr & 0xFFFF)
            out[bidx + 2] = R(0, 25, 0, 0, 0, 0x08) if name == 'jr' else R(0, 25, 0, 26, 0, 0x09)
    return out


def run_unicorn(regs, hi, lo, code, data):
    uc = Uc(UC_ARCH_MIPS, UC_MODE_MIPS32 | UC_MODE_LITTLE_ENDIAN)
    uc.mem_map(0x10000, 0x30000)
    uc.mem_write(CODE, struct.pack('<%dI' % len(code), *code))
    uc.mem_write(DATA, data)
    for i in range(32):
        uc.reg_write(UC_MIPS_REG_0 + i, regs[i])
    uc.reg_write(UC_MIPS_REG_HI, hi)
    uc.reg_write(UC_MIPS_REG_LO, lo)
    uc.emu_start(CODE, CODE + 4 * len(code), count=200000)
    out = [uc.reg_read(UC_MIPS_REG_0 + i) for i in range(32)]
    return out, uc.reg_read(UC_MIPS_REG_HI), uc.reg_read(UC_MIPS_REG_LO), bytes(uc.mem_read(DATA, 4096))


def main():
    exe = sys.argv[1]
    cases = int(sys.argv[2]) if len(sys.argv) > 2 else 500
    seed = int(sys.argv[3]) if len(sys.argv) > 3 else 1
    rng = random.Random(seed)
    blob = [cases]
    spec = []
    for _ in range(cases):
        regs = [rng.getrandbits(32) if rng.random() < 0.7 else rng.choice([0, 1, M, 0x80000000, 0x7FFFFFFF, 0xFFFF]) for _ in range(32)]
        regs[0] = 0
        regs[BASE] = DATA
        hi, lo = rng.getrandbits(32), rng.getrandbits(32)
        code = program(rng, rng.randrange(20, 120))
        data = bytes(rng.getrandbits(8) for _ in range(4096))
        spec.append((regs, hi, lo, code, data))
        blob += regs + [hi, lo, len(code)] + code + list(struct.unpack('<1024I', data))

    with tempfile.TemporaryDirectory() as d:
        inp, outp = os.path.join(d, 'in.bin'), os.path.join(d, 'out.bin')
        open(inp, 'wb').write(struct.pack('<%dI' % len(blob), *blob))
        subprocess.run([exe, inp, outp], check=True)
        raw = open(outp, 'rb').read()

    stride = 32 + 2 + 1024 + 1
    ours = struct.unpack('<%dI' % (stride * cases), raw)
    bad = 0
    for c, (regs, hi, lo, code, data) in enumerate(spec):
        o = ours[c * stride:(c + 1) * stride]
        u_regs, u_hi, u_lo, u_data = run_unicorn(regs, hi, lo, code, data)
        o_regs, o_hi, o_lo = list(o[:32]), o[32], o[33]
        o_data = struct.pack('<1024I', *o[34:34 + 1024])
        status = o[34 + 1024]
        problems = []
        if status:
            problems.append('interpreter raised an exception')
        diffs = [i for i in range(32) if o_regs[i] != u_regs[i]]
        if diffs:
            problems.append('regs ' + ', '.join(f'r{i}: ours {o_regs[i]:08x} unicorn {u_regs[i]:08x}' for i in diffs[:4]))
        if (o_hi, o_lo) != (u_hi, u_lo):
            problems.append(f'hi/lo ours {o_hi:08x}/{o_lo:08x} unicorn {u_hi:08x}/{u_lo:08x}')
        if o_data != u_data:
            at = next(i for i in range(4096) if o_data[i] != u_data[i])
            problems.append(f'data differs at +{at:#x}')
        if problems:
            bad += 1
            if bad <= 5:
                print(f'case {c} (seed {seed}): ' + '; '.join(problems))
    print(f'{cases} programs, {sum(len(s[3]) for s in spec)} instruction words, mismatches: {bad}')
    sys.exit(1 if bad else 0)


main()

"""Directed tests for R3000 behaviour that Unicorn (MIPS32) does not model.
Expected values follow the documented R3000 semantics (psx-spx); each test states why.

Usage: python cpu_directed.py <cb2_cpurun.exe>
"""
import os
import struct
import subprocess
import sys
import tempfile

CODE, DATA = 0x10000, 0x20000
M = 0xFFFFFFFF


def R(rs, rt, rd, sa, fn):
    return (rs << 21) | (rt << 16) | (rd << 11) | (sa << 6) | fn


def I(op, rs, rt, imm):
    return (op << 26) | (rs << 21) | (rt << 16) | (imm & 0xFFFF)


def lw(rt, off, base=28): return I(0x23, base, rt, off)
def lwl(rt, off, base=28): return I(0x22, base, rt, off)
def lwr(rt, off, base=28): return I(0x26, base, rt, off)
def sw(rt, off, base=28): return I(0x2B, base, rt, off)
def addu(rd, rs, rt): return R(rs, rt, rd, 0, 0x21)
def addiu(rt, rs, imm): return I(0x09, rs, rt, imm)
def beq(rs, rt, imm): return I(0x04, rs, rt, imm)
def mtc0(rt, rd): return (0x10 << 26) | (4 << 21) | (rt << 16) | (rd << 11)
def div(rs, rt): return R(rs, rt, 0, 0, 0x1A)
def divu(rs, rt): return R(rs, rt, 0, 0, 0x1B)
def mflo(rd): return R(0, 0, rd, 0, 0x12)
def mfhi(rd): return R(0, 0, rd, 0, 0x10)
def jal(t): return (3 << 26) | ((CODE + 4 * t) >> 2)
NOP = 0


def run(exe, code, regs=None, data=None):
    regs = list(regs or [0] * 32)
    regs[28] = DATA
    data = data or bytes(range(256)) * 16
    code = list(code) + [NOP, NOP]
    blob = [1] + regs + [0, 0, len(code)] + code + list(struct.unpack('<1024I', data))
    with tempfile.TemporaryDirectory() as d:
        inp, outp = os.path.join(d, 'i'), os.path.join(d, 'o')
        open(inp, 'wb').write(struct.pack('<%dI' % len(blob), *blob))
        subprocess.run([exe, inp, outp], check=True)
        out = struct.unpack('<%dI' % (32 + 2 + 1024 + 1), open(outp, 'rb').read())
    return out[:32], out[32], out[33], struct.pack('<1024I', *out[34:34 + 1024]), out[34 + 1024]


failures = 0


def check(name, cond, detail=''):
    global failures
    if not cond:
        failures += 1
    print(('PASS ' if cond else 'FAIL ') + name + ('' if cond else '  ' + detail))


def main():
    exe = sys.argv[1]
    word0 = struct.unpack_from('<I', bytes(range(256)), 0)[0]  # 0x03020100

    # 1. The instruction after a load sees the OLD register value; the one after that sees the new one.
    regs = [0] * 32; regs[8] = 0x11111111
    r, *_ = run(exe, [lw(8, 0), addu(9, 8, 0), addu(10, 8, 0)], regs)
    check('load delay: next instruction reads old value', r[9] == 0x11111111, hex(r[9]))
    check('load delay: instruction after that reads loaded value', r[10] == word0, hex(r[10]))

    # 2. A write to the load's target register in the delay slot cancels the load.
    regs = [0] * 32; regs[8] = 0x11111111
    r, *_ = run(exe, [lw(8, 0), addiu(8, 0, 5), NOP, NOP], regs)
    check('load delay: write to the same register cancels the load', r[8] == 5, hex(r[8]))

    # 3. lwr/lwl pair merges through the in-flight load (no delay between them): unaligned word at +1.
    regs = [0] * 32; regs[8] = M
    r, *_ = run(exe, [lwr(8, 1), lwl(8, 4), NOP], regs)
    check('lwr+lwl unaligned load bypass', r[8] == 0x04030201, hex(r[8]))

    # 4. Consecutive loads to one register: the later load wins.
    r, *_ = run(exe, [lw(8, 0), lw(8, 4), NOP, NOP])
    check('back-to-back loads to the same register: second wins', r[8] == struct.unpack_from('<I', bytes(range(256)), 4)[0], hex(r[8]))

    # 5. Branch delay slot executes; the skipped instruction does not.
    r, *_ = run(exe, [beq(0, 0, 2), addiu(8, 0, 1), addiu(9, 0, 2), addiu(10, 0, 3)])
    check('branch delay slot executes, skipped instruction does not', (r[8], r[9], r[10]) == (1, 0, 3), str((r[8], r[9], r[10])))

    # 6. jal links to the instruction after the delay slot.
    r, *_ = run(exe, [jal(3), NOP, addiu(8, 0, 9), NOP])
    check('jal link address is pc + 8', r[31] == CODE + 8 and r[8] == 0, hex(r[31]))

    # 7. A load in a taken branch's delay slot is not visible to the first instruction at the target.
    regs = [0] * 32; regs[8] = 0x11111111
    r, *_ = run(exe, [beq(0, 0, 2), lw(8, 0), NOP, addu(9, 8, 0), addu(10, 8, 0)], regs)
    check('delay-slot load: target sees old value', r[9] == 0x11111111 and r[10] == word0, f'{r[9]:08x} {r[10]:08x}')

    # 8. Divide results, including the cases the manual leaves open.
    def divcase(op, a, b):
        regs = [0] * 32; regs[8], regs[9] = a, b
        r, *_ = run(exe, [op(8, 9), mflo(10), mfhi(11)], regs)
        return r[10], r[11]
    check('div 7 / 2', divcase(div, 7, 2) == (3, 1), str(divcase(div, 7, 2)))
    check('div -7 / 2', divcase(div, (-7) & M, 2) == ((-3) & M, (-1) & M), str(divcase(div, (-7) & M, 2)))
    check('div by zero, positive dividend: lo = -1, hi = dividend', divcase(div, 5, 0) == (M, 5), str(divcase(div, 5, 0)))
    check('div by zero, negative dividend: lo = 1, hi = dividend', divcase(div, (-5) & M, 0) == (1, (-5) & M), str(divcase(div, (-5) & M, 0)))
    check('div INT_MIN / -1: lo = INT_MIN, hi = 0', divcase(div, 0x80000000, M) == (0x80000000, 0), str(divcase(div, 0x80000000, M)))
    check('divu by zero: lo = -1, hi = dividend', divcase(divu, 5, 0) == (M, 5), str(divcase(divu, 5, 0)))

    # 9. Isolated cache (SR bit 16): stores do not reach memory.
    regs = [0] * 32; regs[9] = 0x10000; regs[10] = 0xDEADBEEF
    _, _, _, data, _ = run(exe, [mtc0(9, 12), sw(10, 0x40), mtc0(0, 12), sw(10, 0x44)], regs)
    stored = struct.unpack_from('<2I', data, 0x40)
    original = struct.unpack_from('<2I', bytes(range(256)) * 16, 0x40)
    check('isolated cache: store ignored, store after un-isolating works',
          stored[0] == original[0] and stored[1] == 0xDEADBEEF, f'{stored[0]:08x} {stored[1]:08x}')

    print(f'{failures} failure(s)')
    sys.exit(1 if failures else 0)


main()

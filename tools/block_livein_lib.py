"""Static analysis of the native MIPS blocks embedded in GOOL scripts: which registers a block may read before writing.

A block is entered by the interpreter's `jalr s5` (0x8003AAD4). Registers the interpreter owns are s0..s7, fp, sp, ra.
Anything else (v0, v1, a0-a3, t0-t9, at, gp) holds a stale value from the previous handler, so a block that reads one
of those before writing it depends on that value. The scan is control-flow aware: "possibly undefined" registers are
propagated along branches, with the delay slot modelled as part of the edge.
"""
import struct

from embedded_blocks import all_blocks

OWNED = frozenset({'zero', 's0', 's1', 's2', 's3', 's4', 's5', 's6', 's7', 'fp', 'sp', 'ra', 'k0', 'k1'})
NAMES = ['zero', 'at', 'v0', 'v1', 'a0', 'a1', 'a2', 'a3', 't0', 't1', 't2', 't3', 't4', 't5', 't6', 't7',
         's0', 's1', 's2', 's3', 's4', 's5', 's6', 's7', 't8', 't9', 'k0', 'k1', 'gp', 'sp', 'fp', 'ra']


def unique_blocks(root):
    return {b for _, _, _, b in all_blocks(root)}


def reg_use_def(word):
    """Returns (reads, writes) for one MIPS word of the integer ISA subset used by blocks."""
    op = word >> 26
    rs, rt, rd = NAMES[(word >> 21) & 31], NAMES[(word >> 16) & 31], NAMES[(word >> 11) & 31]
    fn = word & 63
    if op == 0:
        if fn in (0x00, 0x02, 0x03):
            return {rt}, {rd}
        if fn in (0x04, 0x06, 0x07, 0x20, 0x21, 0x22, 0x23, 0x24, 0x25, 0x26, 0x27, 0x2A, 0x2B):
            return {rs, rt}, {rd}
        if fn == 0x08:
            return {rs}, set()
        if fn == 0x09:
            return {rs}, {rd}
        if fn in (0x10, 0x12):
            return set(), {rd}
        if fn in (0x11, 0x13):
            return {rs}, set()
        return {rs, rt}, set()
    if op in (0x04, 0x05):
        return {rs, rt}, set()
    if op in (0x06, 0x07, 0x01):
        return {rs}, ({'ra'} if op == 1 and ((word >> 16) & 0x1E) == 0x10 else set())
    if op in (0x08, 0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E):
        return {rs}, {rt}
    if op == 0x0F:
        return set(), {rt}
    if op in (0x20, 0x21, 0x23, 0x24, 0x25):
        return {rs}, {rt}
    if op in (0x22, 0x26):
        return {rs, rt}, {rt}
    if op in (0x28, 0x29, 0x2A, 0x2B, 0x2E):
        return {rs, rt}, set()
    if op == 0x03:
        return set(), {'ra'}
    if op == 0x02:
        return set(), set()
    return {rs, rt}, set()  # anything else (GTE, COP0): be conservative


def branch_kind(word):
    """'always', 'never' or 'maybe' for a conditional-branch word, using only static facts (same registers, $zero)."""
    op = word >> 26
    rs, rt = (word >> 21) & 31, (word >> 16) & 31
    if op == 0x04:  # beq
        return 'always' if rs == rt else 'maybe'
    if op == 0x05:  # bne
        return 'never' if rs == rt else 'maybe'
    if rs == 0:  # comparisons of $zero against zero
        if op == 0x06:   # blez
            return 'always'
        if op == 0x07:   # bgtz
            return 'never'
        if op == 0x01:   # regimm: bltz / bgez (and the linking forms)
            return 'always' if (rt & 1) else 'never'
    return 'maybe'


def analyze(block):
    """Returns the set of non-interpreter registers some path reads before any write."""
    n = len(block)
    flagged = set()
    defined = [None] * (n + 1)
    defined[0] = OWNED
    work = [0]
    while work:
        i = work.pop()
        if i >= n:
            continue
        cur = defined[i]
        w = block[i]
        rd, wr = reg_use_def(w)
        flagged |= {r for r in rd if r not in cur}
        nxt = cur | wr
        op, fn = w >> 26, w & 63
        is_branch = op in (0x04, 0x05, 0x06, 0x07, 0x01)
        is_jump = op in (0x02, 0x03) or (op == 0 and fn in (0x08, 0x09))
        succ = []
        if is_branch or is_jump:
            slot = block[i + 1] if i + 1 < n else 0
            srd, swr = reg_use_def(slot)
            flagged |= {r for r in srd if r not in nxt}
            after = nxt | swr
            if is_branch:
                kind = branch_kind(w)
                off = struct.unpack('<h', struct.pack('<H', w & 0xFFFF))[0]
                if kind != 'never':
                    succ.append((i + 1 + off, after))
                if kind != 'always':
                    succ.append((i + 2, after))
        else:
            succ.append((i + 1, nxt))
        for j, st in succ:
            if 0 <= j < n:
                merged = st if defined[j] is None else (defined[j] & st)
                if defined[j] is None or merged != defined[j]:
                    defined[j] = merged
                    work.append(j)
    return flagged

"""Static MIPS R3000 -> C++ translator for the game executable (the "native port" step).

Reads the PS-X EXE, discovers functions, and writes C++ that performs exactly what the interpreter (src/r3000.cpp)
does, instruction by instruction, including branch delay slots and load delay slots. Anything it cannot translate
exactly (e.g. load-delay hazards that need cross-block knowledge) is left to the interpreter at run time, so the
result is always correct; it only ever gets faster.

  python tools/recomp.py <SCUS_941.54> <functions.txt> <out dir> [--parts N]

Output (nothing from the game is committed: the generated files are build artefacts, see .gitignore):
  <out>/recomp_part_K.cpp   translated functions
  <out>/recomp_table.cpp    address -> function table and statistics
"""
import os
import re
import struct
import sys
from collections import defaultdict

BASE = 0x80010000

# Functions the platform replaces by address (Platform::overrides): they must keep going through the interpreter hook.
EXCLUDE = {0x8004A2C4, 0x8004A2D8, 0x8004A364, 0x8004A5CC}


def load_exclusions(path):
    """Addresses (one per line, # comments) of functions that must stay on the interpreter, found by cb2_nativecheck."""
    out = set()
    try:
        for line in open(path):
            line = line.split("#")[0].strip()
            if line:
                out.add(int(line, 16))
    except OSError:
        pass
    return out


def sx16(v):
    return v - 0x10000 if v & 0x8000 else v


class Exe:
    def __init__(self, path):
        d = open(path, "rb").read()
        assert d[:8] == b"PS-X EXE"
        self.pc, self.gp, self.base, self.size = struct.unpack_from("<4I", d, 0x10)
        self.words = list(struct.unpack_from("<%dI" % (self.size // 4), d, 0x800))
        self.end = self.base + self.size

    def w(self, addr):
        return self.words[(addr - self.base) >> 2]

    def has(self, addr):
        return self.base <= addr < self.end and addr % 4 == 0


# ---- decoding helpers ---------------------------------------------------------------------------------------
def fields(w):
    return w >> 26, (w >> 21) & 31, (w >> 16) & 31, (w >> 11) & 31, (w >> 6) & 31, w & 63, w & 0xFFFF


def is_branch(w):
    op, rs, rt, rd, sa, fn, imm = fields(w)
    return op in (1, 4, 5, 6, 7)


def is_jump(w):
    op, rs, rt, rd, sa, fn, imm = fields(w)
    return op in (2, 3) or (op == 0 and fn in (8, 9))


def is_control(w):
    return is_branch(w) or is_jump(w)


def is_load(w):
    op, rs, rt, rd, sa, fn, imm = fields(w)
    if op in (0x20, 0x21, 0x22, 0x23, 0x24, 0x25, 0x26):
        return rt
    if op == 0x10 and rs == 0:
        return rt          # mfc0 (delayed like a load)
    if op == 0x12 and not (w & 0x02000000) and rs in (0, 2):
        return rt          # mfc2 / cfc2
    return 0


def regs_read(w):
    op, rs, rt, rd, sa, fn, imm = fields(w)
    s = set()
    if op == 0:
        if fn in (0, 2, 3):
            s = {rt}
        elif fn in (4, 6, 7):
            s = {rs, rt}
        elif fn in (8, 9, 0x11, 0x13):
            s = {rs}
        elif fn in (0x10, 0x12, 0x0C, 0x0D):
            s = set()
        else:
            s = {rs, rt}
    elif op in (2, 3, 0x0F):
        s = set()
    elif op in (4, 5):
        s = {rs, rt}
    elif op in (1, 6, 7, 8, 9, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E):
        s = {rs}
    elif op == 0x10:
        s = {rt} if rs == 4 else set()
    elif op == 0x12:
        s = {rt} if (not (w & 0x02000000) and rs in (4, 6)) else set()
    elif 0x20 <= op <= 0x26:
        s = {rs} | ({rt} if op in (0x22, 0x26) else set())   # lwl/lwr merge with rt (handled as a pair)
    elif 0x28 <= op <= 0x2E:
        s = {rs, rt}
    elif op in (0x32, 0x3A):
        s = {rs}
    s.discard(0)
    return s


def reg_written(w):
    op, rs, rt, rd, sa, fn, imm = fields(w)
    if op == 0:
        if fn in (0x0C, 0x0D, 0x08, 0x11, 0x13, 0x18, 0x19, 0x1A, 0x1B):
            return 0
        return rd
    if op == 3:
        return 31
    if op == 1:
        return 31 if (rt & 0x1E) == 0x10 else 0
    if op in (2, 4, 5, 6, 7) or 0x28 <= op <= 0x2E or op in (0x32, 0x3A):
        return 0
    if op in (8, 9, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F) or 0x20 <= op <= 0x26:
        return rt
    if op == 0x10 and rs == 0 or op == 0x12 and rs in (0, 2) and not (w & 0x02000000):
        return rt
    return 0


# ---- statement generation -----------------------------------------------------------------------------------
def W(rd, expr):
    return [] if rd == 0 else ["r[%d] = %s;" % (rd, expr)]


def simm(imm):
    return str(sx16(imm))


def load_expr(w):
    """C++ expression for the value a load-type instruction delivers (or None)."""
    op, rs, rt, rd, sa, fn, imm = fields(w)
    a = "(uint32_t)(r[%d] + (uint32_t)(%d))" % (rs, sx16(imm))
    if op == 0x20:
        return "(uint32_t)(int32_t)(int8_t)c.rd8(%s)" % a
    if op == 0x21:
        return "(uint32_t)(int32_t)(int16_t)c.rd16(%s)" % a
    if op == 0x22:
        return "c.lwl(%s, r[%d])" % (a, rt)
    if op == 0x23:
        return "c.rd32(%s)" % a
    if op == 0x24:
        return "c.rd8(%s)" % a
    if op == 0x25:
        return "c.rd16(%s)" % a
    if op == 0x26:
        return "c.lwr(%s, r[%d])" % (a, rt)
    if op == 0x10 and rs == 0:
        return "c.cop0[%d]" % rd
    if op == 0x12 and rs == 0:
        return "c.gteReadData(%d)" % rd
    if op == 0x12 and rs == 2:
        return "c.gteReadCtrl(%d)" % rd
    return None


def plain_stmts(w, addr):
    """Statements for a non-control, non-load instruction. Returns None when the instruction is not supported."""
    op, rs, rt, rd, sa, fn, imm = fields(w)
    a = "(uint32_t)(r[%d] + (uint32_t)(%d))" % (rs, sx16(imm))
    if op == 0:
        t = {
            0x00: "r[%d] << %d" % (rt, sa), 0x02: "r[%d] >> %d" % (rt, sa),
            0x03: "(uint32_t)((int32_t)r[%d] >> %d)" % (rt, sa),
            0x04: "r[%d] << (r[%d] & 31)" % (rt, rs), 0x06: "r[%d] >> (r[%d] & 31)" % (rt, rs),
            0x07: "(uint32_t)((int32_t)r[%d] >> (r[%d] & 31))" % (rt, rs),
            0x20: "r[%d] + r[%d]" % (rs, rt), 0x21: "r[%d] + r[%d]" % (rs, rt),
            0x22: "r[%d] - r[%d]" % (rs, rt), 0x23: "r[%d] - r[%d]" % (rs, rt),
            0x24: "r[%d] & r[%d]" % (rs, rt), 0x25: "r[%d] | r[%d]" % (rs, rt),
            0x26: "r[%d] ^ r[%d]" % (rs, rt), 0x27: "~(r[%d] | r[%d])" % (rs, rt),
            0x2A: "(int32_t)r[%d] < (int32_t)r[%d] ? 1u : 0u" % (rs, rt),
            0x2B: "r[%d] < r[%d] ? 1u : 0u" % (rs, rt),
        }
        if fn in t:
            return W(rd, t[fn])
        if fn == 0x10:
            return W(rd, "c.hi")
        if fn == 0x12:
            return W(rd, "c.lo")
        if fn == 0x11:
            return ["c.hi = r[%d];" % rs]
        if fn == 0x13:
            return ["c.lo = r[%d];" % rs]
        if fn == 0x18:
            return ["c.mult(r[%d], r[%d]);" % (rs, rt)]
        if fn == 0x19:
            return ["c.multu(r[%d], r[%d]);" % (rs, rt)]
        if fn == 0x1A:
            return ["c.div(r[%d], r[%d]);" % (rs, rt)]
        if fn == 0x1B:
            return ["c.divu(r[%d], r[%d]);" % (rs, rt)]
        if fn == 0x0C:
            return ["P.nativeSyscall(0x%X, 0x%08X);" % ((w >> 6) & 0xFFFFF, addr)]
        if fn == 0x0D:
            return ["P.nativeBreak(0x%X, 0x%08X);" % ((w >> 6) & 0xFFFFF, addr)]
        return None
    if op == 0x08 or op == 0x09:
        return W(rt, "r[%d] + (uint32_t)(%d)" % (rs, sx16(imm)))
    if op == 0x0A:
        return W(rt, "(int32_t)r[%d] < %d ? 1u : 0u" % (rs, sx16(imm)))
    if op == 0x0B:
        return W(rt, "r[%d] < (uint32_t)(%d) ? 1u : 0u" % (rs, sx16(imm)))
    if op == 0x0C:
        return W(rt, "r[%d] & 0x%X" % (rs, imm))
    if op == 0x0D:
        return W(rt, "r[%d] | 0x%X" % (rs, imm))
    if op == 0x0E:
        return W(rt, "r[%d] ^ 0x%X" % (rs, imm))
    if op == 0x0F:
        return W(rt, "0x%Xu" % (imm << 16))
    if op == 0x10:
        if rs == 4:
            return ["c.cop0[%d] = r[%d];" % (rd, rt)]
        if rs == 0x10 and fn == 0x10:
            return ["c.cop0[12] = (c.cop0[12] & ~0xFu) | ((c.cop0[12] >> 2) & 0xFu);"]
        return None
    if op == 0x12:
        if w & 0x02000000:
            return ["c.gteCommand(0x%X);" % (w & 0x1FFFFFF)]
        if rs == 4:
            return ["c.gteWriteData(%d, r[%d]);" % (rd, rt)]
        if rs == 6:
            return ["c.gteWriteCtrl(%d, r[%d]);" % (rd, rt)]
        return None
    if op == 0x28:
        return ["c.wr8(%s, r[%d]);" % (a, rt)]
    if op == 0x29:
        return ["c.wr16(%s, r[%d]);" % (a, rt)]
    if op == 0x2A:
        return ["c.swl(%s, r[%d]);" % (a, rt)]
    if op == 0x2B:
        return ["c.wr32(%s, r[%d]);" % (a, rt)]
    if op == 0x2E:
        return ["c.swr(%s, r[%d]);" % (a, rt)]
    if op == 0x32:
        return ["c.gteWriteData(%d, c.rd32(%s));" % (rt, a)]
    if op == 0x3A:
        return ["c.wr32(%s, c.gteReadData(%d));" % (a, rt)]
    return None


def stmts_with_load(w, addr):
    """Statements for any non-control instruction, loads committing immediately (hazards are handled by the caller)."""
    e = load_expr(w)
    if e is not None:
        t = fields(w)[2]
        return ["(void)(%s);" % e] if t == 0 else ["r[%d] = %s;" % (t, e)]
    return plain_stmts(w, addr)

class Unsupported(Exception):
    pass


# ---- function discovery -------------------------------------------------------------------------------------
def discover(exe, fn_list_path):
    entries = set()
    code_start, code_end = 0xFFFFFFFF, 0
    for line in open(fn_list_path):
        m = re.match(r"\s*0x([0-9a-fA-F]+)\s+\d+\s+(\d+)(?:\s+->\s+(\d+))?", line)
        if not m:
            continue
        a, size = int(m.group(1), 16), int(m.group(2))
        entries.add(a)
        code_start = min(code_start, a)
        code_end = max(code_end, a + int(m.group(3) or size))
    code_end = (code_end + 3) & ~3
    return sorted(entries), code_start, code_end


def build_ranges(exe, entries, code_start, code_end):
    entries = set(entries)
    # jal targets inside the code region
    for a in range(code_start, code_end, 4):
        w = exe.w(a)
        if w >> 26 == 3:
            t = ((w & 0x3FFFFFF) << 2) | 0x80000000
            if code_start <= t < code_end:
                entries.add(t)
    # code pointers stored in data (callbacks, jump tables)
    data_ptrs = set()
    for a in range(exe.base, exe.end, 4):
        if code_start <= a < code_end:
            continue
        v = exe.w(a)
        if code_start <= v < code_end and v % 4 == 0:
            data_ptrs.add(v)
    # lui/addiu|ori pairs that build code addresses (function pointers passed as arguments)
    lui = {}
    const_ptrs = set()
    for a in range(code_start, code_end, 4):
        w = exe.w(a)
        op, rs, rt, rd, sa, fn, imm = fields(w)
        if op == 0x0F:
            lui[rt] = imm << 16
        elif op in (0x09, 0x0D) and rs in lui and rt == rs:
            v = (lui[rs] + (sx16(imm) if op == 0x09 else imm)) & 0xFFFFFFFF
            if code_start <= v < code_end and v % 4 == 0:
                const_ptrs.add(v)
        elif rt in lui and op not in (0x2B, 0x28, 0x29):
            lui.pop(rt, None)
    ents = sorted(entries)
    fn_of = lambda a: max((e for e in ents if e <= a), default=None)
    import bisect
    leaders_extra = defaultdict(set)
    for p in sorted(data_ptrs | const_ptrs):
        i = bisect.bisect_right(ents, p) - 1
        if ents[i] == p:
            continue
        leaders_extra[ents[i]].add(p)
    ranges = []
    for i, e in enumerate(ents):
        end = ents[i + 1] if i + 1 < len(ents) else code_end
        ranges.append((e, end))
    return ranges, leaders_extra, data_ptrs | const_ptrs


def find_tables(exe, code_start, code_end):
    """Jump/pointer tables: runs of >= 4 consecutive data words that are all code addresses."""
    tables = []
    a = exe.base
    while a < exe.end:
        if code_start <= a < code_end:
            a += 4
            continue
        b = a
        while b < exe.end and not (code_start <= b < code_end) and code_start <= exe.w(b) < code_end and exe.w(b) % 4 == 0:
            b += 4
        if (b - a) >= 16:
            tables.append((a, b))
            a = b
        else:
            a += 4
    return tables


def merge_units(exe, ranges, tables):
    """Group ranges into translation units. Control that leaves a range without a call (`j`, branches, fall-through, `jr`
    through a jump table) is an edge between ranges; strongly connected components (code that really passes control back
    and forth, such as an interpreter loop and its opcode handlers) become one C++ function with several entry points.
    One-way edges stay separate: they are emitted as tail calls."""
    import bisect
    starts = [s for s, _ in ranges]

    def rng(addr):
        i = bisect.bisect_right(starts, addr) - 1
        return i if 0 <= i < len(ranges) and ranges[i][0] <= addr < ranges[i][1] else None

    code_lo, code_hi = ranges[0][0], ranges[-1][1]
    table_of = lambda v: next(((s, e) for s, e in tables if s <= v < e), None)
    edges = defaultdict(set)
    for i, (s, e) in enumerate(ranges):
        words = [exe.w(a) for a in range(s, e, 4)]
        lui = {}
        loaded = {}
        for k, w in enumerate(words):
            a = s + 4 * k
            op, rs, rt, rd, sa, fn, imm = fields(w)
            if op == 0x0F:
                lui[rt] = imm << 16
            elif op == 0x23 and rs in lui:
                tb = table_of((lui[rs] + sx16(imm)) & 0xFFFFFFFF)
                if tb:
                    loaded[rt] = (tb, k)
            if op == 0 and fn == 8 and rs != 31 and rs in loaded and k - loaded[rs][1] <= 4:
                tb = loaded[rs][0]
                for q in range(tb[0], tb[1], 4):
                    j = rng(exe.w(q))
                    if j is not None and j != i:
                        edges[i].add(j)
            if op in (2, 1, 4, 5, 6, 7):
                t = (((a + 4) & 0xF0000000) | ((w & 0x3FFFFFF) << 2)) if op == 2 else a + 4 + sx16(imm) * 4
                if not (s <= t < e) and code_lo <= t < code_hi:
                    j = rng(t)
                    if j is not None:
                        edges[i].add(j)
        if i + 1 < len(ranges):
            L = len(words) - 1
            while L > 0 and words[L] == 0:          # skip alignment padding
                L -= 1

            def ends(k):
                kop, _, _, _, _, kfn, _ = fields(words[k])
                return kop == 2 or (kop == 0 and kfn == 8)
            if not (ends(L) or (L >= 1 and ends(L - 1))):
                edges[i].add(i + 1)
    # Tarjan's strongly connected components (iterative)
    index, low, onstack, stack, comp = {}, {}, set(), [], {}
    counter = [0]
    ncomp = [0]
    for root in range(len(ranges)):
        if root in index:
            continue
        work = [(root, iter(sorted(edges[root])))]
        index[root] = low[root] = counter[0]; counter[0] += 1
        stack.append(root); onstack.add(root)
        while work:
            v, it = work[-1]
            advanced = False
            for w_ in it:
                if w_ not in index:
                    index[w_] = low[w_] = counter[0]; counter[0] += 1
                    stack.append(w_); onstack.add(w_)
                    work.append((w_, iter(sorted(edges[w_]))))
                    advanced = True
                    break
                elif w_ in onstack:
                    low[v] = min(low[v], index[w_])
            if advanced:
                continue
            work.pop()
            if work:
                low[work[-1][0]] = min(low[work[-1][0]], low[v])
            if low[v] == index[v]:
                while True:
                    w_ = stack.pop(); onstack.discard(w_)
                    comp[w_] = ncomp[0]
                    if w_ == v:
                        break
                ncomp[0] += 1
    groups = defaultdict(list)
    for i in range(len(ranges)):
        groups[comp[i]].append(i)
    out = []
    for members in sorted(groups.values(), key=lambda m: min(m)):
        members.sort()
        out.append(([ranges[m] for m in members], [ranges[m][0] for m in members]))
    return out

# ---- code generation ----------------------------------------------------------------------------------------
class Gen:
    def __init__(self, exe, ranges, extra, ptrs):
        self.exe, self.ranges, self.extra = exe, ranges, extra
        self.entry_set = {a for a, _ in ranges}
        self.native = set(self.entry_set)
        self.stats = defaultdict(int)

    def target_of(self, w, addr):
        op, rs, rt, rd, sa, fn, imm = fields(w)
        if op in (2, 3):
            return ((addr + 4) & 0xF0000000) | ((w & 0x3FFFFFF) << 2)
        return addr + 4 + sx16(imm) * 4

    def gen_function(self, segments, entries):
        """One translation unit: `segments` is a list of (start, end) code ranges that form one C++ function with the
        given entry points; all labels are shared, so control passes freely between the segments."""
        exe = self.exe
        first = segments[0][0]
        inside = lambda a: a % 4 == 0 and any(s <= a < e for s, e in segments)
        seg_words = {s: [exe.w(a) for a in range(s, e, 4)] for s, e in segments}
        leaders = set(entries) | {s for s, _ in segments}
        slots = set()
        for s, e in segments:
            words = seg_words[s]
            n = len(words)
            for i, w in enumerate(words):
                a = s + 4 * i
                if is_control(w):
                    if i + 1 < n:
                        slots.add(a + 4)
                    if i + 2 < n:
                        leaders.add(a + 8)
                    if is_branch(w) or fields(w)[0] in (2, 3):
                        t = self.target_of(w, a)
                        if inside(t):
                            leaders.add(t)
            for p in self.extra.get(s, ()):
                if inside(p):
                    leaders.add(p)
        out = []
        emit = out.append
        hazards = set()
        for s, e in segments:
            words = seg_words[s]
            n = len(words)
            for i, w in enumerate(words):
                rt = is_load(w)
                if not rt:
                    continue
                a = s + 4 * i
                if a in slots or i + 1 >= n:
                    continue
                nxt = words[i + 1]
                fop = fields(nxt)[0]
                pair = fop in (0x22, 0x26) and fields(nxt)[2] == rt and fields(w)[0] in (0x22, 0x26)
                if rt in regs_read(nxt) and not pair:
                    if is_control(nxt) or (a + 4) in leaders:
                        raise Unsupported("load hazard at %08x with a control/leader successor" % a)
                    hazards.add(a)
            for i, w in enumerate(words):
                a = s + 4 * i
                if a in slots and is_load(w) and i >= 1:
                    br = words[i - 1]
                    rt = is_load(w)
                    succ = []
                    if is_branch(br):
                        succ += [a + 4, self.target_of(br, a - 4)]
                    elif fields(br)[0] == 2:
                        succ += [self.target_of(br, a - 4)]
                    elif fields(br)[0] == 3:
                        succ += []
                    else:
                        succ += [a + 4]
                    for t in succ:
                        if exe.has(t) and rt in regs_read(exe.w(t)) and fields(exe.w(t))[0] not in (0x22, 0x26):
                            raise Unsupported("load-in-delay-slot hazard at %08x" % a)
        for s, e in segments:
            words = seg_words[s]
            for i, w in enumerate(words):
                op, rs, rt, rd, sa, fn, imm = fields(w)
                if op == 0x23 and rt == 31 and rs != 29:
                    # `lw ra, off(base)` with a base other than the stack: a computed jump through ra, not a return.
                    # The original return address is lost, so the target continues in the caller's own code: a
                    # non-local exit that C++ call frames cannot express. Such code stays on the interpreter.
                    raise Unsupported("computed return address at %08x" % (s + 4 * i))
        emit("void u_%08x(psx::Platform& P, uint32_t entry) {" % first)
        emit("    r3000::Cpu& c = P.cpu; uint32_t* r = c.r; (void)r;")
        emit("    const uint32_t entryRa = r[31]; (void)entryRa;")
        if len(entries) > 1:
            emit("    switch (entry) {")
            for e_ in entries:
                if e_ != first:
                    emit("      case 0x%08xu: goto L_%08x;" % (e_, e_))
            emit("      default: break; }")
        else:
            emit("    (void)entry;")
        stub = []

        def cond_expr(w):
            op, rs, rt, rd, sa, fn, imm = fields(w)
            if op == 4:
                return "r[%d] == r[%d]" % (rs, rt)
            if op == 5:
                return "r[%d] != r[%d]" % (rs, rt)
            if op == 6:
                return "(int32_t)r[%d] <= 0" % rs
            if op == 7:
                return "(int32_t)r[%d] > 0" % rs
            ge = rt & 1
            return ("(int32_t)r[%d] >= 0" if ge else "(int32_t)r[%d] < 0") % rs

        for start, end in segments:
            words = seg_words[start]
            n = len(words)

            def block_len(i0):
                j = i0
                while j < n:
                    if j > i0 and (start + 4 * j) in leaders:
                        break
                    if is_control(words[j]):
                        j += 2
                        break
                    j += 1
                return min(j, n) - i0

            def slot_code(si):
                sw = words[si]
                if is_control(sw):
                    raise Unsupported("branch in delay slot at %08x" % (start + 4 * si))
                s_ = stmts_with_load(sw, start + 4 * si)
                if s_ is None:
                    raise Unsupported("unsupported instruction %08x at %08x" % (sw, start + 4 * si))
                return s_

            i = 0
            while i < n:
                a = start + 4 * i
                w = words[i]
                if a in leaders:
                    emit("L_%08x: TICK(0x%08xu, %d);" % (a, a, block_len(i)))
                if i == 0 and a not in leaders:
                    emit("    TICK(0x%08xu, %d);" % (a, block_len(i)))
                if is_control(w):
                    op, rs, rt, rd, sa, fn, imm = fields(w)
                    if i + 1 >= n:
                        raise Unsupported("branch at the end of a range")
                    sl = slot_code(i + 1)
                    if is_branch(w):
                        t = self.target_of(w, a)
                        link = op == 1 and (rt & 0x1E) == 0x10
                        emit("    { const bool taken = %s;" % cond_expr(w))
                        if link:
                            emit("      r[31] = 0x%08xu;" % (a + 8))
                        for s_ in sl:
                            emit("      " + s_)
                        if inside(t):
                            emit("      if (taken) goto L_%08x; }" % t)
                        else:
                            emit("      if (taken) { P.tail(0x%08xu); return; } }" % t)
                    elif op == 2:
                        t = self.target_of(w, a)
                        for s_ in sl:
                            emit("    " + s_)
                        if inside(t):
                            emit("    goto L_%08x;" % t)
                        else:
                            emit("    P.tail(0x%08xu); return;" % t)
                    elif op == 3:
                        t = self.target_of(w, a)
                        emit("    r[31] = 0x%08xu;" % (a + 8))
                        for s_ in sl:
                            emit("    " + s_)
                        if t in self.native:
                            emit("    f_%08x(P);" % t)
                        else:
                            emit("    P.call(0x%08xu);" % t)
                    elif fn == 8:  # jr
                        emit("    { const uint32_t t = r[%d];" % rs)
                        for s_ in sl:
                            emit("      " + s_)
                        if rs == 31:
                            emit("      CB2_RETURN(t); }")
                        else:
                            emit("      switch (t) {")
                            for x in sorted(leaders):
                                emit("        case 0x%08xu: goto L_%08x;" % (x, x))
                            emit("        default: P.tail(t); return; } }")
                    else:  # jalr
                        emit("    { const uint32_t t = r[%d];" % rs)
                        if rd:
                            emit("      r[%d] = 0x%08xu;" % (rd, a + 8))
                        for s_ in sl:
                            emit("      " + s_)
                        emit("      P.call(t); }")
                    if (a + 4) in leaders:
                        stub.append((a + 4, a + 8, words[i + 1]))
                    i += 2
                    continue
                lv = is_load(w)
                if lv and (a in hazards):
                    nxt = words[i + 1]
                    emit("    { const uint32_t ld = %s;" % load_expr(w))
                    s2 = stmts_with_load(nxt, a + 4)
                    if s2 is None:
                        raise Unsupported("unsupported instruction %08x at %08x" % (nxt, a + 4))
                    for s_ in s2:
                        emit("      " + s_)
                    emit("      r[%d] = ld; }" % lv if reg_written(nxt) != lv else "      }")
                    i += 2
                    continue
                s_ = stmts_with_load(w, a)
                if s_ is None:
                    emit("    P.nativeInvalid(0x%08xu, 0x%08xu);" % (a, w))
                else:
                    for x in s_:
                        emit("    " + x)
                i += 1
            if inside(end):
                emit("    goto L_%08x;" % end)         # falls through into the next segment of the unit
            else:
                emit("    P.tail(0x%08xu); return;" % end)
        for (la, nxt, sw) in stub:
            emit("L_%08x:" % la)
            for s_ in stmts_with_load(sw, la) or []:
                emit("    " + s_)
            emit("    goto L_%08x;" % nxt if inside(nxt) else "    P.tail(0x%08xu); return;" % nxt)
        emit("}")
        return "\n".join(out)

def main():
    exe_path, fn_list, outdir = sys.argv[1], sys.argv[2], sys.argv[3]
    parts = 16
    if "--parts" in sys.argv:
        parts = int(sys.argv[sys.argv.index("--parts") + 1])
    exe = Exe(exe_path)
    entries, cs, ce = discover(exe, fn_list)
    ranges, extra, ptrs = build_ranges(exe, entries, cs, ce)
    tables = find_tables(exe, cs, ce)
    units = merge_units(exe, ranges, tables)
    g = Gen(exe, ranges, extra, ptrs)
    here = os.path.dirname(os.path.abspath(__file__))
    excluded = set(EXCLUDE) | load_exclusions(os.path.join(here, "recomp_exclude.txt"))
    done_units, skipped = [], []
    unit_of_entry_all = {x: i for i, (segs, ents) in enumerate(units) for x in ents}
    bad_units = set()
    for i, (segs, ents) in enumerate(units):    # pass 1: which units can be translated at all
        if any(x in excluded for x in ents):
            skipped.append((segs[0][0], "excluded"))
            bad_units.add(i)
            continue
        try:
            g.gen_function(segs, ents)
        except Unsupported as ex:
            skipped.append((segs[0][0], str(ex)))
            bad_units.add(i)
    # A unit that exits non-locally (computed return) continues in its caller's code, so its direct callers stay on the
    # interpreter as well.
    nonlocal_units = {i for i in bad_units if any("computed return" in why for s, why in skipped if s == units[i][0][0][0])}
    for i, (segs, ents) in enumerate(units):
        if i in bad_units:
            continue
        for s, e in segs:
            for a in range(s, e, 4):
                w = exe.w(a)
                if w >> 26 == 3:
                    t = ((w & 0x3FFFFFF) << 2) | 0x80000000
                    if unit_of_entry_all.get(t) in nonlocal_units:
                        bad_units.add(i)
                        skipped.append((segs[0][0], "calls the non-local-exit function %08x" % t))
                        break
            if i in bad_units:
                break
    done_units = [u for i, u in enumerate(units) if i not in bad_units]
    g.native = {x for (_, ents) in done_units for x in ents}
    bodies = []
    for (segs, ents) in done_units:             # pass 2: emit, calling only translated entries directly
        bodies.append((segs[0][0], len(segs), sum(e - s for s, e in segs) // 4, g.gen_function(segs, ents)))
    os.makedirs(outdir, exist_ok=True)
    head = '#include "recomp_rt.hpp"\n#include "psx.hpp"\nnamespace recomp {\n'
    all_entries = sorted(g.native)
    unit_of_entry = {x: segs[0][0] for (segs, ents) in done_units for x in ents}
    decl = "".join("void u_%08x(psx::Platform&, uint32_t);\n" % segs[0][0] for (segs, _) in done_units)
    decl += "".join("inline void f_%08x(psx::Platform& P) { u_%08x(P, 0x%08xu); }\n" % (x, unit_of_entry[x], x) for x in all_entries)
    with open(os.path.join(outdir, "recomp_decl.hpp"), "w") as f:
        f.write("#pragma once\n#include <cstdint>\nnamespace psx { class Platform; }\nnamespace recomp {\n" + decl + "}\n")
    # spread units over the parts by size so that the largest ones do not end up together
    bodies.sort(key=lambda b: -b[2])
    buckets = [[] for _ in range(parts)]
    load = [0] * parts
    for b in bodies:
        k = load.index(min(load))
        buckets[k].append(b)
        load[k] += b[2]
    for k in range(parts):
        with open(os.path.join(outdir, "recomp_part_%d.cpp" % k), "w") as f:
            f.write('#include "recomp_decl.hpp"\n' + head)
            for s, nseg, words, b in buckets[k]:
                f.write(b + "\n")
            f.write("}\n")
    with open(os.path.join(outdir, "recomp_table.cpp"), "w") as f:
        f.write('#include "recomp_decl.hpp"\n#include "recomp_rt.hpp"\nnamespace recomp {\nconst Entry kFunctions[] = {\n')
        for x in all_entries:
            f.write("  {0x%08xu, &f_%08x},\n" % (x, x))
        f.write("  {0, nullptr}};\nconst unsigned kFunctionCount = %d;\n}\n" % len(all_entries))
    leaders_all = sorted({int(m.group(1), 16) for _, _, _, b in bodies for m in re.finditer(r"^L_([0-9a-f]{8}): TICK", b, re.M)})
    with open(os.path.join(outdir, "recomp_leaders.cpp"), "w") as f:
        f.write('#include "recomp_rt.hpp"\nnamespace recomp {\nconst uint32_t kLeaders[] = {\n')
        for k in range(0, len(leaders_all), 8):
            f.write("  " + ", ".join("0x%08xu" % x for x in leaders_all[k:k + 8]) + ",\n")
        f.write("  0};\nconst unsigned kLeaderCount = %d;\n}\n" % len(leaders_all))
    covered = sum(sum(e - s for s, e in segs) // 4 for segs, _ in done_units)
    total = sum((e - s) // 4 for s, e in ranges)
    merged = [(sum(e - s for s, e in segs) // 4, segs[0][0], len(ents)) for segs, ents in units if len(ents) > 1]
    print("units: %d (%d merged from several functions); %d entry points translated, %d units left to the interpreter; "
          "words translated %d / %d (%.1f%%)" % (len(units), len(merged), len(all_entries), len(skipped),
                                              covered, total, 100.0 * covered / total))
    print("merged units (words, start, functions):", [(w, "%08x" % s, n) for w, s, n in sorted(merged)[-8:]])
    for s, why in skipped:
        print("  skipped unit %08x: %s" % (s, why))


if __name__ == "__main__":
    main()



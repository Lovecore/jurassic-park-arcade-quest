#!/usr/bin/env python3
"""R1: function discovery and control-flow graph for the JP2015 game binary.

Recursive descent from every named function, then from every direct call /
tail-call target, jump-table entry and (verified) code pointer found in
data. Produces the complete function list with basic blocks, which the
lifter (R2) consumes.

  JP2015_GAME=<dir with game + symbols_original.txt> \
      python tools/recomp/discover.py [-o tools/recomp/out/functions.json]

Imports: PLT stubs (`jmp [GOT]`) are named from vr/q0/got_fixups.txt.
"""
import argparse, bisect, collections, json, os, struct, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import ehframe
from capstone import Cs, CS_ARCH_X86, CS_MODE_32
from capstone import x86_const as X

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.join(HERE, "..", "..")
_ap = argparse.ArgumentParser()
_ap.add_argument("-o", default=None, help="output json (default out/functions.json or out/<so>.functions.json)")
_ap.add_argument("--so", help="lift a shared library instead of the game")
_ap.add_argument("--base", type=lambda s: int(s, 16), default=0, help="load base of --so (hex)")
ARGS = _ap.parse_args()

if ARGS.so:
    # ---- shared library at its arcade base: addresses are base + vaddr ----
    import elfmod
    MOD = elfmod.Mod(ARGS.so, ARGS.base)
    data = MOD.data
    segs = [(ARGS.base + va, off, fs, ms, fl) for va, off, fs, ms, fl in MOD.segs]
    e_phoff, = struct.unpack_from("<I", data, 28)
    e_phentsize, e_phnum = struct.unpack_from("<HH", data, 42)
    # functions: defined FUNC / IFUNC dynamic symbols (IFUNC resolvers too)
    syms = sorted({(ARGS.base + v, s, n) for n, v, s, info, sh in MOD.syms
                   if sh and (info & 0xf) in (2, 10) and v})
    ex = next(s for s in segs if s[4] & 1)
    CODE_LO, CODE_HI = ex[0], ex[0] + ex[2]
else:
    MOD = None
    BASE = os.environ.get("JP2015_GAME", os.path.join(ROOT, "jp_game"))
    data = open(os.path.join(BASE, "game"), "rb").read()
    e_phoff, = struct.unpack_from("<I", data, 28)
    e_phentsize, e_phnum = struct.unpack_from("<HH", data, 42)
    segs = []
    for i in range(e_phnum):
        t, off, va, pa, fs, ms, fl, al = struct.unpack_from("<8I", data, e_phoff + i * e_phentsize)
        if t == 1:
            segs.append((va, off, fs, ms, fl))
    syms = []
    for line in open(os.path.join(BASE, "symbols_original.txt")):
        p = line.split(None, 2)
        if len(p) == 3:
            try:
                syms.append((int(p[0], 16), int(p[1]), p[2].strip()))
            except ValueError:
                pass
    CODE_LO = next(s[0] for s in segs if s[4] & 1)
    CODE_HI = next(a for a, s, n in syms if n == "_IO_stdin_used")   # .rodata starts here

def v2o(v):
    for va, off, fs, ms, fl in segs:
        if va <= v < va + fs:
            return off + v - va
    return None

def u32(v):
    o = v2o(v)
    return struct.unpack_from("<I", data, o)[0] if o is not None and o + 4 <= len(data) else None

def in_code(a):
    return CODE_LO <= a < CODE_HI

# PIC libraries address their GOT through ebx = DT_PLTGOT
GOT = (ARGS.base + MOD.dyn[3]) & 0xffffffff if MOD and 3 in MOD.dyn else None

# ---- imports: PLT stubs -> GOT slot -> name (game only: absolute
# `jmp [GOT]`; a library's PIC stubs `jmp [ebx+n]` are lifted as functions
# whose indirect jump goes through the runtime) ----
got_names = {}
if not MOD:
    for line in open(os.path.join(ROOT, "vr", "q0", "got_fixups.txt")):
        p = line.split()
        if len(p) >= 2:
            got_names[int(p[0], 16)] = p[1]

def plt_slot(a):
    if MOD:
        return None
    o = v2o(a)
    if o is not None and data[o:o + 2] == bytes((0xff, 0x25)):
        return struct.unpack_from("<I", data, o + 2)[0]
    return None

NORETURN = {"exit", "_exit", "abort", "__stack_chk_fail", "__assert_fail", "longjmp",
            "siglongjmp", "__longjmp_chk", "__cxa_throw", "__cxa_rethrow",
            "_Unwind_Resume", "pthread_exit", "__fortify_fail", "err", "errx",
            "__cxa_bad_cast", "__cxa_bad_typeid", "__cxa_pure_virtual",
            "_ZSt9terminatev", "_ZSt17__throw_bad_allocv",
            "_ZSt19__throw_logic_errorPKc", "_ZSt20__throw_length_errorPKc",
            "_ZSt20__throw_out_of_rangePKc", "_ZSt24__throw_out_of_range_fmtPKcz",
            "_ZSt21__throw_bad_cast", "_ZSt16__throw_bad_castv",
            "_ZSt25__throw_bad_function_callv", "_ZSt9terminatev", "__chk_fail"}

named = {a: n for a, s, n in syms if in_code(a) and s > 0}
sym_sizes = {a: s for a, s, n in syms if in_code(a) and s > 0}

# Unwind info: authoritative function starts/ends + C++ landing pads.
_ph = [struct.unpack_from("<8I", data, e_phoff + i * e_phentsize) for i in range(e_phnum)]
EH_HDR = next(p[2] for p in _ph if p[0] == 0x6474e550) + (ARGS.base if MOD else 0)   # PT_GNU_EH_FRAME
FDES = {f["start"]: f for f in ehframe.fdes(data, v2o, EH_HDR) if in_code(f["start"])}
for a, f in FDES.items():
    if a not in sym_sizes or sym_sizes[a] < f["end"] - a:
        sym_sizes[a] = f["end"] - a

# The executable segment starts with the ELF header, dynsym/dynstr, relocs
# and .plt; real code starts at the first function with unwind info or a
# symbol. Data objects that sit at the end of .text are not functions.
DATA_IN_TEXT = {"_fp_hw"}
for a in [a for a, n in named.items() if n in DATA_IN_TEXT]:
    del named[a]
if MOD:
    # .text ends where the last function ends (.rodata/.eh_frame follow in
    # the same executable segment)
    CODE_HI = max([a + s for a, s in sym_sizes.items()] + [f["end"] for f in FDES.values()])
    CODE_LO = min(min(FDES), min(named))      # skip the ELF header at the base
    if 12 in MOD.dyn:                         # .init sits before .plt/.text
        CODE_LO = min(CODE_LO, ARGS.base + MOD.dyn[12])
    if 13 in MOD.dyn:                         # .fini follows .text
        _ex = next(s for s in segs if s[4] & 1)
        CODE_HI = max(CODE_HI, min(ARGS.base + MOD.dyn[13] + 0x40, _ex[0] + _ex[2]))
else:
    CODE_LO = min(min(FDES), min(named))
    import elfmod as _em0
    _gd = _em0.Mod(os.path.join(BASE, "game"), 0, "game").dyn
    if 12 in _gd:                             # _init (.init) precedes .plt/.text
        CODE_LO = min(CODE_LO, _gd[12])
FDES = {a: f for a, f in FDES.items() if in_code(a)}

md = Cs(CS_ARCH_X86, CS_MODE_32)
md.detail = True
_dec = {}
def decode(a):
    ins = _dec.get(a)
    if ins is None:
        o = v2o(a)
        ins = next(md.disasm(data[o:o + 16], a, 1), None) if o is not None else None
        _dec[a] = ins if ins is not None else False
    return ins or None

# ---- recursive descent ----
class Func:
    __slots__ = ("start", "name", "blocks", "calls", "imports", "jumptables",
                 "indirect_jmp", "indirect_call", "tail_calls", "bad", "source",
                 "landing_pads", "sweep")
    def __init__(self, start, name, source):
        self.start, self.name, self.source = start, name, source
        self.blocks = {}            # block start -> end (exclusive)
        self.calls, self.imports, self.tail_calls = set(), set(), set()
        self.jumptables = []        # (jmp addr, table addr, [targets])
        self.landing_pads = list(FDES[start]["landing_pads"]) if start in FDES else []
        self.indirect_jmp = self.indirect_call = 0
        self.bad = None
        self.sweep = False

funcs = {}
work = collections.deque()
seed_kind = {}

def add_func(a, source, name=None):
    if a in funcs or not in_code(a) or plt_slot(a) is not None:
        return
    funcs[a] = Func(a, name or named.get(a) or f"sub_{a:08x}", source)
    work.append(a)

def table_operand(op):
    if op.type == X.X86_OP_MEM and op.mem.scale == 4 and op.mem.base == 0 and op.mem.index != 0:
        return op.mem.disp & 0xffffffff
    return None

def jump_table(ins, prev=()):
    """`jmp [index*4 + table]`, or `mov reg, [index*4 + table]` followed
    (within the block) by `jmp reg` -> table entries while they point into
    code."""
    op = ins.operands[0]
    tbl = table_operand(op)
    rel = 0                     # PIC: entries are offsets from the GOT
    if tbl is None and op.type == X.X86_OP_REG:
        added_ebx = False
        for p in reversed(prev):
            if p.mnemonic == "add" and p.operands[0].type == X.X86_OP_REG and \
                    p.operands[0].reg == op.reg and p.operands[1].type == X.X86_OP_REG and \
                    p.operands[1].reg == X.X86_REG_EBX:
                added_ebx = True
                continue
            if p.mnemonic == "mov" and p.operands[0].type == X.X86_OP_REG \
                    and p.operands[0].reg == op.reg:
                o1 = p.operands[1]
                tbl = table_operand(o1)
                if tbl is None and MOD and GOT is not None and o1.type == X.X86_OP_MEM and \
                        o1.mem.base == X.X86_REG_EBX and o1.mem.index and o1.mem.scale == 4:
                    # mov r, [ebx + i*4 + d]; add r, ebx; jmp r   (ebx = GOT)
                    tbl = (GOT + o1.mem.disp) & 0xffffffff
                    rel = GOT if added_ebx else 0
                break
            if any(o.type == X.X86_OP_REG and o.reg == op.reg for o in p.operands[:1]):
                break
    if tbl is None:
        return None
    out = []
    for i in range(1024):
        v = u32(tbl + 4 * i)
        if v is not None and rel:
            v = (v + rel) & 0xffffffff
        if v is None or not in_code(v):
            break
        if i and (tbl + 4 * i) in named:      # ran into the next object
            break
        out.append(v)
    return tbl, out

NOP_LEA = {"[{r}]", "[{r} + 0]", "[{r} + eiz]", "[{r} + eiz*1]", "[{r} + eiz*1 + 0]"}

def is_padding(lo, hi):
    """Only alignment filler: zeros/int3, or nop-like instructions
    (nop, xchg ax,ax, lea r,[r+0], mov r,r)."""
    o = v2o(lo)
    if all(c in (0x00, 0xcc) for c in data[o:o + (hi - lo)]):
        return True
    a = lo
    while a < hi:
        ins = decode(a)
        if ins is None:
            return False
        m, ops = ins.mnemonic, ins.op_str
        parts = [x.strip() for x in ops.split(",", 1)]
        if m == "nop" or (m == "xchg" and ops == "ax, ax"):
            pass
        elif m == "lea" and len(parts) == 2 and \
                parts[1].replace("dword ptr ", "") in {t.format(r=parts[0]) for t in NOP_LEA}:
            pass
        elif m == "mov" and len(parts) == 2 and parts[0] == parts[1]:
            pass
        else:
            return False
        a += ins.size
    return True

def decodes_cleanly(a, n):
    """n instructions decode without invalid/privileged/odd ones."""
    for _ in range(n):
        ins = decode(a)
        if ins is None or ins.mnemonic in ("in", "out", "insb", "insd", "outsb", "outsd", "arpl",
                                           "bound", "hlt", "int1", "int3", "retf", "ljmp", "lcall",
                                           "les", "lds", "aaa", "aas", "daa", "das", "cli", "sti"):
            return False
        if ins.mnemonic == "ret" or ins.mnemonic == "jmp":
            return True
        a += ins.size
    return True

def is_inside_known(t):
    """t lies inside a named/FDE function body but is not its start: an
    immediate pointing there is data-like (e.g. a label), not a function."""
    i = bisect.bisect_right(_known_starts, t) - 1
    if i < 0:
        return False
    s = _known_starts[i]
    return s != t and t < s + sym_sizes.get(s, 0)

def explore(f):
    lim = f.start + sym_sizes[f.start] if f.start in sym_sizes else None
    todo, seen = [f.start] + f.landing_pads, set()
    while todo:
        b = todo.pop()
        if b in seen:
            continue
        seen.add(b)
        a = b
        blk = []
        while True:
            ins = decode(a)
            if ins is None:
                f.bad = a
                f.blocks[b] = a
                break
            nxt = a + ins.size
            m = ins.mnemonic
            stop = False
            blk.append(ins)
            if m == "ret" or m == "hlt" or m == "ud2":
                stop = True
            elif m == "call":
                op = ins.operands[0]
                if op.type == X.X86_OP_IMM:
                    t = op.imm & 0xffffffff
                    slot = plt_slot(t)
                    if slot is not None:
                        nm = got_names.get(slot, f"got_{slot:08x}")
                        f.imports.add(nm)
                        if nm in NORETURN:
                            stop = True
                    elif t == nxt:
                        pass                        # call/pop PIC idiom
                    else:
                        f.calls.add(t)
                        add_func(t, "call")
                else:
                    f.indirect_call += 1
            elif m == "jmp":
                op = ins.operands[0]
                stop = True
                if op.type == X.X86_OP_IMM:
                    t = op.imm & 0xffffffff
                    slot = plt_slot(t)
                    if slot is not None:
                        f.imports.add(got_names.get(slot, f"got_{slot:08x}"))
                    elif t in named and t != f.start or (lim and not (f.start <= t < lim)) \
                            or (t in funcs and t != f.start):
                        f.tail_calls.add(t)
                        add_func(t, "tailcall")
                    else:
                        todo.append(t)
                else:
                    jt = jump_table(ins, blk[-8:-1])
                    if jt and jt[1]:
                        f.jumptables.append((a, jt[0], jt[1]))
                        todo.extend(jt[1])
                    else:
                        f.indirect_jmp += 1
                        if lim and not f.sweep:
                            # computed goto (GNU `goto *p`): sweep the whole
                            # function range linearly so every block exists;
                            # the lifter makes every instruction a label
                            f.sweep = True
                            s = f.start
                            while s < lim:
                                si = decode(s)
                                if si is None:
                                    break
                                if si.mnemonic in ("ret", "jmp", "hlt", "ud2") and s + si.size < lim:
                                    todo.append(s + si.size)
                                s += si.size
            elif not MOD and m in ("push", "mov") and ins.operands[-1].type == X.X86_OP_IMM and \
                    in_code(ins.operands[-1].imm & 0xffffffff) and \
                    (ins.operands[-1].imm & 0xffffffff) not in funcs:
                # absolute code address as an immediate: a function pointer
                # being stored or passed (callbacks, ops tables built in code)
                t = ins.operands[-1].imm & 0xffffffff
                if decode(t) is not None and not is_inside_known(t):
                    add_func(t, "imm")
            elif m == "lea" and MOD and GOT is not None and ins.operands[1].mem.base == X.X86_REG_EBX \
                    and not ins.operands[1].mem.index:
                # PIC code pointer: lea r, [ebx + f@GOTOFF] (IFUNC resolvers,
                # callbacks); only 16-aligned addresses inside .text
                t = (GOT + ins.operands[1].mem.disp) & 0xffffffff
                if in_code(t) and t % 16 == 0 and t not in funcs:
                    add_func(t, "gotoff")
            elif m.startswith("j") or m.startswith("loop") or m == "jecxz":
                op = ins.operands[0]
                if op.type == X.X86_OP_IMM:
                    todo.append(op.imm & 0xffffffff)
            if stop:
                f.blocks[b] = nxt
                break
            a = nxt
            if a in seen or (a in funcs and a != f.start and a != b):
                f.blocks[b] = a                     # falls into another block/function
                if a not in funcs:
                    todo.append(a)
                break

_known_starts = sorted(sym_sizes)
for a in sorted(named):
    add_func(a, "symbol")
for a in sorted(FDES):
    add_func(a, "fde")

# entry point, DT_INIT / DT_FINI and the init/fini arrays: called by the
# kernel and ld.so, never by the module itself
def _entry_seeds():
    import elfmod as _em
    m = MOD or _em.Mod(os.path.join(BASE, "game"), 0, "game")
    base = ARGS.base if MOD else 0
    out = [base + struct.unpack_from("<I", m.data, 24)[0]]
    for tag in (12, 13):
        if tag in m.dyn:
            out.append(base + m.dyn[tag])
    for arr, sz in ((25, 27), (26, 28), (32, 33)):
        if arr in m.dyn:
            for k in range(m.dyn.get(sz, 0) // 4):
                v = u32(base + m.dyn[arr] + 4 * k)
                if v not in (None, 0, 0xffffffff):
                    out.append(v if not MOD else (base + v) & 0xffffffff)
    return out
for a in _entry_seeds():
    if in_code(a):
        add_func(a, "entry")
# addresses found at run time (the runtime reports calls to code that was
# not lifted; tools/recomp/bringup.sh collects them here)
_seed_file = os.path.join(HERE, "seeds", (os.path.basename(ARGS.so) if MOD else "game") + ".txt")
if os.path.exists(_seed_file):
    for line in open(_seed_file):
        line = line.split("#")[0].strip()
        if line:
            a = int(line, 16)
            if in_code(a):
                add_func(a, "runtime")
while work:
    explore(funcs[work.popleft()])

# ---- code pointers in data: candidates that start functions ----
covered = sorted((b, e) for f in funcs.values() for b, e in f.blocks.items())
cov_starts = [b for b, e in covered]
def is_covered(a):
    i = bisect.bisect_right(cov_starts, a) - 1
    return i >= 0 and covered[i][0] <= a < covered[i][1]

ptr_seeds = 0
if MOD:
    # PIC library: code pointers in data are R_386_RELATIVE relocations
    # (vtables, function tables, IFUNC resolvers via IRELATIVE)
    import elfmod as _em
    for off, typ, sym, addend in MOD.relocs:
        if typ in (_em.R_386_RELATIVE, _em.R_386_IRELATIVE):
            v = (ARGS.base + addend) & 0xffffffff
            if in_code(v) and v not in funcs and not is_covered(v) and decode(v) is not None:
                add_func(v, "reloc")
                ptr_seeds += 1
else:
    for va, off, fs, ms, fl in segs:
        lo = max(va, CODE_HI) if fl & 1 else va
        for p in range(lo, va + fs - 3, 4):
            v = u32(p)
            if v and in_code(v) and v % 16 == 0 and v not in funcs and not is_covered(v) \
                    and not is_inside_known(v) and decodes_cleanly(v, 6):
                add_func(v, "dataptr")
                ptr_seeds += 1
while work:
    explore(funcs[work.popleft()])

# ---- orphans: functions reachable only through computed pointers (or
# dead) that sit in holes right after padding: seed at prologues ----
PROLOGUES = (bytes((0x55, 0x89, 0xe5)), bytes((0x55, 0x57, 0x56, 0x53)),
             bytes((0x57, 0x56, 0x53)), bytes((0x56, 0x53, 0x83, 0xec)),
             bytes((0x53, 0x83, 0xec)), bytes((0x83, 0xec)), bytes((0x55, 0x57, 0x56)))
CODE_START = (min(min(FDES), min(named)) if MOD else
              min(a for a in named if named[a] == "main"))    # header/.plt below this
for _round in range(4):
    covered = sorted((b, e) for f in funcs.values() for b, e in f.blocks.items())
    cov_starts = [b for b, e in covered]
    added = 0
    prev = CODE_START
    for b, e in covered + [(CODE_HI, CODE_HI)]:
        if b > prev:
            a = (prev + 15) & ~15
            while a < b:
                o = v2o(a)
                if any(data[o:o + len(p)] == p for p in PROLOGUES) and a not in funcs:
                    add_func(a, "orphan")
                    added += 1
                    break                      # explore, then rescan the rest of the hole
                if MOD and a not in funcs and is_padding(prev, a) and decodes_cleanly(a, 12):
                    # library: first real code after alignment padding (SSE
                    # string/memory variants start without a prologue)
                    add_func(a, "orphan")
                    added += 1
                    break
                a += 16
        prev = max(prev, e)
    while work:
        explore(funcs[work.popleft()])
    if not added:
        break

# ---- coverage of the code range ----
covered = sorted((b, e) for f in funcs.values() for b, e in f.blocks.items())
merged = []
for b, e in covered:
    if merged and b <= merged[-1][1]:
        merged[-1][1] = max(merged[-1][1], e)
    else:
        merged.append([b, e])
cov_bytes = sum(e - b for b, e in merged)
holes = []
prev = CODE_START
for b, e in merged + [[CODE_HI, CODE_HI]]:
    if b > prev:
        holes.append((prev, b - prev, is_padding(prev, b)))
    prev = max(prev, e)
real_holes = [h for h in holes if not h[2]]

src = collections.Counter(f.source for f in funcs.values())
bad = [f for f in funcs.values() if f.bad]
print(f"code {CODE_LO:#x}..{CODE_HI:#x} ({CODE_HI - CODE_LO} bytes)")
print(f"functions: {len(funcs)} ({', '.join(f'{k} {v}' for k, v in src.most_common())})")
print(f"FDEs {len(FDES)}, with landing pads {sum(1 for f in FDES.values() if f['landing_pads'])}, "
      f"landing pads {sum(len(f['landing_pads']) for f in FDES.values())}")
print(f"basic blocks: {sum(len(f.blocks) for f in funcs.values())}")
print(f"covered by decoded blocks: {cov_bytes} of {CODE_HI - CODE_START} code bytes from main ({100 * cov_bytes / (CODE_HI - CODE_START):.1f}%)")
print(f"holes: {len(holes)}, padding-only {len(holes) - len(real_holes)}, "
      f"non-padding {len(real_holes)} totalling {sum(h[1] for h in real_holes)} bytes")
print(f"jump tables: {len({j for f in funcs.values() for j, t, ts in f.jumptables})}, "
      f"unresolved indirect jmp: {sum(f.indirect_jmp for f in funcs.values())}, "
      f"indirect calls: {sum(f.indirect_call for f in funcs.values())}")
imps = collections.Counter(i for f in funcs.values() for i in f.imports)
print(f"imports referenced: {len(imps)}; decode failures in {len(bad)} functions")
big = sorted(real_holes, key=lambda h: -h[1])[:15]
print("largest non-padding holes:")
for a, n, _ in big:
    i = bisect.bisect_right(sorted(named), a) - 1
    print(f"  {a:#x} +{n:#x}  after {sorted(named)[i] if i >= 0 else 0:#x} "
          f"{named.get(sorted(named)[i], '') if i >= 0 else ''}  bytes {data[v2o(a):v2o(a)+12].hex()}")
for f in bad[:10]:
    print(f"  decode fail in {f.name} at {f.bad:#x}")

args = ARGS
if not args.o:
    args.o = os.path.join(HERE, "out", (os.path.basename(ARGS.so) + ".functions.json") if MOD
                          else "functions.json")
os.makedirs(os.path.dirname(args.o), exist_ok=True)
json.dump({
    "module": os.path.basename(ARGS.so) if MOD else "game",
    "path": os.path.abspath(ARGS.so) if MOD else os.path.abspath(os.path.join(BASE, "game")),
    "base": ARGS.base if MOD else 0,
    "code": [CODE_LO, CODE_HI],
    "functions": [{
        "start": f.start, "name": f.name, "source": f.source,
        "blocks": sorted([b, e] for b, e in f.blocks.items()),
        "calls": sorted(f.calls), "tail_calls": sorted(f.tail_calls),
        "imports": sorted(f.imports), "landing_pads": sorted(f.landing_pads),
        "jumptables": [[j, t, ts] for j, t, ts in f.jumptables],
        "indirect_jmp": f.indirect_jmp, "indirect_call": f.indirect_call,
        "decode_fail": f.bad, "sweep": f.sweep,
    } for f in sorted(funcs.values(), key=lambda f: f.start)],
    "imports": sorted(imps),
    "holes": [[a, n] for a, n, fl in real_holes],
}, open(args.o, "w"))
print(f"wrote {args.o}")

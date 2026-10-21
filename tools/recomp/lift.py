#!/usr/bin/env python3
"""R2: lift the JP2015 game's x86 code to C (see rt/rt.h for the model).

  JP2015_GAME=<dir> python tools/recomp/lift.py [--only ADDR,...] [--out DIR]

Inputs: tools/recomp/out/functions.json (discover.py), the game binary,
vr/q0/got_fixups.txt (import names). Output (default tools/recomp/out/c/):
  funcs.h        prototypes of every lifted function
  imports.h      import id -> name table (RT_IMPORTS)
  table.c        sorted guest-address -> function table for rt_call()
  lifted_NNN.c   generated code, ~400 functions per file
  coverage.txt   instructions not lifted yet (emitted as rt_unimpl)
"""
import argparse, collections, json, os, struct, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import lift_sse
from capstone import Cs, CS_ARCH_X86, CS_MODE_32
from capstone import x86_const as X

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.join(HERE, "..", "..")

# Current module (set by load_module): its bytes, segments at their guest
# addresses, and for the game the import names of its absolute PLT stubs.
data, segs, got_names, IS_GAME = b"", [], {}, False
MODULE_BASE, MODULE_TAG = 0, "game"


def load_module(path, base, is_game, tag="game"):
    global data, segs, got_names, IS_GAME, MODULE_BASE, MODULE_TAG
    MODULE_BASE, MODULE_TAG = base, tag
    data = open(path, "rb").read()
    e_phoff, = struct.unpack_from("<I", data, 28)
    e_phentsize, e_phnum = struct.unpack_from("<HH", data, 42)
    segs = []
    for i in range(e_phnum):
        t, off, va, pa, fs, ms, fl, al = struct.unpack_from("<8I", data, e_phoff + i * e_phentsize)
        if t == 1:
            segs.append((base + va, off, fs))
    IS_GAME = is_game
    got_names = {}
    if is_game:
        for line in open(os.path.join(ROOT, "vr", "q0", "got_fixups.txt")):
            p = line.split()
            if len(p) >= 2:
                got_names[int(p[0], 16)] = p[1]


def v2o(v):
    for va, off, fs in segs:
        if va <= v < va + fs:
            return off + v - va
    return None


def plt_slot(a):
    """Game only: absolute `jmp [GOT]` stub -> GOT slot. Library PLT stubs
    (`jmp [ebx+n]`) are lifted as ordinary functions."""
    if not IS_GAME:
        return None
    o = v2o(a)
    if o is not None and data[o:o + 2] == bytes((0xff, 0x25)):
        return struct.unpack_from("<I", data, o + 2)[0]
    return None


def stamped(slot):
    """Value of a game GOT slot as frozen in the dump (= the arcade address,
    which the arcade library layout reproduces)."""
    o = v2o(slot)
    return struct.unpack_from("<I", data, o)[0] if o is not None else None

md = Cs(CS_ARCH_X86, CS_MODE_32)
md.detail = True

REG32 = ["eax", "ecx", "edx", "ebx", "esp", "ebp", "esi", "edi"]
R16 = {"ax": "eax", "cx": "ecx", "dx": "edx", "bx": "ebx", "sp": "esp", "bp": "ebp",
       "si": "esi", "di": "edi"}
R8L = {"al": "eax", "cl": "ecx", "dl": "edx", "bl": "ebx"}
R8H = {"ah": "eax", "ch": "ecx", "dh": "edx", "bh": "ebx"}
MASK = {1: "0xffu", 2: "0xffffu", 4: "0xffffffffu"}
SBIT = {1: "0x80u", 2: "0x8000u", 4: "0x80000000u"}
UT = {1: "uint8_t", 2: "uint16_t", 4: "uint32_t", 8: "uint64_t"}
ST = {1: "int8_t", 2: "int16_t", 4: "int32_t", 8: "int64_t"}

COND = {
    "o": "of", "no": "!of", "b": "cf", "ae": "!cf", "e": "zf", "ne": "!zf",
    "be": "(cf|zf)", "a": "(!cf&&!zf)", "s": "sf", "ns": "!sf", "p": "pf", "np": "!pf",
    "l": "(sf!=of)", "ge": "(sf==of)", "le": "(zf||sf!=of)", "g": "(!zf&&sf==of)",
}


class Unsupported(Exception):
    pass


class FuncLifter:
    def __init__(self, f, funcs_by_addr, import_ids):
        self.f = f
        self.funcs = funcs_by_addr
        self.imports = import_ids
        self.out = []
        self.labels = set()
        self.unimpl = collections.Counter()
        self.resume_points = []
        self.need_dispatch = False

    # ---- operands ----
    def reg_get(self, name):
        if name in REG32:
            return name
        if name in R16:
            return f"(uint16_t){R16[name]}"
        if name in R8L:
            return f"(uint8_t){R8L[name]}"
        if name in R8H:
            return f"(uint8_t)({R8H[name]}>>8)"
        raise Unsupported(f"reg {name}")

    def reg_set(self, name, v):
        if name in REG32:
            return f"{name} = (uint32_t)({v});"
        if name in R16:
            r = R16[name]
            return f"{r} = ({r} & 0xffff0000u) | (uint16_t)({v});"
        if name in R8L:
            r = R8L[name]
            return f"{r} = ({r} & ~0xffu) | (uint8_t)({v});"
        if name in R8H:
            r = R8H[name]
            return f"{r} = ({r} & ~0xff00u) | ((uint32_t)(uint8_t)({v}) << 8);"
        raise Unsupported(f"reg {name}")

    def addr(self, ins, op):
        m = op.mem
        parts = []
        if m.base:
            parts.append(ins.reg_name(m.base))
        if m.index:
            ix = ins.reg_name(m.index)
            parts.append(f"{ix}*{m.scale}" if m.scale != 1 else ix)
        if m.disp or not parts:
            parts.append(f"0x{m.disp & 0xffffffff:x}u")
        e = " + ".join(parts)
        if m.segment == X.X86_REG_GS:
            e = f"cpu->gs_base + {e}"
        elif m.segment == X.X86_REG_FS:
            raise Unsupported("fs segment")
        for p in parts:
            if p.split("*")[0] not in REG32 and not p.startswith("0x"):
                raise Unsupported(f"addr reg {p}")
        return f"(uint32_t)({e})"

    def get(self, ins, op, size=None):
        size = size or op.size
        if op.type == X.X86_OP_REG:
            return self.reg_get(ins.reg_name(op.reg))
        if op.type == X.X86_OP_IMM:
            return f"0x{op.imm & ((1 << (8 * size)) - 1):x}u"
        if op.type == X.X86_OP_MEM:
            a = self.addr(ins, op)
            return {1: "rd8", 2: "rd16", 4: "rd32", 8: "rd64"}[size] + f"({a})"
        raise Unsupported("operand")

    def set(self, ins, op, v, size=None):
        size = size or op.size
        if op.type == X.X86_OP_REG:
            return self.reg_set(ins.reg_name(op.reg), v)
        if op.type == X.X86_OP_MEM:
            a = self.addr(ins, op)
            return {1: "wr8", 2: "wr16", 4: "wr32", 8: "wr64"}[size] + f"({a}, ({UT[size]})({v}));"
        raise Unsupported("dest operand")

    def emit(self, s):
        self.out.append("    " + s)

    def spill(self):
        return "cpu->eax=eax; cpu->ecx=ecx; cpu->edx=edx; cpu->ebx=ebx; cpu->esp=esp; cpu->ebp=ebp; cpu->esi=esi; cpu->edi=edi;"

    def reload(self):
        return "eax=cpu->eax; ecx=cpu->ecx; edx=cpu->edx; ebx=cpu->ebx; esp=cpu->esp; ebp=cpu->ebp; esi=cpu->esi; edi=cpu->edi;"

    def label(self, a):
        return f"L_{a:08x}"

    # ---- flags ----
    def flags_logic(self, r, size):
        return (f"cf = 0; of = 0; zf = ({r} & {MASK[size]}) == 0; "
                f"sf = ({r} & {SBIT[size]}) != 0; pf = rt_parity({r});")

    def flags_zsp(self, r, size):
        return f"zf = ({r} & {MASK[size]}) == 0; sf = ({r} & {SBIT[size]}) != 0; pf = rt_parity({r});"

    # ---- control-transfer helpers ----
    def call_target(self, t, nxt, tail=False):
        """Direct call (or tail jump) to guest address t."""
        push = "" if tail else f"esp -= 4; wr32(esp, 0x{nxt:x}u); RT_SHPUSH(cpu, 0x{nxt:x}u); "
        end = " return;" if tail else f" RT_SHPOP(cpu); {self.reload()}"
        slot = plt_slot(t)
        if slot is not None:
            # game import: the dump's GOT holds the arcade address, which the
            # recompiled process reproduces -> call the lifted library
            # function directly, or let the runtime dispatch (host thunks)
            name = got_names.get(slot, f"got_{slot:08x}")
            self.imports.setdefault(name, len(self.imports))
            v = stamped(slot)
            if v in self.funcs and name not in OVERRIDE_IMPORTS:
                return f"{push}{self.spill()} f_{v:08x}(cpu) /* {name} */;{end}"
            THUNK_NAMES[v] = name
            return f"{push}{self.spill()} rt_call(cpu, 0x{v:x}u) /* {name} */;{end}"
        if t in self.funcs:
            return f"{push}{self.spill()} f_{t:08x}(cpu);{end}"
        return f"{push}{self.spill()} rt_call(cpu, 0x{t:x}u);{end}"

    # ---- one instruction ----
    def lift(self, ins, nxt, local_targets):
        m = ins.mnemonic
        ops = ins.operands
        E = self.emit
        pfx = ins.prefix[0]
        rep = pfx in (X.X86_PREFIX_REP, X.X86_PREFIX_REPNE)
        locked = m.startswith("lock ") or X.X86_PREFIX_LOCK in ins.prefix
        if m.startswith("lock "):
            m = m[5:]
        if m.startswith("rep ") or m.startswith("repe ") or m.startswith("repne "):
            m = m.split()[-1]

        def sz(i=0):
            return ops[i].size

        # Atomic read-modify-write on memory (lock prefix; xchg with memory is
        # implicitly locked): guest threads run in parallel on host threads,
        # so these must be real atomics (glibc locks, refcounts, malloc).
        if ops and ops[0].type == X.X86_OP_MEM and \
                (locked or (m == "xchg")) and \
                m in ("xadd", "cmpxchg", "xchg", "add", "sub", "and", "or", "xor", "inc", "dec",
                      "bts", "btr", "btc", "neg", "not", "adc", "sbb", "cmpxchg8b"):
            if self.lift_atomic(ins, m):
                return

        # segment registers: gs carries the TLS block (set_thread_area);
        # the others are the flat Linux user selectors
        SEG = {"cs": "0x73u", "ds": "0x7bu", "es": "0x7bu", "ss": "0x7bu", "fs": "0u"}
        if m == "mov" and any(o.type == X.X86_OP_REG and ins.reg_name(o.reg) in ("gs", "fs", "cs", "ds", "es", "ss")
                              for o in ops):
            d, s = ops[0], ops[1]
            dn = ins.reg_name(d.reg) if d.type == X.X86_OP_REG else None
            sn = ins.reg_name(s.reg) if s.type == X.X86_OP_REG else None
            if dn == "gs":
                E(f"cpu->gs_sel = (uint16_t)({self.get(ins, s, 2)}); rt_load_gs(cpu);")
            elif dn in SEG:
                pass                                   # flat model: ignore
            elif sn == "gs":
                E(self.set(ins, d, "cpu->gs_sel", d.size))
            else:
                E(self.set(ins, d, SEG[sn], d.size))
            return

        # data movement
        if m == "mov":
            E(self.set(ins, ops[0], self.get(ins, ops[1])))
        elif m in ("movzx", "movsx"):
            s = sz(1)
            v = self.get(ins, ops[1])
            if m == "movsx":
                v = f"(uint32_t)(int32_t)({ST[s]})({v})"
            E(self.set(ins, ops[0], v))
        elif m == "lea":
            E(self.set(ins, ops[0], self.addr(ins, ops[1])))
        elif m == "xchg":
            a, b = self.get(ins, ops[0]), self.get(ins, ops[1])
            E(f"{{ uint32_t t0 = {a}, t1 = {b}; {self.set(ins, ops[0], 't1')} {self.set(ins, ops[1], 't0')} }}")
        elif m == "push":
            v = self.get(ins, ops[0], 4) if ops[0].type != X.X86_OP_IMM else f"0x{ops[0].imm & 0xffffffff:x}u"
            if sz() == 2:
                E(f"{{ uint32_t t = {v}; esp -= 2; wr16(esp, (uint16_t)t); }}")
            else:
                E(f"{{ uint32_t t = {v}; esp -= 4; wr32(esp, t); }}")
        elif m == "pop":
            if sz() == 2:
                E(f"{{ uint32_t t = rd16(esp); esp += 2; {self.set(ins, ops[0], 't')} }}")
            else:
                E(f"{{ uint32_t t = rd32(esp); esp += 4; {self.set(ins, ops[0], 't')} }}")
        elif m == "pushal":
            E("{ uint32_t s = esp; esp -= 32; wr32(esp+28,eax); wr32(esp+24,ecx); wr32(esp+20,edx); "
              "wr32(esp+16,ebx); wr32(esp+12,s); wr32(esp+8,ebp); wr32(esp+4,esi); wr32(esp,edi); }")
        elif m == "popal":
            E("edi=rd32(esp); esi=rd32(esp+4); ebp=rd32(esp+8); ebx=rd32(esp+16); edx=rd32(esp+20); "
              "ecx=rd32(esp+24); eax=rd32(esp+28); esp += 32;")
        elif m in ("pushfd", "pushf"):
            E("esp -= 4; wr32(esp, 0x202u | cf | (pf<<2) | (zf<<6) | (sf<<7) | (cpu->df<<10) | (of<<11) | cpu->eflags_rest);")
        elif m in ("popfd", "popf"):
            E("{ uint32_t t = rd32(esp); esp += 4; cf=t&1; pf=(t>>2)&1; zf=(t>>6)&1; sf=(t>>7)&1; "
              "cpu->df=(t>>10)&1; of=(t>>11)&1; cpu->eflags_rest = t & 0x00240000u; }")
        elif m == "lahf":
            E("eax = (eax & ~0xff00u) | ((0x02u | cf | (pf<<2) | (zf<<6) | (sf<<7)) << 8);")
        elif m == "sahf":
            E("{ uint32_t t = eax >> 8; cf=t&1; pf=(t>>2)&1; zf=(t>>6)&1; sf=(t>>7)&1; }")
        elif m == "leave":
            E("esp = ebp; ebp = rd32(esp); esp += 4;")
        elif m == "cdq":
            E("edx = (uint32_t)((int32_t)eax >> 31);")
        elif m == "cwde":
            E("eax = (uint32_t)(int32_t)(int16_t)eax;")
        elif m == "cbw":
            E("eax = (eax & 0xffff0000u) | (uint16_t)(int16_t)(int8_t)eax;")
        elif m == "cwd":
            E("edx = (edx & 0xffff0000u) | (uint16_t)(((int16_t)eax) >> 15);")
        elif m == "bswap":
            r = ins.reg_name(ops[0].reg)
            E(f"{r} = ({r}>>24) | (({r}>>8)&0xff00u) | (({r}<<8)&0xff0000u) | ({r}<<24);")
        elif m in ("nop", "fnop", "prefetchnta", "prefetcht0", "prefetcht1", "prefetcht2", "pause"):
            pass
        elif m == "cld":
            E("cpu->df = 0;")
        elif m == "std":
            E("cpu->df = 1;")
        elif m == "clc":
            E("cf = 0;")
        elif m == "stc":
            E("cf = 1;")
        elif m == "cmc":
            E("cf ^= 1;")

        # arithmetic
        elif m in ("add", "adc", "sub", "sbb", "cmp"):
            s = sz()
            M = MASK[s]
            a, b = self.get(ins, ops[0]), self.get(ins, ops[1], s)
            if ops[1].type == X.X86_OP_IMM:
                b = f"0x{ops[1].imm & ((1 << (8 * s)) - 1):x}u"
            if m in ("add", "adc"):
                c = " + cf" if m == "adc" else ""
                E(f"{{ uint32_t a = {a}, b = {b}; uint64_t w = (uint64_t)a + b{c}; uint32_t r = (uint32_t)w & {M}; "
                  f"cf = w > {M}; of = ((a ^ r) & (b ^ r) & {SBIT[s]}) != 0; {self.flags_zsp('r', s)} "
                  f"{self.set(ins, ops[0], 'r')} }}")
            else:
                c = " - cf" if m == "sbb" else ""
                cc = "(uint64_t)a < (uint64_t)b + cf" if m == "sbb" else "a < b"
                wr = "" if m == "cmp" else self.set(ins, ops[0], "r")
                E(f"{{ uint32_t a = {a}, b = {b}; uint32_t r = (a - b{c}) & {M}; cf = {cc}; "
                  f"of = ((a ^ b) & (a ^ r) & {SBIT[s]}) != 0; {self.flags_zsp('r', s)} {wr} }}")
        elif m in ("and", "or", "xor", "test"):
            s = sz()
            op = {"and": "&", "test": "&", "or": "|", "xor": "^"}[m]
            if m == "xor" and ops[0].type == X.X86_OP_REG and ops[1].type == X.X86_OP_REG \
                    and ops[0].reg == ops[1].reg:
                E(f"{self.set(ins, ops[0], '0')} cf = of = sf = 0; zf = 1; pf = 1;")
                return
            a, b = self.get(ins, ops[0]), self.get(ins, ops[1], s)
            wr = "" if m == "test" else self.set(ins, ops[0], "r")
            E(f"{{ uint32_t r = ({a} {op} {b}) & {MASK[s]}; {self.flags_logic('r', s)} {wr} }}")
        elif m in ("inc", "dec"):
            s = sz()
            d = "+" if m == "inc" else "-"
            ofc = f"r == {SBIT[s]}" if m == "inc" else f"r == ({SBIT[s]} - 1)"
            E(f"{{ uint32_t r = ({self.get(ins, ops[0])} {d} 1) & {MASK[s]}; of = {ofc}; "
              f"{self.flags_zsp('r', s)} {self.set(ins, ops[0], 'r')} }}")
        elif m == "neg":
            s = sz()
            E(f"{{ uint32_t a = {self.get(ins, ops[0])}; uint32_t r = (0u - a) & {MASK[s]}; cf = a != 0; "
              f"of = r == {SBIT[s]}; {self.flags_zsp('r', s)} {self.set(ins, ops[0], 'r')} }}")
        elif m == "not":
            E(self.set(ins, ops[0], f"~{self.get(ins, ops[0])}"))
        elif m in ("shl", "sal", "shr", "sar", "rol", "ror", "rcl", "rcr"):
            s = sz()
            bits = 8 * s
            M = MASK[s]
            cnt = "1u" if len(ops) == 1 else (f"(uint32_t)({ops[1].imm & 31})" if ops[1].type == X.X86_OP_IMM
                                               else f"({self.get(ins, ops[1])} & 31u)")
            a = self.get(ins, ops[0])
            if m in ("shl", "sal"):
                body = (f"cf = c <= {bits} ? (a >> ({bits} - c)) & 1 : 0; r = c < 32 ? (a << c) & {M} : 0; "
                        f"of = ((r & {SBIT[s]}) != 0) ^ cf; {self.flags_zsp('r', s)}")
            elif m == "shr":
                body = (f"cf = (a >> (c - 1)) & 1; r = c < 32 ? a >> c : 0; of = (a & {SBIT[s]}) != 0; "
                        f"{self.flags_zsp('r', s)}")
            elif m == "sar":
                body = (f"{{ int32_t sa = ({ST[s]})a; cf = (uint32_t)(sa >> (c - 1)) & 1; "
                        f"r = (uint32_t)(sa >> (c > 31 ? 31 : c)) & {M}; }} of = 0; {self.flags_zsp('r', s)}")
            elif m == "rol":
                body = (f"{{ uint32_t k = c % {bits}; r = k ? ((a << k) | (a >> ({bits} - k))) & {M} : a; }} "
                        f"cf = r & 1; of = ((r & {SBIT[s]}) != 0) ^ cf;")
            elif m == "ror":
                body = (f"{{ uint32_t k = c % {bits}; r = k ? ((a >> k) | (a << ({bits} - k))) & {M} : a; }} "
                        f"cf = (r & {SBIT[s]}) != 0; of = cf ^ ((r & ({SBIT[s]} >> 1)) != 0);")
            elif m == "rcl":
                body = (f"{{ uint64_t w = ((uint64_t)cf << {bits}) | a; uint32_t k = c % {bits + 1}; "
                        f"w = k ? ((w << k) | (w >> ({bits + 1} - k))) : w; r = (uint32_t)w & {M}; "
                        f"cf = (uint32_t)(w >> {bits}) & 1; of = ((r & {SBIT[s]}) != 0) ^ cf; }}")
            else:  # rcr
                body = (f"{{ uint64_t w = ((uint64_t)cf << {bits}) | a; uint32_t k = c % {bits + 1}; "
                        f"w = k ? ((w >> k) | (w << ({bits + 1} - k))) : w; r = (uint32_t)w & {M}; "
                        f"cf = (uint32_t)(w >> {bits}) & 1; of = ((r >> {bits - 1}) ^ (r >> {bits - 2})) & 1; }}")
            E(f"{{ uint32_t c = {cnt}, a = {a}, r; if (c) {{ {body} {self.set(ins, ops[0], 'r')} }} }}")
        elif m in ("shld", "shrd"):
            cnt = f"(uint32_t)({ops[2].imm & 31})" if ops[2].type == X.X86_OP_IMM else "(ecx & 31u)"
            a, b = self.get(ins, ops[0]), self.get(ins, ops[1])
            if sz() != 4:
                raise Unsupported(m + "16")
            if m == "shld":
                body = "r = (a << c) | (b >> (32 - c)); cf = (a >> (32 - c)) & 1;"
            else:
                body = "r = (a >> c) | (b << (32 - c)); cf = (a >> (c - 1)) & 1;"
            E(f"{{ uint32_t c = {cnt}, a = {a}, b = {b}, r; if (c) {{ {body} of = ((r ^ a) >> 31) & 1; "
              f"{self.flags_zsp('r', 4)} {self.set(ins, ops[0], 'r')} }} }}")
        elif m == "imul":
            if len(ops) == 1:
                s = sz()
                v = self.get(ins, ops[0])
                if s == 4:
                    E(f"{{ int64_t w = (int64_t)(int32_t)eax * (int32_t)({v}); eax = (uint32_t)w; "
                      f"edx = (uint32_t)((uint64_t)w >> 32); cf = of = w != (int32_t)w; }}")
                elif s == 2:
                    E(f"{{ int32_t w = (int16_t)eax * (int16_t)({v}); eax = (eax & 0xffff0000u) | (uint16_t)w; "
                      f"edx = (edx & 0xffff0000u) | (uint16_t)(w >> 16); cf = of = w != (int16_t)w; }}")
                else:
                    E(f"{{ int32_t w = (int8_t)eax * (int8_t)({v}); eax = (eax & 0xffff0000u) | (uint16_t)w; "
                      f"cf = of = w != (int8_t)w; }}")
            else:
                s = sz()
                a = self.get(ins, ops[1] if len(ops) == 3 else ops[0])
                b = (f"0x{ops[2].imm & 0xffffffff:x}u" if len(ops) == 3 else self.get(ins, ops[1]))
                E(f"{{ int64_t w = (int64_t)({ST[s]})({a}) * ({ST[s]})({b}); uint32_t r = (uint32_t)w & {MASK[s]}; "
                  f"cf = of = w != ({ST[s]})w; {self.set(ins, ops[0], 'r')} }}")
        elif m == "mul":
            s = sz()
            v = self.get(ins, ops[0])
            if s == 4:
                E(f"{{ uint64_t w = (uint64_t)eax * ({v}); eax = (uint32_t)w; edx = (uint32_t)(w >> 32); "
                  f"cf = of = edx != 0; }}")
            elif s == 2:
                E(f"{{ uint32_t w = (uint32_t)(uint16_t)eax * (uint16_t)({v}); eax = (eax & 0xffff0000u) | (uint16_t)w; "
                  f"edx = (edx & 0xffff0000u) | (w >> 16); cf = of = (w >> 16) != 0; }}")
            else:
                E(f"{{ uint32_t w = (uint32_t)(uint8_t)eax * (uint8_t)({v}); eax = (eax & 0xffff0000u) | (uint16_t)w; "
                  f"cf = of = (w >> 8) != 0; }}")
        elif m in ("div", "idiv"):
            s = sz()
            v = self.get(ins, ops[0])
            a = f"0x{ins.address:x}u"
            de = f"rt_trap(cpu, {a}, \"#DE\");"
            if s == 1:
                if m == "div":
                    E(f"{{ uint32_t d = (uint8_t)({v}), n = (uint16_t)eax; if (!d || n / d > 0xff) {de} "
                      f"eax = (eax & 0xffff0000u) | ((n % d) << 8) | (n / d); }}")
                else:
                    E(f"{{ int32_t d = (int8_t)({v}), n = (int16_t)eax; if (!d || n / d > 127 || n / d < -128) {de} "
                      f"eax = (eax & 0xffff0000u) | ((uint32_t)(uint8_t)(n % d) << 8) | (uint8_t)(n / d); }}")
                return
            if s == 2:
                if m == "div":
                    E(f"{{ uint32_t d = (uint16_t)({v}), n = ((edx & 0xffffu) << 16) | (eax & 0xffffu); "
                      f"if (!d || n / d > 0xffff) {de} "
                      f"eax = (eax & 0xffff0000u) | (n / d); edx = (edx & 0xffff0000u) | (n % d); }}")
                else:
                    E(f"{{ int32_t d = (int16_t)({v}), n = (int32_t)(((edx & 0xffffu) << 16) | (eax & 0xffffu)); "
                      f"if (!d || n / d > 32767 || n / d < -32768) {de} "
                      f"eax = (eax & 0xffff0000u) | (uint16_t)(n / d); edx = (edx & 0xffff0000u) | (uint16_t)(n % d); }}")
                return
            if m == "div":
                E(f"{{ uint32_t d = {v}; uint64_t n = ((uint64_t)edx << 32) | eax; "
                  f"if (!d || n / d > 0xffffffffu) rt_trap(cpu, {a}, \"#DE\"); "
                  f"eax = (uint32_t)(n / d); edx = (uint32_t)(n % d); }}")
            else:
                E(f"{{ int32_t d = (int32_t){v}; int64_t n = (int64_t)(((uint64_t)edx << 32) | eax); "
                  f"if (!d || (n == INT64_MIN && d == -1) || n / d > INT32_MAX || n / d < INT32_MIN) "
                  f"rt_trap(cpu, {a}, \"#DE\"); eax = (uint32_t)(int32_t)(n / d); edx = (uint32_t)(int32_t)(n % d); }}")
        elif m == "xadd":
            s = sz()
            a, b = self.get(ins, ops[0]), self.get(ins, ops[1])
            E(f"{{ uint32_t a = {a}, b = {b}; uint64_t w = (uint64_t)a + b; uint32_t r = (uint32_t)w & {MASK[s]}; "
              f"cf = w > {MASK[s]}; of = ((a ^ r) & (b ^ r) & {SBIT[s]}) != 0; {self.flags_zsp('r', s)} "
              f"{self.set(ins, ops[1], 'a')} {self.set(ins, ops[0], 'r')} }}")
        elif m == "cmpxchg":
            s = sz()
            acc = {4: "eax", 2: "ax", 1: "al"}[s]
            a, b = self.get(ins, ops[0]), self.get(ins, ops[1])
            E(f"{{ uint32_t d = {a}, x = {self.reg_get(acc)}; uint32_t r = (x - d) & {MASK[s]}; zf = r == 0; "
              f"cf = x < d; sf = (r & {SBIT[s]}) != 0; of = ((x ^ d) & (x ^ r) & {SBIT[s]}) != 0; pf = rt_parity(r); "
              f"if (zf) {{ {self.set(ins, ops[0], b)} }} else {{ {self.reg_set(acc, 'd')} }} }}")
        elif m in ("bt", "bts", "btr", "btc") and ops[0].type == X.X86_OP_MEM \
                and ops[1].type == X.X86_OP_REG:
            # bit string: the register is a signed bit offset from the operand
            a = self.addr(ins, ops[0])
            k = self.get(ins, ops[1])
            upd = {"bt": "", "bts": "v | (1u << (b & 7))", "btr": "v & ~(1u << (b & 7))",
                   "btc": "v ^ (1u << (b & 7))"}[m]
            wr = f"wr8(p, (uint8_t)({upd}));" if upd else ""
            E(f"{{ int32_t b = (int32_t){k}; uint32_t p = {a} + (uint32_t)(b >> 3); uint32_t v = rd8(p); "
              f"cf = (v >> (b & 7)) & 1; {wr} }}")
        elif m == "enter":
            n, lvl = ops[0].imm & 0xffff, ops[1].imm & 31
            if lvl:
                raise Unsupported("enter nested")
            E(f"esp -= 4; wr32(esp, ebp); ebp = esp; esp -= {n};")
        elif m == "wait":
            pass
        elif m in ("bt", "bts", "btr", "btc"):
            s = sz()
            bit = (f"{ops[1].imm % (8 * s)}u" if ops[1].type == X.X86_OP_IMM
                   else f"({self.get(ins, ops[1])} % {8 * s}u)")
            a = self.get(ins, ops[0])
            upd = {"bt": "", "bts": "a | (1u << k)", "btr": "a & ~(1u << k)", "btc": "a ^ (1u << k)"}[m]
            wr = self.set(ins, ops[0], upd) if upd else ""
            E(f"{{ uint32_t a = {a}, k = {bit}; cf = (a >> k) & 1; {wr} }}")
        elif m in ("tzcnt", "lzcnt", "popcnt"):
            v = self.get(ins, ops[1])
            if m == "popcnt":
                E(f"{{ uint32_t v = {v}; {self.set(ins, ops[0], '__builtin_popcount(v)')} zf = v == 0; "
                  f"cf = of = sf = pf = 0; }}")
            else:
                fn = "__builtin_ctz(v)" if m == "tzcnt" else "__builtin_clz(v)"
                E(f"{{ uint32_t v = {v}, r = v ? {fn} : 32; {self.set(ins, ops[0], 'r')} cf = v == 0; zf = r == 0; }}")
        elif m in ("bsf", "bsr"):
            v = self.get(ins, ops[1])
            fn = "__builtin_ctz(v)" if m == "bsf" else "31 - __builtin_clz(v)"
            E(f"{{ uint32_t v = {v}; zf = v == 0; if (v) {{ {self.set(ins, ops[0], fn)} }} }}")

        # conditions
        elif m.startswith("set") and m[3:] in COND:
            E(self.set(ins, ops[0], f"({COND[m[3:]]}) ? 1 : 0"))
        elif m.startswith("cmov") and m[4:] in COND:
            E(f"if ({COND[m[4:]]}) {{ {self.set(ins, ops[0], self.get(ins, ops[1]))} }}")

        # string ops
        elif m in ("stosb", "stosw", "stosd", "movsb", "movsw", "movsd", "lodsb", "lodsd",
                   "cmpsb", "cmpsw", "cmpsd", "scasb", "scasw", "scasd") and "xmm" not in ins.op_str:
            n = {"b": 1, "w": 2, "d": 4}[m[-1]]
            k = m[:-1]
            step = f"(cpu->df ? -{n} : {n})"
            rd = {1: "rd8", 2: "rd16", 4: "rd32"}[n]
            wr = {1: "wr8", 2: "wr16", 4: "wr32"}[n]
            acc = {1: "(uint8_t)eax", 2: "(uint16_t)eax", 4: "eax"}[n]
            if k == "stos":
                one = f"{wr}(edi, {acc}); edi += {step};"
            elif k == "movs":
                one = f"{wr}(edi, {rd}(esi)); esi += {step}; edi += {step};"
            elif k == "lods":
                one = f"{self.reg_set({1: 'al', 2: 'ax', 4: 'eax'}[n], f'{rd}(esi)')} esi += {step};"
            elif k == "cmps":
                one = (f"{{ uint32_t a = {rd}(esi), b = {rd}(edi), r = (a - b) & {MASK[n]}; cf = a < b; "
                       f"of = ((a ^ b) & (a ^ r) & {SBIT[n]}) != 0; {self.flags_zsp('r', n)} }} "
                       f"esi += {step}; edi += {step};")
            else:  # scas
                one = (f"{{ uint32_t a = {acc}, b = {rd}(edi), r = (a - b) & {MASK[n]}; cf = a < b; "
                       f"of = ((a ^ b) & (a ^ r) & {SBIT[n]}) != 0; {self.flags_zsp('r', n)} }} "
                       f"edi += {step};")
            if not rep:
                E(one)
            elif k in ("cmps", "scas"):
                until = "!zf" if pfx == X.X86_PREFIX_REP else "zf"     # repe stops on !zf
                E(f"while (ecx) {{ {one} ecx--; if ({until}) break; }}")
            elif k == "stos" and n in (1, 4):
                E(f"if (!cpu->df && ecx) {{ uint32_t nb = ecx * {n}; "
                  + (f"memset(RT_P(edi), (uint8_t)eax, nb);" if n == 1 else
                     f"uint32_t i; for (i = 0; i < ecx; i++) wr32(edi + 4*i, eax);")
                  + f" edi += nb; ecx = 0; }} else while (ecx) {{ {one} ecx--; }}")
            elif k == "movs":
                E(f"if (!cpu->df && ecx && (edi >= esi + ecx*{n} || edi + ecx*{n} <= esi)) {{ "
                  f"memcpy(RT_P(edi), RT_P(esi), ecx * {n}); esi += ecx * {n}; edi += ecx * {n}; ecx = 0; }} "
                  f"else while (ecx) {{ {one} ecx--; }}")
            else:
                E(f"while (ecx) {{ {one} ecx--; }}")

        # misc
        elif m == "cpuid":
            E(f"{self.spill()} rt_cpuid(cpu); {self.reload()}")
        elif m == "rdtsc":
            E("{ uint64_t t = rt_rdtsc(); eax = (uint32_t)t; edx = (uint32_t)(t >> 32); }")
        elif m == "int" and ops and ops[0].type == X.X86_OP_IMM and (ops[0].imm & 0xff) == 0x80:
            # Linux i386 system call; the next instruction is a resume point
            # (a clone child starts there)
            E(f"{self.spill()} rt_syscall(cpu, 0x{nxt:x}u); {self.reload()}")
        elif m in ("hlt", "int3", "ud2", "int", "into", "in", "out"):
            E(f"{self.spill()} rt_trap(cpu, 0x{ins.address:x}u, \"{m}\");")

        # x87
        elif m.startswith("f"):
            self.lift_x87(ins, m)
        else:
            try:
                lift_sse.lift_sse(self, ins, m)
            except lift_sse.Unsupported as ex:
                raise Unsupported(str(ex))

    def lift_atomic(self, ins, m):
        """Locked RMW on memory -> __atomic builtins. Returns False for the
        rare forms left to the plain path (adc/sbb/neg/not)."""
        ops = ins.operands
        E = self.emit
        s = ops[0].size
        T = UT[s]
        p = f"(({T} *)RT_P({self.addr(ins, ops[0])}))"
        M, SB = MASK.get(s, "0"), SBIT.get(s, "0")
        if m == "cmpxchg8b":
            E(f"{{ uint64_t exp = ((uint64_t)edx << 32) | eax, nv = ((uint64_t)ecx << 32) | ebx; "
              f"zf = __atomic_compare_exchange_n((uint64_t *)RT_P({self.addr(ins, ops[0])}), &exp, nv, 0, "
              f"__ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST); if (!zf) {{ eax = (uint32_t)exp; edx = (uint32_t)(exp >> 32); }} }}")
            return True
        if m == "xchg":
            r = ins.reg_name(ops[1].reg)
            E(f"{{ {T} o = __atomic_exchange_n({p}, ({T})({self.reg_get(r)}), __ATOMIC_SEQ_CST); "
              f"{self.reg_set(r, 'o')} }}")
            return True
        if m == "xadd":
            r = ins.reg_name(ops[1].reg)
            E(f"{{ uint32_t b = {self.reg_get(r)}; uint32_t a = __atomic_fetch_add({p}, ({T})b, __ATOMIC_SEQ_CST); "
              f"uint64_t w = (uint64_t)a + b; uint32_t r = (uint32_t)w & {M}; cf = w > {M}; "
              f"of = ((a ^ r) & (b ^ r) & {SB}) != 0; {self.flags_zsp('r', s)} {self.reg_set(r, 'a')} }}")
            return True
        if m == "cmpxchg":
            acc = {4: "eax", 2: "ax", 1: "al"}[s]
            r = ins.reg_name(ops[1].reg)
            E(f"{{ {T} exp = ({T})({self.reg_get(acc)}); uint32_t x = exp; "
              f"int ok = __atomic_compare_exchange_n({p}, &exp, ({T})({self.reg_get(r)}), 0, "
              f"__ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST); uint32_t d = exp; uint32_t rr = (x - d) & {M}; "
              f"zf = ok; cf = x < d; sf = (rr & {SB}) != 0; of = ((x ^ d) & (x ^ rr) & {SB}) != 0; pf = rt_parity(rr); "
              f"if (!ok) {{ {self.reg_set(acc, 'd')} }} }}")
            return True
        if m in ("add", "sub", "and", "or", "xor", "inc", "dec"):
            if m in ("inc", "dec"):
                b = "1u"
            elif ops[1].type == X.X86_OP_IMM:
                b = f"0x{ops[1].imm & ((1 << (8 * s)) - 1):x}u"
            else:
                b = self.get(ins, ops[1], s)
            fn = {"add": "add", "inc": "add", "sub": "sub", "dec": "sub", "and": "and", "or": "or", "xor": "xor"}[m]
            op = {"add": "+", "sub": "-", "and": "&", "or": "|", "xor": "^"}[fn]
            flags = ""
            if fn == "add":
                flags = (f"{'' if m == 'inc' else f'cf = ((uint64_t)a + b) > {M}; '}"
                         f"of = ((a ^ r) & (b ^ r) & {SB}) != 0;")
            elif fn == "sub":
                flags = (f"{'' if m == 'dec' else 'cf = a < b; '}of = ((a ^ b) & (a ^ r) & {SB}) != 0;")
            else:
                flags = "cf = 0; of = 0;"
            E(f"{{ uint32_t b = {b}; uint32_t a = __atomic_fetch_{fn}({p}, ({T})b, __ATOMIC_SEQ_CST); "
              f"uint32_t r = (a {op} b) & {M}; {flags} {self.flags_zsp('r', s)} }}")
            return True
        if m in ("bts", "btr", "btc") and ops[1].type == X.X86_OP_IMM:
            k = ops[1].imm % (8 * s)
            fn = {"bts": "or", "btr": "and", "btc": "xor"}[m]
            v = f"~(({T})1 << {k})" if m == "btr" else f"(({T})1 << {k})"
            E(f"{{ {T} a = __atomic_fetch_{fn}({p}, {v}, __ATOMIC_SEQ_CST); cf = (a >> {k}) & 1; }}")
            return True
        return False

    def st_index(self, s):
        return int(s.strip()[3:-1])

    def lift_x87(self, ins, m):
        E = self.emit
        ops = ins.operands
        mem = [o for o in ops if o.type == X.X86_OP_MEM]
        sts = [p.strip() for p in ins.op_str.split(",")] if ins.op_str and not mem else []
        def memf():
            o = mem[0]
            a = self.addr(ins, o)
            if o.size == 4:
                return f"(rt_x87)rdf32({a})"
            if o.size == 8:
                return f"rdf64({a})"
            raise Unsupported(f"{m} m{o.size * 8}")
        def memi():
            o = mem[0]
            a = self.addr(ins, o)
            return f"(rt_x87)({ST[o.size]})" + {2: "rd16", 4: "rd32", 8: "rd64"}[o.size] + f"({a})"
        ARITH = {"fadd": "a + b", "fmul": "a * b", "fsub": "a - b", "fsubr": "b - a",
                 "fdiv": "a / b", "fdivr": "b / a"}
        if m == "fld":
            if mem:
                if mem[0].size == 10:
                    E(f"fpush(cpu, rt_ld80({self.addr(ins, mem[0])}));")
                else:
                    E(f"fpush(cpu, {memf()});")
            else:
                E(f"{{ rt_x87 v = ST({self.st_index(sts[0])}); fpush(cpu, v); }}")
        elif m in ("fst", "fstp"):
            if mem:
                o = mem[0]
                a = self.addr(ins, o)
                if o.size == 4:
                    E(f"wrf32({a}, (float)ST(0));")
                elif o.size == 8:
                    E(f"wrf64({a}, ST(0));")
                elif o.size == 10:
                    E(f"rt_st80({a}, ST(0));")
                else:
                    raise Unsupported("fst size")
            else:
                E(f"ST({self.st_index(sts[0])}) = ST(0);")
            if m == "fstp":
                E("fpop(cpu);")
        elif m == "fxch":
            i = self.st_index(sts[0]) if sts else 1
            E(f"{{ rt_x87 t = ST(0); ST(0) = ST({i}); ST({i}) = t; }}")
        elif m in ARITH or (m[:-1] in ARITH and m.endswith("p")):
            pop = m.endswith("p") and m[:-1] in ARITH
            base = m[:-1] if pop else m
            expr = ARITH[base]
            if mem:
                E(f"{{ rt_x87 a = ST(0), b = {memf()}; ST(0) = {expr}; }}")
            elif len(sts) == 2:
                d, s = self.st_index(sts[0]), self.st_index(sts[1])
                E(f"{{ rt_x87 a = ST({d}), b = ST({s}); ST({d}) = {expr}; }}")
            elif len(sts) == 1:
                i = self.st_index(sts[0])
                if pop:          # faddp st(i): st(i) = st(i) op st(0)
                    E(f"{{ rt_x87 a = ST({i}), b = ST(0); ST({i}) = {expr}; }}")
                else:            # fadd st(i): st(0) = st(0) op st(i)
                    E(f"{{ rt_x87 a = ST(0), b = ST({i}); ST(0) = {expr}; }}")
            else:                # faddp (no operand) = faddp st(1), st(0)
                E(f"{{ rt_x87 a = ST(1), b = ST(0); ST(1) = {expr}; }}")
            if pop:
                E("fpop(cpu);")
        elif m in ("fiadd", "fimul", "fisub", "fisubr", "fidiv", "fidivr"):
            expr = ARITH["f" + m[2:]]
            E(f"{{ rt_x87 a = ST(0), b = {memi()}; ST(0) = {expr}; }}")
        elif m == "fild":
            E(f"fpush(cpu, {memi()});")
        elif m in ("fist", "fistp", "fisttp"):
            o = mem[0]
            a = self.addr(ins, o)
            conv = {2: "f2i16", 4: "f2i32", 8: "f2i64"}[o.size]
            w = {2: "wr16", 4: "wr32", 8: "wr64"}[o.size]
            if m == "fisttp":
                E(f"{w}({a}, ({UT[o.size]})({ST[o.size]})RT_TRUNC(ST(0)));")
            else:
                E(f"{w}({a}, ({UT[o.size]}){conv}(cpu, ST(0)));")
            if m != "fist":
                E("fpop(cpu);")
        elif m in ("fldz", "fld1", "fldpi", "fldln2", "fldlg2", "fldl2e", "fldl2t"):
            v = {"fldz": "0.0", "fld1": "1.0", "fldpi": "3.14159265358979323846",
                 "fldln2": "0.69314718055994530942", "fldlg2": "0.30102999566398119521",
                 "fldl2e": "1.44269504088896340736", "fldl2t": "3.32192809488736234787"}[m]
            E(f"fpush(cpu, {v});")
        elif m in ("fucomi", "fcomi", "fucompi", "fcompi"):
            i = self.st_index(sts[-1]) if sts else 1
            E(f"FCOMI(ST(0), ST({i}));")
            if m.endswith("pi"):
                E("fpop(cpu);")
        elif m in ("fucom", "fucomp", "fucompp", "fcom", "fcomp", "fcompp", "ficom", "ficomp", "ftst"):
            if m == "ftst":
                b = "0.0"
            elif m.startswith("fi"):
                b = memi()
            elif mem:
                b = memf()
            else:
                b = f"ST({self.st_index(sts[-1]) if sts else 1})"
            E(f"fcom_sw(cpu, ST(0), {b});")
            if m.endswith("pp"):
                E("fpop(cpu); fpop(cpu);")
            elif m.endswith("p"):
                E("fpop(cpu);")
        elif m in ("fnstsw", "fstsw"):
            if mem:
                E(f"wr16({self.addr(ins, mem[0])}, fstsw(cpu));")
            else:
                E("eax = (eax & 0xffff0000u) | fstsw(cpu);")
        elif m in ("fnstcw", "fstcw"):
            E(f"wr16({self.addr(ins, mem[0])}, cpu->fpucw);")
        elif m == "fldcw":
            E(f"cpu->fpucw = rd16({self.addr(ins, mem[0])});")
        elif m in ("fninit", "finit"):
            E("cpu->fpucw = 0x37f; cpu->fpusw = 0; cpu->top = 0;")
        elif m == "fabs":
            E("ST(0) = RT_FABS(ST(0));")
        elif m == "fchs":
            E("ST(0) = -ST(0);")
        elif m == "fsqrt":
            E("ST(0) = RT_SQRT(ST(0));")
        elif m == "fsin":
            E("ST(0) = RT_SIN(ST(0));")
        elif m == "fcos":
            E("ST(0) = RT_COS(ST(0));")
        elif m == "fsincos":
            E("{ rt_x87 v = ST(0); ST(0) = RT_SIN(v); fpush(cpu, RT_COS(v)); }")
        elif m == "fpatan":
            E("{ rt_x87 x = ST(0), y = ST(1); fpop(cpu); ST(0) = RT_ATAN2(y, x); }")
        elif m == "fyl2x":
            E("{ rt_x87 x = ST(0), y = ST(1); fpop(cpu); ST(0) = y * RT_LOG2(x); }")
        elif m == "f2xm1":
            E("ST(0) = RT_EXP2(ST(0)) - 1.0;")
        elif m == "fscale":
            E("ST(0) = RT_LDEXP(ST(0), (int)RT_TRUNC(ST(1)));")
        elif m == "frndint":
            E("ST(0) = frnd(cpu, ST(0));")
        elif m == "fprem":
            E("{ rt_x87 r = RT_FMOD(ST(0), ST(1)); ST(0) = r; cpu->fpusw &= ~0x400; }")
        elif m.startswith("fcmov"):
            c = {"fcmovb": "cf", "fcmovnb": "!cf", "fcmove": "zf", "fcmovne": "!zf",
                 "fcmovbe": "(cf|zf)", "fcmovnbe": "(!cf&&!zf)", "fcmovu": "pf", "fcmovnu": "!pf"}[m]
            E(f"if ({c}) ST(0) = ST({self.st_index(sts[-1])});")
        elif m == "ffree":
            pass
        elif m == "fxam":
            E("rt_fxam(cpu, ST(0));")
        elif m == "fyl2xp1":
            E("{ rt_x87 x = ST(0), y = ST(1); fpop(cpu); ST(0) = y * RT_LOG2(x + 1.0); }")
        elif m == "fxtract":
            E("{ rt_x87 v = ST(0); int e = 0; rt_x87 fr = v == 0 ? v : (rt_x87)frexp((double)v, &e); "
              "ST(0) = v == 0 ? -INFINITY : (rt_x87)(e - 1); fpush(cpu, v == 0 ? v : fr * 2); }")
        elif m == "fprem1":
            E("{ rt_x87 r = remainder(ST(0), ST(1)); ST(0) = r; cpu->fpusw &= ~0x400; }")
        elif m in ("fnclex", "fclex"):
            E("cpu->fpusw &= 0x7f00;")
        elif m in ("fnstenv", "fstenv", "fnsave", "fsave"):
            a = self.addr(ins, mem[0])
            body = (f"wr32({a}, 0xffff0000u | cpu->fpucw); wr32({a} + 4, 0xffff0000u | fstsw(cpu)); "
                    f"wr32({a} + 8, 0xffffffffu); {{ int k; for (k = 12; k < 28; k += 4) wr32({a} + k, 0); }}")
            if "save" in m:
                body += (f" {{ int k; for (k = 0; k < 8; k++) rt_st80({a} + 28 + 10 * k, ST(k)); }} "
                         "cpu->fpucw = 0x37f; cpu->fpusw = 0; cpu->top = 0;")
            E(body)
        elif m == "fldenv":
            a = self.addr(ins, mem[0])
            E(f"cpu->fpucw = rd16({a}); cpu->fpusw = rd16({a} + 4);")
        else:
            raise Unsupported(m)

    # ---- whole function ----
    def run(self):
        f = self.f
        if f["start"] in HOST_FUNCS and f["start"] not in HOST_WRAP:
            # implemented by the runtime (rt/hostgame.c); [esp] = return address
            return [f"/* {f['name']}: host implementation ({HOST_FUNCS[f['start']]}) */",
                    f"void f_{f['start']:08x}(rt_cpu *cpu) {{ rt_host_fn(cpu, 0x{f['start']:x}u); }}", ""]
        insns = {}
        targets = set(b for b, e in f["blocks"]) | set(f.get("landing_pads", [])) | {f["start"]}
        for b, e in f["blocks"]:
            o = v2o(b)
            if o is None:
                self.unimpl["block outside file"] += 1
                continue
            for ins in md.disasm(data[o:o + (e - b)], b):
                if ins.address >= e:
                    break
                insns[ins.address] = ins
        for j, t, ts in f["jumptables"]:
            targets.update(ts)
        for ins in insns.values():
            m = ins.mnemonic
            if (m.startswith("j") or m.startswith("loop")) and ins.operands and \
                    ins.operands[0].type == X.X86_OP_IMM:
                t = ins.operands[0].imm & 0xffffffff
                if t in insns:
                    targets.add(t)
        if f.get("sweep"):
            # computed gotos: any instruction may be a jump target
            targets.update(insns)
        jt_by_jmp = {j: ts for j, t, ts in f["jumptables"]}
        all_labels = sorted(a for a in targets if a in insns)
        # wrapped: the lifted body is fo_<addr>, f_<addr> enters the runtime
        name = f"{'fo' if f['start'] in HOST_WRAP else 'f'}_{f['start']:08x}"
        o = self.out
        o.append(f"/* {f['name']} */")
        o.append(f"void {name}(rt_cpu *cpu)")
        o.append("{")
        o.append("    uint32_t eax=cpu->eax, ecx=cpu->ecx, edx=cpu->edx, ebx=cpu->ebx, esp=cpu->esp, "
                 "ebp=cpu->ebp, esi=cpu->esi, edi=cpu->edi;")
        o.append("    uint32_t cf = 0, zf = 0, sf = 0, of = 0, pf = 0, dispatch_t = 0;")
        # resume points: entered with cpu->resume_at set (clone child after
        # its syscall; later also C++ landing pads / longjmp targets)
        resume = sorted({a + insns[a].size for a in insns
                         if (insns[a].mnemonic == "call" or
                             (insns[a].mnemonic == "int" and insns[a].op_str.strip() in ("0x80", "128")))
                         and a + insns[a].size in insns}
                        | {p for p in f.get("landing_pads", []) if p in insns})
        if resume:
            targets.update(resume)
            cases = " ".join(f"case 0x{r:x}u: cpu->resume_at = 0; goto {self.label(r)};" for r in resume)
            o.append(f"    if (__builtin_expect(cpu->resume_at != 0, 0)) switch (cpu->resume_at) {{ {cases} default: break; }}")
            self.resume_points.extend(resume)
        o.append(f"    goto {self.label(f['start'])};")
        addrs = sorted(insns)
        # Fall-through destinations that are not the next emitted instruction
        # need labels too (decoded blocks can overlap or leave gaps).
        for i, a in enumerate(addrs):
            nxt = a + insns[a].size
            if (i + 1 == len(addrs) or addrs[i + 1] != nxt) and nxt in insns:
                targets.add(nxt)
        prev_falls = None
        for idx, a in enumerate(addrs):
            ins = insns[a]
            nxt = a + ins.size
            if prev_falls is not None and prev_falls != a:
                if prev_falls in insns:
                    o.append(f"    goto {self.label(prev_falls)};")
                else:
                    o.append(f"    {self.call_target(prev_falls, prev_falls, tail=True)}")
            if a in targets:
                o.append(f"{self.label(a)}:;")
            # host pc -> guest instruction: line = offset in the module
            # (tools/recomp/symbolize.py with -gline-tables-only objects)
            o.append(f'#line {a - MODULE_BASE} "{MODULE_TAG}"')
            m = ins.mnemonic
            prev_falls = nxt
            try:
                if m in ("ret", "repz ret", "rep ret"):
                    n = ins.operands[0].imm if ins.operands else 0
                    self.emit(f"{{ uint32_t ra = rd32(esp); esp += {4 + n}; {self.spill()} RT_RET(cpu, ra); return; }}")
                    prev_falls = None
                elif m == "call":
                    op = ins.operands[0]
                    if op.type == X.X86_OP_IMM:
                        t = op.imm & 0xffffffff
                        if t == nxt:
                            self.emit(f"esp -= 4; wr32(esp, 0x{nxt:x}u);")
                        else:
                            self.emit(self.call_target(t, nxt))
                            slot = plt_slot(t)
                            if slot is not None and got_names.get(slot) in NORETURN:
                                self.emit("return;")
                                prev_falls = None
                    else:
                        tv = self.get(ins, op, 4)
                        self.emit(f"{{ uint32_t t = {tv}; esp -= 4; wr32(esp, 0x{nxt:x}u); {self.spill()} "
                                  f"RT_SHPUSH(cpu, 0x{nxt:x}u); rt_call(cpu, t); RT_SHPOP(cpu); {self.reload()} }}")
                elif m == "jmp":
                    op = ins.operands[0]
                    prev_falls = None
                    if op.type == X.X86_OP_IMM:
                        t = op.imm & 0xffffffff
                        if t in insns and (t in targets):
                            self.emit(f"goto {self.label(t)};")
                        else:
                            self.emit(self.call_target(t, nxt, tail=True))
                    else:
                        tv = self.get(ins, op, 4)
                        ts = jt_by_jmp.get(a)
                        if ts:
                            sw = " ".join(f"case 0x{t:x}u: goto {self.label(t)};" for t in sorted(set(ts)) if t in insns)
                            self.emit(f"{{ uint32_t t = {tv}; switch (t) {{ {sw} default: {self.spill()} "
                                      f"rt_call(cpu, t); return; }} }}")
                        else:
                            # unknown target: one shared dispatch over all labels
                            # per function (per-jump switches grow quadratically)
                            self.need_dispatch = True
                            self.emit(f"dispatch_t = {tv}; goto L_dispatch;")
                elif (m.startswith("j") and m[1:] in COND) or m in ("jecxz", "jcxz") or m.startswith("loop"):
                    t = ins.operands[0].imm & 0xffffffff
                    if m == "jecxz":
                        c = "ecx == 0"
                    elif m == "jcxz":
                        c = "(uint16_t)ecx == 0"
                    elif m == "loop":
                        c = "--ecx != 0"
                    elif m == "loope":
                        c = "--ecx != 0 && zf"
                    elif m == "loopne":
                        c = "--ecx != 0 && !zf"
                    else:
                        c = COND[m[1:]]
                    if t in insns:
                        self.emit(f"if ({c}) goto {self.label(t)};")
                    else:
                        self.emit(f"if ({c}) {{ {self.call_target(t, nxt, tail=True)} }}")
                else:
                    self.lift(ins, nxt, targets)
            except Unsupported as ex:
                self.unimpl[str(ex)] += 1
                self.emit(f"{self.spill()} rt_unimpl(cpu, 0x{a:x}u, \"{ins.mnemonic} {ins.op_str}\"); {self.reload()}")
        if prev_falls is not None:
            # block runs off the end of this function's code into another one
            o.append(f"    {self.call_target(prev_falls, prev_falls, tail=True)}")
        if self.need_dispatch:
            sw = " ".join(f"case 0x{t:x}u: goto {self.label(t)};" for t in all_labels)
            o.append(f"L_dispatch: switch (dispatch_t) {{ {sw} default: {self.spill()} "
                     f"rt_call(cpu, dispatch_t); return; }}")
        o.append("}")
        o.append("")
        if f["start"] in HOST_WRAP:
            o.append(f"/* {f['name']}: runtime wrapper ({HOST_FUNCS[f['start']]}) around fo_{f['start']:08x} */")
            o.append(f"void f_{f['start']:08x}(rt_cpu *cpu) {{ rt_host_fn(cpu, 0x{f['start']:x}u); }}")
            o.append("")
        return o


# game imports the runtime may intercept (dlopen of a replaced library
# returns a fake handle; dlsym then hands out thunk addresses)
THUNK_NAMES = {}
OVERRIDE_IMPORTS = {"dlopen", "dlsym", "dlclose", "dlerror", "dlvsym",
                    "fclose"}       # fclose(NULL) after a failed fopen crashes glibc

# game functions implemented by the runtime (tools/recomp/host_funcs.txt)
# ("wrap" as a third word: the lifted body is kept as fo_<addr> for the
# runtime to call around its own code)
HOST_FUNCS, HOST_WRAP = {}, set()
for _line in open(os.path.join(os.path.dirname(os.path.abspath(__file__)), "host_funcs.txt")):
    _p = _line.split("#")[0].split()
    if len(_p) in (2, 3):
        HOST_FUNCS[int(_p[0], 16)] = _p[1]
        if len(_p) == 3 and _p[2] == "wrap":
            HOST_WRAP.add(int(_p[0], 16))

NORETURN = {"exit", "_exit", "abort", "__stack_chk_fail", "__assert_fail", "longjmp",
            "siglongjmp", "__longjmp_chk", "__cxa_throw", "__cxa_rethrow", "_Unwind_Resume",
            "pthread_exit", "__fortify_fail", "__chk_fail", "_ZSt9terminatev"}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--functions", nargs="+", default=[os.path.join(HERE, "out", "functions.json")],
                    help="functions.json of each module (game and/or libraries)")
    ap.add_argument("--out", default=os.path.join(HERE, "out", "c"))
    ap.add_argument("--only", help="comma-separated hex addresses")
    ap.add_argument("--per-file", type=int, default=400)
    args = ap.parse_args()
    modules = [json.load(open(p)) for p in args.functions]
    funcs = {}
    for mj in modules:
        for f in mj["functions"]:
            funcs[f["start"]] = f
    only = {int(x, 16) for x in args.only.split(",")} if args.only else None
    os.makedirs(args.out, exist_ok=True)
    stale = {f for f in os.listdir(args.out) if f.startswith("lifted_") and f.endswith(".c")}
    import_ids = {}
    unimpl = collections.Counter()
    per_func = {}
    resume_tab = []
    files = []
    sel = []
    for mj in modules:
        is_game = mj.get("module", "game") == "game"
        path = mj.get("path") or os.path.join(os.environ.get("JP2015_GAME", os.path.join(ROOT, "jp_game")), "game")
        tag = "game" if is_game else "".join(c if c.isalnum() else "_" for c in mj["module"])
        load_module(path, mj.get("base", 0), is_game, mj.get("module", "game"))
        msel = sorted(f["start"] for f in mj["functions"] if only is None or f["start"] in only)
        sel += msel
        for i in range(0, len(msel), args.per_file):
            chunk = msel[i:i + args.per_file]
            base_name = f"lifted_{tag}_{i // args.per_file:03d}.c"
            fn = os.path.join(args.out, base_name)
            stale.discard(base_name)
            parts = ['/* generated by tools/recomp/lift.py — do not edit */\n'
                     '#include "rt.h"\n#include "funcs.h"\n'
                     '#if defined(__clang__)\n#pragma clang diagnostic ignored "-Wunused-label"\n'
                     '#pragma clang diagnostic ignored "-Wunused-variable"\n'
                     '#pragma clang diagnostic ignored "-Wunused-but-set-variable"\n#endif\n\n']
            for a in chunk:
                fl = FuncLifter(funcs[a], funcs, import_ids)
                parts.append("\n".join(fl.run()) + "\n")
                unimpl.update(fl.unimpl)
                resume_tab += [(r, a) for r in fl.resume_points]
                if fl.unimpl:
                    per_func[a] = (sum(fl.unimpl.values()), funcs[a]["name"], funcs[a]["source"])
            text = "".join(parts)
            old = open(fn).read() if os.path.exists(fn) else None
            if old != text:                     # unchanged files keep their timestamp
                with open(fn, "w") as out:
                    out.write(text)
            files.append(fn)
    for f in stale:                             # modules/chunks no longer lifted
        os.remove(os.path.join(args.out, f))
    with open(os.path.join(args.out, "funcs.h"), "w") as h:
        h.write("/* generated by tools/recomp/lift.py */\n#pragma once\n#include \"rt.h\"\n")
        for a in sorted(funcs):
            h.write(f"void f_{a:08x}(rt_cpu *cpu);\n")
        for a in sorted(HOST_WRAP):
            h.write(f"void fo_{a:08x}(rt_cpu *cpu);   /* lifted body of a wrapped function */\n")
        h.write(f"#define RT_NFUNCS {len(funcs)}\n")
    with open(os.path.join(args.out, "table.c"), "w") as t:
        t.write('/* generated by tools/recomp/lift.py */\n#include "funcs.h"\n'
                "const struct { uint32_t addr; void (*fn)(rt_cpu *); } rt_func_table[] = {\n")
        for a in sorted(sel):
            t.write(f"    {{ 0x{a:x}u, f_{a:08x} }},\n")
        t.write("};\nconst unsigned rt_func_count = sizeof(rt_func_table) / sizeof(rt_func_table[0]);\n")
        t.write("/* resume points (address inside a function -> that function) */\n"
                "const struct { uint32_t addr; void (*fn)(rt_cpu *); } rt_resume_table[] = {\n")
        for r, a in sorted(resume_tab):
            t.write(f"    {{ 0x{r:x}u, f_{a:08x} }},\n")
        t.write("    { 0, 0 }\n};\n"
                "const unsigned rt_resume_count = sizeof(rt_resume_table) / sizeof(rt_resume_table[0]) - 1;\n")
    with open(os.path.join(args.out, "thunks.c"), "w") as t:
        t.write("/* generated by tools/recomp/lift.py: guest addresses the game reaches through\n"
                " * replaced or overridden imports (arcade addresses from the dump GOT) */\n"
                "#include <stdint.h>\n"
                "const struct { uint32_t addr; const char *name; } rt_thunk_table[] = {\n")
        for a, n in sorted(THUNK_NAMES.items()):
            t.write(f'    {{ 0x{a:x}u, "{n}" }},\n')
        t.write("    { 0, 0 }\n};\n")
    with open(os.path.join(args.out, "imports.h"), "w") as h:
        h.write("/* generated by tools/recomp/lift.py */\n#pragma once\nstatic const char *const RT_IMPORTS[] = {\n")
        for name, iid in sorted(import_ids.items(), key=lambda x: x[1]):
            h.write(f'    "{name}", /* {iid} */\n')
        h.write("};\n")
    total_unimpl = sum(unimpl.values())
    with open(os.path.join(args.out, "coverage.txt"), "w") as c:
        c.write(f"functions {len(sel)}, unlifted instructions {total_unimpl}\n")
        for k, v in unimpl.most_common():
            c.write(f"{v:8d}  {k}\n")
        c.write("\nfunctions with unlifted instructions (most first):\n")
        for a, (n, nm, src) in sorted(per_func.items(), key=lambda x: -x[1][0]):
            c.write(f"{n:8d}  {a:#x} {nm} ({src})\n")
    print(f"lifted {len(sel)} functions into {len(files)} files; {len(import_ids)} imports; "
          f"unlifted instructions {total_unimpl} ({len(unimpl)} kinds)")
    for k, v in unimpl.most_common(25):
        print(f"  {v:7d}  {k}")


if __name__ == "__main__":
    main()

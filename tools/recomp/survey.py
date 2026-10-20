#!/usr/bin/env python3
"""Size the static-recompilation job for the JP2015 game binary.

Disassembles every function symbol in the executable segment of the
devirtualized `game` (JP2015_GAME dir, like tools/disasm.py) and reports:
function / instruction counts, instruction classes the lifter must cover
(x87, SSE, MMX, string ops, segment-prefixed TLS accesses, privileged or
odd ones), indirect branches and jump tables, calls into the PLT (imports),
decode failures, and text not covered by any symbol.

  python tools/recomp/survey.py [--json out.json]

Output is a markdown summary on stdout.
"""
import argparse, bisect, collections, json, os, struct, sys
from capstone import Cs, CS_ARCH_X86, CS_MODE_32
from capstone import x86_const as X

BASE = os.environ.get("JP2015_GAME", os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "jp_game"))
data = open(os.path.join(BASE, "game"), "rb").read()

# ---- ELF: program headers and section headers ----
e_phoff, = struct.unpack_from("<I", data, 28)
e_shoff, = struct.unpack_from("<I", data, 32)
e_phentsize, e_phnum, e_shentsize, e_shnum, e_shstrndx = struct.unpack_from("<HHHHH", data, 42)
segs = []
for i in range(e_phnum):
    t, off, va, pa, fs, ms, fl, al = struct.unpack_from("<8I", data, e_phoff + i * e_phentsize)
    if t == 1:
        segs.append((va, off, fs, fl))
text_seg = next(s for s in segs if s[3] & 1)          # PF_X
sections = {}
if e_shoff and e_shnum:
    shs = [struct.unpack_from("<10I", data, e_shoff + i * e_shentsize) for i in range(e_shnum)]
    stroff = shs[e_shstrndx][4]
    for sh in shs:
        name = data[stroff + sh[0]:data.index(b"\0", stroff + sh[0])].decode()
        sections[name] = (sh[3], sh[5])                # addr, size

def v2o(v):
    for va, off, fs, _ in segs:
        if va <= v < va + fs:
            return off + v - va
    return None

plt = sections.get(".plt")
def in_plt(a):
    """PLT stub: `jmp dword ptr [abs]` (ff 25) - the dump has no section
    headers, so recognise stubs by shape."""
    if plt:
        return plt[0] <= a < plt[0] + plt[1]
    o = v2o(a)
    return o is not None and data[o:o + 2] == bytes((0xff, 0x25))

# ---- symbols ----
syms = []
for line in open(os.path.join(BASE, "symbols_original.txt")):
    p = line.split(None, 2)
    if len(p) == 3:
        try:
            syms.append((int(p[0], 16), int(p[1]), p[2].strip()))
        except ValueError:
            pass
tva, toff, tsz, _ = text_seg
# The executable PT_LOAD also holds .rodata; code ends where .rodata starts
# (_IO_stdin_used is its first object).
for a, sz, n in syms:
    if n == "_IO_stdin_used" and tva < a < tva + tsz:
        tsz = a - tva
funcs = sorted({(a, s, n) for a, s, n in syms if tva <= a < tva + tsz and s > 0 and not in_plt(a)})

md = Cs(CS_ARCH_X86, CS_MODE_32)
md.detail = True

cls = collections.Counter()
mn = collections.Counter()
x87 = collections.Counter()
sse = collections.Counter()
odd = collections.Counter()
ind_jmp = ind_call = jtab = 0
plt_calls = 0
plt_targets = collections.Counter()
decode_fail = []
covered = 0
n_ins = 0
funcs_with = collections.Counter()

ODD = {"cpuid", "rdtsc", "int", "int3", "hlt", "in", "out", "sysenter", "syscall",
       "lock", "xchg", "cmpxchg", "cmpxchg8b", "xadd", "bound", "enter", "lahf", "sahf",
       "das", "daa", "aaa", "aas", "aam", "aad", "into", "iret", "lds", "les", "ud2",
       "pushal", "popal", "pushfd", "popfd", "pushf", "popf", "cld", "std", "bswap",
       "bsf", "bsr", "bt", "bts", "btr", "btc", "shld", "shrd", "rcl", "rcr", "setalc"}

for a, size, name in funcs:
    off = v2o(a)
    code = data[off:off + size]
    end = a
    feats = set()
    for ins in md.disasm(code, a):
        n_ins += 1
        end = ins.address + ins.size
        m = ins.mnemonic
        mn[m] += 1
        regs = ins.op_str
        if m.startswith("f") and m not in ("fs",):
            x87[m] += 1
            feats.add("x87")
        if "xmm" in regs:
            sse[m] += 1
            feats.add("sse")
        if "mm" in regs and "xmm" not in regs:
            feats.add("mmx")
            odd["mmx:" + m] += 1
        if ins.prefix[0] in (X.X86_PREFIX_REP, X.X86_PREFIX_REPNE) and m.split()[0] not in ("ret",):
            if any(s in m for s in ("movs", "stos", "cmps", "scas", "lods")):
                odd["rep " + m] += 1
                feats.add("rep")
        if "gs:" in regs or "fs:" in regs:
            odd["seg " + ("gs" if "gs:" in regs else "fs")] += 1
            feats.add("tls")
        base_m = m.replace("lock ", "")
        if base_m in ODD or m.startswith("lock"):
            odd[m] += 1
        if m == "jmp" and ins.operands and ins.operands[0].type != X.X86_OP_IMM:
            ind_jmp += 1
            op = ins.operands[0]
            if op.type == X.X86_OP_MEM and op.mem.index != 0 and op.mem.scale == 4:
                jtab += 1
                feats.add("jumptable")
        if m == "call":
            op = ins.operands[0]
            if op.type == X.X86_OP_IMM:
                if in_plt(op.imm):
                    plt_calls += 1
                    plt_targets[op.imm] += 1
            else:
                ind_call += 1
                feats.add("indcall")
    covered += size
    if end < a + size:
        # stopped early: data in code, or a decode failure
        tail = data[off + (end - a):off + min(size, end - a + 8)]
        if any(b != 0 and b != 0x90 for b in tail):
            decode_fail.append((a, name, end, tail.hex()))
    for f in feats:
        funcs_with[f] += 1

fa = [f[0] for f in funcs]
gaps = 0
prev_end = tva
for a, s, n in funcs:
    if a > prev_end:
        gaps += a - prev_end
    prev_end = max(prev_end, a + s)

out = {
    "text": [hex(tva), tsz], "plt": [hex(plt[0]), plt[1]] if plt else None,
    "functions": len(funcs), "instructions": n_ins, "covered_bytes": covered,
    "gap_bytes": gaps, "distinct_mnemonics": len(mn),
    "indirect_jmp": ind_jmp, "jump_tables": jtab, "indirect_call": ind_call,
    "plt_calls": plt_calls, "plt_targets": len(plt_targets),
    "funcs_with": dict(funcs_with), "decode_stops": len(decode_fail),
}

print(f"# Recompilation survey: `{os.path.basename(BASE)}/game`\n")
print(f"- executable segment {hex(tva)}..{hex(tva + tsz)} ({tsz/1e6:.1f} MB); "
      f".plt {out['plt']}")
print(f"- functions (symbols with size): **{len(funcs)}**, instructions **{n_ins}**, "
      f"{len(mn)} distinct mnemonics")
print(f"- bytes covered by function symbols {covered} / gaps between them {gaps}")
print(f"- indirect jmp {ind_jmp} (jump tables {jtab}), indirect call {ind_call}")
print(f"- calls into .plt {plt_calls} to {len(plt_targets)} distinct stubs")
print(f"- functions using: " + ", ".join(f"{k} {v}" for k, v in sorted(funcs_with.items())))
print(f"- functions where linear decode stopped early: {len(decode_fail)}")
print("\n## x87 mnemonics\n")
print(", ".join(f"{m} {c}" for m, c in x87.most_common()))
print("\n## SSE mnemonics\n")
print(", ".join(f"{m} {c}" for m, c in sse.most_common()) or "none")
print("\n## Unusual / lifter-relevant\n")
print(", ".join(f"{m} {c}" for m, c in odd.most_common()))
print("\n## Top 60 mnemonics\n")
print(", ".join(f"{m} {c}" for m, c in mn.most_common(60)))
if decode_fail:
    print("\n## Early decode stops (first 25)\n")
    for a, n, e, t in decode_fail[:25]:
        print(f"- {a:#x} {n}: stopped at {e:#x}, next bytes {t}")

ap = argparse.ArgumentParser()
ap.add_argument("--json")
args = ap.parse_args()
if args.json:
    out["mnemonics"] = dict(mn)
    json.dump(out, open(args.json, "w"), indent=1)

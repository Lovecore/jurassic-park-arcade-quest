#!/usr/bin/env python3
"""Pick functions the differential harness can run on random inputs.

A candidate's whole call tree (direct calls and tail calls) must be free of
imports, indirect calls, TLS/segment accesses and instructions the lifter
does not handle yet. Writes "addr flags name" lines; flags: x = uses x87,
s = uses SSE/MMX.

  JP2015_GAME=<dir> python tools/recomp/harness/candidates.py > cands.txt
"""
import json, os, re, struct, sys
from capstone import Cs, CS_ARCH_X86, CS_MODE_32

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(HERE, "..", "out")
BASE = os.environ["JP2015_GAME"]
data = open(os.path.join(BASE, "game"), "rb").read()
e_phoff, = struct.unpack_from("<I", data, 28)
e_phnum, = struct.unpack_from("<H", data, 44)
segs = [struct.unpack_from("<8I", data, e_phoff + i * 32) for i in range(e_phnum)]
segs = [(s[2], s[1], s[4]) for s in segs if s[0] == 1]

def v2o(v):
    for va, off, fs in segs:
        if va <= v < va + fs:
            return off + v - va

fj = json.load(open(os.path.join(OUT, "functions.json")))
funcs = {f["start"]: f for f in fj["functions"]}
unlifted = set()
cov = open(os.path.join(OUT, "c", "coverage.txt")).read()
for m in re.finditer(r"^\s+\d+\s+(0x[0-9a-f]+) ", cov, re.M):
    unlifted.add(int(m.group(1), 16))

md = Cs(CS_ARCH_X86, CS_MODE_32)
info = {}
for a, f in funcs.items():
    x87 = sse = bad = False
    for b, e in f["blocks"]:
        o = v2o(b)
        for ins in md.disasm(data[o:o + (e - b)], b):
            if ins.address >= e:
                break
            s = ins.op_str
            if "gs:" in s or "fs:" in s:
                bad = True
            if ins.mnemonic in ("rdtsc", "rdtscp"):     # time-dependent result
                bad = True
            if ins.mnemonic.startswith("f"):
                x87 = True
            if "xmm" in s or re.search(r"\bmm\d", s):
                sse = True
    bad = bad or bool(f["imports"]) or f["indirect_call"] > 0 or f["indirect_jmp"] > 0 \
        or a in unlifted or bool(f.get("landing_pads")) or f.get("decode_fail")
    info[a] = (bad, x87, sse, f["calls"] + f["tail_calls"])

ok = {}
def good(a, stack=()):
    if a in ok:
        return ok[a]
    if a not in info or a in stack:
        return a in stack           # recursion: fine if the rest is fine
    bad, x87, sse, callees = info[a]
    r = not bad and all(good(c, stack + (a,)) for c in callees)
    ok[a] = r
    return r

n = 0
for a in sorted(funcs):
    if good(a):
        bad, x87, sse, _ = info[a]
        flags = ("x" if x87 else "") + ("s" if sse else "") or "-"
        print(f"{a:08x} {flags} {funcs[a]['name']}")
        n += 1
print(f"{n} candidates of {len(funcs)}", file=sys.stderr)

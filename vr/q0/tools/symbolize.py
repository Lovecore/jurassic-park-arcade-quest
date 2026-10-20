"""Symbolize game addresses from vr_shim.log CRASH dumps (or argv)."""
import bisect
import re
import sys

import jp_paths

SYMF = jp_paths.SYMBOLS
syms = {}
for line in open(SYMF):
    p = line.split()
    if len(p) == 3:
        syms[int(p[0], 16)] = (int(p[1]), p[2])
addrs = sorted(syms)


def name(a):
    i = bisect.bisect_right(addrs, a) - 1
    if i < 0:
        return "?"
    s = addrs[i]
    return f"{syms[s][1]}+{a - s:#x}"


src = sys.argv[1:] or re.findall(r"= ([0-9a-f]{8})$", open(
    jp_paths.ROOT / "vr/q0/vr_shim.log").read(), re.M)
for a in src:
    v = int(a, 16)
    print(f"{v:08x}  {name(v)}")

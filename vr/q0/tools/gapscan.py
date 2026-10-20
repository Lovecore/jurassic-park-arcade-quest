"""Score every lib variant at every page-aligned base inside layout gaps."""
import collections
import sys

sys.path.insert(0, ".")
import solve_got2 as S  # noqa: E402

GAPS = [tuple(int(x, 16) for x in g.split("-")) for g in sys.argv[1:]]
libs = S.collect_libs()
stale = sorted(S.read_stale())
for lo, hi in GAPS:
    vals = [A for A in stale if lo <= A < hi]
    res = []
    for lib in libs:
        e = lib["extent"]
        if e > hi - lo:
            continue
        c = collections.Counter()
        for A in vals:
            for off in lib["syms"]:
                B = A - off
                if off < e and B & 0xFFF == 0 and lo <= B and B + e <= hi:
                    c[B] += 1
        for B, v in c.most_common(1):
            res.append((v, lib["var"][:30], lib["fam"], hex(B), hex(e)))
    res.sort(reverse=True)
    print(f"gap {lo:08x}-{hi:08x} ({hi - lo:#x}) stale-in-gap={len(vals)}")
    for r in res[:8]:
        print("   ", r)

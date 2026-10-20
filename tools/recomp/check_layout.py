#!/usr/bin/env python3
"""Check the recovered arcade library layout (got-layout.md) against the
dump: each library placed at its arcade base must put every symbol the
game's GOT references exactly where the dump's stamped GOT entry points.

  python tools/recomp/check_layout.py <pristine game> <debs dir>
"""
import collections, os, struct, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from elfmod import Mod

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..")
# library -> (path inside the extracted debs, arcade base) from got-layout.md
LAYOUT = {
    "libc-2.19.so":        ("x_libc6_2.19-0ubuntu6.6_i386/lib/i386-linux-gnu/libc-2.19.so", 0xb7141000),
    "libgcc_s.so.1":       ("x_libgcc1_4.9-20140406-0ubuntu1_i386/lib/i386-linux-gnu/libgcc_s.so.1", 0xb72ef000),
    "libm-2.19.so":        ("x_libc6_2.19-0ubuntu6.6_i386/lib/i386-linux-gnu/libm-2.19.so", 0xb7320000),
    "libpthread-2.19.so":  ("x_libc6_2.19-0ubuntu6.6_i386/lib/i386-linux-gnu/libpthread-2.19.so", 0xb736f000),
    "libdl-2.19.so":       ("x_libc6_2.19-0ubuntu6.6_i386/lib/i386-linux-gnu/libdl-2.19.so", 0xb75f0000),
    "libstdc++.so.6.0.19": ("x_libstdc++6_4.8.2-19ubuntu1_i386/usr/lib/i386-linux-gnu/libstdc++.so.6.0.19", 0xb75f5000),
}

def main():
    game, debs = sys.argv[1], sys.argv[2]
    mods = {n: Mod(os.path.join(debs, p), b) for n, (p, b) in LAYOUT.items()}
    g = open(game, "rb").read()
    res, bad = collections.Counter(), []
    for line in open(os.path.join(ROOT, "vr", "q0", "got_fixups.txt")):
        p = line.split()
        if len(p) < 3:
            continue
        slot, name, lib = int(p[0], 16), p[1], p[2].split(":")[-1]
        m = mods.get(lib)
        if not m:
            res["library not lifted (GL, SDL, ...)"] += 1
            continue
        stamped = struct.unpack_from("<I", g, 0xa94000 + slot - 0x8adc000)[0]
        a = m.addr(name)
        if a == stamped:
            res["match"] += 1
        elif name in m.ifuncs:
            res["ifunc (CPU-specific implementation)"] += 1
        else:
            res["MISMATCH"] += 1
            bad.append(f"{name} ({lib}): stamped {stamped:#x}, layout {a:#x}" if a else f"{name}: not exported")
    for k, v in res.most_common():
        print(f"{v:5d}  {k}")
    for b in bad:
        print("  ", b)
    return 1 if bad else 0

if __name__ == "__main__":
    sys.exit(main())

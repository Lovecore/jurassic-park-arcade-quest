"""Print PLT stub + call-site context for GOT slots (manual naming aid)."""
import sys

sys.path.insert(0, __import__("os").path.dirname(__import__("os").path.abspath(__file__)))
from callsites import Game  # noqa: E402
from capstone import CS_ARCH_X86, CS_MODE_32, Cs  # noqa: E402

SYMS = {}
import jp_paths  # noqa: E402

for line in open(jp_paths.SYMBOLS):
    p = line.split()
    if len(p) == 3:
        SYMS[int(p[0], 16)] = p[2]
ADDRS = sorted(SYMS)


def owner(a):
    import bisect
    i = bisect.bisect_right(ADDRS, a) - 1
    return f"{SYMS[ADDRS[i]]}+{a - ADDRS[i]:#x}" if i >= 0 else "?"


g = Game()
stubs = g.plt_stubs()
md = Cs(CS_ARCH_X86, CS_MODE_32)
nsites = int(sys.argv[1])
for s in [int(x, 16) for x in sys.argv[2:]]:
    st = stubs.get(s)
    sites = g.call_sites([st]).get(st, []) if st else []
    print(f"=== slot {s:#x} stub {st:#x} sites={len(sites)}" if st else f"=== slot {s:#x} no stub")
    for cs in sites[:nsites]:
        o = g.v2o(cs - 30)
        ins = [x for x in md.disasm(g.d[o:o + 48], cs - 30)]
        ins = [x for x in ins if x.address >= cs - 30]
        print(f"  -- {cs:#x} in {owner(cs)}")
        for x in ins[-12:]:
            print(f"     {x.address:08x} {x.mnemonic} {x.op_str}")

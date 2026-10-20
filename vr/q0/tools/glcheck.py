"""Score libGL base hypotheses by GL enum validity of constant call args."""
import glob
import sys

sys.path.insert(0, ".")
sys.path.insert(0, "tools")
import solve_got2 as S  # noqa: E402
from callsites import Game  # noqa: E402
from elfsyms import load_extent, load_symtab  # noqa: E402

CAPS = {0x0B71, 0x0BE2, 0x0B44, 0x0DE1, 0x0BC0, 0x0B50, 0x0B57, 0x0BA1, 0x0C11,
        0x8037, 0x809D, 0x0B90, 0x0B60, 0x8513, 0x0DE0, 0x806F, 0x0B10, 0x0B20,
        0x3000, 0x3001, 0x3002, 0x3003, 0x3004, 0x3005, 0x4000, 0x4001, 0x4002,
        0x4003, 0x4004, 0x4005, 0x4006, 0x4007, 0x0BF2, 0x0C60, 0x0C61, 0x0B42,
        0x0B43, 0x0B54, 0x803A, 0x809E, 0x809F, 0x80A0, 0x8642, 0x884F, 0x0D80}
TEX_TARGETS = {0x0DE1, 0x0DE0, 0x8513, 0x806F, 0x84F5, 0x8C1A}
BLEND = {0, 1, 0x300, 0x301, 0x302, 0x303, 0x304, 0x305, 0x306, 0x307, 0x308}
RULES = {  # name -> {arg index: valid set}
    "glEnable": {0: CAPS}, "glDisable": {0: CAPS}, "glIsEnabled": {0: CAPS},
    "glBlendFunc": {0: BLEND, 1: BLEND},
    "glMatrixMode": {0: {0x1700, 0x1701, 0x1702}},
    "glBindTexture": {0: TEX_TARGETS},
    "glTexParameteri": {0: TEX_TARGETS, 1: set(range(0x2800, 0x2804)) | {0x8072, 0x813C, 0x813D, 0x8191, 0x884C, 0x884D, 0x84FE}},
    "glTexParameterf": {0: TEX_TARGETS},
    "glDepthFunc": {0: set(range(0x200, 0x208))},
    "glAlphaFunc": {0: set(range(0x200, 0x208))},
    "glCullFace": {0: {0x404, 0x405, 0x408}},
    "glFrontFace": {0: {0x900, 0x901}},
    "glClear": {0: {m for m in range(0x4800) if m & ~0x4700 == 0 and m}},
    "glPixelStorei": {0: set(range(0x0CF0, 0x0CF6)) | set(range(0x0D00, 0x0D06))},
    "glHint": {0: set(range(0x0C50, 0x0C55)) | {0x8192, 0x84EF, 0x8B8B}},
    "glPolygonMode": {0: {0x404, 0x405, 0x408}, 1: {0x1B00, 0x1B01, 0x1B02}},
    "glShadeModel": {0: {0x1D00, 0x1D01}},
    "glEnableClientState": {0: set(range(0x8074, 0x807A)) | {0x8450, 0x8457}},
    "glDisableClientState": {0: set(range(0x8074, 0x807A)) | {0x8450, 0x8457}},
    "glBindBuffer": {0: {0x8892, 0x8893, 0x88EB, 0x88EC}},
    "glBindFramebuffer": {0: {0x8D40, 0x8CA8, 0x8CA9}},
    "glBindFramebufferEXT": {0: {0x8D40, 0x8CA8, 0x8CA9}},
    "glBindRenderbuffer": {0: {0x8D41}}, "glBindRenderbufferEXT": {0: {0x8D41}},
    "glActiveTexture": {0: set(range(0x84C0, 0x84E0))},
    "glClientActiveTexture": {0: set(range(0x84C0, 0x84E0))},
    "glGetIntegerv": {0: set(range(0x0B00, 0x10000))},
    "glDrawBuffer": {0: set(range(0x400, 0x40D)) | set(range(0x8CE0, 0x8CF0))},
    "glReadBuffer": {0: set(range(0x400, 0x40D)) | set(range(0x8CE0, 0x8CF0))},
    "glCreateShader": {0: {0x8B30, 0x8B31}},
    "glStencilOp": {0: {0, 0x150A, 0x1E00, 0x1E01, 0x1E02, 0x1E03, 0x8507, 0x8508}},
    "glStencilFunc": {0: set(range(0x200, 0x208))},
    "glLightfv": {0: set(range(0x4000, 0x4008))}, "glLightf": {0: set(range(0x4000, 0x4008))},
    "glFogi": {0: set(range(0x0B60, 0x0B67)) | {0x8450}}, "glFogf": {0: set(range(0x0B60, 0x0B67))},
    "glFogfv": {0: set(range(0x0B60, 0x0B67))},
    "glTexEnvi": {0: {0x2300, 0x8500, 0x8861}}, "glTexEnvf": {0: {0x2300, 0x8500, 0x8861}},
    "glBegin": {0: set(range(0, 10))},
    "glDrawArrays": {0: set(range(0, 10))}, "glDrawElements": {0: set(range(0, 10))},
}


def score(g, stubs, names):
    """names: slot -> name. Returns (checked, ok, per-name detail)."""
    want = {s: n for s, n in names.items() if n in RULES and s in stubs}
    sites = g.call_sites([stubs[s] for s in want])
    checked = ok = 0
    detail = []
    for s, n in want.items():
        for cs in sites.get(stubs[s], [])[:12]:
            args = g.const_args(cs)
            for k, valid in RULES[n].items():
                if k in args:
                    checked += 1
                    good = args[k] in valid
                    ok += good
                    if not good:
                        detail.append(f"{n}[{k}]={args[k]:#x}@{cs:#x}")
    return checked, ok, detail


def main():
    g = Game()
    stubs = g.plt_stubs()
    slots = S.read_stale()
    for p in sorted(glob.glob("debs/nv/x/*/usr/lib/nvidia-*/libGL.so.*")):
        syms, e = load_symtab(p), load_extent(p)
        for B in [int(x, 16) for x in sys.argv[1:]]:
            names = {}
            for A, gs in slots.items():
                if B <= A < B + e and (A - B) in syms:
                    for ga in gs:
                        names[ga] = syms[A - B]
            c, k, det = score(g, stubs, names)
            print(f"{p.split('/')[-1]:18s} base={B:08x} named={len(names):3d} "
                  f"argchecks={c:3d} valid={k:3d}  bad={det[:4]}")


if __name__ == "__main__":
    main()

#!/usr/bin/env python3
"""Solve the Sentinel-stamped 0xb7xxxxxx GOT/descriptor addresses (v2).

The TP dump froze every bound import as an absolute address in the arcade's
Ubuntu 14.04 library layout, and the protector replaced every PLT reloc
offset with 0xdeadbeef, so the import names exist nowhere in the binary.
We recover the arcade's library layout (lib -> base) and name each slot so
the shim can re-resolve it with dlsym at load time.

v1 (solve_got.py) accepted bases by raw vote count. That bound ~110 slots to
the wrong function: libraries with uniform-stride exports (GL dispatch
stubs) collect near-equal votes at shifted bases, and dense stub regions
hand chance votes to unrelated libraries. v2 instead pins a LAYOUT table in
which every base is backed by evidence (exact-match coverage, back-to-back
contiguity of ld.so mappings, and for libGL call-site enum validation via
tools/glcheck.py). tools/gapscan.py is the discovery aid for the gaps.

glibc libs also match against libc6-dbg .symtab: slots bound to an IFUNC
hold the selected implementation (__memcpy_ssse3), which is mapped back to
the public name (memcpy) that any modern libc exports.

Outputs vr/q0/got_fixups.txt (<slot> <symbol> <lib>) and vr/q0/got_fixups.h.
"""
import collections
import glob
import os
import struct
import sys

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "tools"))
from callsites import Game  # noqa: E402
from elfsyms import exported_names, ifunc_names, load_extent, load_symtab  # noqa: E402

import jp_paths  # noqa: E402

JP = str(jp_paths.ROOT)
Q0 = JP + "/vr/q0"
GAME = jp_paths.pristine_game()
DBG = Q0 + "/debs/x/libc6-dbg_2.19-0ubuntu6.6_i386/usr/lib/debug/lib/i386-linux-gnu/"

# Arcade library layout: (soname family, variant substring, base, evidence).
# Libraries are mapped top-down back to back by ld.so; see
# ai_docs/jp2015-q0-boot/got-layout.md for the full derivation.
LAYOUT = [
    ("libc-2.19.so", "libc6_2.19-0ubuntu6.6", 0xB7141000, "218 exact, +dbg IFUNC"),
    ("libgcc_s.so.1", "libgcc1_4.9-20140406-0ubuntu1", 0xB72EF000, "libc end..libudev, _Unwind_Resume"),
    ("libudev.so.1.3.5", "libudev1_204-5ubuntu20_", 0xB730C000, "23/24 in gap"),
    ("libm-2.19.so", "libc6_2.19-0ubuntu6.6", 0xB7320000, "ends at librt gap, +dbg"),
    ("libpthread-2.19.so", "libc6_2.19-0ubuntu6.6", 0xB736F000, "54 exact"),
    ("libglut.so.3.9.0", "freeglut3_2.8.1-1", 0xB738B000, "pthread end, glutBitmapCharacter"),
    ("libGLU.so.1.3.1", "libglu1-mesa_9.0.0-2", 0xB73C5000, "glut end, gluErrorString/Build2DMipmaps"),
    ("libGL.so.346.96", "nvidia-346", 0xB7439000, "164/164 enum args; ends at SDL"),
    ("libSDL-1.2.so.0.11.4", "libsdl1.2debian_1.2.15-8ubuntu1.1", 0xB7558000, "ends at libdl"),
    ("libdl-2.19.so", "u1404", 0xB75F0000, "ends at libstdc++"),
    ("libstdc++.so.6.0.19", "libstdc++6_4.8.2-19ubuntu1", 0xB75F5000, "29/29, dl end, trusty release"),
    ("libogg.so.0.8.1", "libogg0_1.3.1-1ubuntu1", 0xB76EA000, "ends at libtheora"),
    ("libtheora.so.0.3.10", "libtheora0_1.1.1+dfsg.1-3.2", 0xB76F3000, "19 exact"),
]

NEVER = {"_init", "_fini", "_end", "_edata", "__bss_start"}

# Slots named from call-site semantics where the arcade build drifts from the
# reference file. libGL's glX section sits -0x150 from 346.96 (all four
# agree); see tools/slotctx.py output in the ai_docs layout note.
SEMANTIC = {
    0x8ADD588: ("glXGetProcAddressARB", "1234 GLEW call sites, string arg"),
    0x8ADD904: ("glXGetClientString", "glxewGetExtension: (dpy, GLX_EXTENSIONS)"),
    0x8ADD264: ("glXQueryVersion", "glxewContextInit: (dpy, &maj, &min)"),
    0x8ADD70C: ("glXSwapIntervalSGI", "SM_SetVSync(0|1)"),
}

# Arcade PhysX 2.8.1 loader (b76dd000..b76ea000, between libstdc++ and
# libogg). No reference file yet; the shim stubs these.
PHYSX_SLOTS = {0x8ADD910: "NxCreatePhysicsSDK"}


def variant_of(path):
    if "/x/" in path:
        return path.split("/x/")[1].split("/")[0]
    return "u1404"


def collect_libs():
    """Every candidate lib file (all variants) with symbols and extent."""
    paths = []
    for f in glob.glob(Q0 + "/u1404/lib/i386-linux-gnu/*.so*"):
        if not os.path.islink(f):
            paths.append(f)
    for f in glob.glob(Q0 + "/u1404libs/*.so.*"):
        if not os.path.islink(f) and len(f.split(".so.")[1].split(".")) > 1:
            paths.append(f)
    for pat in ("/debs/x/*/**/lib/i386-linux-gnu/**/*.so*",
                "/debs/nv/x/*/usr/lib/nvidia-*/*.so*"):
        for f in glob.glob(Q0 + pat, recursive=True):
            if os.path.islink(f) or "/gconv/" in f or "/debug/" in f:
                continue
            paths.append(f)
    libs = []
    for p in paths:
        try:
            syms = load_symtab(p)
        except Exception:
            continue
        if not syms:
            continue
        fam = os.path.basename(p)
        ifn = set()
        dbg = DBG + fam
        if variant_of(p).startswith("libc6_2.19-0ubuntu6.6_") and os.path.exists(dbg):
            ifn = ifunc_names(p)
            for off, nm in load_symtab(dbg, funcs_only=True).items():
                syms.setdefault(off, nm)
        libs.append({"path": p, "fam": fam, "var": variant_of(p), "syms": syms,
                     "extent": load_extent(p), "ifunc": ifn,
                     "exported": exported_names(p) if ifn else set()})
    return libs


def public_name(name, ifn, exported):
    """__memcpy_chk_ssse3 -> __memcpy_chk ; __strlen_sse2_bsf -> strlen.

    Only for implementation names; exported names (__strcat_chk) are kept.
    """
    if name in exported:
        return name
    best = None
    for p in ifn:
        if name.startswith(p + "_") or name.startswith("__" + p + "_"):
            if best is None or len(p) > len(best):
                best = p
    return best or name


def read_stale():
    """stale value -> [slot vaddr] for the GOT and the envelope descriptors."""
    d = open(GAME, "rb").read()
    phoff = struct.unpack_from("<I", d, 28)[0]
    phsz, phnum = struct.unpack_from("<HH", d, 42)
    loads = [struct.unpack_from("<IIIII", d, phoff + i * phsz) for i in range(phnum)]

    def v2o(v):
        for t, o, va, pa, fs in loads:
            if t == 1 and va <= v < va + fs:
                return o + v - va
        return None

    slots = collections.defaultdict(list)
    for base, n in ((0x8ADB000, 4640), (0x90E73EC, 162)):
        vals = struct.unpack_from(f"<{n}I", d, v2o(base))
        for i, v in enumerate(vals):
            if 0xB7000000 <= v < 0xB8000000:
                slots[v].append(base + i * 4)
    return slots


def place(libs):
    """Resolve LAYOUT rows to lib dicts; refuse overlaps."""
    by = {}
    for lib in libs:
        by.setdefault(lib["fam"], []).append(lib)
    placed = []
    for fam, var, base, why in LAYOUT:
        cands = [lb for lb in by.get(fam, []) if var in lb["var"] or var in lb["path"]]
        if not cands:
            sys.exit(f"LAYOUT: no file for {fam} / {var}")
        lib = cands[0]
        lo, hi = base, base + lib["extent"]
        for plo, phi, other in placed:
            if lo < phi and plo < hi:
                sys.exit(f"LAYOUT: {fam} {lo:08x}-{hi:08x} overlaps {other['fam']}")
        placed.append((lo, hi, lib))
    return sorted(placed, key=lambda p: p[0])


def main():
    libs = collect_libs()
    slots = read_stale()
    placed = place(libs)
    stubs = Game().plt_stubs()
    fixups, unres, physx, data = [], [], [], []
    per = collections.Counter()
    for A in sorted(slots):
        owner = next((p for p in placed if p[0] <= A < p[1]), None)
        hit = None
        if owner:
            nm = owner[2]["syms"].get(A - owner[0])
            if nm and nm not in NEVER:
                hit = public_name(nm, owner[2]["ifunc"], owner[2]["exported"])
        for ga in slots[A]:
            if ga in SEMANTIC:
                fixups.append((ga, A, SEMANTIC[ga][0], owner[2]))
                per[owner[2]["fam"]] += 1
            elif ga in PHYSX_SLOTS:
                physx.append((ga, A, PHYSX_SLOTS[ga]))
            elif hit:
                fixups.append((ga, A, hit, owner[2]))
                per[owner[2]["fam"]] += 1
            elif ga not in stubs:
                data.append((ga, A))
            else:
                unres.append((ga, A, owner[2]["fam"] if owner else "?"))

    for lo, hi, lib in placed:
        print(f"  {lo:08x}-{hi:08x} {lib['fam']:24s} {lib['var'][:36]:36s} "
              f"slots={per[lib['fam']]}", file=sys.stderr)
    with open(Q0 + "/got_fixups.txt", "w") as f, open(Q0 + "/got_fixups.h", "w") as h:
        h.write("/* generated by vr/q0/solve_got2.py — do not edit */\n")
        h.write("typedef struct { unsigned int addr, stale; const char *sym; } GotFixup;\n")
        h.write("static const GotFixup GOT_FIXUPS[] = {\n")
        for ga, A, nm, lib in sorted(fixups):
            f.write(f"{ga:08x} {nm} {lib['var']}:{lib['fam']}\n")
            h.write(f"  {{ 0x{ga:08x}, 0x{A:08x}, \"{nm}\" }},\n")
        h.write("};\n")
        h.write("/* arcade PhysX loader imports: no library here, shim stubs them */\n")
        h.write("static const GotFixup GOT_PHYSX[] = {\n")
        for ga, A, nm in sorted(physx):
            h.write(f"  {{ 0x{ga:08x}, 0x{A:08x}, \"{nm}\" }},\n")
        h.write("};\n")
        h.write("/* code slots with no name: shim traps them */\n")
        h.write("static const unsigned int GOT_UNRESOLVED[] = {\n")
        for ga, A, owner in sorted(unres):
            h.write(f"  0x{ga:08x},  /* stale 0x{A:08x} in {owner} */\n")
        h.write("  0  /* terminator */\n};\n")
        h.write("/* data words with 0xb7 values and no PLT stub, left as is:\n")
        for ga, A in sorted(data):
            h.write(f"   0x{ga:08x} = 0x{A:08x}\n")
        h.write(" */\n")
    print(f"fixed {len(fixups)} slots, physx {len(physx)}, data {len(data)}, "
          f"unresolved {len(unres)}", file=sys.stderr)
    for ga, A, owner in sorted(unres, key=lambda u: u[1]):
        print(f"  unresolved {ga:08x} stale {A:08x} in {owner}", file=sys.stderr)


if __name__ == "__main__":
    main()

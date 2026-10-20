#!/usr/bin/env python3
"""R3: build the guest address space of the arcade process.

Places the game and the open-source i386 libraries it imports at the
addresses they had on the cabinet (recovered in got-layout.md, verified by
check_layout.py), applies every relocation the dynamic linker would
(eager binding, no lazy PLT), and lays out static TLS like glibc on i386
(variant II, block below the thread pointer). Libraries that are replaced
on the host (GL, GLU, glut, SDL, udev, ogg, theora, PhysX loader) get thunk
addresses instead: the game's stamped GOT entries already point into their
arcade ranges, so those addresses are mapped to thunk names.

  python tools/recomp/image.py <game (patched)> <debs dir> [-o tools/recomp/out/image]

Writes image.bin (concatenated segments) and image.json:
  modules  [{name, base, segs:[[vaddr, file_off_in_bin, filesz, memsz, flags]],
             init, init_array, fini_array, eh_frame_hdr, tls}]
  thunks   {addr: name}   guest addresses that call host implementations
  irelative [[where, resolver]]  IFUNC slots to fill by running resolvers
  tls      {size, align, modules: {name: offset below tp}}
  entry    game entry point
"""
import argparse, json, os, struct, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import elfmod
from check_layout import LAYOUT

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..")

# libraries replaced by host code: name in got_fixups.txt -> arcade range
THUNKED = {
    "libGL.so.346.96": (0xb7439000, 0xb7558000),
    "libGLU.so.1.3.1": (0xb73c5000, 0xb7438000),
    "libglut.so.3.9.0": (0xb738b000, 0xb73c5000),
    "libSDL-1.2.so.0.11.4": (0xb7558000, 0xb75f0000),
    "libudev.so.1.3.5": (0xb730c000, 0xb731f000),
    "libogg.so.0.8.1": (0xb76ea000, 0xb76f3000),
    "libtheora.so.0.3.10": (0xb76f3000, 0xb7746000),
    "libPhysXLoader.so.1": (0xb76dd000, 0xb76ea000),
}
# load order = symbol lookup order (game first: it is linked -rdynamic)
ORDER = ["libstdc++.so.6.0.19", "libm-2.19.so", "libgcc_s.so.1", "libpthread-2.19.so",
         "libdl-2.19.so", "libc-2.19.so"]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("game")
    ap.add_argument("debs")
    ap.add_argument("-o", default=os.path.join(ROOT, "tools", "recomp", "out", "image"))
    args = ap.parse_args()

    game = elfmod.Mod(args.game, 0, "game")
    libs = {n: elfmod.Mod(os.path.join(args.debs, LAYOUT[n][0]), LAYOUT[n][1], n) for n in ORDER}
    mods = [game] + [libs[n] for n in ORDER]

    # guest memory: dict vaddr-page -> bytearray via segment buffers
    segbufs = []          # (mod, vaddr(abs), bytearray(memsz), flags)
    for m in mods:
        for va, off, fs, ms, fl in m.segs:
            b = bytearray(ms)
            b[:fs] = m.data[off:off + fs]
            segbufs.append([m, m.base + va, b, fl, fs])

    def locate(a):
        for s in segbufs:
            if s[1] <= a < s[1] + len(s[2]):
                return s
        raise KeyError(hex(a))

    def r32(a):
        s = locate(a)
        return struct.unpack_from("<I", s[2], a - s[1])[0]

    def w32(a, v):
        s = locate(a)
        struct.pack_into("<I", s[2], a - s[1], v & 0xffffffff)

    # static TLS (glibc i386: TLS_TCB_AT_TP, blocks below tp in load order)
    tls_off, tls = 0, {}
    tls_align = 16
    for m in mods:
        if m.tls:
            va, off, fs, ms, al = m.tls
            al = max(al, 1)
            tls_off = (tls_off + ms + al - 1) // al * al
            tls[m.name] = {"offset": tls_off, "vaddr": m.base + va, "filesz": fs, "memsz": ms}
            tls_align = max(tls_align, al)
    tls_size = (tls_off + tls_align - 1) // tls_align * tls_align

    # thunk addresses: stamped GOT entries of the game that point into
    # replaced libraries, named from got_fixups.txt
    thunks = {}
    for line in open(os.path.join(ROOT, "vr", "q0", "got_fixups.txt")):
        p = line.split()
        if len(p) < 3:
            continue
        slot, name, lib = int(p[0], 16), p[1], p[2].split(":")[-1]
        if lib in THUNKED:
            thunks[r32(slot)] = name
    # symbols that libraries import from replaced libraries get synthetic
    # thunk addresses in a reserved range
    SYN = 0xe0000000
    syn = {}

    def lookup(name, requester):
        if name in game.exports and game.exports[name]:
            return game.exports[name]
        for n in ORDER:
            a = libs[n].addr(name)
            if a is not None and libs[n].exports[name]:
                return a
        return None

    unresolved = {}
    irelative = []
    for m in mods[1:]:
        for off, typ, sym, addend in m.relocs:
            P = m.base + off
            if typ == elfmod.R_386_RELATIVE:
                w32(P, addend + m.base)
            elif typ == elfmod.R_386_IRELATIVE:
                irelative.append([P, m.base + addend])
            elif typ in (elfmod.R_386_32, elfmod.R_386_PC32, elfmod.R_386_GLOB_DAT, elfmod.R_386_JMP_SLOT):
                S = lookup(sym, m.name)
                if S is None:
                    if sym not in syn:
                        syn[sym] = SYN + 16 * len(syn)
                    S = syn[sym]
                    unresolved.setdefault(sym, []).append(m.name)
                if typ == elfmod.R_386_32:
                    w32(P, S + addend)
                elif typ == elfmod.R_386_PC32:
                    w32(P, S + addend - P)
                else:
                    w32(P, S)
            elif typ in (elfmod.R_386_TLS_TPOFF, elfmod.R_386_TLS_TPOFF32,
                         elfmod.R_386_TLS_DTPMOD32, elfmod.R_386_TLS_DTPOFF32):
                # symbol's module and value: local (sym None) -> this module
                if sym:
                    owner = next((x for x in mods if sym in x.exports and x.tls), m)
                    val = owner.exports.get(sym, 0)
                else:
                    owner, val = m, 0
                t = tls.get(owner.name)
                if t is None:
                    raise ValueError(f"TLS reloc in {m.name} for {sym}: no TLS block")
                if typ == elfmod.R_386_TLS_TPOFF:        # tp-relative, negative
                    w32(P, val + addend - t["offset"])
                elif typ == elfmod.R_386_TLS_TPOFF32:    # positive offset
                    w32(P, t["offset"] - val - addend)
                elif typ == elfmod.R_386_TLS_DTPMOD32:
                    w32(P, 1 + [x.name for x in mods].index(owner.name))
                else:
                    w32(P, val + addend)
            elif typ in (0,):
                pass
            else:
                raise ValueError(f"{m.name}: relocation type {typ} at {P:#x}")
    for name, a in syn.items():
        thunks[a] = name

    os.makedirs(args.o, exist_ok=True)
    out_mods = []
    with open(os.path.join(args.o, "image.bin"), "wb") as f:
        for m in mods:
            segs = []
            for s in segbufs:
                if s[0] is m:
                    segs.append([s[1], f.tell(), s[4], len(s[2]), s[3]])
                    f.write(s[2])
            dyn = m.dyn
            def arr(tag, sz_tag):
                if tag not in dyn:
                    return []
                return [r32(m.base + dyn[tag] + 4 * k) for k in range(dyn.get(sz_tag, 0) // 4)]
            out_mods.append({
                "name": m.name, "base": m.base, "segs": segs,
                "init": (m.base + dyn[12]) if 12 in dyn else None,
                "init_array": arr(25, 27), "fini_array": arr(26, 28),
                "eh_frame_hdr": (m.base + m.eh_frame_hdr) if hasattr(m, "eh_frame_hdr") else None,
                "tls": tls.get(m.name),
            })
    entry = struct.unpack_from("<I", game.data, 24)[0]
    json.dump({"modules": out_mods, "thunks": {f"{a:#x}": n for a, n in sorted(thunks.items())},
               "irelative": irelative, "tls": {"size": tls_size, "align": tls_align},
               "entry": entry, "unresolved_from_libs": unresolved},
              open(os.path.join(args.o, "image.json"), "w"), indent=1)
    print(f"modules: {', '.join(f'{m.name}@{m.base:#x}' for m in mods)}")
    print(f"thunks: {len(thunks)} ({len(syn)} synthetic for library imports of replaced libs)")
    print(f"IRELATIVE: {len(irelative)}; TLS: {tls_size} bytes {tls}")
    print(f"library imports resolved to nothing: {sorted(unresolved)[:40]}")


if __name__ == "__main__":
    main()

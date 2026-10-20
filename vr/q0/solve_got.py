#!/usr/bin/env python3
"""Solve the Sentinel-stamped 0xb7xxxxxx GOT/descriptor addresses against
Ubuntu 14.04 i386 library symbol tables.

Outputs vr/q0/got_fixups.txt:  <got_vaddr> <symbol> <lib>   per line.
Only slots that resolve unambiguously are emitted.
"""
import struct, glob, collections, sys, bisect, os

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "tools"))
import jp_paths  # noqa: E402
JP = str(jp_paths.ROOT)
GAME = jp_paths.pristine_game()

def load_dynsym(path):
    """ELF32 dynsym: {offset: name} using DT_HASH or DT_GNU_HASH for count."""
    d = open(path, "rb").read()
    if len(d) < 64 or d[:4] != b'\x7fELF': return {}
    phoff = struct.unpack_from("<I", d, 28)[0]
    phsz, phnum = struct.unpack_from("<HH", d, 42)
    loads = []; dyn = None
    for i in range(phnum):
        t, off, va, pa, fs = struct.unpack_from("<IIIII", d, phoff + i * phsz)
        if t == 1: loads.append((va, off, fs))
        if t == 2: dyn = (off, fs)
    def v2o(v):
        for va, o, s in loads:
            if va <= v < va + s: return o + v - va
        return None
    tg = {}
    for k in range(0, dyn[1], 8):
        tag, val = struct.unpack_from("<iI", d, dyn[0] + k)
        if tag == 0: break
        tg[tag] = val
    if 4 in tg and v2o(tg[4]) is not None:
        nch = struct.unpack_from("<II", d, v2o(tg[4]))[1]
    elif 0x6ffffef5 in tg and v2o(tg[0x6ffffef5]) is not None:
        go = v2o(tg[0x6ffffef5])
        nbuckets, symoffset, bloom_size, bloom_shift = struct.unpack_from("<IIII", d, go)
        buckets = struct.unpack_from(f"<{nbuckets}I", d, go + 16 + 4 * bloom_size)
        chain_off = go + 16 + 4 * bloom_size + 4 * nbuckets
        last = max([b for b in buckets if b] or [0])
        nch = symoffset
        if last >= symoffset:
            i = last - symoffset
            while True:
                h = struct.unpack_from("<I", d, chain_off + 4 * i)[0]
                i += 1
                if h & 1: break
            nch = symoffset + i
    else:
        return {}
    syms = {}
    for i in range(nch):
        o = v2o(tg[6]) + i * 16
        name, val, size, info = struct.unpack_from("<IIIB", d, o)
        if val == 0: continue
        so = v2o(tg[5]) + name
        e = d.index(b'\0', so)
        syms.setdefault(val, d[so:e].decode(errors='replace'))
    return syms

def elf_loads(path):
    d = open(path, "rb").read()
    phoff = struct.unpack_from("<I", d, 28)[0]
    phsz, phnum = struct.unpack_from("<HH", d, 42)
    out = []
    for i in range(phnum):
        t, off, va, pa, fs = struct.unpack_from("<IIIII", d, phoff + i * phsz)
        if t == 1: out.append((va, off, fs))
    return out

def main():
    libs = {}
    for pat in ["libc-*.so", "libpthread-*.so", "librt-*.so", "libdl-*.so",
                "libm-*.so", "ld-*.so", "libanl-*.so", "libnsl-*.so",
                "libnss_*-*.so", "libresolv-*.so", "libcrypt-*.so",
                "libutil-*.so", "libBrokenLocale-*.so"]:
        for f in glob.glob(JP + "/vr/q0/u1404/lib/i386-linux-gnu/" + pat):
            libs[f.split("/")[-1]] = load_dynsym(f)
    for f in glob.glob(JP + "/vr/q0/u1404libs/*.so.*"):
        if ".so." in f and not f.endswith((".so.6", ".so.1")):  # real files, not symlinks-copies
            libs[f.split("/")[-1]] = load_dynsym(f)
    # versioned variants from launchpad debs (e.g. 4 mesa builds; votes decide)
    for f in glob.glob(JP + "/vr/q0/debs/x/*/**/lib/i386-linux-gnu/**/*.so*", recursive=True):
        if os.path.islink(f): continue
        var = f.split("/x/")[1].split("_i386")[0]
        libs[var + ":" + f.split("/")[-1]] = load_dynsym(f)
    libs = {k: v for k, v in libs.items() if v}
    print("libs:", sum(len(v) for v in libs.values()), "symbols total", file=sys.stderr)

    # gather all stale slots with their GOT addresses
    loads = elf_loads(GAME)
    d = open(GAME, "rb").read()
    def v2o(v):
        for va, o, s in loads:
            if va <= v < va + s: return o + v - va
        return None
    slots = {}  # stale_value -> [got_vaddr]
    o = v2o(0x8adb000)
    vals = struct.unpack_from("<4640I", d, o)
    for i, v in enumerate(vals):
        if 0xb7000000 <= v < 0xb8000000:
            slots.setdefault(v, []).append(0x8adb000 + i * 4)
    o2 = v2o(0x90e73ec)
    dvals = struct.unpack_from("<162I", d, o2)
    for i, v in enumerate(dvals):
        if 0xb7000000 <= v < 0xb8000000:
            slots.setdefault(v, []).append(0x90e73ec + i * 4)
    stale = sorted(slots)
    print("unique stale addrs:", len(stale), file=sys.stderr)

    votes = collections.Counter()
    for A in stale:
        for ln, syms in libs.items():
            for off in syms:
                B = A - off
                if 0xb7000000 <= B < 0xb8000000 and B % 0x1000 == 0:
                    votes[(ln, B)] += 1
    best = {}
    for (ln, B), c in votes.items():
        if ln not in best or c > best[ln][1]:
            best[ln] = (B, c)
    # collapse variants: family = soname (part after ':'); keep highest-voted
    fam = {}
    for ln, (B, c) in best.items():
        fkey = ln.split(":")[-1]
        if fkey not in fam or c > fam[fkey][2]:
            fam[fkey] = (ln, B, c)
    order = sorted(fam.values(), key=lambda x: -x[2])  # resolve in vote order
    for ln, B, c in order:
        print(f"  {ln:44s} base={B:08x} votes={c}", file=sys.stderr)

    nres = 0
    unres_slots = []
    with open(JP + "/vr/q0/got_fixups.txt", "w") as f, \
         open(JP + "/vr/q0/got_fixups.h", "w") as h:
        h.write("/* generated by solve_got.py — slot, stale value, symbol */\n")
        h.write("typedef struct { unsigned int addr, stale; const char *sym; } GotFixup;\n")
        h.write("static const GotFixup GOT_FIXUPS[] = {\n")
        for A in stale:
            hit = None
            for ln, B, c in order:
                off = A - B
                if off in libs[ln]:
                    hit = libs[ln][off]; break
            if hit:
                for ga in slots[A]:
                    f.write(f"{ga:08x} {hit} {ln}\n")
                    h.write(f"  {{ 0x{ga:08x}, 0x{A:08x}, \"{hit}\" }},\n")
                nres += 1
            else:
                unres_slots.extend(slots[A])
        h.write("};\n")
        h.write(f"#define GOT_FIXUPS_N {sum(len(v) for v in slots.values()) - len(unres_slots)}\n")
        h.write("static const unsigned int GOT_UNRESOLVED[] = {\n")
        for ga in sorted(unres_slots):
            h.write(f"  0x{ga:08x},\n")
        h.write("};\n")
    print(f"resolved {nres}/{len(stale)} -> vr/q0/got_fixups.txt/.h", file=sys.stderr)
    print("unresolved:", [hex(a) for a in stale if all(a - B not in libs[ln] for ln, B, c in order)][:24], file=sys.stderr)

if __name__ == "__main__":
    main()

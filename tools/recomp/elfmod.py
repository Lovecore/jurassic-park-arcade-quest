"""ELF32 i386 shared-object reader for the recompiler's static linker.

Mod(path, base) parses program headers, the dynamic section, dynsym and
relocations (DT_REL + DT_JMPREL), and exposes:
  segs      [(vaddr, offset, filesz, memsz, flags)] (unrelocated)
  exports   {name: unrelocated value} for defined dynamic symbols
  imports   [names] of undefined dynamic symbols
  relocs    [(offset, type, symname or None, addend-in-place)]
  needed    DT_NEEDED names, soname
All addresses become guest addresses by adding `base`.
"""
import struct

R_386_32, R_386_PC32, R_386_GLOB_DAT, R_386_JMP_SLOT, R_386_RELATIVE = 1, 2, 6, 7, 8
R_386_TLS_TPOFF, R_386_TLS_DTPMOD32, R_386_TLS_DTPOFF32, R_386_TLS_TPOFF32 = 14, 35, 36, 37
R_386_IRELATIVE = 42


class Mod:
    def __init__(self, path, base=0, name=None):
        self.path, self.base = path, base
        self.name = name or path.replace("\\", "/").split("/")[-1]
        d = self.data = open(path, "rb").read()
        assert d[:4] == b"\x7fELF" and d[4] == 1, "not ELF32"
        e_phoff, = struct.unpack_from("<I", d, 28)
        e_phentsize, e_phnum = struct.unpack_from("<HH", d, 42)
        self.segs, dyn, self.tls = [], None, None
        for i in range(e_phnum):
            t, off, va, pa, fs, ms, fl, al = struct.unpack_from("<8I", d, e_phoff + i * e_phentsize)
            if t == 1:
                self.segs.append((va, off, fs, ms, fl))
            elif t == 2:
                dyn = (va, fs)
            elif t == 7:
                self.tls = (va, off, fs, ms, al)
            elif t == 0x6474e550:
                self.eh_frame_hdr = va
        self.dyn = {}
        self.needed = []
        if dyn:
            o = self.v2o(dyn[0])
            for k in range(dyn[1] // 8):
                tag, val = struct.unpack_from("<iI", d, o + 8 * k)
                if tag == 0:
                    break
                if tag == 1:
                    self.needed.append(val)
                else:
                    self.dyn.setdefault(tag, val)
        strtab, symtab = self.dyn.get(5), self.dyn.get(6)
        self.strsz = self.dyn.get(10, 0)

        def cstr(off):
            o = self.v2o(strtab) + off
            return d[o:d.index(b"\0", o)].decode("latin1")
        self.cstr = cstr
        self.needed = [cstr(n) for n in self.needed]
        self.soname = cstr(self.dyn[14]) if 14 in self.dyn else self.name
        # symbol count from DT_HASH nchain, or DT_GNU_HASH walk
        nsyms = 0
        if 4 in self.dyn:
            nsyms = struct.unpack_from("<I", d, self.v2o(self.dyn[4]) + 4)[0]
        elif 0x6ffffef5 in self.dyn:
            nsyms = self._gnu_hash_count(self.dyn[0x6ffffef5])
        self.syms = []
        for k in range(nsyms):
            name, value, size, info, other, shndx = struct.unpack_from("<IIIBBH", d, self.v2o(symtab) + 16 * k)
            self.syms.append((cstr(name), value, size, info, shndx))
        # symbol versioning: DT_VERSYM entries with bit 15 set are hidden
        # (non-default, compat) versions; the linker binds the default one
        versym = self.dyn.get(0x6ffffff0)
        hidden = [False] * len(self.syms)
        if versym is not None:
            o = self.v2o(versym)
            for k in range(len(self.syms)):
                hidden[k] = bool(struct.unpack_from("<H", d, o + 2 * k)[0] & 0x8000)
        self.exports = {}
        self.imports = []
        for k, (nm, value, size, info, shndx) in enumerate(self.syms):
            if not nm:
                continue
            if shndx == 0:
                self.imports.append(nm)
            elif (info >> 4) in (1, 2, 10):        # GLOBAL / WEAK / GNU_UNIQUE
                if not hidden[k]:
                    self.exports[nm] = value
                else:
                    self.exports.setdefault(nm, value)
        self.ifuncs = {nm for nm, v, s, info, sh in self.syms if (info & 0xf) == 10 and sh}
        self.relocs = []
        for tag_off, tag_sz in ((17, 18), (23, 2)):     # DT_REL/DT_RELSZ, DT_JMPREL/DT_PLTRELSZ
            if tag_off in self.dyn:
                o = self.v2o(self.dyn[tag_off])
                for k in range(self.dyn.get(tag_sz, 0) // 8):
                    off, info = struct.unpack_from("<II", d, o + 8 * k)
                    sym = info >> 8
                    self.relocs.append((off, info & 0xff, self.syms[sym][0] if sym else None,
                                        struct.unpack_from("<I", d, self.v2o(off))[0]
                                        if self.v2o(off) is not None else 0))

    def _gnu_hash_count(self, gh):
        d = self.data
        o = self.v2o(gh)
        nbuckets, symoffset, bloom_size, bloom_shift = struct.unpack_from("<4I", d, o)
        buckets = o + 16 + 4 * bloom_size
        chains = buckets + 4 * nbuckets
        last = max(struct.unpack_from(f"<{nbuckets}I", d, buckets) or [0])
        if last < symoffset:
            return symoffset
        while True:
            h = struct.unpack_from("<I", d, chains + 4 * (last - symoffset))[0]
            last += 1
            if h & 1:
                return last

    def v2o(self, v):
        for va, off, fs, ms, fl in self.segs:
            if va <= v < va + fs:
                return off + v - va
        return None

    def span(self):
        lo = min(s[0] for s in self.segs) & ~0xfff
        hi = max(s[0] + s[3] for s in self.segs)
        return lo, (hi + 0xfff) & ~0xfff

    def addr(self, name):
        v = self.exports.get(name)
        return None if v is None else (self.base + v) & 0xffffffff

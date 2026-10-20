"""ELF32 helpers for the GOT solver: full .symtab (debug files) and load extents."""
import struct


def _alias_key(name):
    """Prefer the public alias: no leading underscore, then shortest."""
    return (name.startswith("_"), len(name), name)


def load_symtab(path, funcs_only=False):
    """Return {st_value: name}.

    Exported .dynsym names win (they are what dlsym can find); among aliases
    at one address the public one is kept (close over __close). .symtab only
    fills addresses dynsym does not cover (IFUNC implementations, locals).
    """
    d = open(path, "rb").read()
    shoff = struct.unpack_from("<I", d, 32)[0]
    shentsize, shnum, shstrndx = struct.unpack_from("<HHH", d, 46)
    secs = [struct.unpack_from("<IIIIIIIIII", d, shoff + i * shentsize)
            for i in range(shnum)]
    out = {}
    for want in (11, 2):  # SHT_DYNSYM, then SHT_SYMTAB
        layer = {}
        for s in secs:
            if s[1] != want:
                continue
            strsec = secs[s[6]]
            for k in range(s[5] // 16):
                name, val, size, info, other, shndx = struct.unpack_from(
                    "<IIIBBH", d, s[4] + k * 16)
                if val == 0 or shndx == 0:
                    continue
                if funcs_only and info & 0xF not in (2, 10):
                    continue
                so = strsec[4] + name
                nm = d[so:d.index(b"\0", so)].decode(errors="replace")
                if nm and (val not in layer or _alias_key(nm) < _alias_key(layer[val])):
                    layer[val] = nm
        for val, nm in layer.items():
            out.setdefault(val, nm)
    return out


def load_extent(path):
    """Highest vaddr+memsz over PT_LOAD (size of the mapped image)."""
    d = open(path, "rb").read()
    phoff = struct.unpack_from("<I", d, 28)[0]
    phsz, phnum = struct.unpack_from("<HH", d, 42)
    hi = 0
    for i in range(phnum):
        t, off, va, pa, fs, ms = struct.unpack_from("<IIIIII", d, phoff + i * phsz)
        if t == 1:
            hi = max(hi, va + ms)
    return (hi + 0xFFF) & ~0xFFF


def ifunc_names(path):
    """Public STT_GNU_IFUNC names exported by a lib's dynsym."""
    d = open(path, "rb").read()
    shoff = struct.unpack_from("<I", d, 32)[0]
    shentsize, shnum, _ = struct.unpack_from("<HHH", d, 46)
    secs = [struct.unpack_from("<IIIIIIIIII", d, shoff + i * shentsize)
            for i in range(shnum)]
    out = set()
    for s in secs:
        if s[1] != 11:
            continue
        strsec = secs[s[6]]
        for k in range(s[5] // 16):
            name, val, size, info = struct.unpack_from("<IIIB", d, s[4] + k * 16)
            if info & 0xF == 10:
                so = strsec[4] + name
                out.add(d[so:d.index(b"\0", so)].decode())
    return out


def exported_names(path):
    """Defined .dynsym names (what dlsym can return)."""
    d = open(path, "rb").read()
    shoff = struct.unpack_from("<I", d, 32)[0]
    shentsize, shnum, _ = struct.unpack_from("<HHH", d, 46)
    secs = [struct.unpack_from("<IIIIIIIIII", d, shoff + i * shentsize)
            for i in range(shnum)]
    out = set()
    for s in secs:
        if s[1] != 11:
            continue
        strsec = secs[s[6]]
        for k in range(s[5] // 16):
            name, val, size, info, other, shndx = struct.unpack_from(
                "<IIIBBH", d, s[4] + k * 16)
            if shndx:
                so = strsec[4] + name
                out.add(d[so:d.index(b"\0", so)].decode(errors="replace"))
    return out

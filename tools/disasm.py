#!/usr/bin/env python3
"""Disassemble functions from the JP2015 'game_original' ELF by symbol name or address."""
import os, struct, sys, bisect
from capstone import Cs, CS_ARCH_X86, CS_MODE_32

BASE = os.environ.get("JP2015_GAME", os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "jp_game"))
# The TP-patched binary is fully devirtualized (Sentinel prologue calls
# replaced with real code) and is the one we run — analyze it, not the
# envelope-wrapped original. Symbol addresses are identical in both.
BIN = BASE + "/game"
SYMS = BASE + "/symbols_original.txt"

_data = open(BIN, "rb").read()
_e = lambda off, fmt: struct.unpack_from("<" + fmt, _data, off)
_phoff = _e(28, "I")[0]
_phentsize, _phnum = _e(42, "HH")
_segs = []
for i in range(_phnum):
    off = _phoff + i * _phentsize
    p_type, p_offset, p_vaddr, p_paddr, p_filesz = struct.unpack_from("<IIIII", _data, off)
    if p_type == 1:
        _segs.append((p_vaddr, p_offset, p_filesz))

def v2o(v):
    for va, o, sz in _segs:
        if va <= v < va + sz:
            return o + v - va
    return None

# load symbols: sorted addrs + name map
_addrs, _names = [], {}
for line in open(SYMS):
    parts = line.split(None, 2)
    if len(parts) == 3:
        a = int(parts[0], 16)
        _addrs.append(a)
        _names[a] = parts[2].rstrip("\n")
_addrs.sort()

def sym_at(a):
    return _names.get(a)

def sym_name(name):
    for a, n in _names.items():
        if n == name:
            return a
    return None

def nearest(a):
    i = bisect.bisect_right(_addrs, a) - 1
    if i >= 0:
        base = _addrs[i]
        return f"{_names[base]}+0x{a-base:x}" if a != base else _names[base]
    return hex(a)

def disasm(addr, size=None, name=None):
    if name:
        sy = sym_name(name)
        if sy is None:
            print(f"symbol not found: {name}")
            return
        addr = sy
    if size is None:
        i = bisect.bisect_right(_addrs, addr) - 1
        nxt = _addrs[i + 1] if i + 1 < len(_addrs) else addr + 256
        size = min(nxt - addr, 8192)
    off = v2o(addr)
    if off is None:
        print(f"addr 0x{addr:x} not mapped")
        return
    code = _data[off:off + size]
    md = Cs(CS_ARCH_X86, CS_MODE_32)
    md.detail = False
    for ins in md.disasm(code, addr):
        line = f"{ins.address:08x}  {ins.mnemonic:8s} {ins.op_str}"
        if ins.mnemonic.startswith("call") or ins.mnemonic.startswith("j"):
            try:
                tgt = int(ins.op_str, 0)
                s = sym_at(tgt)
                if s:
                    line += f"   ; {s}"
            except ValueError:
                pass
        print(line)
        if ins.mnemonic == "ret" and name:
            break

if __name__ == "__main__":
    args = sys.argv[1:]
    if not args:
        print("usage: disasm.py <symbol|0xaddr> [size]")
        sys.exit(1)
    if args[0].startswith("0x"):
        disasm(int(args[0], 16), int(args[1], 0) if len(args) > 1 else None)
    else:
        disasm(None, int(args[1], 0) if len(args) > 1 else None, name=args[0])

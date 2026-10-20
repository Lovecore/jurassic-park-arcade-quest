"""Map GOT slots -> PLT stubs -> call sites -> constant stack args.

Used to validate symbol hypotheses for stamped GOT slots: a slot named
glMatrixMode must be called with 0x1700..0x1702, glEnable with a valid cap,
and so on. Args are recovered from `mov dword ptr [esp + k], imm` in the
few instructions before each call (gcc -O2 i386 style).
"""
import collections
import struct

from capstone import CS_ARCH_X86, CS_MODE_32, Cs

import jp_paths

GAME = jp_paths.pristine_game()


class Game:
    def __init__(self, path=GAME):
        d = open(path, "rb").read()
        self.d = d
        phoff = struct.unpack_from("<I", d, 28)[0]
        phsz, phnum = struct.unpack_from("<HH", d, 42)
        self.loads = [struct.unpack_from("<IIIIII", d, phoff + i * phsz)
                      for i in range(phnum)]
        self.text = [(va, o, fs) for t, o, va, pa, fs, ms, *_ in
                     [x + (0,) for x in self.loads] if t == 1 and va == 0x8048000][0]

    def v2o(self, a):
        for t, o, va, pa, fs, ms in self.loads:
            if t == 1 and va <= a < va + fs:
                return o + a - va
        return None

    def plt_stubs(self, lo=0x8139700, hi=0x813c000):
        """slot -> stub vaddr for `ff 25 <slot>` jumps in the PLT."""
        out = {}
        o = self.v2o(lo)
        blob = self.d[o:o + (hi - lo)]
        i = blob.find(b"\xff\x25")
        while i >= 0:
            slot = struct.unpack_from("<I", blob, i + 2)[0]
            out.setdefault(slot, lo + i)
            i = blob.find(b"\xff\x25", i + 2)
        return out

    def call_sites(self, targets):
        """target vaddr -> [call-site vaddr] for e8 rel32 calls in .text."""
        va, o, fs = self.text
        blob = self.d[o:o + fs]
        tset = set(targets)
        out = collections.defaultdict(list)
        i = blob.find(b"\xe8")
        while i >= 0:
            if i + 5 <= len(blob):
                rel = struct.unpack_from("<i", blob, i + 1)[0]
                tgt = (va + i + 5 + rel) & 0xFFFFFFFF
                if tgt in tset:
                    out[tgt].append(va + i)
            i = blob.find(b"\xe8", i + 1)
        return out

    def const_args(self, site, window=40):
        """{arg_index: imm} from movs to [esp+4k] in the bytes before site."""
        md = Cs(CS_ARCH_X86, CS_MODE_32)
        best = {}
        for back in range(window, 0, -1):
            o = self.v2o(site - back)
            code = self.d[o:o + back]
            ins = list(md.disasm(code, site - back))
            if not ins or ins[-1].address + ins[-1].size != site:
                continue
            args = {}
            for x in ins:
                if x.mnemonic == "call":
                    args.clear()
                if x.mnemonic == "mov" and x.op_str.startswith("dword ptr [esp"):
                    dst, _, src = x.op_str.partition(", ")
                    k = 0
                    if "+" in dst:
                        k = int(dst.split("+")[1].rstrip("]").strip(), 0)
                    try:
                        args[k // 4] = int(src, 0)
                    except ValueError:
                        args.pop(k // 4, None)
            if len(args) > len(best):
                best = args
        return best

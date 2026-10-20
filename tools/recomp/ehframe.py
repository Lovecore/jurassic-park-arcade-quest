"""Parse .eh_frame_hdr / .eh_frame / LSDA (.gcc_except_table) of the game.

fdes(data, v2o, hdr_va) -> list of dicts:
  {"start", "end", "lsda", "landing_pads": [addr, ...], "cie_aug"}

Every compiled function with unwind info has an FDE (named or static), so
the FDE starts are the authoritative function list; landing pads are the
mid-function entry points used by C++ exception handling.
"""
import struct

DW_EH_PE_omit = 0xff


class Reader:
    def __init__(self, data, v2o, va):
        self.data, self.v2o, self.va = data, v2o, va

    @property
    def off(self):
        return self.v2o(self.va)

    def u8(self):
        v = self.data[self.off]
        self.va += 1
        return v

    def fmt(self, f, n):
        v = struct.unpack_from(f, self.data, self.off)[0]
        self.va += n
        return v

    def uleb(self):
        r = s = 0
        while True:
            b = self.u8()
            r |= (b & 0x7f) << s
            s += 7
            if not b & 0x80:
                return r

    def sleb(self):
        r = s = 0
        while True:
            b = self.u8()
            r |= (b & 0x7f) << s
            s += 7
            if not b & 0x80:
                if b & 0x40:
                    r -= 1 << s
                return r

    def enc(self, e, datarel_base=0, func_base=0):
        if e == DW_EH_PE_omit:
            return None
        at = self.va
        f = e & 0x0f
        if f == 0x00 or f == 0x03:
            v = self.fmt("<I", 4)
        elif f == 0x01:
            v = self.uleb()
        elif f == 0x02:
            v = self.fmt("<H", 2)
        elif f == 0x04:
            v = self.fmt("<Q", 8)
        elif f == 0x09:
            v = self.sleb()
        elif f == 0x0a:
            v = self.fmt("<h", 2)
        elif f == 0x0b:
            v = self.fmt("<i", 4)
        elif f == 0x0c:
            v = self.fmt("<q", 8)
        else:
            raise ValueError(f"pointer encoding {e:#x}")
        app = e & 0x70
        if app == 0x10:
            v += at
        elif app == 0x30:
            v += datarel_base
        elif app == 0x40:
            v += func_base
        elif app not in (0x00,):
            raise ValueError(f"pointer application {e:#x}")
        v &= 0xffffffff
        if e & 0x80:                       # indirect
            o = self.v2o(v)
            v = struct.unpack_from("<I", self.data, o)[0] if o is not None else None
        return v


def parse_lsda(data, v2o, lsda, func_start):
    r = Reader(data, v2o, lsda)
    lp_enc = r.u8()
    lpstart = r.enc(lp_enc) if lp_enc != DW_EH_PE_omit else func_start
    tt_enc = r.u8()
    if tt_enc != DW_EH_PE_omit:
        r.uleb()
    cs_enc = r.u8()
    cs_len = r.uleb()
    end = r.va + cs_len
    pads = []
    while r.va < end:
        start = r.enc(cs_enc)
        length = r.enc(cs_enc)
        lp = r.enc(cs_enc)
        r.uleb()                           # action
        if lp:
            pads.append((lpstart + lp) & 0xffffffff)
    return pads


def fdes(data, v2o, hdr_va):
    h = Reader(data, v2o, hdr_va)
    version = h.u8()
    eh_ptr_enc, cnt_enc, tbl_enc = h.u8(), h.u8(), h.u8()
    eh_frame = h.enc(eh_ptr_enc, datarel_base=hdr_va)
    count = h.enc(cnt_enc, datarel_base=hdr_va)
    entries = [(h.enc(tbl_enc, datarel_base=hdr_va), h.enc(tbl_enc, datarel_base=hdr_va))
               for _ in range(count)]
    cies = {}
    out = []
    for loc, fde_va in entries:
        r = Reader(data, v2o, fde_va)
        length = r.fmt("<I", 4)
        ptr_at = r.va
        cie_off = r.fmt("<I", 4)
        cie_va = ptr_at - cie_off
        cie = cies.get(cie_va)
        if cie is None:
            c = Reader(data, v2o, cie_va)
            c.fmt("<I", 4)
            c.fmt("<I", 4)                 # CIE id 0
            ver = c.u8()
            aug = b""
            while True:
                ch = c.u8()
                if not ch:
                    break
                aug += bytes([ch])
            c.uleb()
            c.sleb()
            c.u8() if ver == 1 else c.uleb()
            info = {"aug": aug.decode(), "fde_enc": 0, "lsda_enc": DW_EH_PE_omit}
            if aug.startswith(b"z"):
                c.uleb()
                for ch in aug[1:]:
                    if ch == ord("L"):
                        info["lsda_enc"] = c.u8()
                    elif ch == ord("R"):
                        info["fde_enc"] = c.u8()
                    elif ch == ord("P"):
                        c.enc(c.u8())
                    elif ch == ord("S"):
                        pass
            cie = cies[cie_va] = info
        start = r.enc(cie["fde_enc"])
        rng = r.enc(cie["fde_enc"] & 0x0f)
        lsda = None
        if cie["aug"].startswith("z"):
            r.uleb()
            if cie["lsda_enc"] != DW_EH_PE_omit:
                lsda = r.enc(cie["lsda_enc"])
        pads = parse_lsda(data, v2o, lsda, start) if lsda else []
        out.append({"start": start, "end": (start + rng) & 0xffffffff, "lsda": lsda,
                    "landing_pads": pads, "cie_aug": cie["aug"]})
    return out

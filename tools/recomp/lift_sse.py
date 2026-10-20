"""SSE/SSE2/MMX lifting for lift.py.

lift_sse(fl, ins, m) emits C for one SSE/MMX instruction or raises
Unsupported. Values go through rt_v128 temporaries: S (source), D (current
destination), R (result). MMX registers live in cpu->mm[] and use the low
8 bytes of a v128 (lane counts halve).
"""
from capstone import x86_const as X


class Unsupported(Exception):
    pass


def vreg(name):
    if name.startswith("xmm"):
        return ("xmm", int(name[3:]))
    if name.startswith("mm"):
        return ("mm", int(name[2:]))
    return None


def width(ins, op):
    """Register width in bytes of an xmm/mm operand (16 or 8); memory: op.size."""
    if op.type == X.X86_OP_REG:
        r = vreg(ins.reg_name(op.reg))
        if r:
            return 16 if r[0] == "xmm" else 8
    return op.size


def load(fl, ins, op, var, size=None):
    """C statements loading operand into rt_v128 var (zero-extended)."""
    if op.type == X.X86_OP_REG:
        name = ins.reg_name(op.reg)
        r = vreg(name)
        if r and r[0] == "xmm":
            if size in (4, 8):
                return (f"{var}.q[0] = 0; {var}.q[1] = 0; "
                        + (f"{var}.d[0] = cpu->xmm[{r[1]}].d[0];" if size == 4
                           else f"{var}.q[0] = cpu->xmm[{r[1]}].q[0];"))
            return f"{var} = cpu->xmm[{r[1]}];"
        if r and r[0] == "mm":
            return f"{var}.q[0] = cpu->mm[{r[1]}]; {var}.q[1] = 0;"
        return f"{var}.q[0] = 0; {var}.q[1] = 0; {var}.d[0] = {fl.reg_get(name)};"
    if op.type == X.X86_OP_MEM:
        a = fl.addr(ins, op)
        n = size or op.size
        if n == 16:
            return f"{var} = ld128({a});"
        if n == 8:
            return f"{var}.q[0] = rd64({a}); {var}.q[1] = 0;"
        if n == 4:
            return f"{var}.q[0] = 0; {var}.q[1] = 0; {var}.d[0] = rd32({a});"
        raise Unsupported(f"sse mem size {n}")
    if op.type == X.X86_OP_IMM:
        return f"{var}.q[0] = 0x{op.imm & 0xffffffffffffffff:x}ull; {var}.q[1] = 0;"
    raise Unsupported("sse operand")


def store(fl, ins, op, var, size=None):
    if op.type == X.X86_OP_REG:
        name = ins.reg_name(op.reg)
        r = vreg(name)
        if r and r[0] == "xmm":
            return f"cpu->xmm[{r[1]}] = {var};"
        if r and r[0] == "mm":
            return f"cpu->mm[{r[1]}] = {var}.q[0];"
        return fl.reg_set(name, f"{var}.d[0]")
    if op.type == X.X86_OP_MEM:
        a = fl.addr(ins, op)
        n = size or op.size
        if n == 16:
            return f"st128({a}, {var});"
        if n == 8:
            return f"wr64({a}, {var}.q[0]);"
        if n == 4:
            return f"wr32({a}, {var}.d[0]);"
    raise Unsupported("sse dest")


# name -> (lane field, elem bytes, expression of a (dst lane) and b (src lane))
LANE = {
    "addps": ("f", 4, "a + b"), "subps": ("f", 4, "a - b"), "mulps": ("f", 4, "a * b"),
    "divps": ("f", 4, "a / b"), "minps": ("f", 4, "sse_minf(a, b)"), "maxps": ("f", 4, "sse_maxf(a, b)"),
    "addpd": ("g", 8, "a + b"), "subpd": ("g", 8, "a - b"), "mulpd": ("g", 8, "a * b"),
    "divpd": ("g", 8, "a / b"), "minpd": ("g", 8, "sse_mind(a, b)"), "maxpd": ("g", 8, "sse_maxd(a, b)"),
    "andps": ("d", 4, "a & b"), "orps": ("d", 4, "a | b"), "xorps": ("d", 4, "a ^ b"), "andnps": ("d", 4, "~a & b"),
    "andpd": ("q", 8, "a & b"), "orpd": ("q", 8, "a | b"), "xorpd": ("q", 8, "a ^ b"), "andnpd": ("q", 8, "~a & b"),
    "pand": ("q", 8, "a & b"), "por": ("q", 8, "a | b"), "pxor": ("q", 8, "a ^ b"), "pandn": ("q", 8, "~a & b"),
    "paddb": ("b", 1, "a + b"), "paddw": ("w", 2, "a + b"), "paddd": ("d", 4, "a + b"), "paddq": ("q", 8, "a + b"),
    "psubb": ("b", 1, "a - b"), "psubw": ("w", 2, "a - b"), "psubd": ("d", 4, "a - b"), "psubq": ("q", 8, "a - b"),
    "paddusb": ("b", 1, "sat_u8((int)a + b)"), "paddusw": ("w", 2, "sat_u16((int)a + b)"),
    "psubusb": ("b", 1, "sat_u8((int)a - b)"), "psubusw": ("w", 2, "sat_u16((int)a - b)"),
    "paddsb": ("sb", 1, "sat_s8((int)a + b)"), "paddsw": ("sw", 2, "sat_s16((int)a + b)"),
    "psubsb": ("sb", 1, "sat_s8((int)a - b)"), "psubsw": ("sw", 2, "sat_s16((int)a - b)"),
    "pmullw": ("sw", 2, "(int16_t)((int32_t)a * b)"), "pmulhw": ("sw", 2, "(int16_t)(((int32_t)a * b) >> 16)"),
    "pmulhuw": ("w", 2, "(uint16_t)(((uint32_t)a * b) >> 16)"),
    "pcmpeqb": ("b", 1, "a == b ? 0xff : 0"), "pcmpeqw": ("w", 2, "a == b ? 0xffff : 0"),
    "pcmpeqd": ("d", 4, "a == b ? 0xffffffffu : 0"),
    "pcmpgtb": ("sb", 1, "a > b ? -1 : 0"), "pcmpgtw": ("sw", 2, "a > b ? -1 : 0"), "pcmpgtd": ("sd", 4, "a > b ? -1 : 0"),
    "pminub": ("b", 1, "a < b ? a : b"), "pmaxub": ("b", 1, "a > b ? a : b"),
    "pminsw": ("sw", 2, "a < b ? a : b"), "pmaxsw": ("sw", 2, "a > b ? a : b"),
    "pavgb": ("b", 1, "(uint8_t)(((unsigned)a + b + 1) >> 1)"), "pavgw": ("w", 2, "(uint16_t)(((unsigned)a + b + 1) >> 1)"),
}
UNARY = {"sqrtps": ("f", 4, "sqrtf(b)"), "sqrtpd": ("g", 8, "sqrt(b)"), "rsqrtps": ("f", 4, "1.0f / sqrtf(b)"),
         "rcpps": ("f", 4, "1.0f / b")}
CMPPS = {"cmpeqps": "a == b", "cmpltps": "a < b", "cmpleps": "a <= b", "cmpunordps": "a != a || b != b",
         "cmpneqps": "!(a == b)", "cmpnltps": "!(a < b)", "cmpnleps": "!(a <= b)", "cmpordps": "a == a && b == b"}
# scalar single/double arithmetic on the low lane
SCALAR = {"addss": ("f", "a + b"), "subss": ("f", "a - b"), "mulss": ("f", "a * b"), "divss": ("f", "a / b"),
          "minss": ("f", "sse_minf(a, b)"), "maxss": ("f", "sse_maxf(a, b)"), "sqrtss": ("f", "sqrtf(b)"),
          "addsd": ("g", "a + b"), "subsd": ("g", "a - b"), "mulsd": ("g", "a * b"), "divsd": ("g", "a / b"),
          "minsd": ("g", "sse_mind(a, b)"), "maxsd": ("g", "sse_maxd(a, b)"), "sqrtsd": ("g", "sqrt(b)")}
UNPACK = {"punpcklbw": ("b", 1, 0), "punpcklwd": ("w", 2, 0), "punpckldq": ("d", 4, 0), "punpcklqdq": ("q", 8, 0),
          "punpckhbw": ("b", 1, 1), "punpckhwd": ("w", 2, 1), "punpckhdq": ("d", 4, 1), "punpckhqdq": ("q", 8, 1),
          "unpcklps": ("d", 4, 0), "unpckhps": ("d", 4, 1), "unpcklpd": ("q", 8, 0), "unpckhpd": ("q", 8, 1)}
SHIFT = {"psrlw": ("w", 16, ">>", False), "psrld": ("d", 32, ">>", False), "psrlq": ("q", 64, ">>", False),
         "psllw": ("w", 16, "<<", False), "pslld": ("d", 32, "<<", False), "psllq": ("q", 64, "<<", False),
         "psraw": ("sw", 16, ">>", True), "psrad": ("sd", 32, ">>", True)}
MOVES128 = {"movaps", "movups", "movapd", "movupd", "movdqa", "movdqu", "lddqu", "movntps", "movntdq", "movntpd"}


def lift_sse(fl, ins, m):
    E = fl.emit
    ops = ins.operands
    if m == "emms":
        return
    if m in MOVES128:
        E(f"{{ rt_v128 S; {load(fl, ins, ops[1], 'S', 16)} {store(fl, ins, ops[0], 'S', 16)} }}")
        return
    if m in ("movq", "movd"):
        n = 8 if m == "movq" else 4
        if ops[0].type == X.X86_OP_MEM or (ops[0].type == X.X86_OP_REG and
                                            not vreg(ins.reg_name(ops[0].reg))):
            E(f"{{ rt_v128 S; {load(fl, ins, ops[1], 'S', n)} {store(fl, ins, ops[0], 'S', n)} }}")
        else:
            E(f"{{ rt_v128 S; {load(fl, ins, ops[1], 'S', n)} {store(fl, ins, ops[0], 'S')} }}")
        return
    if m in ("movss", "movsd"):
        n = 4 if m == "movss" else 8
        lane = "d[0]" if n == 4 else "q[0]"
        if ops[0].type == X.X86_OP_MEM:
            E(f"{{ rt_v128 S; {load(fl, ins, ops[1], 'S')} {store(fl, ins, ops[0], 'S', n)} }}")
        elif ops[1].type == X.X86_OP_MEM:
            E(f"{{ rt_v128 S; {load(fl, ins, ops[1], 'S', n)} {store(fl, ins, ops[0], 'S')} }}")
        else:
            E(f"{{ rt_v128 S, D; {load(fl, ins, ops[1], 'S')} {load(fl, ins, ops[0], 'D')} "
              f"D.{lane} = S.{lane}; {store(fl, ins, ops[0], 'D')} }}")
        return
    if m in ("movlps", "movlpd", "movhps", "movhpd"):
        hi = m.startswith("movh")
        if ops[0].type == X.X86_OP_MEM:
            E(f"{{ rt_v128 S; {load(fl, ins, ops[1], 'S')} wr64({fl.addr(ins, ops[0])}, S.q[{1 if hi else 0}]); }}")
        else:
            E(f"{{ rt_v128 D; {load(fl, ins, ops[0], 'D')} D.q[{1 if hi else 0}] = rd64({fl.addr(ins, ops[1])}); "
              f"{store(fl, ins, ops[0], 'D')} }}")
        return
    if m in ("movlhps", "movhlps"):
        a, b = ("1", "0") if m == "movlhps" else ("0", "1")
        E(f"{{ rt_v128 S, D; {load(fl, ins, ops[1], 'S')} {load(fl, ins, ops[0], 'D')} "
          f"D.q[{a}] = S.q[{b}]; {store(fl, ins, ops[0], 'D')} }}")
        return
    if m in LANE or m in UNARY or m in CMPPS:
        if m in CMPPS:
            field, eb, expr = "f", 4, f"({CMPPS[m]}) ? 0xffffffffu : 0"
        else:
            field, eb, expr = (LANE.get(m) or UNARY[m])
        w = width(ins, ops[0])
        n = w // eb
        src_size = 16 if w == 16 else 8
        res = f"R.{field}[k]" if m not in CMPPS else "R.d[k]"
        E(f"{{ rt_v128 S, D, R; int k; {load(fl, ins, ops[1], 'S', src_size)} {load(fl, ins, ops[0], 'D')} R = D; "
          f"for (k = 0; k < {n}; k++) {{ __typeof__(D.{field}[0]) a = D.{field}[k], b = S.{field}[k]; (void)a; "
          f"{res} = {expr}; }} {store(fl, ins, ops[0], 'R')} }}")
        return
    if m in SCALAR:
        field, expr = SCALAR[m]
        n = 4 if field == "f" else 8
        E(f"{{ rt_v128 S, D; {load(fl, ins, ops[1], 'S', n if ops[1].type == X.X86_OP_MEM else None)} "
          f"{load(fl, ins, ops[0], 'D')} {{ __typeof__(D.{field}[0]) a = D.{field}[0], b = S.{field}[0]; (void)a; "
          f"D.{field}[0] = {expr}; }} {store(fl, ins, ops[0], 'D')} }}")
        return
    if m in UNPACK:
        field, eb, high = UNPACK[m]
        w = width(ins, ops[0])
        n = w // eb               # lanes in the register
        half = n // 2
        base = half if high else 0
        E(f"{{ rt_v128 S, D, R; int k; {load(fl, ins, ops[1], 'S', 16 if w == 16 else 8)} {load(fl, ins, ops[0], 'D')} "
          f"R.q[0] = R.q[1] = 0; for (k = 0; k < {half}; k++) {{ R.{field}[2*k] = D.{field}[{base}+k]; "
          f"R.{field}[2*k+1] = S.{field}[{base}+k]; }} {store(fl, ins, ops[0], 'R')} }}")
        return
    if m in ("packssdw", "packsswb", "packuswb"):
        w = width(ins, ops[0])
        if m == "packssdw":
            src, dst, sat, n = "sd", "sw", "sat_s16", w // 4
        elif m == "packsswb":
            src, dst, sat, n = "sw", "sb", "sat_s8", w // 2
        else:
            src, dst, sat, n = "sw", "b", "sat_u8", w // 2
        E(f"{{ rt_v128 S, D, R; int k; {load(fl, ins, ops[1], 'S', 16 if w == 16 else 8)} {load(fl, ins, ops[0], 'D')} "
          f"R.q[0] = R.q[1] = 0; for (k = 0; k < {n}; k++) {{ R.{dst}[k] = {sat}(D.{src}[k]); "
          f"R.{dst}[{n}+k] = {sat}(S.{src}[k]); }} {store(fl, ins, ops[0], 'R')} }}")
        return
    if m == "pmuludq":
        w = width(ins, ops[0])
        n = 2 if w == 16 else 1
        E(f"{{ rt_v128 S, D, R; int k; {load(fl, ins, ops[1], 'S', 16 if w == 16 else 8)} {load(fl, ins, ops[0], 'D')} "
          f"R = D; for (k = 0; k < {n}; k++) R.q[k] = (uint64_t)D.d[2*k] * S.d[2*k]; {store(fl, ins, ops[0], 'R')} }}")
        return
    if m == "pmaddwd":
        w = width(ins, ops[0])
        n = w // 4
        E(f"{{ rt_v128 S, D, R; int k; {load(fl, ins, ops[1], 'S', 16 if w == 16 else 8)} {load(fl, ins, ops[0], 'D')} "
          f"R = D; for (k = 0; k < {n}; k++) R.sd[k] = (int32_t)((int64_t)D.sw[2*k] * S.sw[2*k] + "
          f"(int64_t)D.sw[2*k+1] * S.sw[2*k+1]); {store(fl, ins, ops[0], 'R')} }}")
        return
    if m == "psadbw":
        w = width(ins, ops[0])
        n = w // 8
        E(f"{{ rt_v128 S, D, R; int k, j; {load(fl, ins, ops[1], 'S', 16 if w == 16 else 8)} {load(fl, ins, ops[0], 'D')} "
          f"R.q[0] = R.q[1] = 0; for (k = 0; k < {n}; k++) {{ unsigned s = 0; for (j = 0; j < 8; j++) "
          f"s += D.b[8*k+j] > S.b[8*k+j] ? D.b[8*k+j] - S.b[8*k+j] : S.b[8*k+j] - D.b[8*k+j]; R.q[k] = s; }} "
          f"{store(fl, ins, ops[0], 'R')} }}")
        return
    if m in ("pshufd", "pshufw", "pshuflw", "pshufhw"):
        imm = ops[2].imm & 0xff
        if m == "pshufd":
            body = " ".join(f"R.d[{k}] = S.d[{(imm >> (2 * k)) & 3}];" for k in range(4))
            init = "R.q[0] = R.q[1] = 0;"
        elif m == "pshufw":
            body = " ".join(f"R.w[{k}] = S.w[{(imm >> (2 * k)) & 3}];" for k in range(4))
            init = "R.q[0] = R.q[1] = 0;"
        elif m == "pshuflw":
            body = " ".join(f"R.w[{k}] = S.w[{(imm >> (2 * k)) & 3}];" for k in range(4))
            init = "R = S;"
        else:
            body = " ".join(f"R.w[{4 + k}] = S.w[{4 + ((imm >> (2 * k)) & 3)}];" for k in range(4))
            init = "R = S;"
        w = width(ins, ops[0])
        E(f"{{ rt_v128 S, R; {load(fl, ins, ops[1], 'S', 16 if w == 16 else 8)} {init} {body} "
          f"{store(fl, ins, ops[0], 'R')} }}")
        return
    if m == "shufps":
        imm = ops[2].imm & 0xff
        E(f"{{ rt_v128 S, D, R; {load(fl, ins, ops[1], 'S', 16)} {load(fl, ins, ops[0], 'D')} "
          f"R.d[0] = D.d[{imm & 3}]; R.d[1] = D.d[{(imm >> 2) & 3}]; R.d[2] = S.d[{(imm >> 4) & 3}]; "
          f"R.d[3] = S.d[{(imm >> 6) & 3}]; {store(fl, ins, ops[0], 'R')} }}")
        return
    if m == "shufpd":
        imm = ops[2].imm & 3
        E(f"{{ rt_v128 S, D, R; {load(fl, ins, ops[1], 'S', 16)} {load(fl, ins, ops[0], 'D')} "
          f"R.q[0] = D.q[{imm & 1}]; R.q[1] = S.q[{(imm >> 1) & 1}]; {store(fl, ins, ops[0], 'R')} }}")
        return
    if m in ("psrldq", "pslldq"):
        n = ops[1].imm & 0xff
        n = 16 if n > 15 else n
        if m == "psrldq":
            body = f"for (k = 0; k < 16; k++) R.b[k] = k + {n} < 16 ? D.b[k + {n}] : 0;"
        else:
            body = f"for (k = 0; k < 16; k++) R.b[k] = k >= {n} ? D.b[k - {n}] : 0;"
        E(f"{{ rt_v128 D, R; int k; {load(fl, ins, ops[0], 'D')} {body} {store(fl, ins, ops[0], 'R')} }}")
        return
    if m in SHIFT:
        field, bits, op, arith = SHIFT[m]
        w = width(ins, ops[0])
        n = w * 8 // bits
        if ops[1].type == X.X86_OP_IMM:
            cnt = f"{ops[1].imm & 0xff}ull"
        else:
            cnt = "S.q[0]"
        pre = "" if ops[1].type == X.X86_OP_IMM else load(fl, ins, ops[1], 'S', 16 if w == 16 else 8)
        if arith:
            body = (f"for (k = 0; k < {n}; k++) R.{field}[k] = (c >= {bits}) ? (D.{field}[k] < 0 ? -1 : 0) "
                    f": (D.{field}[k] >> c);")
        else:
            body = f"for (k = 0; k < {n}; k++) R.{field}[k] = (c >= {bits}) ? 0 : (D.{field}[k] {op} c);"
        E(f"{{ rt_v128 S, D, R; int k; uint64_t c; (void)S; {pre} c = {cnt}; {load(fl, ins, ops[0], 'D')} R = D; "
          f"{body} {store(fl, ins, ops[0], 'R')} }}")
        return
    if m in ("cvtps2pd", "cvtpd2ps", "cvtdq2ps", "cvtdq2pd", "cvtps2dq", "cvttps2dq", "cvtpd2dq", "cvttpd2dq"):
        size = 8 if m in ("cvtps2pd", "cvtdq2pd") else 16
        body = {
            "cvtps2pd": "R.g[0] = S.f[0]; R.g[1] = S.f[1];",
            "cvtpd2ps": "R.q[0] = R.q[1] = 0; R.f[0] = (float)S.g[0]; R.f[1] = (float)S.g[1];",
            "cvtdq2ps": "for (k = 0; k < 4; k++) R.f[k] = (float)S.sd[k];",
            "cvtdq2pd": "R.g[0] = S.sd[0]; R.g[1] = S.sd[1];",
            "cvtps2dq": "for (k = 0; k < 4; k++) R.sd[k] = cvt_i32(S.f[k], 0);",
            "cvttps2dq": "for (k = 0; k < 4; k++) R.sd[k] = cvt_i32(S.f[k], 1);",
            "cvtpd2dq": "R.q[0] = R.q[1] = 0; R.sd[0] = cvt_i32(S.g[0], 0); R.sd[1] = cvt_i32(S.g[1], 0);",
            "cvttpd2dq": "R.q[0] = R.q[1] = 0; R.sd[0] = cvt_i32(S.g[0], 1); R.sd[1] = cvt_i32(S.g[1], 1);",
        }[m]
        E(f"{{ rt_v128 S, R; int k; (void)k; {load(fl, ins, ops[1], 'S', size)} {body} {store(fl, ins, ops[0], 'R')} }}")
        return
    if m in ("cvttsd2si", "cvtsd2si", "cvttss2si", "cvtss2si"):
        size = 8 if "sd" in m else 4
        lane = "S.g[0]" if size == 8 else "S.f[0]"
        tr = 1 if m.startswith("cvtt") else 0
        E(f"{{ rt_v128 S; {load(fl, ins, ops[1], 'S', size)} "
          f"{fl.reg_set(ins.reg_name(ops[0].reg), f'(uint32_t)cvt_i32({lane}, {tr})')} }}")
        return
    if m in ("cvtsi2sd", "cvtsi2ss", "cvtss2sd", "cvtsd2ss"):
        if m.startswith("cvtsi2"):
            v = fl.get(ins, ops[1], 4)
            lane = "D.g[0] = (double)(int32_t)" if m == "cvtsi2sd" else "D.f[0] = (float)(int32_t)"
            E(f"{{ rt_v128 D; {load(fl, ins, ops[0], 'D')} {lane}({v}); {store(fl, ins, ops[0], 'D')} }}")
        else:
            size = 4 if m == "cvtss2sd" else 8
            conv = "D.g[0] = S.f[0];" if m == "cvtss2sd" else "D.f[0] = (float)S.g[0];"
            E(f"{{ rt_v128 S, D; {load(fl, ins, ops[1], 'S', size)} {load(fl, ins, ops[0], 'D')} {conv} "
              f"{store(fl, ins, ops[0], 'D')} }}")
        return
    if m in ("ucomiss", "comiss", "ucomisd", "comisd"):
        size = 4 if m.endswith("ss") else 8
        lane = "f" if size == 4 else "g"
        E(f"{{ rt_v128 S, D; {load(fl, ins, ops[1], 'S', size)} {load(fl, ins, ops[0], 'D')} "
          f"FCOMI(D.{lane}[0], S.{lane}[0]); }}")
        return
    if m == "pmovmskb":
        E(f"{{ rt_v128 S; int k; uint32_t r = 0; {load(fl, ins, ops[1], 'S')} "
          f"for (k = 0; k < {width(ins, ops[1])}; k++) r |= (uint32_t)(S.b[k] >> 7) << k; "
          f"{fl.reg_set(ins.reg_name(ops[0].reg), 'r')} }}")
        return
    if m in ("movmskps", "movmskpd"):
        n, sh = (4, 31) if m == "movmskps" else (2, 63)
        fld = "d" if n == 4 else "q"
        E(f"{{ rt_v128 S; int k; uint32_t r = 0; {load(fl, ins, ops[1], 'S')} "
          f"for (k = 0; k < {n}; k++) r |= (uint32_t)(S.{fld}[k] >> {sh}) << k; "
          f"{fl.reg_set(ins.reg_name(ops[0].reg), 'r')} }}")
        return
    if m == "palignr":
        w = width(ins, ops[0])
        imm = ops[2].imm & 0xff
        E(f"{{ rt_v128 S, D, R; uint8_t t[32]; int k; {load(fl, ins, ops[1], 'S', 16 if w == 16 else 8)} "
          f"{load(fl, ins, ops[0], 'D')} memcpy(t, S.b, {w}); memcpy(t + {w}, D.b, {w}); R.q[0] = R.q[1] = 0; "
          f"for (k = 0; k < {w}; k++) R.b[k] = k + {imm} < {2 * w} ? t[k + {imm}] : 0; "
          f"{store(fl, ins, ops[0], 'R')} }}")
        return
    if m == "pshufb":
        w = width(ins, ops[0])
        E(f"{{ rt_v128 S, D, R; int k; {load(fl, ins, ops[1], 'S', 16 if w == 16 else 8)} {load(fl, ins, ops[0], 'D')} "
          f"R.q[0] = R.q[1] = 0; for (k = 0; k < {w}; k++) R.b[k] = (S.b[k] & 0x80) ? 0 : D.b[S.b[k] & {w - 1}]; "
          f"{store(fl, ins, ops[0], 'R')} }}")
        return
    if m == "ptest":
        E(f"{{ rt_v128 S, D; {load(fl, ins, ops[1], 'S', 16)} {load(fl, ins, ops[0], 'D')} "
          f"zf = ((D.q[0] & S.q[0]) | (D.q[1] & S.q[1])) == 0; cf = ((~D.q[0] & S.q[0]) | (~D.q[1] & S.q[1])) == 0; "
          f"of = sf = pf = 0; }}")
        return
    if m == "pcmpistri":
        imm = ops[2].imm & 0xff
        E(f"{{ rt_v128 S, D; {load(fl, ins, ops[1], 'S', 16)} {load(fl, ins, ops[0], 'D')} "
          f"ecx = rt_pcmpistri(&D, &S, {imm}, &cf, &zf, &sf, &of); pf = 0; }}")
        return
    if m == "pinsrw":
        v = fl.get(ins, ops[1], 2) if ops[1].type == X.X86_OP_MEM else fl.reg_get(ins.reg_name(ops[1].reg))
        lanes = 8 if width(ins, ops[0]) == 16 else 4
        E(f"{{ rt_v128 D; {load(fl, ins, ops[0], 'D')} D.w[{(ops[2].imm & 0xff) % lanes}] = (uint16_t)({v}); "
          f"{store(fl, ins, ops[0], 'D')} }}")
        return
    if m in ("sfence", "lfence", "mfence"):
        return
    if m in ("ldmxcsr",):
        E(f"cpu->mxcsr = rd32({fl.addr(ins, ops[0])});")
        return
    if m in ("stmxcsr",):
        E(f"wr32({fl.addr(ins, ops[0])}, cpu->mxcsr ? cpu->mxcsr : 0x1f80u);")
        return
    raise Unsupported(m)

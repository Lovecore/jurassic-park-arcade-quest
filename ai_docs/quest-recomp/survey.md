# Recompilation survey: `jp2015-analysis/game`

- executable segment 0x8048000..0x8977204 (9.6 MB); .plt None
- functions (symbols with size): **14638**, instructions **1607576**, 276 distinct mnemonics
- bytes covered by function symbols 6468232 / gaps between them 3237031
- indirect jmp 424 (jump tables 213), indirect call 2731
- calls into .plt 31498 to 521 distinct stubs
- functions using: indcall 1127, jumptable 192, mmx 9, rep 732, sse 300, tls 1109, x87 3191
- functions where linear decode stopped early: 9

## x87 mnemonics

fstp 43990, fld 38024, fxch 13701, fmul 13351, fst 6123, faddp 5604, fadd 5599, fldz 3713, fild 2903, fucomi 2854, fsub 2265, fucompi 2212, fld1 2194, fmulp 2098, fcompi 1406, fldcw 1372, fcomi 1305, fabs 1198, fsubr 1047, fdiv 1035, fsubp 997, fchs 996, fistp 992, fdivp 648, fdivr 607, fcmovbe 516, fnstcw 504, fsubrp 472, fcmovnbe 461, fsqrt 355, fdivrp 343, fcmovnb 178, fcmovb 144, fyl2x 101, fist 58, fnstsw 44, fldln2 36, fcmove 29, fcmovne 24, fldlg2 21, fucom 18, fucompp 16, frndint 15, fpatan 10, fsincos 10, fucomp 8, fldl2e 6, f2xm1 6, fscale 6, fcos 5, fiadd 4, fprem 2, fninit 1, ficom 1, fcom 1, fidivr 1, fisttp 1, fsin 1

## SSE mnemonics

movdqa 1210, movaps 867, movapd 722, movsd 452, movdqu 426, movhpd 290, mulpd 257, addps 252, movss 251, mulps 247, addpd 214, movhps 204, movd 197, movlps 193, movups 180, shufps 166, paddd 154, punpcklwd 151, xorps 146, movq 139, pshufd 138, unpcklpd 138, pxor 136, pminub 133, pmaxub 133, punpckhwd 124, punpcklbw 121, cvtdq2ps 74, cvtpd2ps 74, punpckldq 73, punpckhbw 72, packssdw 71, psrldq 70, xorpd 65, cvtps2dq 65, pmuludq 64, psrad 60, cvtdq2pd 56, movlhps 53, psubusw 53, subps 46, cvtps2pd 44, maxps 44, subpd 40, unpckhpd 36, movhlps 33, pand 32, pmulhw 30, psubd 29, minps 27, paddw 26, por 25, pmaddwd 24, pcmpgtd 24, movlpd 22, pmullw 20, punpcklqdq 19, packuswb 18, paddusw 18, pminsw 17, pmaxsw 17, unpcklps 16, pandn 14, pcmpgtw 12, psubw 12, andpd 10, paddsw 10, divpd 9, maxpd 9, andps 6, sqrtpd 5, psraw 5, pcmpgtb 4, divps 3, punpckhqdq 2, pslldq 2, psllw 2, sqrtps 2, punpckhdq 1

## Unusual / lifter-relevant

lock xadd 5069, seg gs 2214, rep rep stosd 641, rep repe cmpsb 563, lock add 434, rep rep movsd 234, cld 154, bswap 154, mmx:movq 121, mmx:paddq 71, shrd 69, rep rep movsb 60, mmx:movd 57, mmx:pxor 50, mmx:psrlq 46, mmx:pmuludq 25, bt 23, mmx:psllq 22, mmx:pand 15, out 14, pushfd 12, int3 12, rcr 12, sahf 10, popfd 9, rep repne scasb 8, cpuid 6, shld 6, mmx:por 6, bts 6, xchg 5, rdtsc 3, in 2, rep rep stosb 2, pushal 2, lds 2, hlt 1, lock cmpxchg 1, das 1, aaa 1, aad 1, daa 1, aam 1, les 1, seg fs 1, mmx:pcmpeqd 1, std 1, bsr 1

## Top 60 mnemonics

mov 639520, call 101379, lea 100446, test 62906, jmp 60779, add 60653, je 56588, cmp 53120, push 49958, fstp 43990, fld 38024, sub 34404, pop 33273, jne 29300, xor 21175, ret 16749, nop 15221, fxch 13701, fmul 13351, movzx 9601, and 8758, or 7339, jle 7328, jg 7126, fst 6123, shl 5915, jbe 5620, faddp 5604, fadd 5599, imul 5357, lock xadd 5069, ja 4072, fldz 3713, shr 3638, fild 2903, fucomi 2854, sar 2716, js 2564, jge 2472, fsub 2265, fucompi 2212, fld1 2194, jb 2187, fmulp 2098, jl 1988, leave 1681, inc 1632, fcompi 1406, jae 1403, fldcw 1372, fcomi 1305, movdqa 1210, fabs 1198, fsubr 1047, fdiv 1035, fsubp 997, fchs 996, fistp 992, movsx 921, cmove 899

## Early decode stops (first 25)

- 0x83db3c0 sha256_block_data_order: stopped at 0x83db6e5, next bytes c631a8c82703b0c7
- 0x83db800 sha512_block_data_order: stopped at 0x83dc15d, next bytes dbb5e938b548f35b
- 0x852ec60 XjtuvsQASGOovEo: stopped at 0x852ed83, next bytes 8f716770b3cc09b3
- 0x85350e0 BeItCX63ra5ghuK: stopped at 0x85352dc, next bytes c75f5029b121b259
- 0x8537190 K3Mlni1crGh2sse: stopped at 0x8537a48, next bytes c7525029b18bf181
- 0x8579b90 aL1SaApGKWRFOLl: stopped at 0x8579bfa, next bytes fe765a25aae0d45b
- 0x857bbc0 N3zncStBmlAwLFJ: stopped at 0x857bda1, next bytes ffffe7774b81f367
- 0x857dbf0 EeSOPA4XcOFrDTt: stopped at 0x857e0f1, next bytes c731b35f81f2e677
- 0x857fc90 uLGFLFALOG78YJB: stopped at 0x857fe0f, next bytes ffee19b3eb29527a

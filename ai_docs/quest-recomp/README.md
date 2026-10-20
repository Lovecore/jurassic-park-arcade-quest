# Quest 3 APK via static recompilation — plan and running log

Goal (user, 2026-10-20): complete the full decompile / recreation of the
game so it is playable on a Meta Quest 3 as an APK.

## Route decision

"Decompile / recreation" is done as a **static recompilation**: every x86
function of the devirtualized `game` is translated mechanically to C that
operates on an emulated i386 CPU state over guest memory, then compiled for
AArch64 Android together with a hand-written runtime that replaces the
Linux libraries the game imports. This is the approach of N64Recomp /
XenonRecomp-style projects. Hand-decompiling 1.6 M instructions into
readable source is not feasible; the recompiled C is the "recreation",
and individual subsystems can later be hand-rewritten in place (VR camera,
input, renderer) because the C is linkable function by function.

Why not only emulation (box64 + gl4es, VR_ENGINEERING_PLAN Track B): it
stays the fallback, but it carries JIT overhead on a phone-class CPU and
box32 cannot even start the binary yet (`box64-probe.md`). Recompiled code
runs at native speed and every platform call is under our control.

What this binary makes possible: it is non-PIE, fully devirtualized, and
has 18.7k symbols (-rdynamic), the import table is recovered (614 GOT
slots, `got-layout.md`), and the shim already reimplements the arcade
environment (dongle, RIO, paths).

## Size of the job (survey 2026-10-20, `survey.md`)

- code 0x8048000..0x8977204 (9.6 MB); 14,638 named functions, 1.61 M
  instructions, 276 distinct mnemonics
- 3.2 MB of code between named functions: static functions without
  symbols -> function discovery needed (R1)
- 31,498 calls to 521 import stubs (libc, libm, pthread, libstdc++, GL,
  GLU, glut, SDL 1.2, ogg/theora/vorbis, udev, PhysX loader)
- x87 floating point in 3,191 functions (the engine's math), SSE in 300,
  MMX in 9, `lock xadd` 5,069 (atomic refcounts), gs: TLS 2,214 (mostly
  the stack protector `gs:0x14`), 213 jump tables, 2,731 indirect calls
- statically linked third-party code: OpenCV, libxml2, zlib, OpenSSL
  (incl. asm with inline data), Sentinel HASP client (obfuscated; the
  shim emulates its API so these bodies need not run)

## Architecture

```
APK (arm64-v8a, OpenXR)
 ├─ jp2015_recomp.so     generated C: one C function per x86 function,
 │                        guest state struct, x87/SSE helpers
 ├─ runtime/             hand-written
 │   ├─ guest memory: the 32-bit address space reserved at a fixed base;
 │   │   image sections loaded at their link addresses (base + vaddr)
 │   ├─ import thunks: the 521 imports reimplemented over guest memory
 │   │   (libc/libm/pthread/libstdc++ ABI-correct for i386 layouts)
 │   ├─ GL: desktop GL 2.x/3.x compat calls -> GLES 3.2 (Adreno 740)
 │   ├─ SDL 1.2 surface: window/input/audio -> Android + OpenXR + AAudio
 │   ├─ arcade env from vr/shim: HASP + programs_dec, RIO, cabinet type
 │   └─ VR: VidOVR* implementations fed by OpenXR poses; eye images
 │       rendered into OpenXR swapchains
 └─ assets: the user's own dump, copied to the headset (never in the APK
    we would distribute; this is a personal build)
```

Development loop: the same generated C also builds for x86-64 Windows and
Linux, so most of the runtime can be brought up and debugged on this PC
against the known-good TeknoParrot run before touching the headset.

## Phases

- **R0 survey** — done (`tools/recomp/survey.py`, `survey.md`).
- **R1 function discovery + CFG**: recursive descent from symbols, call
  targets, jump tables, code pointers in data; complete function list with
  basic blocks; reconcile the 3.2 MB of unnamed code.
- **R2 lifter**: x86 -> C for the integer subset first, then x87, SSE/MMX,
  string ops, atomics; jump tables -> switch; indirect calls -> dispatch
  table lookup; per-function C files.
- **R3 runtime on Windows/Linux x86-64**: guest memory loader, import
  thunks for libc/libm/pthread/libstdc++, threads, TLS/stack protector;
  goal: recompiled game reaches its main loop with stub GL.
- **R4 graphics + platform**: GL through a real driver on the PC first,
  SDL 1.2 replacement, audio, input; goal: attract mode on the PC from
  recompiled code, compared frame by frame with TeknoParrot.
- **R5 Android/Quest**: NDK build, GLES translation, OpenXR session, VR
  render path (needs the mode-2 investigation from `ai_docs/windows-pc`),
  controller -> gun input; goal: attract in the headset.
- **R6 playable**: PhysX 2.8.1 (the game links it dynamically; TP ships the
  Linux libs; on Quest it must be recompiled too or replaced), audio mix,
  gun aim, performance on XR2 Gen 2.

Honest scale: R1-R3 are weeks of work, R4-R6 months. The phases each end
in something testable, and the VR render-path work on the TeknoParrot
build (`ai_docs/windows-pc`) feeds R5 directly.

## Status log

### 2026-10-20
- Plan written. R0 survey done: tool `tools/recomp/survey.py` (bounds
  code at `_IO_stdin_used`, detects PLT stubs by `ff 25` since the dump
  has no section headers). Output `survey.md`.
- **R1 function discovery done** (`tools/recomp/discover.py`, `ehframe.py`):
  19,778 functions (14,289 named, 2,475 from .eh_frame FDEs, 1,248 call
  targets, 898 tail-call targets, 612 code pointers in data, 256 orphans
  at prologues in holes); 11,081 FDEs parsed, 12,922 C++ landing pads;
  517 jump tables (`jmp [t+i*4]` and `mov r,[t+i*4]; jmp r`), 259
  unresolved indirect jmps (lifted as a dispatch over all block labels).
  Code range is 0x8139730..0x8977204 (header/dynsym/.plt below). What is
  left uncovered (113 KB) is HASP envelope code (not needed: the HASP
  API is emulated) and a few computed-goto regions in zlib/AES/LAPACK.
  Gotchas: the dump has no section headers; data symbols inside .text
  (`_fp_hw`) and bogus code pointers into the dynsym area had to be
  excluded.
- **R2 lifter working** (`tools/recomp/lift.py`, `rt/rt.h`): all 19,778
  functions -> C (50 files, ~290 MB), compiles with zero errors (i686
  clang -O1, ~5 min on 16 threads). 98.7% of instructions lifted; the
  remaining 20,958 are SSE/MMX (mostly OpenCV) and emit rt_unimpl.
  Model: guest regs and flags as C locals (flags never cross calls, so
  dead flag code is removed by the C compiler), real guest stack in
  guest memory, x87 as a stack of `rt_x87` (double; long double with
  -DRT_X87_LD on x86 hosts), imports through rt_import(id), indirect
  calls through a sorted function table.
- **Differential harness** (`tools/recomp/harness/`): a 32-bit Windows
  exe maps the patched game image at its link addresses (RT_MEM = 0) and
  runs each candidate natively and lifted from identical snapshots with
  random registers/args (pointers into a scratch buffer, small ints,
  kernel-half words; x87 functions get floats in +-100), comparing regs,
  data segment, scratch and stack. `run_all.sh` restarts past functions
  that kill the process (the original loading esp from garbage).
  Candidates (`candidates.py`): call tree free of imports, indirect
  calls, TLS, rdtsc and unlifted instructions: 5,622 functions.
  Result (8 trials each, seed 7), 80-bit build: **4,130 pass**, 1,488
  untested (every random input crashed the original), 1 process crash,
  **1 fail** = sub_08390640, whose only difference is the x87 sticky
  precision-exception bit (PE) left in eax by fnstsw (not modelled).
  Double build: same, plus precision-only deviations (catastrophic
  cancellation in cv::Moments, threshold compare in sub_085de3f0) that
  vanish at 80 bits. Bugs found and fixed on the way: missing labels for
  fall-through targets, EFLAGS AC/ID bits through pushfd/popfd (OpenSSL
  cpuid detection), `repz ret`.
  Harness lessons: reserve one 64 KB-aligned region for all PT_LOADs;
  code segment read-only; clear DF and restore the host FPU word after
  native runs; guard page + headroom under the guest stack; poison ebp
  region that points into itself; time native runs so lifted-slower is
  not a failure.

### Next (R2/R3)
- SSE/MMX lifting (OpenCV, some memcpy-like code), then structured inputs
  for the 1,488 untested functions (or trace-driven tests from a real
  TeknoParrot run: record a function's inputs live, replay lifted).
- Import strategy (decided): lift the open-source i386 libraries the game
  imports as well (libstdc++ 4.8.2, libgcc_s 4.9, libm/libc 2.19 pieces,
  libogg/libtheora, the PhysX 2.8 i386 libs TeknoParrot ships), so their
  internals keep the guest ABI (std::string layout, C++ exceptions via the
  real libgcc unwinder over the guest stack + .eh_frame). Host-side
  thunks only at the edges: libc syscall-level functions (files, time,
  memory, threads), GL (generated from the GL registry; pointer args are
  guest addresses + RT_MEM), SDL 1.2 (~42 functions -> host SDL2/Android),
  udev (stub), X11 (4 functions, stub).

### 2026-10-20 (cont.) — userland libraries and process model
- Downloaded the exact arcade library builds from Launchpad into
  `ext/debs/` (libc6 2.19-0ubuntu6.6, libgcc1 4.9-20140406-0ubuntu1,
  libstdc++6 4.8.2-19ubuntu1; i386). `tools/recomp/elfmod.py` reads ELF32
  .so files (dynsym with versioning and STB_GNU_UNIQUE, relocations, TLS).
- **Arcade layout verified** (`tools/recomp/check_layout.py`): placing the
  libraries at the bases from got-layout.md reproduces every stamped GOT
  entry of the dump: 355 exact + 28 IFUNC (CPU-specific implementation
  selected at load) = all 383 slots of those libraries. So if the
  recompiled process loads these libraries at the same bases, the frozen
  GOT is simply correct: no GOT repair needed.
- `tools/recomp/image.py` builds the relocated guest image (static
  linker: RELATIVE, 32, PC32, GLOB_DAT, JMP_SLOT, TLS TPOFF/DTPMOD/DTPOFF,
  IRELATIVE list, static TLS layout like glibc i386) — used for analysis
  (code pointers in relocated data seed function discovery). What it
  cannot resolve is ld.so state (`_rtld_global`, `___tls_get_addr`,
  `_dl_*`).
- **Decision: run the real dynamic linker, emulate the kernel.** Lift
  ld-2.19.so as well and start the process like the kernel does (map game
  + interpreter, auxv with AT_SYSINFO pointing at a syscall thunk, jump to
  ld.so's entry). ld.so does relocation, TLS, IRELATIVE, init order,
  dl_iterate_phdr (C++ unwinding) and dlopen exactly as on the cabinet.
  The emulated `mmap2` returns the arcade base when ld.so maps a known
  library file, so the lifted code (lifted at those addresses) and the
  stamped GOT both line up. Host code only at the edges:
  - Linux i386 syscalls (`call *%gs:0x10` / `int 0x80`): on the Quest this
    is mostly 1:1 to arm64 Linux syscalls with struct translation; on this
    Windows PC a ~60-syscall emulation for the dev loop. `__clone` is
    handled as a host function (new host thread running lifted code).
  - Replaced libraries: the game's DT_NEEDED entries for them are dropped
    in the guest image; the game's `dlopen`/`dlsym` (libdl GOT slots) are
    intercepted for those names and return thunk addresses (arcade
    addresses where the dump has them). GL thunks are generated from the
    GL registry; SDL 1.2 (42 functions) maps to the host platform layer.
  - ogg/theora (pure code) and the PhysX 2.8 i386 libs from TeknoParrot
    can be lifted like the other libraries.
- All modules discovered and lifted together (`lift.py --functions ...`,
  output `tools/recomp/out/c_all/`): game + ld-2.19.so (base b7760000,
  free choice) + libc/libm/libpthread/libdl/libgcc_s/libstdc++ (arcade
  bases) + librt (b7748000, free; the game lists it in DT_NEEDED but uses
  nothing from it). ~29.4k functions; unlifted instructions down to ~250,
  all in dead HASP envelope code or CPU paths the emulated cpuid hides
  (AVX/TSX/RDRAND). Game imports whose frozen GOT value is a lifted
  library function become direct C calls (36,702 call sites); the rest
  (replaced libraries, dlopen/dlsym/dlclose/dlerror) go through the
  runtime (`thunks.c` names them).
  Discovery additions for libraries: PIC jump tables (`mov r,[ebx+i*4+d];
  add r,ebx; jmp r`, ebx = DT_PLTGOT), GOT-relative code pointers
  (`lea r,[ebx+f@GOTOFF]`, IFUNC implementations), relocation-derived
  code pointers, computed-goto functions swept linearly over their FDE
  range (every instruction becomes a label: vfprintf, strftime...),
  orphans after alignment padding (SSE string variants).
  Lifter additions: SSSE3/SSE4.2 (`palignr`, `pshufb`, `ptest`, full
  `pcmpistri`), `tzcnt`/`popcnt`, x87 `fxam`/`fyl2xp1`/`fxtract`/`fprem1`/
  `fnstenv`/`fldenv`/`fnsave`, segment-register moves (gs = TLS selector),
  `int 0x80` -> rt_syscall with resume points (`cpu->resume_at`; a clone
  child enters libc's `__clone` right after its syscall).
  The arcade CPU picked SSSE3/SSE4.2 string routines for the IFUNC slots
  frozen in the dump (e.g. memcpy -> __memcpy_ssse3, strcmp -> SSE4.2),
  which is why those instructions had to be lifted.
- Harness re-run with SSE functions included (80-bit build, 8 trials,
  seed 7): **4,311 pass** (149 using SSE), 1 fail (the known PE status
  bit), 1,746 untested, 3 process crashes from random inputs.
- **Runtime (R3, Windows x64 dev host)** in `tools/recomp/rt/`:
  `kernel.c` (4 GB guest reservation, ELF loading of game + ld.so like
  execve, initial stack/auxv with AT_SYSINFO -> syscall entry, rt_call
  dispatch, emulated cpuid of an SSE4.2-class Intel CPU), `syscalls.c`
  (~90 Linux i386 syscalls: VFS with /pm -> game folder and /lib ->
  extracted libraries, files/dirs/stat64/getdents64, mmap2 placing each
  library at its arcade base, brk, time, clone -> host thread entering the
  resume point, set_thread_area/gs TLS per thread, futex on
  WaitOnAddress; dlopen/dlsym of replaced libraries -> fake handles and
  thunk addresses), `hostlibs.c` (SDL/GL/... host side; bring-up stub
  that logs each function once and returns 0). Build:
  `tools/recomp/build_process.sh` -> `out/process/jp2015.exe`.

### 2026-10-20 (cont.) — first boots of the recompiled process
- `out/process/jp2015.exe` (Windows x64) boots the lifted ld.so, which
  opens and maps every lifted library at its arcade base through the
  emulated syscalls, sets up TLS (set_thread_area + `mov gs`), applies
  relocations, runs library initializers (libpthread: set_tid_address,
  robust list, SIGCANCEL/SIGSETXID handlers, getrlimit, uname) and enters
  the game: `__libc_csu_init`, C++ static constructors, OpenSSL init,
  malloc via brk.
- Fixes found by booting:
  - DT_VERNEED entries naming dropped libraries -> unlinked by the loader
    (ld.so asserted in _dl_check_map_versions).
  - Game relocations bound to replaced libraries (glutBitmap8By13 COPY,
    four X11 JMP_SLOTs) -> neutralized by the loader; X11 slots get thunks.
  - Discovery seeds: DT_INIT/DT_FINI/entry/init_array (libpthread's _init
    is in .init before .text; the game's _init too), absolute code
    addresses used as instruction immediates (368 callbacks), relaxed
    data-pointer seeds (any 16-aligned code address that decodes cleanly).
  - Calls through a function pointer that is a game PLT stub -> rt_call
    follows `jmp [slot]`.
  - Computed-goto functions: one shared dispatch switch per function (a
    switch per indirect jump was quadratic: one libc file needed 7 GB
    and never finished compiling).
  - getpwuid_r pulled in NSS modules (libnss_compat, libnsl): the runtime
    writes /etc/nsswitch.conf with `files` only and libnss_files is lifted
    (base b7790000, free).
  - libogg/libtheora (exact arcade builds from Launchpad) verified at their
    arcade bases (all 33 frozen GOT entries match) and lifted instead of
    replaced.
- Tooling: `discover_all.sh`, `build_process.sh` (incremental: unchanged
  generated files keep their timestamps), `bringup.sh` (build, boot, add
  missing-function seeds per module under `tools/recomp/seeds/`, repeat),
  `gen_gl.py` (3,214 GL thunks from the Khronos registry; buffer-offset
  pointer arguments handled), `rt/hostlibs.c` (SDL 1.2 subset on Win32 +
  WGL, GL dispatch, GLU, glut; audio/joystick/udev/X11 report failure).
- Boot progress (Windows dev host, recompiled process): "Welcome to 1.08",
  window + GL context on the RTX 4080 (OpenGL 4.6 NVIDIA 610.88) through
  the host SDL layer, GL calls through the generated thunks, HASP dongle
  and RIO board emulated (`host_funcs.txt` -> `rt/hostgame.c`, ported
  from the shim; dongle seeded with cabinet type 4), `Boot proc start.`
  with many worker threads (clone -> host threads).
- Runtime bugs found on the way: `munmap` left pages committed and dirty,
  so a later anonymous mmap returned stale data (glibc's mmapped malloc
  chunks rely on zeroed prev_size) -> decommit on munmap; mmap address
  hints honoured only if the whole range is free; brk can shrink;
  SCHED_OTHER priority range 0..0 (pthread_create EINVAL otherwise);
  getdents (old) layout; getdents end sentinel passed to _findclose;
  host heap reserved at start; NSS limited to `files`; /etc, /proc/cpuinfo
  and the cabinet's g5/dino/pmuser -> /pm/pmuser link provided by the VFS.
- Lifter: lock-prefixed RMW and xchg-with-memory are host atomics
  (`__atomic_*`): guest threads run truly in parallel; guest memory
  accessors are volatile (no hoisting of spin-wait reads).
- **Non-local control flow (decided, 2026-10-20):** the game's task system
  (ProcLoop / ProcDoSleepWork) is a stack-copying coroutine scheduler built
  on `_setjmp` / `__longjmp_chk` (a task saves its context, copies its
  stack slice to the heap and longjmps to the scheduler, which later copies
  it back and longjmps in). The lifted code handles this, setjmp/longjmp in
  general and C++ landing pads with one mechanism:
  - a per-thread shadow stack of expected return addresses (pushed by
    every lifted call, popped after it returns); a `ret` whose guest return
    address does not match the top -> `rt_bounce`;
  - `rt_bounce` discards all host frames (`__builtin_longjmp` to the
    thread's trampoline) — safe because the whole guest state is in cpu +
    guest memory — and the trampoline re-enters the lifted function that
    contains the target through its resume switch;
  - every call-return site and landing pad is a resume point
    (`cpu->resume_at`, sorted `rt_resume_table`); a jump to one (e.g.
    longjmp's `jmp *edx`) also bounces.
  Host stack stays bounded; the guest's own stack copying works unchanged.

### 2026-10-20 — milestone: recompiled game in attract mode
- The statically recompiled process (game + ld.so + glibc/libm/libpthread/
  libdl/librt/libgcc_s/libstdc++/libogg/libtheora/libnss_files, all lifted
  to C and compiled for Windows x64) boots to attract mode: gamelog
  "Boot proc kill. / Entering attract mode.", runs until stopped, renders
  on the RTX 4080 through the host SDL/GL layer: high-score pages over
  the 3D jungle and the 3D waterfall/helicopter intro
  (`recompiled-attract-*.png`, read back from the game's GL back buffer
  with `JPRT_SHOT_EVERY=N`).
- What it took after the coroutine fix: the trampoline must use the CRT
  `_setjmp(buf, NULL)`/`longjmp` (restores callee-saved registers, no SEH
  unwind); `__builtin_setjmp` lost register state on x64. Clone children
  get their own shadow stack.
- Known gaps: no vsync (glXSwapIntervalSGI stub -> ~1,400 fps), no audio
  (SDL_OpenAudio / SDL_mixer fail), no input mapping yet (keyboard ->
  RIO/start/coin), PhysX loader slot returns 0, the window repaint path
  is unverified (frames verified via read-back only).

### 2026-10-20 (cont.) — PhysX, tooling, frame pacing, GL buffers
- PhysX: libPhysXLoader (0xb76de000), libPhysXCore (0xa0000000),
  libNxCooking, and libNxCharacter are now lifted at their arcade bases.
  discover.py no longer requires .eh_frame; it seeds from
  symbols, init/fini, and imm values. The "PhysX slot" in the frozen
  GOT is really `NxGetCookingLib`. Pointing it at TP's
  `NxCreatePhysicsSDK` wrote through a garbage 5th argument into libc
  `__exit_funcs`. JPRT_WATCH found it; the fix was loading the real
  loader at its base.
- Tooling: lifted C carries `#line <guest-offset> "<module>"`. Building
  with `-gline-tables-only` lets `symbolize.py` map a host pc to the guest
  instruction. Other runtime switches: `JPRT_WATCH=addr` (write
  watchpoint), `JPRT_PEEK`, `JPRT_SHOT_EVERY=N` (back-buffer BMP),
  `JPRT_FPSCAP`, and `JPRT_VSYNC`. The swap interval is applied after
  GL context creation, which gives about 60 fps.
- SDL 1.2 event layout fixed (keysym.sym at +8). Mouse events are
  delivered.
- glMapBuffer/glUnmapBuffer: one guest-memory shadow per bind target,
  copied with glBufferSubData on unmap. This fixed the "too many mapped
  buffers" fatal. A 150 s run is clean, and attract mode cycles through
  the title cards and the in-engine gun demo (lasers, targets, HUD).
- Still unimplemented: SDL_RWFromFile (audio path), and the glX query
  stubs (glXGetCurrentDisplay, glXQueryVersion, glXGetClientString).

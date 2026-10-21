# JP2015 Q0 boot — self-contained launch under emulation

Goal (set 2026-10-19): launch the Raw Thrills *Jurassic Park* 1.08 Linux build
reliably on AArch64 under emulation, with no host system dependencies, so the
same stack can be packaged into a Meta Quest 3 APK (see
`VR_ENGINEERING_PLAN.md`, Track B).

## Definition of done (Q0 boot)

1. One script turns the pristine TP `game` (sha256 `c18a9329...`, also kept
   as `game.q0bak`) into the runnable binary, checksummed, including every
   binary edit (preundef step etc.).
2. Runs from a pinned i386 rootfs + `vr_shim.so` (+ PhysX libs if found),
   nothing from the host.
3. In a clean container: boots to attract mode and stays up N minutes.
4. With `JPVR_TRAPABORT=1`: zero TRAP hits (return-0 traps are a probe only).
5. Frame dump shows rendering; FPS logged (Q0 perf gate, still unmeasured).

Booting is not the same as Quest-ready: the Q0 perf gate (>=30 fps) has not
been measured yet.

## Status log

### 2026-10-19

- Picked up from an earlier session (notes not kept in the repo).
  State there: boot crashed in the GOT-repair trap at slot `0x8add910`.
- Fixed shim build: `trap_log` was dropped as unused (only called from
  inline asm) -> `__attribute__((used, visibility("hidden")))`.
- Found that v1 GOT solver (`vr/q0/solve_got.py`) bound ~170 slots to the
  wrong function (all 107 libGL slots, stray ALSA/pulse/gconv/xcb hits).
  Wrote `vr/q0/solve_got2.py` with a pinned, evidence-backed arcade library
  layout -> see `got-layout.md`. Result: 614 slots fixed, 0 dlsym failures,
  1 PhysX slot stubbed, 4 data words left alone, 0 unresolved.
- Shim: GL names fall back to `glXGetProcAddressARB` (glvnd libGL does not
  export NVIDIA's full entry-point set); PhysX slot group; list terminator.
- Game now runs its own `main`: writes `g5/dino/pmuser/gamelog.txt`
  (needed `mkdir g5/dino/pmuser` — dir missing from the dump), stays up.
- Dongle: `DongleInitialize` failed (`Failed aldeen: 2`). HASP HL client
  API is statically linked; shim now emulates it in process (see
  `boot-fixes.md`). Dongle init passes.
- Further boot fixes, all in the shim (details in `boot-fixes.md`):
  realtime `pthread_create` fallback, synthetic `/proc/cpuinfo`, `/etc/udev`
  write redirect + `fclose(NULL)` guard, `R_386_COPY` repair (stdio + C++
  typeinfo held arcade values), crash reporter, sampling profiler.
- Runner image `jp2015-qemu` lacked Xvfb entirely (earlier "Xvfb" hits were
  files in the i386 rootfs); rebuilt with xvfb/x11-apps/imagemagick.
  `vr/q0/boot_qemu.sh` is the one-command boot test.
- Game now: GL context up (llvmpipe, GL 4.5 compat), "Welcome to 1.08",
  I/O board probe passes (RIO not connected, fine), resource loading runs.
- llvmpipe's JIT is unreliable under qemu-i386 (AVX2 detection, then
  crashes in JIT'd `glGenerateMipmap`); it also caused the mass shader/FBO
  failures and the imgLoad lockup. Test bed now uses softpipe
  (`GALLIUM_DRIVER=softpipe`, GL 3.3 compat): FBOs OK, only 4 real GLSL
  errors left (`gl_FragData` + `out_Bloom` in one fragment shader).
- Programs: `ResLoadSMProgramPath` runs `DongleDecrypt` (HASP AES) over
  each `data/programs_enc` file. The emulated `hasp_decrypt` now swaps in
  the matching `programs_dec` plaintext (1516 pairs, indexed by length +
  FNV-1a 64). Programs load; "Boot proc kill." = boot proc complete.
- **First rendered frame** (`first-frame-2026-10-19.png`): "Loading" +
  "RIO Disconnected" banner.
- Packaging: `vr/kit/` = self-contained kit (static qemu-i386 for AArch64,
  minimal i386 bookworm rootfs from `Dockerfile.rootfs`, shim, `run.sh`,
  dd-based `patch_game.sh`). `vr/kit/build_kit.sh` builds it;
  `vr/kit/test/test_kit.sh` runs it on `jp2015-cleanhost` (plain arm64
  Debian + Xvfb, no qemu, no i386 libs). 329 MB, no game files inside.
- RIO emulation, swap-interval no-op, GLSL rewrite (now code-only and
  version-aware: 0 compile failures), shell-outs skipped.
- "Loading" forever was not slowness (1.56 GB read in the first 45 s, then
  idle): `BootProcKill` sends cabinet type 0 to `GameDiagStart()`. Fixed by
  seeding the emulated dongle with cabinet type 4 (upright) and restoring
  the `g5/dino/pmuser -> ../../pmuser` link (cabinet's real saves).
- `paths.c`: `/pm/` mapped for all path-taking imports (qemu -L cannot
  create new files under /pm).
- **Attract mode reached** (`attract-2026-10-19.png`, "PRESS START TO
  BEGIN"), from the kit on the clean host. Log: "Entering attract mode.",
  no errors, saved adjustments accepted.
- Perf data: game logic + trivial scene ~70 fps under qemu; full 3D attract
  0.06 fps (16 s/frame) because GL is software-rasterized inside the
  emulated x86 process. The Q0 gate needs a GPU path; see
  `quest-readiness.md`.

- Soak (20 min, clean host): stays up and the attract cycle animates
  (`attract-3d-2026-10-19.png`), but after ~19 min a later attract page
  hits `imgLoad` lockup 1169 (compressed-texture proxy check) under
  softpipe. Formats seen: DXT1 0x83F0, DXT5 0x83F3, LATC1 0x8C70, LATC2
  0x8C72; the refusing call is still to be caught (gl_compat logs
  "proxy refused" with all three proxy values). `JPVR_OVERLAY` (asset
  overrides, paths.c) lets a test reorder pages via atorder.txt.
- box64/box32 probe for native GL: see `box64-probe.md`. Loader accepts
  the binary but jumps to a stamped GOT entry before any library
  constructor (and preload crashes box64), so the GOT repair has to move
  into the loader path. Next phase.

## Gotchas

- `cp`/`rm` are aliased interactive in this shell: use `/bin/cp -f`,
  `/bin/rm -f` (a silent "not overwritten" deployed a stale shim once).
- `grep` is a shell function here; use `/usr/bin/grep` in scripts.
- REA/Ghidra snapshot `vr/ghidra/game.snapshot` is of the pristine binary
  (`game.q0bak`), not the current preundef-patched `game`; point REA at
  `game.q0bak` (`vr/ghidra/decompile_funcs.sh` does).

## Open questions for the user

- Original arcade drive image or its `/usr/lib` (exact NVIDIA 346.x libGL,
  libPhysXLoader / PhysX 2.8.1 core libs) would replace guesswork.

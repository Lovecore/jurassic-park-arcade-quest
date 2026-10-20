# box64 / box32 perf-path probe (2026-10-19)

Goal: run the game with native AArch64 GL (box32 wraps libGL/GLU/GLX, SDL1,
X11, udev, ogg/vorbis, libc/libm/pthread/dl/rt for 32-bit guests) instead
of software GL inside qemu-i386, which gives 0.06-0.1 fps in 3D attract.

## Setup

- `vr/box64/Dockerfile`: box64 v0.4.5 (75ff158, 2026-10-19) built with
  `-DARM_DYNAREC=ON -DBOX32=ON` -> image `jp2015-box64`.
- `vr/box64/Dockerfile.run`: arm64 bookworm + native Mesa/SDL1.2/X11/udev
  + Xvfb -> image `jp2015-box64run`.
- `vr/box64/probe.sh`: i386-only libs from the kit rootfs via
  `BOX64_LD_LIBRARY_PATH`, patched binary from `vr/kit/test/state/game`.

## Findings

1. `BOX64_LD_PRELOAD=vr_shim.so` crashes box64 itself: NULL deref in
   `old_elf_lookup` (`src/elfs/elfhash32.c:83`) from `RelocateElfREL` while
   relocating the preloaded lib. Not caused by the shim's hash style (still
   crashes with `--hash-style=both`; the shim is now built that way anyway).
2. Without preload, box64 maps the game (non-PIE at 0x8048000) and jumps
   straight to `RIP=0xb715a990`, a stamped arcade libc address: the entry
   path calls `__libc_start_main` through a GOT slot that no relocation
   covers (the Sentinel envelope stripped them; see got-layout.md). box64
   never logs loading the game's DT_NEEDED libs or running library
   constructors before that jump, so the shim (also tried as
   `libPhysXLoader.so.1`, a DT_NEEDED with no used symbols) never runs.
3. box64's loader otherwise accepts the binary (no parse errors).

## What it would take

- Teach the box64 32-bit loader to (a) load DT_NEEDED + run constructors
  before entry for this binary, and (b) apply the GOT repair itself, e.g. a
  small box64 patch that reads got_fixups.h-style data, or a box64 option
  to run a preload's constructor first. The repair data is already
  computed (614 slots), so this is plumbing, not new reverse engineering.
- Alternative: emit a repaired binary file (write the 614 resolved slots
  as proper R_386_JUMP_SLOT relocations + a real .rel.plt). Harder, but
  then every loader (glibc, box64, FEX) handles it without a shim step.
- FEX-Emu (32-bit guest + GL thunks) is the other candidate to evaluate.

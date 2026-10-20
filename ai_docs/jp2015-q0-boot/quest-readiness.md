# Quest 3 readiness (2026-10-19)

## Ready (carries over unchanged)

- **Binary preparation**: pristine TP binary -> runnable binary via
  `patch_game.sh` (sh + dd + sha256sum; all present in Android toybox).
- **Shim** (`vr/shim/`, i386 `vr_shim.so`): every arcade-environment
  dependency is answered in process: GOT repair, HASP dongle + program
  decryption, RIO I/O board, root-only paths, /proc/cpuinfo, realtime
  threads, shell-outs, NVIDIA-only GLSL, swap interval. Nothing on the host
  has to imitate the cabinet.
- **Filesystem**: no root, no bind mounts. `/pm` comes from the rootfs
  symlink for reads and from `paths.c` for writes; saves live in the game
  folder's own `pmuser/`.
- **Kit layout**: AArch64 static emulator + i386 rootfs + launcher. Runs
  from any writable directory; an APK can carry the same tree in its
  private data dir (needs targetSdk <= 28 to exec from app storage, as
  planned for the dynarec anyway).
- **Hook points for VR** already exist:
  - frame present: `SDL_GL_SwapBuffers` hook (gl_compat.c) -> frame
    capture / eye submission
  - inputs: RIO report buffer (io_emu.c header documents the layout) ->
    controller buttons; gun aim via io_gyrogun (next to map)
  - VR render path: VidOVR stubs + mode-2 patch (shim.c, JPVR_ENABLE=1)

## Not ready (ordered by risk)

1. **Performance / GPU path (blocking).** Under qemu-i386 the GL driver is
   emulated x86 Mesa doing software rasterization: 0.06 fps in 3D attract.
   The game logic itself is not the problem (~70 fps on a trivial scene
   under plain TCG). Required: run GL natively, i.e. box64's box32 mode
   with libGL wrapped to the native AArch64 driver (Quest: Adreno GLES via
   gl4es or Zink/Turnip), or a GL-forwarding layer. This is the Q0 perf
   gate in `VR_ENGINEERING_PLAN.md` and the next work item. First box32
   probe: `box64-probe.md` (GOT repair must run inside the loader path).
2. **Display**: today an X server (Xvfb/Termux:X11). The APK needs either
   an embedded X server or an offscreen GL context (SDL/GLX replacement in
   the shim) feeding the OpenXR swapchain.
3. **PhysX 2.8.1**: `NxCreatePhysicsSDK` returns NULL. Attract runs; gameplay
   physics untested. Needs the original libPhysXLoader/PhysXCore (i386).
4. **Audio**: SDL dummy driver; JPS "DEV_DSP" sound system fails to init.
5. **Gun input**: io_gyrogun path not yet emulated; START/coin via RIO
   report buffer not yet wired.
6. **Attract cycle lockup** after ~19 min under softpipe (imgLoad 1169 on a
   later page); may be softpipe-specific, recheck on the GPU path.
7. **Rootfs size**: 319 MB, mostly Mesa/LLVM for software GL; shrinks once
   GL is native.

## Verification recipe

```
vr/kit/build_kit.sh                    # kit from source (Docker on the build box)
vr/kit/test/test_kit.sh 600            # clean arm64 host, screenshots in vr/kit/test/shots
```
Expect in `<game>/pmuser/gamelog.txt`: "Entering attract mode." and in
`vr/kit/test/vr_shim.log`: 614 GOT fixes, 0 dlsym failures, RIO connected,
cabinet type 4, fps lines.

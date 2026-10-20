# JP2015 Native VR — Engineering Plan

Goal: run the Raw Thrills *Jurassic Park* (1.08, 2015, Linux x86) arcade build
natively in VR on a modern headset, using the engine's own compiled-out VR path.

**Target platform (2026-10-16): Meta Quest standalone APK.** Track A (PC-host
daemon) is on hold. See "Track B — Quest standalone APK" below; the recon
findings and the VidOVR shim work apply unchanged to both tracks.

## Key recon findings (2026-10-11)

- Binary: `game` (TP-patched) / `game_original`, 32-bit x86 ELF, non-PIE,
  linked `-rdynamic` → 18,702 internal symbols exported
  (`symbols_original.txt`, analysis tool: `tools/disasm.py`).
- Protected with Sentinel (HASP) Envelope (`comaladdinhlx86envspoony`);
  garbled strings + trampoline farm at 0x9044000–0x904a8b0 are the protector's.
  TP patch only changed entry/init; all VR-relevant code is byte-identical
  between `game` and `game_original` (verified).
- The g5 engine has a **complete built-in VR render path**, compiled out via
  `ENABLE_OCULUSVR 0` (see `g5/g5engine/src/g5/OVR_CWrapper.h`):
  - `shaderOVR` (g5/shaders/shaderOVR/) — classic DK1/DK2-era barrel warp +
    chromatic aberration, driven by LensCenter/ScreenCenter/Scale/ScaleIn/
    HmdWarpParam/ChromAbParam uniforms. `shaderOVRSetupFunc` (0x831f7f0) is real.
  - `gRenderLayerSetupDataOVR` (0x8b10480) — stereo render-layer table,
    selected by `RenderLayersInit` (0x8300030) when display mode == 2.
  - Display-mode global: `VidDisp+0x6a4` = **0x8e7b3e4**. 0=normal, 1=multi-screen
    (`-g` flag), 2=OVR. Read by RenderLoop, RenderLayerRender,
    RenderStateStart, RenderState_SetRenderTarget, camPersp, UpdateCam,
    ObjCamBuildMatrices, CamUnProject, ScreenHitFX, etc. — all real code.
- What is missing: only the **Vid-level VR glue**, compiled to `repz ret`
  stubs at 0x835dfa0–0x835e010, plus the `OVR_*` C wrappers (envelope
  trampolines, effectively no-ops):

  | Addr       | Called from                                   | Role (inferred)                  |
  |------------|-----------------------------------------------|----------------------------------|
  | 0x835dfa0  | GameSysStart+0x3d8 (mode==2 init)             | VidOVRInit → OVR_Init            |
  | 0x835dfb0  | g5_Final+0xd0                                 | VidOVRShutdown → OVR_Shutdown    |
  | 0x835dfc0  | GameSysStart+0x3ec (&w,&h)                    | VidOVRGetResolution              |
  | 0x835dfd0  | UpdateCam+0x83d, ObjCamBuildMatrices+0x82d, CamBuildReflectionMatrices+0xb17 | VidOVRGetProjParams (yfov, aspect, offset) |
  | 0x835dfe0  | RenderLayerSetScreenCam+0x156 (`fldz;ret`)    | float return — IPD or scale      |
  | 0x835dff0  | RenderLayerSetScreenCam+0x179                 | likely orientation/pose          |
  | 0x835e000  | shaderOVRSetupFunc+0xf8                       | VidOVRGetSConfigParams (warp uniforms) |

- Activation: `GameSysStart` (0x81bdd10) default-writes mode=0 at **0x81bde0a**
  (`c7 05 6c 4e db 08 00 00 00 00`; imm32 at 0x81bde10). `-g` handler
  (0x81b9d18) writes mode=1 only; no CLI flag yields 2 in this build.
  Mode==2 branch (0x81be0e8) calls VidOVRInit + VidOVRGetResolution, stores
  mode to 0x8e7b3e4, recomputes aspect from HMD res.

## Architecture

```
 [32-bit game proc]                          [64-bit host]
 game (+ LD_PRELOAD vr_shim.so)              vr_daemon (OpenXR/Monado)
   - ctor patches stubs -> our code    SHM    - head pose -> ring buffer
   - VidOVR* impls read pose from SHM <====>  - receives SBS frames
   - swap-hook: glReadPixels -> SHM   ====>   - submits eye layers to OpenXR
```

- 32-bit constraint: modern OpenXR runtimes are 64-bit; in-proc loading is
  impossible → split into in-process shim (patching + capture) and 64-bit
  daemon (runtime + compositor), bridged by SHM/UDP.
- Distortion: prefer identity HmdWarpParam in the engine and let OpenXR apply
  the HMD's native distortion (avoids double warp). Alternative: feed real
  DK-style params and display the pre-warped frame raw — fallback only.

## Track B — Quest standalone APK (2026-10-16)

### Reality check

- `game` is a 32-bit x86 glibc ELF (Ubuntu 14.04 era). DT_NEEDED:
  libSDL-1.2, libGL/libGLU/libglut (desktop GL via **GLX/X11**, GLEW
  embedded), libtheora/libogg/libvorbisfile, libPhysXLoader (stub-able),
  libudev (gun hardware, stub-able), libX11, libc/libstdc++/pthread/rt/dl/m.
- Quest = Android (Horizon OS), AArch64, GLES3/Vulkan, OpenXR. No x86, no
  X11, no desktop GL, no glibc. No source code available.
- Therefore "port to APK" = **emulation + graphics/display bridge**, not a
  recompile. Options ranked:

  | Option | Perf | Effort | Verdict |
  |---|---|---|---|
  | box64 `box32` dynarec + i386 rootfs | good (JIT) | medium | **primary** |
  | qemu-i386 user-mode + i386 rootfs | poor (interpreter/TCG) | low | fallback / compat baseline |
  | Static recompilation x86→ARM64 | native | months–years (moonshot; feasible only because the binary is devirtualized w/ 18.7k symbols) | long-term option |
  | PC renders, Quest streams (Virtual Desktop style) | n/a | medium | defeats "standalone" goal — reject unless emulation perf fails |

- Key precedent: Winlator / GameHub / Boxvidra ship box64 + rootfs + GL
  bridge inside sideloaded APKs on Android. They target Windows/Wine; ours is
  the same architecture with a plain i386 Linux userland (simpler — no Wine).
- box86 (armhf host) is NOT usable: Quest 2/3 CPUs run AArch64 userspace and
  the big cores lack AArch32 EL0. box64's `box32` mode runs i386 guests from
  an arm64 process — that is the correct emulator. qemu-i386 works everywhere
  but ~5–10× slower.

### Target architecture (single APK)

```
APK (sideloaded, targetSdk 27 → SELinux untrusted_app_27 allows execmem
     for the dynarec; personal/sideload build, store rules don't apply)
├─ OpenXR Activity (arm64 native) — owns the headset
│    - creates OpenXR session, 2 eye swapchains
│    - reads head pose each frame → writes to shared-mem ring buffer
│    - maps controller pose/buttons → gun aim/trigger events
│    - displays frames received from the game process
├─ Runner process (started via proot-style ptrace sandbox, no root):
│    box64(box32) + minimal i386 Ubuntu 14.04 rootfs + ./game
│    ├─ LD_PRELOAD vr_shim.so (i386) — unchanged Phase 1 work:
│    │    patches mode→2, implements VidOVR* stubs,
│    │    VidOVRGetOrientation reads pose from the shm ring
│    ├─ GL: gl4es (arm64-wrapped by box64) or x86 Mesa→virgl over socket
│    │    → Android GLES3 driver
│    ├─ window: Xvfb (or SDL dummy + offscreen GLX) — no X display needed
│    └─ frame capture: swap-hook in shim → shm/AHardwareBuffer → Activity
└─ assets: game dump (g5/, pmuser/, AnisoSpecTex.ppm, etc.) on shared storage
```

- Frame transport v1: shim glReadPixels → POSIX shm → Activity uploads to
  swapchain texture (simple, measure first). v2 (if needed): virgl resource
  sharing / AHardwareBuffer zero-copy.
- Identity warp in engine (JPVR_WARP unset); OpenXR compositor handles
  Quest lens distortion. Each eye half of the SBS frame → one swapchain.
- Audio: SDL1.2 audio in guest → pulse/SDL bridge → AAudio (Termux-proven
  approach); details at Q3.
- Input: arcade gun is gyro-aim + trigger via USB serial (io_gyrogun/RIO).
  v1: Quest controller ray → gun x/y + trigger via shim hooking the input
  read path (addresses TBD — Phase Q2 investigation).

### Phases (Quest track)

- **Q0 — perf spike on the Mac (no Quest needed).** This Mac is AArch64
  Apple Silicon — the same constraint as Quest. Build the whole stack in a
  UTM arm64 Ubuntu VM: qemu-i386 and/or box64+box32, i386 Ubuntu 14.04
  rootfs with SDL1.2/Mesa, Xvfb, empty libPhysXLoader stub, run `game -n
  -f1360x768` headless. Measure FPS (llvmpipe first, then gl4es/virgl).
  **Gate: ≥30 fps software or working GL accel → proceed; else rethink.**
  M-series perf is an upper bound for XR2 Gen 2 — derate ~50%.
- **Q1 — VR shim on the emulator stack.** JPVR_ENABLE=1 boot: mode 2, SBS
  stereo in the Xvfb framebuffer, verify via /tmp/vr_shim.log + frame dumps
  (screenshot the framebuffer). Same acceptance as Track A Phase 1.
- **Q2 — Quest hello-world.** Port the Q0 recipe to Termux on the Quest
  (proot + box64 + Termux:X11/virgl) as a 2D panel; measure real XR2 perf.
  Gate: playable framerate at reduced res.
- **Q3 — OpenXR viewer + integration.** Standalone APK skeleton (arm64
  OpenXR activity), shm frame transport, pose injection into
  VidOVRGetOrientation, controller→gun mapping, audio routing.
- **Q4 — productize.** Single APK packaging (rootfs + emulator + assets),
  boot flow, watchdog/restart (mirrors utils/go.sh), perf/thermal tuning.

### Risks (Quest track)

- **Performance is the whole ballgame.** 2015 arcade PC game under CPU
  emulation; XR2 Gen 2 single-thread ≈ low-end 2015 x86, dynarec overhead on
  top. Mitigations: box64 dynarec tuning, gl4es batching, lower res, drop
  attract videos. If Q0/Q2 gates fail: fall back to PC-render + Quest-stream,
  or commit to static recompilation.
- box32 maturity: experimental; expect missing syscall/lib wrappers — that's
  why qemu-i386 is the compat baseline and why the rootfs carries real i386
  .so's (SDL/Mesa/theora) rather than relying on wrapped libs initially.
- GL feature gaps: game is GL2-era + GLSL shaders (g5/shaders/*.gls) — inside
  gl4es's comfort zone; GLX pbuffer/SGIX calls from GLEW are init-time only.
- Legal: personal interoperability project on owned hardware; do not
  redistribute the game assets.

## Phases

1. **Enable mode 2, flat-screen stereo** (no HMD needed)
   - Build `vr_shim.so` (gcc -m32, LD_PRELOAD).
   - Runtime patches (mprotect + memcpy, non-PIE fixed vaddrs):
     - 0x81bde10: imm32 0→2 (force OVR mode) — or patch `-g` handler
       (0x81b9d1e imm) to keep it opt-in via `-g`.
     - 0x835dfa0/0x835dfb0: jmp to no-op impls.
     - 0x835dfc0: write FULL window resolution (confirmed from GameSysStart
       decompilation: GGG mode multiplies width by VidNumScreens, OVR mode
       does not — so return e.g. 1920x1080, not per-eye 960x1080; the
       engine halves the window per eye downstream).
     - 0x835dfd0: return yfov/aspect/offset (DK2: ~96.7deg total h, offset
       per eye handled by engine layer table — verify semantics from callers).
     - 0x835dfe0 / 0x835dff0: reverse exact expected returns from
       RenderLayerSetScreenAsm caller code first.
     - 0x835e000: DK2 warp params (HmdWarpParam 1,0.22,0.24,0; ChromAb
       0.996,-0.004,1.014,0) or identity for passthrough capture.
   - Milestone: game boots on Linux with stereoscopic SBS image in window.
   - Test env: 32-bit-capable Linux (VM or container, multiarch), GPU GL,
     X11. Needs libs: libSDL-1.2, libGL, libglut, libtheora, libogg/vorbis,
     libPhysXLoader (bundled?), libudev.
2. **Head tracking**: implement pose read (0x835dff0 / OVR_GetOrientation
   path) from SHM; recenter binding (OVR_ResetOrientation); verify camera
   rotation in attract mode.
3. **Frame output**: hook glXSwapBuffers/glutSwapBuffers in shim,
   glReadPixels the SBS frame to SHM; 64-bit daemon submits left/right halves
   as OpenXR layers. Tune warp identity vs pre-warped. Measure latency; if
   readback too slow, move to FBO texture interception.
4. **Input**: map VR controller pose to gun aim (io_gyrogun_set_screen /
   GameGunInit input path), trigger/hook to gun trigger, recenter button.
5. **Polish**: per-HMD profiles (res/FOV/IPD), async reprojection options,
   attract-mode behavior, performance, launcher script (`vr_go.sh`).

## Risks / open questions

- ~~Sentinel envelope integrity/timing~~ — resolved 2026-10-16: the patched
  `game` is statically devirtualized; patch bytes are final at load time.
  Residual risk: the dead envelope code may still run CRC/watchdog threads —
  watch for crashes at M2.
- ~~ABI of stubs 0x835dfe0/0x835dff0 unknown~~ — resolved 2026-10-16:
  confirmed GetIPDistance / GetOrientation from devirtualized
  RenderLayerSetScreenCam.
- The mode==2 path never shipped enabled → expect engine-side bugs
  (UI/attract pages may render per-eye incorrectly, ScreenHitFX alignment).
- glReadPixels SBS capture perf at 90 Hz — may need GPU-side transfer.
- Where to run: needs a Linux box/VM with VR runtime (Monado recommended;
  SteamVR Linux also possible). macOS host can only do analysis, not testing.

## Dependency analysis (2026-10-15)

Only **108 undefined dynamic symbols**, and they are all libc / libstdc++ /
libpthread / libdl / libX11 (XOpenDisplay, XCloseDisplay, XFlush,
XGetWindowAttributes). SDL 1.2, OpenGL, glut, theora, ogg/vorbis, udev and
PhysX are all loaded via **dlopen/dlsym at runtime** — no link-time symbols.

Consequences for the test VM:
- Link-time requirements are trivial; any i386 glibc userland works.
- Missing dlopen'd libs surface as runtime log errors, not loader failures.
- `libPhysXLoader.so.1` (a DT_NEEDED entry with zero referenced symbols) can be
  satisfied by an **empty stub .so** — `gcc -m32 -shared -o libPhysXLoader.so.1`
  from an empty C file, placed in `vr/deps/`.

## Devirtualization discovery (2026-10-16)

The TP patch is a **full static devirtualization**: every Sentinel Envelope
prologue (`call 0x904a8b0` at 13,257 function entries) was replaced in-place
with the original function prologue bytes (same length, all offsets preserved).
The patched `game` is therefore fully readable plaintext — no runtime unpacking
concerns for the shim's poll thread. **All further analysis targets `game`,
not `game_original`** (tools/disasm.py default switched).

Newly confirmed from the devirtualized code:

- `VidNumScreens(type)`: GGG(1) → 3.0 (3-projector dome), OVR(2) → 1.0,
  NORMAL(0) → 2.0.
- `RenderLayerSetScreenCam` OVR branch (0x82ffe86): eye lateral offset =
  `IPD * (screen - 0.5)` → **screen 0 = left eye, screen 1 = right eye**;
  then calls GetOrientation(quat) and builds the per-eye rotation matrix.
- Stub ABIs now fully confirmed: 0x835dfe0 = `float VidOVRGetIPDistance(void)`
  (result consumed by x87 math), 0x835dff0 = `void VidOVRGetOrientation(float*)`
  (single buffer arg). No longer "probable".

## REA toolchain (2026-10-16)

- REA 6.4.0 (`~/Documents/rea`, `node scripts/rea.mjs`) — usable providers:
  - **ghidra**: healthy with `GHIDRA_INSTALL_DIR=$HOME/tools/ghidra_12.1.4_PUBLIC`
    and `JAVA_HOME=$HOME/tools/jdk/jdk-21.0.12.1+1/Contents/Home` (both were
    already installed; also exported in ~/.profile and ~/.zshrc).
  - **rizin**: healthy via Homebrew rizin (`/opt/homebrew/bin/rizin`); handles
    ELF32 + dynsym import fine. `inspect-binary-layout` is Linux-x64-only —
    not usable for this binary.
- Ghidra snapshot of patched `game`: `vr/ghidra/game.snapshot` (first full
  analysis is slow; subsequent `rea function`/`rea decompile` calls reuse it).

## Phase 1 status (2026-10-15)

Full Phase 1 plan approved (session plan file). Built so far:

- `vr/shim/shim.c` + `Makefile` — LD_PRELOAD shim: constructor-spawned poll
  thread waits for Envelope unpack (byte-verify against known plaintext), then
  patches the mode default (0x81bde10 imm32 → 2) and emits 5-byte near jmps
  over the seven VidOVR stubs (DK2 profile constants, identity warp default,
  `JPVR_WARP=1` enables DK2 barrel warp). Logging to /tmp/vr_shim.log;
  `JPVR_DUMP=1` dumps the runtime-decrypted RenderLayerSetScreenCam and
  VidNumScreens bodies 20 s after patching.
- `vr/run_vr.sh` — launcher (sets LD_PRELOAD/LD_LIBRARY_PATH, runs `./game -n`).
- Both syntax-checked clean on host; 32-bit build requires the Linux VM.

Decompilation review (2026-10-16, Ghidra snapshot `vr/ghidra/game.snapshot`,
pseudocode in `vr/ghidra/decomp/*.c`):

- All 10 VR-path functions decompiled; every shim ABI assumption confirmed
  (signatures, call sites, mode==2 control flow in GameSysStart/RenderLoop/
  UpdateCam/RenderLayerSetScreenCam).
- `ParseCommandLineArgs` fully mapped: `-f<w>x<h>` resolution, `-a<f>x<f>`
  aspect override, `-ap` autoplay, `-b`/`-bb`, `-g` = GGG mode (writes 1 to
  mode field 0x8db4e6c), `-sc`, `-sl`, `-G`, `-crcgen`. **No flag writes 2**
  → runtime patch of the mode default remains required.
- GameSysStart mode==2 branch confirmed: VidOVRInit → VidOVRGetResolution
  (writes width/height straight into CLArgs) → aspect = w/h → mode stored to
  0x8e7b3e4; window is NOT width-multiplied (unlike GGG ×3).
- Caveat folded into shim.c: `shaderOVRSetupFunc` hardcodes screen=0 when
  calling VidOVRGetSConfigParams, so per-eye LensCenter mirroring never
  varies — moot for identity warp; Phase 3 must determine how per-eye
  distortion selection was meant to work (texm matrix suspected).

Next blocker: the 32-bit Linux test environment (VM or host) — see Phase 1
plan, work item 1.

## Q0 boot status (2026-10-19)

Full log: `ai_docs/jp2015-q0-boot/`. Summary:

- Game boots under qemu-i386 on AArch64 to its main loop and renders
  (softpipe; "Loading" screen at ~70 fps). No host dependencies: the kit
  (`vr/kit/`, built by `vr/kit/build_kit.sh`) carries a static qemu-i386,
  a minimal i386 bookworm rootfs and the shim; verified on a clean arm64
  container with no qemu or i386 libs (`vr/kit/test/test_kit.sh`).
- Reproducible binary: `vr/kit/patch_game.sh` (dd only) or
  `vr/q0/make_game.py` turns the pristine TP binary (sha c18a93...) into
  the runnable one (sha 795862...), 52 annotated patches.
- Shim now covers: validated GOT repair (arcade layout recovered, NVIDIA
  346 libGL), HASP dongle emulation + programs_enc->programs_dec decrypt,
  RIO I/O board emulation, root/arcade-environment fixes, GL compat
  (GLSL output rewrite, fps counter), diagnostics.
- **Attract mode reached** ("PRESS START TO BEGIN") from the kit on the
  clean host (cabinet type seeded in the emulated dongle, saves link
  restored, /pm mapping, RIO emulation).
- Perf: ~70 fps game loop on a trivial scene under qemu TCG, but 0.06 fps
  in 3D attract with software GL inside the emulated process. **Next: Q0
  perf gate with native GL** (box64/box32 + wrapped libGL, or GL
  forwarding). Readiness detail: `ai_docs/jp2015-q0-boot/quest-readiness.md`.
- Open: PhysX 2.8.1 runtime absent, audio, gun/START input wiring.

## Artifacts so far

- `symbols_original.txt` — full symbol map (addr, size, name)
- `tools/disasm.py` — disassembler w/ symbol resolution (venv: `.venv`, capstone)
- `vr/shim/`, `vr/run_vr.sh` — Phase 1 shim + launcher
- `vr/ghidra/game.snapshot` + `vr/ghidra/decomp/*.c` — Ghidra snapshot of the
  patched `game` and decompiled pseudocode for all VR-path functions
- This plan.

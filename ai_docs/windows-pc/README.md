# JP2015 on the Windows PC — assessment and running log

Picks up from the Mac work (`ai_docs/jp2015-q0-boot/`). Machine: Windows 11 Pro,
RTX 4080 SUPER (driver 610.88), Ryzen 9 7950X3D, 63 GB RAM. Constraint from
the user: Windows-only (no Linux install). All downloads go to E:, never C:
(C: has ~14 GB free). Never commit game files; ask before pushing.

## Environment survey (2026-10-19)

- Present: git, Python 3.12, Docker Desktop (WSL2 backend, distro
  `docker-desktop`, stopped), nvidia-smi.
- Absent: gcc/clang/cmake, Java (Ghidra), TeknoParrot, SteamVR, Meta
  Quest Link app, any registered OpenXR runtime.
- Game dump: not found on D: or E: (searched for `*Jurassic*` folders and
  `symbols_original.txt`). Path still needed from the user.
- Note: Docker Desktop means a WSL2 kernel is already installed. Its disk
  image defaults to `%LOCALAPPDATA%\Docker\wsl` on C:, so it must be moved
  to E: (Settings > Resources > Disk image location) before pulling images.
- No `CLAUDE.md` exists in the repo. These notes follow the layout already
  in use: one folder per work stream under `ai_docs/` with a dated
  `README.md` running log.

## Where the project stands

The game is a 32-bit x86 Linux ELF. On the Mac it boots to attract mode
inside a self-contained kit (qemu-i386 + i386 rootfs + `vr_shim.so`). The
shim stands in for the whole cabinet: GOT repair, HASP dongle and program
decryption, RIO I/O board, paths, GLSL fixes. Every verified run used
software GL, so 3D is ~0.07 fps. The engine's compiled-out Oculus DK2
stereo path (display mode 2) is mapped and the shim can enable it
(`JPVR_ENABLE=1`), but it has never been seen rendering at a usable rate.

Open, roughly by how much they block VR:
1. No GPU rendering path anywhere yet (the Q0 perf gate).
2. No frame output to a headset (OpenXR or OpenVR) and no pose input.
3. No input: START/coin through the RIO report buffer, gun aim.
4. PhysX 2.8.1 runtime missing (`NxCreatePhysicsSDK` returns NULL), so
   gameplay is untested past attract. Audio is a dummy driver.
5. imgLoad lockup 1169 after ~19 min of attract under softpipe; may be
   renderer-specific.
6. Standalone Quest: box64/box32 cannot run the binary yet because the
   GOT repair happens in a preload that box64 never runs
   (`box64-probe.md`).

## What this machine can do

The game cannot run natively on Windows: it needs a Linux loader. That
leaves these routes.

### R1. TeknoParrot + an injected Windows DLL -> PC VR (fastest to VR)

The dump is prepared for TeknoParrot (TP), which loads Raw Thrills Linux
ELFs on Windows and renders through the Windows OpenGL driver, so it runs
on the real 4080. The game is non-PIE, so its code sits at the same
addresses as on Linux, and every VR patch in `vr/shim/shim.c` is a
fixed-address memory patch plus small x86 functions (mode 0 -> 2, the
seven VidOVR stubs). The plan:
- Baseline: game under TP, smooth? survives 20+ min of attract?
- Port the VR part of the shim to a 32-bit Windows DLL injected into the
  TP game process: same patches, same VidOVR implementations, pose read
  from the VR runtime.
- Frame output: hook `wglSwapBuffers`, submit the left and right halves of
  the side-by-side frame as eye textures. Use OpenVR (SteamVR ships a
  32-bit `openvr_api.dll`) in process, or a 64-bit OpenXR helper fed over
  shared memory (the plan's Track A daemon, on Windows).
- To the Quest 3: Air Link / Quest Link, Steam Link, or Virtual Desktop.

Pros: GPU-speed rendering now; TP already handles dongle, I/O, paths,
GOT; the VR reverse engineering carries over unchanged. Cons: depends on a
closed third-party loader whose internals are unknown (how it maps memory,
whether it tolerates injection, how it exposes I/O for START/coin/gun);
PC-tethered, not standalone; the Linux shim's environment fixes do not
carry over (they are not needed under TP).

### R2. Docker Desktop (WSL2) Linux container, native x86, CPU GL

Already installed. `run.sh` native x86 mode runs the game in an i386
container with no emulation. GPU is not available: WSL2's GPU path
(`/usr/lib/wsl/lib`, Mesa d3d12) is 64-bit only. So rendering is llvmpipe
on 16 Zen 4 cores: much faster than qemu on the Mac (probably playable-ish
at low res, unmeasured), but not VR rate. Display through Xvfb +
screenshots/VNC, or an X server on Windows.

Good for: testing everything that is not GPU speed on the existing Linux
stack: mode-2 stereo correctness (Q1), lockup 1169 under llvmpipe, RIO
input wiring, PhysX, the box64 loader fix (x86 host can still test the
repaired-ELF idea with the plain glibc loader). Cost: moving Docker's disk
image to E:, a few GB of images. Conflicts with "no Linux/WSL" if that was
meant strictly; it is a container, not an installed distro.

### R3. Platform-independent work (Python only)

- Repaired-ELF generator: write the 614 recovered GOT slots back as real
  `R_386_JUMP_SLOT` relocations and a real `.rel.plt`. Then any loader
  (glibc, box64, FEX) runs the binary without a preload step. This
  unblocks the standalone-Quest path (`box64-probe.md`, "What it would
  take"). Testable only on Linux (R2) or the Quest.
- Reverse engineering for input (RIO buttons, io_gyrogun aim) with
  `tools/disasm.py` (`pip install capstone` into `.venv`). Ghidra needs a
  JDK, downloadable to E:.

### R4. The Quest 3 itself over adb

The Quest is AArch64. The Mac-built kit (`vr/kit/out/`, not in git) runs
on AArch64 Linux userlands, so it could run in Termux on the headset via
adb from this PC (platform-tools to E:). It would only confirm that it
boots: the rendering is still software and slow. Needs the kit rebuilt
(Docker with arm64 emulation) or copied from the Mac.

## Recommendation

1. Next step: the TeknoParrot baseline (R1, first bullet). It is cheap and
   decides the PC VR route: if TP runs the game smoothly on the 4080, the
   quickest way to the game in a Quest 3 is TP + an injected DLL with the
   existing VidOVR patches + OpenVR, streamed over Link.
2. In parallel, if a container is acceptable: R2 to keep the Linux stack
   testable (mode-2 stereo check, lockup 1169), and R3's repaired-ELF
   generator, which is the key to the standalone build later.
3. Leave R4 until a GPU path exists for AArch64 (box64 + gl4es/Zink).

## Status log

### 2026-10-19
- Pulled `26da632`. Read the README, plan, Q0 log,
  quest-readiness, box64-probe, kit launcher and x86 setup script.
- Surveyed the machine (above). Wrote this assessment. Waiting on: dump
  path, OK for TeknoParrot and Docker/WSL2 container use.

### 2026-10-20
- Dump transferred from the Mac to
  `E:\jp2015-vr\Jurassic Park (1.08)(2015)[Raw Thrills Linux PC][TP]\`
  (4.7 GB). `game` = pristine TP (sha c18a9329...), `game_original` =
  6bb8a70d..., 1516 files in `programs_dec/` (matches `programs_enc/`).
  Not in the dump: `symbols_original.txt`. The dump also carries Windows
  `libogg.dll`, `libvorbis.dll`, `libvorbisfile.dll` (TP support files)
  and an `env/` folder.
- Analysis bundle at `jp2015-analysis-items/jp2015-analysis/`: patched
  `game` (795862...), `game.q0bak` (c18a93...), `game_original`,
  `symbols_original.txt`, all matching its HASHES.txt. It has the layout
  `tools/disasm.py` expects, so set
  `JP2015_GAME=E:/jp2015-vr/jp2015-analysis-items/jp2015-analysis` for
  analysis.
- `.gitignore`: added the dump zip, the analysis bundle and `/ext/`
  (external tools downloaded into the repo dir).
- TeknoParrot: the official download page (teknoparrot.com/en/Home/Download)
  points to `TPBootstrapper.zip` from github.com/nzgamer41/TPBootstrapper.
  Downloaded to `ext/TeknoParrot/` (zip sha 5358f89f..., exe v2.0.0.6,
  not Authenticode-signed). It is a GUI installer that fetches the TP
  components itself; the user runs it. Install target: `ext/TeknoParrot/`.
- TP installed. Profile `GameProfiles/JurassicPark.xml`: EmulatorType
  `ElfLdr2` (TP's own ELF loader, `ext/TeknoParrot/ElfLdr2/`), 32-bit,
  ExecutableName `game`, no admin. Metadata says NVIDIA "OK". TP ships no
  verification list for this game (`MD5/` has none), so its optional
  DAT/XML verification prompt can be skipped; our own check is the sha256.
- TP UI 1.0.0.2249 wanted a ROM scanner + DAT to add games. Bypassed: an
  added game is just `UserProfiles/<name>.xml`, so
  `UserProfiles/JurassicPark.xml` was made from
  `GameProfiles/JurassicPark.xml` with `<GamePath>` set to the dump's
  `game`. Restart TP to pick it up.
- **TP baseline: pass.** Game ran from 09:25 to past 09:56 (31+ min) in
  attract with "great FPS" (user) and no lockup; process
  `ElfLdr2/BudgieLoader.exe` (32-bit). Saves go to the dump's `pmuser/`.
- Inside the TP process (read-only inspection, `ReadProcessMemory`):
  - game ELF mapped at its link address 0x8048000; mode default at
    0x81bde0a still `c7 05 6c4edb08 00000000`; VidOVR stubs still
    `repz ret`. TP's own edits: ~2400 small runs in .text (call sites in
    many functions) and ~3350 in data (GOT etc.); none touch our VR sites.
  - GL path: SDL.dll (TP) -> opengl32 -> TP's `regal32.dll`/`tpgfx.dll` ->
    NVIDIA `nvoglv32.dll` (real GPU). Audio: SDL_mixer -> DirectSound.
  - Launch: TeknoParrotUi runs `.\ElfLdr2\BudgieLoader.exe "<dump>\game"`.
- TP ships the Linux i386 PhysX 2.8 runtime in `ElfLdr2/libs/`
  (`libPhysXCore.so`, `libPhysXLoader.so.1`, `libNxCharacter.so.1`,
  `libNxCooking.so`): candidate answer to open problem 4 for the Linux kit.
- Toolchain: llvm-mingw 20261006 (ucrt, x86_64 host; clang 23.1.3) in
  `ext/llvm-mingw-20261006-ucrt-x86_64/` (zip sha 317492c4...). Builds
  32-bit Windows DLLs.
- New `vr/win/`: `jpvr.c` = VR patches as a proxy `version.dll` in
  `ElfLdr2/` (see vr/win/README.md). Built and installed 09:56. Next: launch
  from TP, read `ElfLdr2/jpvr.log`, expect a side-by-side stereo frame on
  the monitor (Q1 acceptance on the GPU path).
- **First mode-2 run under TP (10:24): black window.** `jpvr.log`: image
  mapped 844 ms after load, all 8 patches applied, VidOVRInit +
  GetResolution(1920x1080) called, runtime display mode = 2,
  GetSConfigParams called exactly once, then no per-frame VR calls
  (GetProjParams / GetIPDistance / GetOrientation) in 3+ min.
  - Stack sampling of the main thread (Wow64GetThreadContext + stack scan,
    scratch tools) shows the game alive: GameCoreLoop -> RenderLoop ->
    RenderLoop_RenderLayer -> WndSwap every frame, attract pages advancing
    (movie pages, HST/high-score pages). gamelog.txt stays empty under TP.
  - RenderLoop disassembly: mode != 0 jumps to 0x83065f8; mode 2 goes to
    0x8306880: for screen < VidNumScreens(2) (= 1.0) RenderLayerRender(
    layer 0 = 0x8e249e0, screen) + CheckScreenGrab, then
    RenderLoop_RenderLayer(7, -1), FadeUpdate, WndSwap. So all stereo work
    hangs off layer 0 of gRenderLayerSetupDataOVR (0x8b10480).
  - shaderOVR/frag.gls with our identity params samples Texture0 at the
    unwarped coordinate, so black = its input texture is empty (scene not
    rendered into it, or a shader/FBO failure). Attract checks of the mode
    (AtUpdate, AtProcRun) only test == 1 (GGG), not relevant.
  - Next: GL diagnostics inside jpvr (wrap the game's GOT slots for shader
    compile/link status and FBO completeness, like the Linux shim's
    JPVR_GLDIAG), and dump gRenderLayerSetupDataOVR vs the mode-0 table to
    see which layers feed layer 0. To return to stock: delete
    `ext/TeknoParrot/ElfLdr2/version.dll`.

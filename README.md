# jp2015-vr

Porting Raw Thrills' **Jurassic Park Arcade (2015, v1.08, Linux build)** to VR,
with the **Meta Quest 3** as the target. This repo holds the tooling, the
compatibility shim, the launch kit and all notes. It contains **no game
files**: you supply your own TP dump.

## Goal

Launch the game reliably with no system dependencies, then make it ready to
port to Quest 3 (standalone APK long term, PC VR streaming first).

Status (2026-10-19): the game boots to attract mode from the self-contained
kit on a clean AArch64 Linux host. Rendering is software-only there (about
0.07 fps in 3D), so the next phase is running on a real GPU. Details:
`ai_docs/jp2015-q0-boot/README.md` and `quest-readiness.md`.

## Where to read first

| File | What |
|---|---|
| `VR_ENGINEERING_PLAN.md` | Overall plan: tracks, phases, risks |
| `ai_docs/jp2015-q0-boot/README.md` | Running status log of the boot work |
| `ai_docs/jp2015-q0-boot/quest-readiness.md` | What is ready, what blocks Quest |
| `ai_docs/jp2015-q0-boot/boot-fixes.md` | Every environment fix in the shim and why |
| `ai_docs/jp2015-q0-boot/got-layout.md` | How the dump's broken imports were recovered |
| `ai_docs/jp2015-q0-boot/box64-probe.md` | box64 (native-GPU on ARM) probe findings |
| `kimi-export-session_-20261019-201710.md` | Earlier session transcript (history) |

## Layout

- `vr/shim/` — `vr_shim.so` source (i386): import repair, dongle and I/O
  board emulation, path/GL compatibility, VR render-mode patches.
- `vr/kit/` — launcher (`run.sh`), binary patcher (`patch_game.sh`, sh+dd
  only), prebuilt i386 libs in `lib32/` (no compiler needed), kit builder
  for AArch64 (`build_kit.sh`, Docker), x86 host setup.
- `vr/q0/` — analysis tooling (import solver, call-site checkers), boot
  test scripts, `game_patches.json` (the 52 binary patches, annotated).
- `vr/box64/` — box64/box32 build + probe.
- `tools/disasm.py` — symbol-aware disassembler (`pip install -r requirements.txt`).

## Setup on the RTX 4080 Windows PC

The game is a 32-bit Linux program, so it runs in Linux on that machine.

### Option A: Ubuntu on the PC (recommended, real GPU)

Install Ubuntu 24.04 (dual boot, or boot it from an external SSD), with the
NVIDIA driver. Then:

```sh
git clone git@github.com:Lovecore/jp2015-vr.git && cd jp2015-vr
ln -s "/path/to/Jurassic Park (1.08)(2015)[Raw Thrills Linux PC][TP]" jp_game
sudo vr/kit/setup_x86_host.sh jp_game      # 32-bit libs, NVIDIA 32-bit GL, /pm link
vr/kit/run.sh jp_game                       # native, no emulation
```

PC VR to the Quest 3 from here: SteamVR + ALVR (both run on Linux).

### Option B: WSL2 on Windows (quick check, software GL)

WSL2 runs 32-bit Linux programs and shows windows via WSLg, but its GPU path
is 64-bit only, so this game renders with Mesa llvmpipe on the CPU (native
x86, far faster than emulation, not full speed).

```powershell
wsl --install -d Ubuntu-24.04
```
Then inside Ubuntu, same steps as Option A (the setup script skips the
NVIDIA part when `nvidia-smi` is missing). Keep the game folder inside the
Linux filesystem (`~/`), not under `/mnt/c`, for file speed.

### Copying the game

Copy your whole TP dump folder (including `pmuser/`, `programs_dec/` and
`symbols_original.txt`) to the PC. `run.sh` finds the pristine binary by
hash and patches a copy into `vr/kit/state/`; the dump is used in place.

## Options (env)

`JPVR_LOG` shim log path; `JPVR_FPS_EVERY` fps logging; `JPVR_GLDIAG=1`
shader/texture diagnostics; `JPVR_SAMPLE=ms` thread sampler;
`JPVR_CABTYPE` cabinet type (default 4, upright); `JPVR_OVERLAY` asset
overrides; `JPVR_ENABLE=1` VR render mode (stereo, phase 1). Full list in
the header of `vr/shim/shim.c`.

## Legal

Personal interoperability project on owned hardware. Do not commit or
redistribute game files or patched game binaries.

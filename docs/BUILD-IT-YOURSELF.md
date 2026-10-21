# Build it yourself

This repository has no game files and no prebuilt app. To play *Jurassic
Park Arcade* on your Quest 3 you build the app yourself from your own copy
of the game. Five commands do it; this page explains what each needs.

> **The app you build is for your own headset.** It contains the game's
> code in recompiled form, so do not share the APK or your game files.

## What you need

### Files you supply

| What | Where it comes from | Checked by |
|---|---|---|
| **The game folder**: *Jurassic Park Arcade* v1.08, the Raw Thrills Linux PC build, in the folder layout of a TeknoParrot dump (`game`, `g5/`, `pmuser/`, `programs_dec/`, ...) | Your own copy of the game | `game` must have SHA-256 `c18a9329e72a363937155d245440694e58b5a1e27984016ae7648af3413b4269` |
| **The PhysX 2.8 runtime** (NVIDIA, i386): `libPhysXLoader.so.1`, `libPhysXCore.so`, `libNxCooking.so`, `libNxCharacter.so.1` | Ships with TeknoParrot (`ElfLdr2/libs/`). Install TeknoParrot into `ext/TeknoParrot/`, or point the prepare step at any folder with the four files | Each file's SHA-256 (in `tools/setup/prepare_game.sh`) |

Nothing else from the game is needed: the function map the recompiler
uses (`tools/recomp/symbols_original.txt`) is in this repository.

### Downloaded for you

`tools/setup/fetch_deps.sh` downloads these into `ext/` (gitignored) and
checks each against a pinned SHA-256:

- the i386 Ubuntu 14.04 libraries the game loads: glibc 2.19, libgcc,
  libstdc++ 4.8, libogg, libtheora (from Launchpad's archive);
- `gl.xml`, the Khronos OpenGL registry (pinned to a commit);
- the Khronos OpenXR loader for Android 1.1.65 (Maven Central);
- Python's `capstone` package, into the repository's own `.venv`.

### Tools to install

| Tool | Version used | Notes |
|---|---|---|
| Windows 10 or 11, x64 | | 16 GB RAM or more; about 15 GB free disk (game copy 4.7 GB, build outputs 4 GB, the headset copy of the data) |
| Git for Windows | any recent | Provides the `sh` (Git Bash), `curl` and `sha256sum` the scripts use |
| Python | 3.10 or newer | `python` on the PATH; the scripts make a `.venv` |
| Android SDK | NDK **27.1.12297006**, build-tools **36.0.0**, platform **android-36**, platform-tools | Easiest via Android Studio's SDK Manager. Set `ANDROID_SDK` to the SDK folder |
| A JDK (17 or newer) | Android Studio's bundled JBR | For `keytool` and `apksigner`. Set `JAVA_HOME` if Android Studio is not in its default place |
| Meta Quest 3 | Horizon OS with developer mode | Developer mode is switched on in the Meta Horizon phone app (it needs a free developer account) |
| A USB-C data cable | | For installing |

Optional: [llvm-mingw](https://github.com/mstorsjo/llvm-mingw/releases)
unpacked into `ext/` builds the Windows version too (`WINDOWS=1`).

## The steps

All commands run in **Git Bash** from the repository root.

```sh
# 0. once per shell: where your Android SDK is
export ANDROID_SDK="/c/Users/<you>/AppData/Local/Android/Sdk"

# 1. downloads and checks the free dependencies (a couple of minutes)
sh tools/setup/fetch_deps.sh

# 2. checks your game and PhysX files, makes the patched working copy
sh tools/setup/prepare_game.sh "/path/to/your/game folder" ["/path/to/PhysX libs"]

# 3. discovers functions, lifts the game to C, builds the APK
sh tools/build_all.sh

# 4. headset plugged in, developer mode on, "Allow USB debugging" accepted
sh tools/quest/install.sh
```

Then on the headset: **Library, filter "Unknown sources", Jurassic Park
Arcade.** The first start after the data copy unpacks it (a few minutes);
after that it starts in seconds.

### What each step does, and how long it takes

1. **`fetch_deps.sh`**: about 10 MB of downloads, each checked against
   its hash. Re-running skips what is already present and correct.
2. **`prepare_game.sh`**: checks the game binary and the PhysX files by
   hash, applies the 52 documented binary patches
   (`vr/kit/patch_game.sh`, from `vr/q0/game_patches.json`) to a copy in
   `ext/game_work/`, checks the result (`7958621...`), and records where
   your game folder is (`ext/game_dir.txt`). Your game folder is only read.
3. **`build_all.sh`**:
   - function discovery for the game and 15 libraries, the first time only
     (about 2 minutes);
   - lifting to C: about 575 MB of generated C in `tools/recomp/out/c_all`
     (a few minutes);
   - the APK: compiling that C for arm64 is the long part, **about 15 to 60
     minutes** the first time depending on your CPU (it uses 16 parallel
     jobs; set `JOBS`). Later builds recompile only what changed.
4. **`install.sh`**: installs or updates the app. The first time it also
   copies your game folder (about 5 GB) and the i386 libraries to the
   headset; that takes a few minutes over USB.

### Checking it worked

- After step 3: `tools/quest/out/jp2015vr.apk` exists (about 90 MB).
- While the game runs, `adb logcat -s jprt` shows its log. Healthy signs:
  `openxr: session state 5` (focused) and lines like
  `frame 4801 (72.0 fps) game 7.0 ms avg`. The same log is in
  `/sdcard/Android/data/com.jp2015.vr/files/jprt.log` on the headset.

## Updating, settings, removing

- **Update:** pull the repository, then `sh tools/build_all.sh` and
  `sh tools/quest/install.sh` again. The game data on the headset is kept.
  Keep `ext/keystore/` (the signing key made by your first build): an app
  signed with a different key cannot update yours.
- **Settings:** the screen is 4 m wide; resize it with the right thumbstick
  (remembered). Runtime switches go in `files/jprt.env` on the headset; see
  [`tools/quest/README.md`](../tools/quest/README.md).
- **Remove:** uninstall the app from the headset's Library. This also
  deletes the copied game data.

## Troubleshooting

| Symptom | Cause and fix |
|---|---|
| `'game' is not the expected v1.08 TeknoParrot binary` | A different version or a modified file. Only this exact build is supported (the recompiler works from its addresses). |
| `PhysX library ... has an unexpected sha256` | A different PhysX build. Use the one that ships with TeknoParrot's ElfLdr2. |
| `SHA-256 mismatch` in `fetch_deps.sh` | A download was corrupted or a mirror changed. Re-run; if it persists, open an issue. |
| `Android SDK with an NDK not found` | Set `ANDROID_SDK`, and install NDK 27.1.12297006 in the SDK Manager. |
| `adb: device unauthorized` | Put the headset on and accept "Allow USB debugging" (tick "Always allow"). Unplug and replug the cable if no prompt shows. |
| Not in the Library | Sideloaded apps are under the Library's filter: **Unknown sources**. |
| The app closes right after starting | No game data yet: run `sh tools/quest/install.sh` with the headset connected. The log says `cannot load .../game`. |
| No sound | Start the game from the headset (not over USB while the headset sleeps); the app reopens its audio when it gets focus. |
| A build step fails partway | Re-run the same command; every step resumes. `FORCE_DISCOVER=1 sh tools/build_all.sh` redoes discovery from scratch. |

## How the build works

`docs/` and [`ai_docs/`](../ai_docs/) explain the recompiler and the
runtime in depth; the short version is in the [top-level
README](../README.md#how-it-works). For an AI coding agent driving this
build, see [`AGENTS.md`](../AGENTS.md).

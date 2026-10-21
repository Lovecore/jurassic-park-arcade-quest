# AGENTS.md: building this project with an AI coding agent

Instructions for an agentic system (Claude Code, Codex, Cursor, ...) asked
to build *Jurassic Park Arcade* for a user's Meta Quest 3 from this
repository. The human guide with the same steps is
[`docs/BUILD-IT-YOURSELF.md`](docs/BUILD-IT-YOURSELF.md); read it too.

## Goal

Produce `tools/quest/out/jp2015vr.apk` from the user's own game copy and
install it, with the game data, on their Quest 3, so the game starts from
the headset's Library (Unknown sources > Jurassic Park Arcade).

## Hard rules

1. **Never commit, upload or share game files or build outputs.** That
   includes the game folder, `ext/`, `tools/recomp/out/`,
   `tools/quest/out*/` and the APK (it contains the game's code,
   recompiled). They are gitignored; keep it that way.
2. **Keep every download inside the repository** (`ext/`, `.venv/`). The
   scripts already do; do not add caches or installs elsewhere without the
   user's consent (some users have little space on their system drive).
3. **Do not push or open pull requests** unless the user asks.
4. **Do not change the game version or the pinned hashes.** The recompiler
   works from this exact binary's addresses; a hash mismatch means the
   input is wrong, not the script.
5. **Leave stereo and VR off** (`JPRT_STEREO`, `JPRT_VR`): they are
   experimental and made a tester motion sick.

## What only the human can do

Ask the user for these; do not guess or work around them:

- the path to **their game folder** (Jurassic Park Arcade v1.08, Raw
  Thrills Linux PC, TeknoParrot dump layout);
- where the **PhysX 2.8 libraries** are (default: TeknoParrot installed in
  `ext/TeknoParrot/`, files in `ElfLdr2/libs/`);
- installing **Git for Windows, Python 3.10+, Android Studio** (SDK with NDK
  27.1.12297006, build-tools 36.0.0, platform android-36) if missing;
- switching on **developer mode** (Meta Horizon phone app), plugging the
  headset in, and accepting **"Allow USB debugging"** inside the headset;
- launching the game from the headset.

## Procedure

Run everything with `sh` (Git Bash on Windows) from the repository root.
Each step is safe to re-run.

| # | Command | Success looks like | If it fails |
|---|---|---|---|
| 0 | `export ANDROID_SDK=<sdk path>` | `ls "$ANDROID_SDK/ndk"` lists `27.1.12297006` | Ask the user to install it via Android Studio's SDK Manager |
| 1 | `sh tools/setup/fetch_deps.sh` | Ends with `dependencies ready in .../ext`; every line `ok` or `download` | `SHA-256 mismatch`: re-run once; if it persists, stop and report the URL |
| 2 | `sh tools/setup/prepare_game.sh "<game folder>" ["<PhysX dir>"]` | Ends with `ready. Next: sh tools/build_all.sh` | Hash errors: wrong game version or PhysX build; report to the user, do not patch around it |
| 3 | `sh tools/build_all.sh` | Ends with `done: tools/quest/out/jp2015vr.apk` | See below |
| 4 | `adb devices` | The headset listed as `device` | `unauthorized`: ask the user to accept the prompt in the headset; none listed: cable/developer mode |
| 5 | `sh tools/quest/install.sh` | Ends with `Done. On the headset: Library > Unknown sources > Jurassic Park Arcade.` | `adb` not found: set `ADB=<path to adb.exe>` (in `$ANDROID_SDK/platform-tools`) |
| 6 | Ask the user to start the game | `adb logcat -s jprt` shows `openxr: session state 5` and `frame N (7x.x fps)` | `cannot load .../game`: data missing, re-run step 5 |

Timing: step 1 is a few minutes, step 3 is long the first time (discovery
about 2 minutes, lifting a few minutes, then compiling about 575 MB of
generated C for arm64: 15 to 60 minutes). Run step 3 in the background
with a long timeout and poll its output instead of assuming it hung. Step 5
copies about 5 GB the first time.

### Step 3 failures

- `no prepared game` / `missing ext/...`: steps 1 or 2 did not complete;
  re-run them.
- `Android SDK with an NDK not found`: `ANDROID_SDK` is wrong or unset.
- `keytool` or `apksigner` errors: no JDK; set `JAVA_HOME` to Android
  Studio's `jbr` folder.
- A compiler error inside `tools/recomp/out/c_all/`: generated code should
  not fail; re-run with `FORCE_DISCOVER=1 sh tools/build_all.sh` once, then
  report the first error line to the user.

## Useful facts

- The app id is `com.jp2015.vr`; its files are in
  `/sdcard/Android/data/com.jp2015.vr/files/` (`game/`, `libs/`,
  `jprt.log`, `jprt.env`, `screen.txt`).
- Runtime switches go in `files/jprt.env` (`KEY=VALUE` lines); the list is
  in [`tools/quest/README.md`](tools/quest/README.md).
- Updating keeps the headset's data; uninstalling deletes it. Keep
  `ext/keystore/`: a different signing key cannot update the installed app.
- How the recompiler and runtime work: [`README.md`](README.md) and the
  logs in [`ai_docs/`](ai_docs/).

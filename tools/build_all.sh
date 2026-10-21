#!/bin/sh
# From your prepared game to the Quest APK, in one go:
#   1. function discovery for the game and its libraries (first time only;
#      FORCE_DISCOVER=1 to redo)                       tools/recomp/discover_all.sh
#   2. lifting to C + the GL thunks                      tools/recomp/build_process.sh
#   3. the APK                                           tools/quest/build_apk.sh
# Run tools/setup/fetch_deps.sh and tools/setup/prepare_game.sh first.
# WINDOWS=1 also builds the Windows executable (needs llvm-mingw in ext/).
#
#   sh tools/build_all.sh
set -eu
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/.." && pwd)
EXT="$ROOT/ext"
O="$ROOT/tools/recomp/out"
fail() { echo "ERROR: $*"; exit 1; }

# ---- prerequisites ----
[ -f "$EXT/game_work/game" ] || fail "no prepared game: run sh tools/setup/prepare_game.sh \"<your game folder>\""
for f in debs/x_libc6_2.19-0ubuntu6.6_i386/lib/i386-linux-gnu/libc-2.19.so khronos/gl.xml \
         openxr/aar/jni/arm64-v8a/libopenxr_loader.so TeknoParrot/ElfLdr2/libs/libPhysXCore.so; do
    [ -f "$EXT/$f" ] || fail "missing ext/$f: run sh tools/setup/fetch_deps.sh (PhysX: prepare_game.sh)"
done
SDK=${ANDROID_SDK:-/e/choom/tools/android-sdk}
[ -d "$SDK/ndk" ] || fail "Android SDK with an NDK not found at $SDK: set ANDROID_SDK (see docs/BUILD-IT-YOURSELF.md)"
if [ "${WINDOWS:-}" = 1 ]; then
    ls -d "$EXT"/llvm-mingw-*/bin > /dev/null 2>&1 || fail "WINDOWS=1 needs llvm-mingw unpacked in ext/"
fi

# ---- 1. discovery ----
if [ ! -f "$O/functions.json" ] || [ ! -f "$O/libNxCharacter.so.1.functions.json" ] || [ "${FORCE_DISCOVER:-}" = 1 ]; then
    echo "== discovering functions (game + 15 libraries; several minutes)"
    sh "$ROOT/tools/recomp/discover_all.sh"
fi

# ---- 2. lifting ----
echo "== lifting to C"
if [ "${WINDOWS:-}" = 1 ]; then
    sh "$ROOT/tools/recomp/build_process.sh"
else
    QUEST_ONLY=1 sh "$ROOT/tools/recomp/build_process.sh"
fi

# ---- 3. the APK ----
echo "== building the Quest APK (the first build compiles ~575 MB of C: long)"
sh "$ROOT/tools/quest/build_apk.sh"
echo
echo "done: tools/quest/out/jp2015vr.apk. Install with: sh tools/quest/install.sh"

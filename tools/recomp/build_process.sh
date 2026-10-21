#!/bin/sh
# Builds the recompiled arcade process for Windows x64 (dev host):
#   1. lifts the game + i386 libraries (functions.json from discover.py)
#   2. generates the GL thunks from the Khronos gl.xml
#   3. compiles the generated C and the runtime (rt/) with llvm-mingw
#   -> tools/recomp/out/process/jp2015.exe (skipped with QUEST_ONLY=1)
# Run: JPRT_LIBDIR=ext/debs tools/recomp/out/process/jp2015.exe [-v] "<game dir>"
set -eu
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../.." && pwd)
O="$HERE/out"
TC=${TOOLCHAIN:-$(ls -d "$ROOT"/ext/llvm-mingw-*/bin 2>/dev/null | tail -1)}
CC="$TC/x86_64-w64-mingw32-clang"
PY="$ROOT/.venv/Scripts/python"
[ -x "$PY" ] || PY=python
# the patched game + symbols (tools/setup/prepare_game.sh), else the analysis bundle
[ -f "$ROOT/ext/game_work/game" ] && _JG="$ROOT/ext/game_work" || _JG="$ROOT/jp2015-analysis-items/jp2015-analysis"
export JP2015_GAME=${JP2015_GAME:-$_JG}
if [ "${NOLIFT:-}" != 1 ]; then
    "$PY" "$HERE/lift.py" --out "$O/c_all" --functions "$O/functions.json" \
        "$O/ld-2.19.so.functions.json" "$O/libc-2.19.so.functions.json" "$O/libdl-2.19.so.functions.json" \
        "$O/libgcc_s.so.1.functions.json" "$O/libm-2.19.so.functions.json" \
        "$O/libpthread-2.19.so.functions.json" "$O/librt-2.19.so.functions.json" \
        "$O/libstdc++.so.6.0.19.functions.json" "$O/libogg.so.0.8.1.functions.json"         "$O/libtheora.so.0.3.10.functions.json" "$O/libnss_files-2.19.so.functions.json"         "$O/libPhysXLoader.so.1.functions.json" "$O/libPhysXCore.so.functions.json"         "$O/libNxCooking.so.functions.json" "$O/libNxCharacter.so.1.functions.json" | head -1
fi
"$PY" "$HERE/gen_gl.py" "$ROOT/ext/khronos/gl.xml" -o "$O/gl_thunks.c" > /dev/null
# QUEST_ONLY=1: stop here (the Quest build compiles the lifted C itself)
if [ "${QUEST_ONLY:-}" = 1 ]; then echo "lifted into $O/c_all (Windows build skipped)"; exit 0; fi
P="$O/process"
mkdir -p "$P/obj"
CFLAGS="${EXTRA_CFLAGS:-} -O1 -gline-tables-only -w -fno-strict-aliasing -I$HERE/rt -I$O/c_all"
ls "$O"/c_all/lifted_*.c "$O/c_all/table.c" "$O/c_all/thunks.c" | xargs -P "${JOBS:-16}" -I{} sh -c '
    o="'"$P"'/obj/$(basename {} .c).o"
    [ "$o" -nt {} ] && [ "$o" -nt "'"$HERE/rt/rt.h"'" ] || '"$CC $CFLAGS"' -c {} -o "$o"'
# objects of modules no longer lifted
for o in "$P"/obj/lifted_*.o; do [ -e "$O/c_all/$(basename "$o" .o).c" ] || rm -f "$o"; done
"$CC" -O1 -w -I"$HERE/rt" -c "$O/gl_thunks.c" -o "$P/obj/rt_gl_thunks.o"
for s in kernel syscalls hostlibs hostaudio audiodev hostgame gles glsl_es texdecode plat_win plat_posix video_win; do
    "$CC" -O1 -g -Wall -Wno-unused-function -I"$HERE/rt" -c "$HERE/rt/$s.c" -o "$P/obj/rt_$s.o"
done
"$CC" -o "$P/jp2015.exe" "$P"/obj/*.o -lsynchronization -lopengl32 -lgdi32 -luser32 -lwinmm -Wl,--stack,67108864
echo "built $P/jp2015.exe"

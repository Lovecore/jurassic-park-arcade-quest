#!/bin/sh
# Builds the differential harness (32-bit Windows) from the lifted C.
#   tools/recomp/harness/build.sh  -> tools/recomp/out/harness/harness.exe
# Run: harness.exe <patched game> tools/recomp/out/cands.txt [trials] [seed] [addr]
set -eu
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../../.." && pwd)
TC=${TOOLCHAIN:-$(ls -d "$ROOT"/ext/llvm-mingw-*/bin | tail -1)}
CC="$TC/i686-w64-mingw32-clang"
C="$ROOT/tools/recomp/out/c"
O="$ROOT/tools/recomp/out/harness${VARIANT:+-$VARIANT}"
mkdir -p "$O/obj"
CFLAGS="${EXTRA_CFLAGS:-} -O1 -w -fno-strict-aliasing -I$ROOT/tools/recomp/rt -I$C"
# lifted code: rebuild objects that are older than their source
ls "$C"/lifted_*.c "$C/table.c" | xargs -P "${JOBS:-16}" -I{} sh -c '
    o="'"$O"'/obj/$(basename {} .c).o"
    [ "$o" -nt {} ] || '"$CC $CFLAGS"' -c {} -o "$o"'
"$CC" $CFLAGS -c "$HERE/harness.c" -o "$O/obj/harness.o"
"$CC" -c "$HERE/native.S" -o "$O/obj/native.o"
"$CC" -o "$O/harness.exe" "$O"/obj/*.o -Wl,--stack,8388608 -Wl,--nxcompat
echo "built $O/harness.exe"

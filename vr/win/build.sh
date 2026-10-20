#!/bin/sh
# Builds jpvr as a proxy version.dll (32-bit) with llvm-mingw.
#   vr/win/build.sh            -> vr/win/out/version.dll
#   vr/win/build.sh install    -> also copies it next to TeknoParrot's
#                                 ElfLdr2/BudgieLoader.exe
# TOOLCHAIN: llvm-mingw bin dir (default: ext/llvm-mingw-*/bin in the repo)
# TP_DIR:    TeknoParrot install (default: ext/TeknoParrot in the repo)
set -eu
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../.." && pwd)
TC=${TOOLCHAIN:-$(ls -d "$ROOT"/ext/llvm-mingw-*/bin | tail -1)}
TP_DIR=${TP_DIR:-$ROOT/ext/TeknoParrot}
mkdir -p "$HERE/out"
"$TC/i686-w64-mingw32-gcc" -O2 -Wall -Wextra -shared -static \
    -o "$HERE/out/version.dll" \
    "$HERE/jpvr.c" "$HERE/diag.c" "$HERE/hooks.S" "$HERE/thunks.S" "$HERE/version.def"
echo "built $HERE/out/version.dll"
if [ "${1:-}" = install ]; then
    cp "$HERE/out/version.dll" "$TP_DIR/ElfLdr2/version.dll"
    echo "installed $TP_DIR/ElfLdr2/version.dll"
fi

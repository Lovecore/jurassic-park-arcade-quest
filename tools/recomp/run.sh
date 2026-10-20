#!/bin/sh
# Run the recompiled game (Windows x64): sh tools/recomp/run.sh [game dir]
# Keys: 4 = P1 start, 5 = P2 start, mouse = P1 aim, left button = P1 fire.
# Env: JPRT_VSYNC=0 / JPRT_FPSCAP=n (pacing), JPRT_AUDIO=0, JPRT_INPUT=rio.
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../.." && pwd)
GAME=${1:-"$ROOT/Jurassic Park (1.08)(2015)[Raw Thrills Linux PC][TP]"}
mkdir -p "$HERE/out/run"
cd "$HERE/out/run" && JPRT_LIBDIR=$(cygpath -m "$ROOT/ext/debs") exec "$HERE/out/process/jp2015.exe" "$(cygpath -m "$GAME")"

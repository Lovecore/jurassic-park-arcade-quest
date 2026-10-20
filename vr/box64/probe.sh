#!/bin/bash
# Q0 perf probe: run the game under box64's box32 mode with native AArch64
# GL/SDL/X11 (wrapped), i386-only libs from the kit rootfs, and the shim.
#   vr/box64/probe.sh [seconds] [ENV=val ...]
# Leaves vr/box64/out/{run.log,vr_shim.log,shots/}.
set -u
SECS=${1:-300}; shift || true
JP=$(cd "$(dirname "$0")/../.." && pwd)
GAME=$(cd "${JP2015_GAME:-$JP/jp_game}" && pwd -P)   # your TP dump (or a symlink to it)
O=$JP/vr/box64/out
mkdir -p "$O/shots" && /bin/rm -f "$O"/shots/*.png "$O/vr_shim.log"
docker run --rm --platform linux/arm64 \
    -v "$JP/vr/kit/out/jp2015-kit:/kit" -v "$JP/vr/kit/test/state:/state" \
    -v "$GAME:/pm" -v "$O:/o" \
    -e SECS="$SECS" -e SHOT_EVERY="${SHOT_EVERY:-60}" -e ENVS="$*" \
    jp2015-box64run sh -c '
Xvfb :0 -screen 0 1360x768x24 -nolisten tcp >/o/xvfb.log 2>&1 & sleep 2
R=/kit/rootfs
export DISPLAY=:0 SDL_AUDIODRIVER=dummy
export BOX64_LD_LIBRARY_PATH=/o/lib:$R/usr/lib/i386-linux-gnu:$R/lib/i386-linux-gnu
export BOX64_LD_PRELOAD=${PRELOAD-$R/jpdeps/vr_shim.so}
export JPVR_LOG=/o/vr_shim.log JPVR_FPS_EVERY=${JPVR_FPS_EVERY:-30}
for kv in $ENVS; do export "$kv"; done
cd /pm
timeout -k 10 $SECS box64 /state/game -n -f1360x768 > /o/run.log 2>&1 &
G=$!; N=0
while kill -0 $G 2>/dev/null; do
    sleep $SHOT_EVERY; N=$((N + SHOT_EVERY))
    xwd -root -silent 2>/dev/null | convert xwd:- /o/shots/t$(printf %04d $N).png 2>/dev/null
done
wait $G; echo "exit=$? (124/137 = still running at timeout)"'

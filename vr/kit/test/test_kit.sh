#!/bin/bash
# Run the built kit on the clean AArch64 host image (no qemu, no i386 libs)
# for <seconds>, screenshotting every SHOT_EVERY s into vr/kit/test/shots/.
#   vr/kit/test/test_kit.sh [seconds] [GUEST_ENV=val ...]
set -u
SECS=${1:-300}; shift || true
JP=$(cd "$(dirname "$0")/../../.." && pwd)
GAME=$(cd "${JP2015_GAME:-$JP/jp_game}" && pwd -P)   # your TP dump (or a symlink to it)
T=$JP/vr/kit/test
mkdir -p "$T/shots" && /bin/rm -f "$T"/shots/*.png
docker run --rm --platform linux/arm64 \
    -v "$JP/vr/kit/out/jp2015-kit:/kit" -v "$GAME:/game" -v "$T:/t" \
    -e SECS="$SECS" -e SHOT_EVERY="${SHOT_EVERY:-60}" -e ENVS="$*" \
    jp2015-cleanhost sh -c '
Xvfb :0 -screen 0 1360x768x24 -nolisten tcp >/t/xvfb.log 2>&1 & sleep 2
export DISPLAY=:0 JPKIT_STATE=/t/state JPVR_LOG=/t/vr_shim.log
for kv in $ENVS; do export "$kv"; done
timeout -k 10 $SECS /kit/run.sh /game > /t/run.log 2>&1 &
G=$!; N=0
while kill -0 $G 2>/dev/null; do
    sleep $SHOT_EVERY; N=$((N + SHOT_EVERY))
    xwd -root -silent 2>/dev/null | convert xwd:- /t/shots/t$(printf %04d $N).png 2>/dev/null
done
wait $G; echo "exit=$? (124/137 = still running at timeout)"'

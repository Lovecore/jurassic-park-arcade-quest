#!/bin/bash
# Q0 boot under qemu-i386 (arm64 Docker VM), headless Xvfb, from the pinned
# rootfs + vr_shim. Usage: vr/q0/boot_qemu.sh [seconds] [GUEST_ENV=val ...]
# Host env: SHOT_EVERY=<s> (default 60) screenshot interval, GALLIUM=<drv>
# (default softpipe; llvmpipe's JIT is unreliable under qemu-i386).
# Leaves: vr/q0/boot.log, vr/q0/vr_shim.log, vr/q0/shots/tNNNN.png, game
# logs in <gamedir>/g5/dino/pmuser/.
set -u
SECS=${1:-150}; shift || true
JP=$(cd "$(dirname "$0")/../.." && pwd)
GAME=$(cd "${JP2015_GAME:-$JP/jp_game}" && pwd -P)   # your TP dump (or a symlink to it)
EXTRA=""
for kv in "$@"; do EXTRA="$EXTRA -E $kv"; done
[ -e "$GAME/g5/dino/pmuser" ] || ln -s ../../pmuser "$GAME/g5/dino/pmuser"
mkdir -p "$JP/vr/q0/shots"
/bin/rm -f "$JP/vr/q0/vr_shim.log" "$JP"/vr/q0/shots/*.png
docker run --rm --platform linux/arm64 -v "$JP/vr/q0/rootfs:/rootfs" \
    -v "$GAME:/pm" -v "$JP:/jp" -e SECS="$SECS" -e EXTRA="$EXTRA" \
    -e GALLIUM="${GALLIUM:-softpipe}" -e SHOT_EVERY="${SHOT_EVERY:-60}" \
    jp2015-qemu sh -c '
Xvfb :99 -screen 0 1360x768x24 -nolisten tcp >/jp/vr/q0/xvfb.log 2>&1 & sleep 2
cd /pm
timeout -k 10 $SECS qemu-i386 -L /rootfs -E DISPLAY=:99 -E SDL_AUDIODRIVER=dummy \
    -E LIBGL_ALWAYS_SOFTWARE=1 -E LP_NATIVE_VECTOR_WIDTH=128 -E GALLIUM_DRIVER=$GALLIUM \
    -E LD_PRELOAD=/jpdeps/vr_shim.so \
    -E JPVR_LOG=/jp/vr/q0/vr_shim.log $EXTRA /pm/game -n -f1360x768 \
    > /jp/vr/q0/boot.log 2>&1 &
G=$!; T=0
while kill -0 $G 2>/dev/null; do
    sleep $SHOT_EVERY; T=$((T + SHOT_EVERY))
    xwd -root -display :99 -silent 2>/dev/null | \
        convert xwd:- /jp/vr/q0/shots/t$(printf %04d $T).png 2>/dev/null
done
wait $G; echo "exit=$? (124/137 = still running at timeout)"'
/bin/cp -f "$(ls -1 "$JP"/vr/q0/shots/*.png 2>/dev/null | tail -1)" "$JP/vr/q0/shot.png" 2>/dev/null
true

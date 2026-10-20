#!/bin/bash
# Q0 boot test — run INSIDE the jp2015-q0 container, cwd = game dir (/pm).
# Usage: ./boot_test.sh [seconds]   (default 45)
set -u
SECS=${1:-45}
GAME=/pm
DEPS=/jp/vr/q0/deps
LOG=/jp/vr/q0/boot.log
SHOT=/jp/vr/q0/boot_shot.png

export DISPLAY=:99
export SDL_AUDIODRIVER=dummy
export LIBGL_ALWAYS_SOFTWARE=1
export LD_LIBRARY_PATH=$DEPS:${LD_LIBRARY_PATH:-}

Xvfb :99 -screen 0 1360x768x24 & XVFB_PID=$!
sleep 2

cd "$GAME"
echo "=== boot $(date) ===" > "$LOG"
timeout --signal=KILL "$SECS" ./game -n -f1360x768 >> "$LOG" 2>&1
RC=$?
echo "=== exit rc=$RC (137=SIGKILL expected; still running at timeout means it stayed up) ===" >> "$LOG"

# framebuffer screenshot (proof of render)
xwd -root -display :99 -silent 2>/dev/null | convert xwd:- "$SHOT" 2>/dev/null \
    && echo "screenshot: $SHOT" >> "$LOG"

kill $XVFB_PID 2>/dev/null
tail -40 "$LOG"

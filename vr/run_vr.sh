#!/bin/sh
# run_vr.sh — Phase 1 launcher: JP2015 with the VR shim preloaded.
#
# Run from anywhere; paths resolve relative to this script.
#   JPVR_WARP=1 ./run_vr.sh     # DK2-style barrel warp instead of identity
#   JPVR_DUMP=0 ./run_vr.sh     # skip post-boot memory dumps
#
# Extra args are passed to the game (e.g. -f1920x1080).

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
JP_ROOT=$(dirname "$SCRIPT_DIR")
GAME_DIR="${JP2015_GAME:-$JP_ROOT/jp_game}"
SHIM="$SCRIPT_DIR/shim/vr_shim.so"
DEPS="$SCRIPT_DIR/deps"   # drop stub/extra 32-bit libs here (e.g. libPhysXLoader.so.1)

if [ ! -f "$SHIM" ]; then
    echo "run_vr.sh: shim not built — run 'make' in $SCRIPT_DIR/shim" >&2
    exit 1
fi

cd "$GAME_DIR" || exit 1

export JPVR_ENABLE=1
export JPVR_WARP=${JPVR_WARP:-0}
export JPVR_DUMP=${JPVR_DUMP:-1}
export LD_PRELOAD="$SHIM${LD_PRELOAD:+:$LD_PRELOAD}"
export LD_LIBRARY_PATH="$DEPS${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"

# No -f flag: in OVR mode the window size comes from VidOVRGetResolution.
exec ./game -n "$@" 2>&1 | tee /tmp/jpvr_game.log

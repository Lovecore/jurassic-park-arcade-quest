#!/bin/sh
# JP2015 kit launcher: runs the Raw Thrills Jurassic Park 1.08 Linux build.
#  - AArch64 host (Quest/Termux, Raspberry Pi, a Docker VM on a Mac): i386
#    user-mode emulation, using only what is in this kit.
#  - x86 host (dev PC, e.g. an NVIDIA box): runs natively, no emulation.
#    With the host's 32-bit loader and GL (multiarch i386 + the NVIDIA
#    driver's 32-bit libs) it uses the real GPU; the kit rootfs fills in
#    any missing i386 library. Without them it uses the kit's loader and
#    software GL. /pm must exist on x86 hosts (run.sh says how).
#
#   ./run.sh <game dir> [game args...]      (default args: -n -f1360x768)
#
# <game dir> is your own TP dump (contains game, g5/, programs_dec/). It is
# never modified except for the game's own save/log dir g5/dino/pmuser/.
# Needs an X display ($DISPLAY). Env passed through to the guest:
#   JPVR_* (shim options), LIBGL_*, GALLIUM_DRIVER, MESA_*, LP_*, SDL_*.
# Kit env: JPKIT_STATE (default <kit>/state), JPKIT_EMU (default
# <kit>/bin/qemu-i386).
set -eu

KIT=$(cd "$(dirname "$0")" && pwd)
[ $# -ge 1 ] || { sed -n '2,15p' "$0" >&2; exit 2; }
GAME_DIR=$(cd "$1" && pwd); shift
[ $# -gt 0 ] || set -- -n -f1360x768
STATE=${JPKIT_STATE:-$KIT/state}
EMU=${JPKIT_EMU:-$KIT/bin/qemu-i386}
PRISTINE=c18a9329e72a363937155d245440694e58b5a1e27984016ae7648af3413b4269
PATCHED=795862124cb4bdf0e1064ab145cd0cd7ff9f030078c2aa74839fb700ae4f7d4a

sha() { sha256sum "$1" 2>/dev/null | cut -d' ' -f1; }

mkdir -p "$STATE"
if [ "$(sha "$STATE/game")" != "$PATCHED" ]; then
    src=""
    for f in "$GAME_DIR/game" "$GAME_DIR/game.q0bak" "$GAME_DIR/game.orig"; do
        [ "$(sha "$f")" = "$PRISTINE" ] && { src=$f; break; }
    done
    [ -n "$src" ] || { echo "run.sh: no pristine TP game binary in $GAME_DIR" >&2; exit 1; }
    cp "$src" "$STATE/game.tmp"
    sh "$KIT/patch_game.sh" "$STATE/game.tmp"
    [ "$(sha "$STATE/game.tmp")" = "$PATCHED" ] || { echo "run.sh: patch failed" >&2; exit 1; }
    chmod 755 "$STATE/game.tmp" && mv "$STATE/game.tmp" "$STATE/game"
fi

# The game reads and writes absolute /pm/... paths.
ln -sfn "$GAME_DIR" "$KIT/rootfs/pm"
# On the cabinet g5/dino/pmuser was a link to /pm/pmuser (saved adjustments,
# audits); dumps drop the link, so restore it.
if [ ! -e "$GAME_DIR/g5/dino/pmuser" ] && [ -d "$GAME_DIR/pmuser" ]; then
    ln -s ../../pmuser "$GAME_DIR/g5/dino/pmuser"
fi
mkdir -p "$GAME_DIR/g5/dino/pmuser"

ENVS="-E LD_PRELOAD=/jpdeps/vr_shim.so -E JPVR_LOG=${JPVR_LOG:-$STATE/vr_shim.log}"
ENVS="$ENVS -E DISPLAY=${DISPLAY:-:0} -E SDL_AUDIODRIVER=${SDL_AUDIODRIVER:-dummy}"
ENVS="$ENVS -E LIBGL_ALWAYS_SOFTWARE=${LIBGL_ALWAYS_SOFTWARE:-1}"
ENVS="$ENVS -E GALLIUM_DRIVER=${GALLIUM_DRIVER:-softpipe}"
for kv in $(env | grep -E '^(JPVR_|LIBGL_|MESA_|LP_|SDL_)' | grep -vE '^(JPVR_LOG|LIBGL_ALWAYS_SOFTWARE|SDL_AUDIODRIVER)='); do
    ENVS="$ENVS -E $kv"
done

cd "$GAME_DIR"
R=$KIT/rootfs
case $(uname -m) in
x86_64|i?86)
    # Native: no -L translation, so /pm itself must point at the game.
    if [ "$(readlink /pm 2>/dev/null)" != "$GAME_DIR" ] &&
       { [ -e /pm ] && [ ! -L /pm ] || ! ln -sfn "$GAME_DIR" /pm 2>/dev/null; }; then
        echo "run.sh: /pm must link to the game folder; run once:" >&2
        echo "  sudo ln -sfn \"$GAME_DIR\" /pm" >&2
        exit 1
    fi
    export JPVR_LOG=${JPVR_LOG:-$STATE/vr_shim.log}
    export SDL_AUDIODRIVER=${SDL_AUDIODRIVER:-dummy}
    # lib32/: prebuilt shim + PhysX stub (+ libglut.so.3 link made by
    # setup_x86_host.sh); the kit rootfs, if built, is the last fallback.
    KITLIBS=$KIT/lib32:$R/usr/lib/i386-linux-gnu:$R/lib/i386-linux-gnu
    SHIM=$KIT/lib32/vr_shim.so
    if [ -e /lib/ld-linux.so.2 ] && [ -z "${JPKIT_KIT_LOADER:-}" ]; then
        # Host i386 glibc + GL first (real GPU); kit libs as fallback.
        export LD_LIBRARY_PATH=${JPKIT_GL_LIBDIR:+$JPKIT_GL_LIBDIR:}$KITLIBS
        export LD_PRELOAD=$SHIM
        exec /lib/ld-linux.so.2 "$STATE/game" "$@"
    fi
    [ -e "$R/lib/ld-linux.so.2" ] || { echo "run.sh: no i386 loader; run setup_x86_host.sh or build the kit" >&2; exit 1; }
    export LIBGL_ALWAYS_SOFTWARE=${LIBGL_ALWAYS_SOFTWARE:-1}
    export GALLIUM_DRIVER=${GALLIUM_DRIVER:-llvmpipe}
    exec "$R/lib/ld-linux.so.2" --library-path "$KITLIBS" \
        --preload "$SHIM" "$STATE/game" "$@"
    ;;
esac
# shellcheck disable=SC2086
exec "$EMU" -L "$R" $ENVS "$STATE/game" "$@"

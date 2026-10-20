#!/bin/bash
# One-time setup of an x86_64 Debian/Ubuntu dev PC (e.g. the RTX 4080 box)
# to run the game natively on the real GPU:
#   - enables i386 multiarch and installs the game's 32-bit runtime libs
#   - checks for the NVIDIA driver's 32-bit GL libraries
#   - links libglut.so.3 into vr/kit/lib32 and /pm -> the game folder
#
#   sudo vr/kit/setup_x86_host.sh "/path/to/Jurassic Park (1.08)(2015)[Raw Thrills Linux PC][TP]"
set -euo pipefail
KIT=$(cd "$(dirname "$0")" && pwd)
GAME_DIR=$(cd "${1:?usage: $0 <game dir>}" && pwd)
[ "$(id -u)" = 0 ] || { echo "run with sudo" >&2; exit 1; }

dpkg --add-architecture i386
apt-get update
# freeglut is libglut3.12 on newer releases, freeglut3 on older ones.
GLUT=libglut3.12:i386
apt-cache show libglut3.12:i386 >/dev/null 2>&1 || GLUT=freeglut3:i386
apt-get install -y libc6:i386 libstdc++6:i386 libsdl1.2debian:i386 libgl1:i386 \
    libglu1-mesa:i386 "$GLUT" libtheora0:i386 libogg0:i386 libvorbisfile3:i386 \
    libudev1:i386 libx11-6:i386 libxext6:i386 libgl1-mesa-dri:i386

# GPU: the NVIDIA driver needs its 32-bit GL userspace for this game.
if command -v nvidia-smi >/dev/null; then
    ver=$(nvidia-smi --query-gpu=driver_version --format=csv,noheader | head -1 | cut -d. -f1)
    if ! ls /usr/lib/i386-linux-gnu/libGLX_nvidia.so.0 >/dev/null 2>&1; then
        echo "NVIDIA driver $ver found but no 32-bit GL libs; installing libnvidia-gl-$ver:i386"
        apt-get install -y "libnvidia-gl-$ver:i386" || \
            echo "WARNING: install the 32-bit NVIDIA GL package for driver $ver manually"
    fi
else
    echo "WARNING: nvidia-smi not found; the game will fall back to Mesa"
fi

glut=$(ls /usr/lib/i386-linux-gnu/libglut.so.3* 2>/dev/null | head -1)
[ -n "$glut" ] && ln -sfn "$glut" "$KIT/lib32/libglut.so.3"

if [ -e /pm ] && [ ! -L /pm ]; then
    echo "WARNING: /pm exists and is not a link; move it aside and rerun" >&2
else
    ln -sfn "$GAME_DIR" /pm
fi
echo "done. Run: $KIT/run.sh \"$GAME_DIR\""

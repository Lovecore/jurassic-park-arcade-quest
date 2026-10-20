#!/bin/bash
# Assemble the self-contained JP2015 kit into vr/kit/out/jp2015-kit/:
#   bin/qemu-i386   static AArch64 qemu-user (Debian bookworm qemu-user-static)
#   rootfs/         minimal i386 bookworm runtime (Dockerfile.rootfs) + shim
#   run.sh, patch_game.sh, README.txt
# Needs Docker (with arm64 + 386 platforms) on the build machine only.
# Contains no game files: the kit runs against the user's own dump.
set -euo pipefail
KIT_SRC=$(cd "$(dirname "$0")" && pwd)
JP=$(cd "$KIT_SRC/../.." && pwd)
OUT=$KIT_SRC/out/jp2015-kit

/bin/rm -rf "$OUT" && mkdir -p "$OUT/bin" "$OUT/rootfs"

echo "== shim"
docker run --rm --platform linux/386 -v "$JP/vr/shim:/src" jp2015-q0 \
    sh -c 'cd /src && make clean >/dev/null && make' >/dev/null

echo "== rootfs"
docker build -q --platform linux/386 -f "$KIT_SRC/Dockerfile.rootfs" -t jp2015-rootfs "$KIT_SRC" >/dev/null
cid=$(docker create --platform linux/386 jp2015-rootfs)
docker export "$cid" | tar -x -C "$OUT/rootfs" --exclude='dev/*' --exclude='proc/*' --exclude='sys/*'
docker rm "$cid" >/dev/null
/bin/cp -f "$JP/vr/shim/vr_shim.so" "$OUT/rootfs/jpdeps/vr_shim.so"

echo "== qemu-i386 (static, arm64)"
docker run --rm --platform linux/arm64 -v "$OUT/bin:/out" debian:bookworm-slim sh -c '
    apt-get update -qq && cd /tmp && apt-get download -qq qemu-user-static &&
    dpkg-deb -x qemu-user-static_*.deb x && cp x/usr/bin/qemu-i386-static /out/qemu-i386' >/dev/null

echo "== scripts"
/bin/cp -f "$KIT_SRC/run.sh" "$KIT_SRC/patch_game.sh" "$KIT_SRC/setup_x86_host.sh" "$OUT/"
mkdir -p "$OUT/lib32" && /bin/cp -f "$JP/vr/shim/vr_shim.so" "$KIT_SRC/lib32/libPhysXLoader.so.1" "$OUT/lib32/"
/bin/cp -f "$JP/vr/shim/vr_shim.so" "$KIT_SRC/lib32/vr_shim.so"   # keep the committed prebuilt current
/bin/cp -f "$KIT_SRC/README.txt" "$OUT/" 2>/dev/null || true
chmod +x "$OUT/run.sh" "$OUT/patch_game.sh" "$OUT/bin/qemu-i386"

du -sh "$OUT" "$OUT/rootfs" "$OUT/bin/qemu-i386" | sed 's|'"$KIT_SRC"'/||'

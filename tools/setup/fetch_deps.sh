#!/bin/sh
# Downloads every freely available build input into ext/ (gitignored), checks
# each against its known SHA-256, and unpacks it:
#   ext/debs/        the i386 Ubuntu 14.04 libraries the game loads
#                    (glibc 2.19, libgcc, libstdc++, libogg, libtheora)
#   ext/khronos/     gl.xml (OpenGL registry), for the GL thunk generator
#   ext/openxr/      the Khronos OpenXR loader for Android (Maven Central)
# Already present and matching: skipped. Needs curl and python.
#
#   sh tools/setup/fetch_deps.sh
set -eu
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../.." && pwd)
EXT="$ROOT/ext"
PY="$ROOT/.venv/Scripts/python"; [ -x "$PY" ] || PY="$ROOT/.venv/bin/python"; [ -x "$PY" ] || PY=python

sha() { sha256sum "$1" | cut -d' ' -f1; }

# Python for the recompiler (capstone), in the repo's .venv; no pip cache
# (its default location is outside the repo)
if [ ! -x "$ROOT/.venv/Scripts/python" ] && [ ! -x "$ROOT/.venv/bin/python" ]; then
    echo "creating .venv"
    python -m venv "$ROOT/.venv"
    PY="$ROOT/.venv/Scripts/python"; [ -x "$PY" ] || PY="$ROOT/.venv/bin/python"
fi
"$PY" -m pip install -q --no-cache-dir --disable-pip-version-check -r "$ROOT/requirements.txt"
echo "ok        python: $("$PY" -c 'import capstone; print("capstone", capstone.__version__)')"

# fetch <url> <dest> <sha256>
fetch() {
    if [ -f "$2" ] && [ "$(sha "$2")" = "$3" ]; then echo "ok        $(basename "$2")"; return; fi
    mkdir -p "$(dirname "$2")"
    echo "download  $(basename "$2")"
    curl -fsSL --retry 3 -o "$2.part" "$1"
    if [ "$(sha "$2.part")" != "$3" ]; then
        echo "SHA-256 mismatch for $(basename "$2") (got $(sha "$2.part"))"; rm -f "$2.part"; exit 1
    fi
    mv "$2.part" "$2"
}

LP=https://launchpad.net/ubuntu/+archive/primary/+files
while read -r deb sum; do
    fetch "$LP/$(printf %s "$deb" | sed 's/+/%2B/g')" "$EXT/debs/$deb" "$sum"
    x="$EXT/debs/x_${deb%.deb}"
    [ -d "$x" ] || "$PY" -I "$HERE/extract_deb.py" "$EXT/debs/$deb" "$x"
done <<DEBS
libc6_2.19-0ubuntu6.6_i386.deb 02c9482c301424384bf68fc43d7f0aa73b6a3f6600898cf4bb92c1669279836b
libgcc1_4.9-20140406-0ubuntu1_i386.deb addbdd46018dac987e25af99885522d23867dfd1a669f6af2525e2b9ef8d200f
libstdc++6_4.8.2-19ubuntu1_i386.deb 0adf4abc79879c2201b2818290b79f64df4f743561676e5891f9983a5883d5cc
libogg0_1.3.1-1ubuntu1_i386.deb 2c9de4d22e82a94b6718382024e4db547291781509c5d510bd5198c200942e60
libtheora0_1.1.1+dfsg.1-3.2_i386.deb 413a25560ed4a3fd68a08a9b696c0cbfe9d968887607f42429c378c495d5168d
DEBS

# OpenGL registry, pinned (KhronosGroup/OpenGL-Registry, 2026-08-28)
fetch https://raw.githubusercontent.com/KhronosGroup/OpenGL-Registry/1cdd228e34966dd6b95bd203e9f84faba0f371a1/xml/gl.xml \
      "$EXT/khronos/gl.xml" b9ca2cfa5c676e901c20d34af3407f1687cde0f1336a5ff7a8974d04c7494ad3

# OpenXR loader for Android 1.1.65; build_apk.sh uses its unpacked jni/ and headers
AAR="$EXT/openxr/openxr_loader_for_android-1.1.65.aar"
fetch https://repo1.maven.org/maven2/org/khronos/openxr/openxr_loader_for_android/1.1.65/openxr_loader_for_android-1.1.65.aar \
      "$AAR" a298c326085b09181c85235fb04d37510939bd30938ed4322c093c09780e8643
if [ ! -f "$EXT/openxr/aar/jni/arm64-v8a/libopenxr_loader.so" ]; then
    "$PY" -I -c "import sys, zipfile; zipfile.ZipFile(sys.argv[1]).extractall(sys.argv[2])" "$AAR" "$EXT/openxr/aar"
fi
echo "dependencies ready in $EXT"

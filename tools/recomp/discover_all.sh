#!/bin/sh
# Function discovery for the game and every lifted i386 library, at the
# addresses the recompiled process uses (arcade bases; librt and ld.so free).
set -eu
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../.." && pwd)
PY="$ROOT/.venv/Scripts/python"
[ -x "$PY" ] || PY=python
D=${DEBS:-$ROOT/ext/debs}
L=$D/x_libc6_2.19-0ubuntu6.6_i386/lib/i386-linux-gnu
export JP2015_GAME=${JP2015_GAME:-$ROOT/jp2015-analysis-items/jp2015-analysis}
"$PY" "$HERE/discover.py" | sed -n 2p
while read -r so base; do
    printf '%-22s ' "$(basename "$so")"
    "$PY" "$HERE/discover.py" --so "$so" --base "$base" | sed -n 2p
done <<LIBS
$L/ld-2.19.so b7760000
$L/libc-2.19.so b7141000
$L/libm-2.19.so b7320000
$L/libpthread-2.19.so b736f000
$L/libdl-2.19.so b75f0000
$L/librt-2.19.so b7748000
$D/x_libgcc1_4.9-20140406-0ubuntu1_i386/lib/i386-linux-gnu/libgcc_s.so.1 b72ef000
$D/x_libstdc++6_4.8.2-19ubuntu1_i386/usr/lib/i386-linux-gnu/libstdc++.so.6.0.19 b75f5000
$D/x_libogg0_1.3.1-1ubuntu1_i386/usr/lib/i386-linux-gnu/libogg.so.0.8.1 b76ea000
$D/x_libtheora0_1.1.1+dfsg.1-3.2_i386/usr/lib/i386-linux-gnu/libtheora.so.0.3.10 b76f3000
$L/libnss_files-2.19.so b7790000
LIBS

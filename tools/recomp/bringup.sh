#!/bin/sh
# Bring-up loop for the recompiled process: build, boot, and when the run
# stops at a call into code that was not lifted, record the address as a
# discovery seed for its module and go again.
#   tools/recomp/bringup.sh "<game dir>" [max rounds]
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../.." && pwd)
GAME=$1
MAX=${2:-10}
RUN="$HERE/out/run"
mkdir -p "$RUN" "$HERE/seeds"
module_of() {    # guest address -> seed file name
    a=$((0x$1))
    if   [ $a -ge $((0xb7790000)) ]; then echo libnss_files-2.19.so
    elif [ $a -ge $((0xb7760000)) ]; then echo ld-2.19.so
    elif [ $a -ge $((0xb7748000)) ]; then echo librt-2.19.so
    elif [ $a -ge $((0xb76f3000)) ]; then echo libtheora.so.0.3.10
    elif [ $a -ge $((0xb76ea000)) ]; then echo libogg.so.0.8.1
    elif [ $a -ge $((0xb76de000)) ]; then echo libPhysXLoader.so.1
    elif [ $a -ge $((0xb75f5000)) ]; then echo libstdc++.so.6.0.19
    elif [ $a -ge $((0xb75f0000)) ]; then echo libdl-2.19.so
    elif [ $a -ge $((0xb736f000)) ]; then echo libpthread-2.19.so
    elif [ $a -ge $((0xb7320000)) ]; then echo libm-2.19.so
    elif [ $a -ge $((0xb72ef000)) ]; then echo libgcc_s.so.1
    elif [ $a -ge $((0xb7141000)) ]; then echo libc-2.19.so
    elif [ $a -ge $((0xa0500000)) ] && [ $a -lt $((0xa0600000)) ]; then echo libNxCharacter.so.1
    elif [ $a -ge $((0xa0400000)) ] && [ $a -lt $((0xa0500000)) ]; then echo libNxCooking.so
    elif [ $a -ge $((0xa0000000)) ] && [ $a -lt $((0xa0400000)) ]; then echo libPhysXCore.so
    elif [ $a -ge $((0x08048000)) ] && [ $a -lt $((0x09200000)) ]; then echo game
    else echo NONE; fi
}
i=0
while [ $i -lt "$MAX" ]; do
    i=$((i + 1))
    "$HERE/discover_all.sh" > "$HERE/out/discover.log" 2>&1
    "$HERE/build_process.sh" > "$HERE/out/build_process.log" 2>&1 || { echo "build failed"; tail "$HERE/out/build_process.log"; exit 1; }
    (cd "$RUN" && JPRT_LIBDIR="$ROOT/ext/debs" timeout "${RUNTIME:-120}" "$HERE/out/process/jp2015.exe" "$GAME" > stdout.txt 2> stderr.txt)
    miss=$(grep -a "no lifted function there" "$RUN/jprt.log" | sed 's/.*call to \([0-9a-f]*\):.*/\1/' | head -1)
    if [ -z "$miss" ]; then
        echo "round $i: no missing function; last log lines:"
        tail -5 "$RUN/jprt.log"
        exit 0
    fi
    mod=$(module_of "$miss")
    echo "round $i: missing $miss in $mod"
    if [ "$mod" = NONE ] || grep -qx "$miss" "$HERE/seeds/$mod.txt" 2>/dev/null; then
        echo "  not module code or already seeded: a real bug, stopping"; tail -5 "$RUN/jprt.log"; exit 1
    fi
    echo "$miss" >> "$HERE/seeds/$mod.txt"
done

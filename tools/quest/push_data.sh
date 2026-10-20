#!/bin/sh
# Copies what the Quest build needs at run time to the headset (adb):
#   <game dir>      -> /sdcard/Android/data/com.jp2015.vr/files/game
#   i386 libraries  -> /sdcard/Android/data/com.jp2015.vr/files/libs (flat)
# The game itself is your own copy; nothing here goes into the APK or git.
#
#   sh tools/quest/push_data.sh ["<game dir>"]
# Install and start the APK once first (it creates its data directory).
set -eu
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../.." && pwd)
GAME=${1:-"$ROOT/Jurassic Park (1.08)(2015)[Raw Thrills Linux PC][TP]"}
PKG=com.jp2015.vr
DEST=/sdcard/Android/data/$PKG/files
ADB=${ADB:-adb}
D="$ROOT/ext/debs"
TP="$ROOT/ext/TeknoParrot/ElfLdr2/libs"
L=$D/x_libc6_2.19-0ubuntu6.6_i386/lib/i386-linux-gnu
STAGE="$ROOT/ext/quest_libs"

[ -f "$GAME/game" ] || { echo "no game at $GAME"; exit 1; }
"$ADB" get-state > /dev/null || { echo "no headset: connect the Quest (developer mode, USB debugging)"; exit 1; }

# the libraries the recompiled process loads, flattened (sonames resolved
# by the runtime's alias table)
mkdir -p "$STAGE"
for f in "$L"/ld-2.19.so "$L"/libc-2.19.so "$L"/libm-2.19.so "$L"/libpthread-2.19.so "$L"/libdl-2.19.so \
         "$L"/librt-2.19.so "$L"/libnss_files-2.19.so \
         "$D"/x_libgcc1_4.9-20140406-0ubuntu1_i386/lib/i386-linux-gnu/libgcc_s.so.1 \
         "$D"/x_libstdc++6_4.8.2-19ubuntu1_i386/usr/lib/i386-linux-gnu/libstdc++.so.6.0.19 \
         "$D"/x_libogg0_1.3.1-1ubuntu1_i386/usr/lib/i386-linux-gnu/libogg.so.0.8.1 \
         "$D"/x_libtheora0_1.1.1+dfsg.1-3.2_i386/usr/lib/i386-linux-gnu/libtheora.so.0.3.10 \
         "$TP"/libPhysXLoader.so.1 "$TP"/libPhysXCore.so "$TP"/libNxCooking.so "$TP"/libNxCharacter.so.1; do
    cp "$f" "$STAGE/"
done

"$ADB" shell mkdir -p "$DEST/game" "$DEST/libs"
echo "pushing libraries..."
"$ADB" push "$STAGE/." "$DEST/libs/"
echo "pushing the game (about 5 GB; takes a while)..."
for e in "$GAME"/*; do
    case "$(basename "$e")" in game_original|*.q0bak) continue ;; esac
    "$ADB" push "$e" "$DEST/game/"
done
echo "done. Start the app from the headset (Library > Unknown sources) or:"
echo "  $ADB shell am start -n $PKG/android.app.NativeActivity"
echo "log: $ADB logcat -s jprt  (also $DEST/jprt.log)"

#!/bin/sh
# One step from this PC to a Quest 3 that launches the game from its Library:
# installs (or updates) the APK, and copies the game data the first time.
#
#   sh tools/quest/install.sh ["<game dir>"]
#
# The headset: developer mode on, USB cable in, "Allow USB debugging"
# accepted. Afterwards the game is in Library > Unknown sources as
# "Jurassic Park Arcade". Updating keeps the copied data; uninstalling the
# app deletes it (5 GB to copy again).
set -u
export MSYS_NO_PATHCONV=1
HERE=$(cd "$(dirname "$0")" && pwd)
PKG=com.jp2015.vr
DEST=/sdcard/Android/data/$PKG/files
APK="$HERE/out/jp2015vr.apk"
if [ -z "${ADB:-}" ]; then
    if command -v adb > /dev/null; then ADB=adb
    elif [ -x /e/choom/tools/android-sdk/platform-tools/adb.exe ]; then ADB=/e/choom/tools/android-sdk/platform-tools/adb.exe
    else echo "adb not found: set ADB=<path to adb>"; exit 1
    fi
fi
export ADB
winpath() { if command -v cygpath > /dev/null; then cygpath -m "$1"; else printf "%s" "$1"; fi; }

[ -f "$APK" ] || { echo "no APK at $APK: build it with sh tools/quest/build_apk.sh"; exit 1; }
state=$("$ADB" get-state 2>/dev/null)
if [ "$state" != device ]; then
    echo "no headset ready (adb state: ${state:-none})."
    echo "Plug the Quest in, put it on and accept \"Allow USB debugging\", then run this again."
    exit 1
fi

echo "installing $(basename "$APK")..."
"$ADB" install -r "$(winpath "$APK")" || exit 1

if "$ADB" shell "[ -f $DEST/game/game ]"; then
    echo "game data already on the headset"
else
    # the app creates its data directory on its first start
    echo "first install: starting the app once to create its data folder..."
    "$ADB" shell am start -n $PKG/android.app.NativeActivity > /dev/null
    i=0
    until "$ADB" shell "[ -d $DEST ]" || [ $i -ge 20 ]; do i=$((i + 1)); sleep 1; done
    "$ADB" shell am force-stop $PKG
    sh "$HERE/push_data.sh" "$@" || exit 1
fi
echo
echo "Done. On the headset: Library > Unknown sources > Jurassic Park Arcade."
echo "(The first start after copying the data unpacks it: a few minutes.)"

#!/bin/sh
# Builds the Quest 3 APK of the recompiled game (arm64-v8a, NativeActivity).
#   1. compiles the lifted C (tools/recomp/out/c_all, from build_process.sh /
#      lift.py) and the runtime (tools/recomp/rt) with the Android NDK
#   2. links libjp2015.so (+ the Khronos OpenXR loader)
#   3. packages, aligns and signs tools/quest/out/jp2015vr.apk
# The APK holds no game data: push it with tools/quest/push_data.sh.
#
# Env: ANDROID_SDK (default E:/choom/tools/android-sdk), NDK (default newest
#      in $ANDROID_SDK/ndk), JAVA_HOME (default Android Studio's JBR),
#      JOBS (16), OPT (-O2), ABI (arm64-v8a; x86_64 for the Android emulator,
#      which runs the flat-screen mode)
set -eu
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../.." && pwd)
RC="$ROOT/tools/recomp"
SDK=${ANDROID_SDK:-/e/choom/tools/android-sdk}
NDK=${NDK:-$(ls -d "$SDK"/ndk/* | tail -1)}
BT=$(ls -d "$SDK"/build-tools/* | tail -1)
PLATFORM=$(ls -d "$SDK"/platforms/android-* | tail -1)
JAVA_HOME=${JAVA_HOME:-"/c/Program Files/Android/Android Studio/jbr"}
JOBS=${JOBS:-16}
OPT=${OPT:--O2}
API=29
ABI=${ABI:-arm64-v8a}
case "$ABI" in arm64-v8a) TRIPLE=aarch64 ;; x86_64) TRIPLE=x86_64 ;; *) echo "ABI: arm64-v8a or x86_64"; exit 1 ;; esac
CC="$NDK/toolchains/llvm/prebuilt/windows-x86_64/bin/clang.exe"
XR="$ROOT/ext/openxr/aar"
O="$HERE/out"
[ "$ABI" = arm64-v8a ] || O="$HERE/out-$ABI"
OBJ="$O/obj$OPT"            # per optimisation level
LIBDIR="$O/apk/lib/$ABI"
mkdir -p "$OBJ" "$LIBDIR"

[ -f "$RC/out/c_all/table.c" ] || { echo "lift first: tools/recomp/build_process.sh"; exit 1; }
[ -f "$XR/jni/$ABI/libopenxr_loader.so" ] || { echo "OpenXR loader missing: see tools/quest/README.md"; exit 1; }

TARGET="--target=$TRIPLE-linux-android$API"
CFLAGS="$TARGET $OPT -fPIC -fno-strict-aliasing -ffunction-sections -fdata-sections"
LIFTED_FLAGS="$CFLAGS -w -gline-tables-only -I$RC/rt -I$RC/out/c_all"
RT_FLAGS="$CFLAGS -g -Wall -Wno-unused-function -I$RC/rt -I$XR/prefab/modules/headers/include -I$NDK/sources/android/native_app_glue"

echo "compiling lifted code ($(ls "$RC"/out/c_all/lifted_*.c | wc -l) files, $JOBS jobs)..."
ls "$RC"/out/c_all/lifted_*.c "$RC/out/c_all/table.c" "$RC/out/c_all/thunks.c" "$RC/out/gl_thunks.c" |
    xargs -P "$JOBS" -I{} sh -c '
        o="'"$OBJ"'/$(basename {} .c).o"
        [ "$o" -nt {} ] && [ "$o" -nt "'"$RC/rt/rt.h"'" ] || "'"$CC"'" '"$LIFTED_FLAGS"' -c {} -o "$o"'
for o in "$OBJ"/lifted_*.o; do [ -e "$RC/out/c_all/$(basename "$o" .o).c" ] || rm -f "$o"; done

echo "compiling the runtime..."
for s in kernel syscalls hostlibs hostaudio audiodev hostgame gles glsl_es texdecode plat_posix video_android android_main; do
    "$CC" $RT_FLAGS -c "$RC/rt/$s.c" -o "$OBJ/rt_$s.o"
done
"$CC" $CFLAGS -w -c "$NDK/sources/android/native_app_glue/android_native_app_glue.c" -o "$OBJ/native_app_glue.o"

echo "linking libjp2015.so..."
cp "$XR/jni/$ABI/libopenxr_loader.so" "$LIBDIR/"
"$CC" $TARGET -shared -o "$LIBDIR/libjp2015.so" "$OBJ"/*.o \
    -L"$LIBDIR" -lopenxr_loader -llog -landroid -lEGL -lGLESv3 -laaudio -lm -ldl \
    -Wl,--gc-sections -Wl,-z,max-page-size=16384 -Wl,--no-undefined -u ANativeActivity_onCreate

echo "packaging..."
# resources: the launcher icon (tools/quest/res)
rm -f "$O/res.zip"
"$BT/aapt2.exe" compile --dir "$(cygpath -w "$HERE/res")" -o "$(cygpath -w "$O/res.zip")" || exit 1
"$BT/aapt2.exe" link -o "$O/base.apk" --manifest "$HERE/AndroidManifest.xml" -I "$PLATFORM/android.jar" \
    --min-sdk-version $API --target-sdk-version 32 "$O/res.zip" || exit 1
PY="$ROOT/.venv/Scripts/python"
[ -x "$PY" ] || PY=python
"$PY" -I "$HERE/add_libs.py" "$O/base.apk" "$O/apk" "$O/unaligned.apk"
"$BT/zipalign.exe" -f -p 16 "$O/unaligned.apk" "$O/aligned.apk"

KS="$ROOT/ext/keystore/debug.keystore"
if [ ! -f "$KS" ]; then
    mkdir -p "$(dirname "$KS")"
    "$JAVA_HOME/bin/keytool.exe" -genkeypair -keystore "$(cygpath -w "$KS")" -storepass android -keypass android \
        -alias androiddebugkey -keyalg RSA -keysize 2048 -validity 10000 -dname "CN=Android Debug,O=Android,C=US"
fi
PATH="$JAVA_HOME/bin:$PATH" "$BT/apksigner.bat" sign --ks "$(cygpath -w "$KS")" --ks-pass pass:android \
    --out "$(cygpath -w "$O/jp2015vr.apk")" "$(cygpath -w "$O/aligned.apk")"
ls -la "$O/jp2015vr.apk"
echo "install: adb install -r $O/jp2015vr.apk ; data: tools/quest/push_data.sh"

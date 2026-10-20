# Quest 3 build of the recompiled game

The APK runs the statically recompiled arcade process (`tools/recomp`). It
contains no game files: you push your own copy of the game to the headset.

## What it does

- The game's x86 code (the game plus its i386 libraries) runs as lifted C,
  compiled for arm64. The emulated Linux i386 kernel runs on Android's
  Linux (`rt/plat_posix.c`).
- Rendering is OpenGL ES 3.2 through `rt/gles.c`:
  - shaders are translated to GLSL ES by `rt/glsl_es.c`;
  - S3TC and LATC textures are software-decoded by `rt/texdecode.c`.
- The game renders into an offscreen target, which appears as a 3.2 m wide
  screen 2.4 m in front of you (an OpenXR quad layer, in
  `rt/video_android.c`).
- Audio is the game's own SDL_mixer, played through AAudio.

### Controls (Touch controllers)

| Control | Action |
| --- | --- |
| Right controller aim | Gun cursor (P1) |
| Right trigger | Fire (P1) |
| A | P1 start |
| X | P2 start |
| B | `q` (game input) |
| Y | `w` (game input) |
| Left trigger | Mouse button 3 (game input 0x157) |
| Left thumbstick click | Recentre the screen |

The game is set to free play, so no coins are needed.

## Build (Windows PC)

Prerequisites, all already on this PC:
- Android SDK with NDK 27, build-tools 36 and platform android-36, in
  `E:/choom/tools/android-sdk`.
- Android Studio's JBR (for keytool and apksigner).
- The OpenXR loader AAR, unpacked in `ext/openxr/aar` (from Maven Central:
  `org.khronos.openxr:openxr_loader_for_android`).

Steps:

```sh
sh tools/recomp/build_process.sh     # lift (once) -> tools/recomp/out/c_all
sh tools/quest/build_apk.sh          # -> tools/quest/out/jp2015vr.apk
```

## Install and run

1. Turn on developer mode on the Quest and connect it over USB, with USB
   debugging allowed.
2. Install: `adb install -r tools/quest/out/jp2015vr.apk`
3. Start the app once from Library > Unknown sources. It quits because it
   finds no data.
4. Push the data: `sh tools/quest/push_data.sh`. This copies the game
   folder (about 5 GB) and the i386 libraries to
   `/sdcard/Android/data/com.jp2015.vr/files/`.
5. Start the app again.

To see the log, run `adb logcat -s jprt`. The log is also written to
`files/jprt.log`.

You can set runtime switches in `files/jprt.env`, one `KEY=VALUE` per
line:
- `JPRT_CORES=all`: let guest threads use every core (the default pins
  them to one core for x86 memory ordering).
- `JPRT_AUDIO=0`: turn audio off.
- `JPRT_DECODE_DXT=1`: force software texture decoding.
- `JPRT_SHOT_EVERY=N`: save a frame every N frames.

## Status

- Built, not yet run on a headset.
- Everything except the Android shell (EGL, OpenXR, AAudio) is tested on
  the PC. The PC test uses the same GLES path on an OpenGL ES 3.2 context
  (`JPRT_GLES=1 sh tools/recomp/run.sh`).
- Next: stereo 3D (render the game's camera per eye, using the engine's
  OVR mode 2), and P2 aim from the left controller.

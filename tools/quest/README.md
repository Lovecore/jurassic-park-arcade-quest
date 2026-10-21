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
- The game renders into an offscreen target, which appears as a 2.4 m wide
  screen 2.4 m in front of you (an OpenXR quad layer, in
  `rt/video_android.c`). The layer is mipmapped and uses the Quest
  compositor's supersampling and sharpening (`XR_FB_composition_layer_settings`).
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
| Right thumbstick up / down | Screen bigger / smaller |

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
- `JPRT_NATIVE_DXT=1`: upload S3TC textures natively where the GPU supports them (the default decodes them in software, as the Quest needs anyway).
- `JPRT_SHOT_EVERY=N`: save a frame every N frames.
- `JPRT_SCREEN_W=2.4`: starting screen width in metres (1.2 to 4.8).

## Status (2026-10-20)

Verified:
- **PC, OpenGL ES 3.2 context:** attract mode and gameplay, using the same
  GLES path the headset uses (`JPRT_GLES=1 sh tools/recomp/run.sh`).
- **Android emulator (API 36, x86_64), flat mode:** both the x86_64 APK and
  the arm64 APK (under the emulator's ARM translation) do the same things:
  - boot, attract mode, audio device open;
  - P1 joins with key 4;
  - level 1 plays, and tapping moves the gun reticle.
- **Android XR emulator (API 34):** the OpenXR path works.
  - The runtime is found and the session reaches FOCUSED.
  - The sRGB swapchain is created.
  - The game shows as the screen quad in the composited headset view
    (`ai_docs/quest-recomp/android-xr-emulator-openxr-2026-10-20.png`).

- **Quest 3 (2026-10-21):** boots, the OpenXR session reaches FOCUSED,
  audio plays, and the game shows on the screen quad in both eyes
  (Adreno 740, OpenGL ES 3.2). The `glCopyImageSubData` frame copy works.
  The game runs at about 45-50 fps, with guest threads pinned to one core.

Not yet verified:
- Touch controller bindings in gameplay.
- arm64 memory ordering with `JPRT_CORES=all`. The default, one core for
  guest threads, is the safe setting.

Data push: when adb cannot create directories in the app's storage (seen
on the XR emulator), `push_data.sh` sends a single `data.tar` instead. The
app unpacks it on its next start. You can force this with
`MODE=tar sh tools/quest/push_data.sh`.

Performance (2026-10-21, Quest 3, gameplay): a steady 72 fps; the game
thread uses about 7 ms of CPU per frame (budget 13.9 ms at 72 Hz). The log
prints this every 600 frames ("game ... ms avg ... (cpu ...)"). What it took:
- the game's job worker polled its queue nonstop (a whole core, contending
  for the lock the main thread needs): JobWorker / JobSubmit / JobFlush are
  host functions now (`rt/hostgame.c`) that sleep and wake properly;
- per-call runtime overhead: the thunk-table scan is hashed, find_lifted is
  cached, GL dispatch skips its string compares;
- `RT_MEM` has hidden visibility (no GOT load per guest memory access);
- `XR_EXT_performance_settings`: CPU and GPU at sustained-high.
To profile: `adb push $NDK/simpleperf/bin/android/arm64/simpleperf
/data/local/tmp/` then `simpleperf record --app com.jp2015.vr` (the APK is
profileable).

Next:
- P2 aim from the left controller.
- Performance: a multi-core memory model, and `-O3` / LTO.

## Experimental: stereo and full VR (off; for tinkerers)

The shipped experience is the large flat screen. The code below works but
is **disabled by default**: on 2026-10-21 full head-tracked VR made the
tester motion sick (the rail camera never stops moving), and the stereo
"3D screen" looked wrong. Nothing here runs unless you set it in
`files/jprt.env` (or the environment on the PC).

- `JPRT_VR=1` (Quest): full VR. The engine's compiled-out Oculus DK2 path
  (display mode 2) renders both eyes; the frame goes out as an OpenXR
  projection layer with the head orientation predicted for its display time
  (`rt/video_android.c`, "VR" section). Runs at 72 fps with 1306x1216
  eyes. `JPRT_EYE_H` sets the eye height; the field is symmetric (54 x 55
  degrees half angles: the Quest 3 lenses' widest). Not done: head position,
  the HUD as a floating panel (it is drawn into each eye), aiming with the
  controller ray, comfort options.
- `JPRT_STEREO=1`: both eyes side by side, for a preview on the PC
  (`sh tools/recomp/run.sh`; `JPRT_EYE_W` / `JPRT_EYE_H`, `JPRT_HALF_YFOV`).
- `JPRT_IPD`: eye separation in game units (default 0.064).

How it hooks the engine (`rt/hostgame.c`, `host_funcs.txt`):
- `ParseCommandLineArgs` (0x81b9c10) is wrapped (lift.py `wrap`: the lifted
  body stays as `fo_<addr>`) to set display mode 2 at 0x8db4e6c.
- The seven VidOVR stubs (0x835dfa0-0x835e000) are host functions:
  one-eye resolution, projection (the engine renders `tan(0.65 * yfov)`,
  yfov in radians), IPD (returned in st(0)), head orientation (x y z w, the
  same axes as OpenXR), identity lens warp for shaderOVR.
- The engine sees a one-eye display split into two half-width "screens";
  the real framebuffer is two eyes wide and `hostlibs.c` doubles viewports
  and scissors aimed at it.
- Mode 2 skips the 2D layers 1-6 (HUD, logos, attract text):
  `RenderLoop_RenderLayer` (0x8305bc0) is wrapped to draw them per eye.
- `JPRT_VPLOG=<frame>` traces one frame's GL targets, viewports and draws.

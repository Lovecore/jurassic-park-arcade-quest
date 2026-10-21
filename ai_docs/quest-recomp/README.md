
### 2026-10-20 (cont.) — milestone: the Quest 3 APK (`tools/quest/`)
- **GLES backend** (`rt/gles.c`, `glsl_es.c`, `texdecode.c`):
  - The game's 89 GL entry points were mapped onto ES 3.2.
  - Desktop GLSL 1.50 is translated to GLSL ES. The translator evaluates
    the preprocessor conditionals, promotes int literals, assigns output
    locations from `glBindFragDataLocation`, and renames ES-reserved
    words. All 182 captured shaders validate with glslang.
  - DXT1/DXT5/LATC1/LATC2 are software-decoded (LATC as R8/RG8 plus a
    swizzle, so they stay readable).
  - Proxy textures, `GENERATE_MIPMAP`, `GL_QUADS`, `glGetTexImage` and
    `glDrawBuffer(BACK)` on FBOs are emulated.
  - Tested on the PC through an ES 3.2 context (`JPRT_GLES=1`).
- **Platform layer** (`rt/plat.h`, `plat_win.c`, `plat_posix.c`): locks,
  threads, native futex, time, the 4 GB guest reservation, files and
  directories, fault handling. Audio output (`audiodev.c`) is waveOut on
  Windows and AAudio on Android. The window/context layer (`video.h`) is
  `video_win.c` on Windows and `video_android.c` (EGL + OpenXR) on Android.
- **Android app:** NativeActivity, no Java (`android_main.c`).
  - The game renders into an offscreen "default framebuffer". It is copied
    into an OpenXR sRGB swapchain and shown as a 3.2 m quad layer 2.4 m in
    front of the player.
  - Touch controllers: the right aim ray becomes the SDL mouse (P1 gun),
    the triggers become mouse buttons, A/X are start and B/Y are q/w;
    left-stick click recentres the screen. `khr/simple_controller` is
    bound as a fallback.
  - Without an OpenXR runtime the app falls back to flat mode (EGL window,
    touch input).
- **Arm64 memory ordering:** guest threads are pinned to one core
  (`JPRT_CORES=all` overrides).
- **Build:**
  - `tools/quest/build_apk.sh` (NDK 27, `-O2`, arm64-v8a or x86_64) →
    signed APK of about 96 MB.
  - `tools/quest/push_data.sh` pushes your own game copy and the i386
    libraries (with the `data.tar` fallback).
- **Verified:**
  - Android emulator: attract mode → P1 start → level 1 with tap aiming,
    for both the x86_64 APK and the arm64 APK under translation
    (`android-emulator-gameplay-2026-10-20.png`).
  - Android XR emulator: the OpenXR session reaches FOCUSED and the game
    shows in the headset view (`android-xr-emulator-openxr-2026-10-20.png`).
- **Emulators** (all on E:): AVDs in `ext/avd` (`jp2015`: API 36; `jpxr`:
  Android XR) with `ANDROID_AVD_HOME=E:/jp2015-vr/ext/avd` and
  `ANDROID_USER_HOME=E:/jp2015-vr/ext/android_user`. The XR emulator's SDK
  is `ext/android-sdk`.

### 2026-10-21 — milestone: on the real Quest 3, packaged
- **First run on hardware** (Quest 3, Adreno 740, ES 3.2). Fixes:
  - `xrEnumerateSwapchainFormats`/`Images` use the two-call idiom (the
    Quest lists more than 64 formats: `XR_ERROR_SIZE_INSUFFICIENT`).
  - Blurry 3D scene: the game's post passes draw into COLOR_ATTACHMENT1/2
    through `glDrawBuffers`; ES requires slot i = attachment i and Adreno
    refuses, so the motion-blur velocity map was garbage. `gles.c` swaps the
    attachment into the slot while the FBO is in that state.
  - Screen layer: mipmaps, `XR_FB_composition_layer_settings`
    supersampling + sharpening.
  - Audio: an AAudio data callback reading a ring the game fills (the old
    blocking writes dropped most samples); the stream is reopened when the
    session regains focus (Horizon OS mutes unfocused apps' streams).
- **Performance** (gameplay numbers from the frame log):
  - The game's job worker polled its queue nonstop: JobWorker / JobSubmit /
    JobFlush are host functions that sleep and wake (futex words).
  - Thunk lookup hashed (was 17% of the main thread), `find_lifted`
    cached, GL dispatch without string compares, `RT_MEM` hidden.
  - `XR_EXT_performance_settings` sustained-high. Result: 72 fps, ~7 ms
    game CPU per frame.
- **Stereo / VR experiments** (off by default, documented in
  `tools/quest/README.md`): lift.py `wrap` entries; the engine's DK2 path
  (display mode 2, VidOVR stubs as host functions, one-eye display with a
  doubled framebuffer, per-eye 2D layers); `JPRT_VR=1` as an OpenXR
  projection layer with predicted head orientation. Full VR caused motion
  sickness (the rail camera), so the flat screen stays the experience.
- **Packaging:** "Jurassic Park Arcade" 1.0 with an icon, 4 m screen by
  default (thumbstick size saved), `tools/quest/install.sh`.
- Screenshots: `quest3-gameplay-2026-10-21.jpg`,
  `quest3-mission-select-2026-10-21.jpg` (frames rendered on the headset).

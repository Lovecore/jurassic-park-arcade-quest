# Q0 — emulation perf spike (Mac, Docker arm64 VM)

Goal: prove the 32-bit x86 `game` binary can boot and render under emulation
on AArch64, and measure FPS. Same constraints as Quest (arm64, no x86, no
desktop GL), so this is the dress rehearsal for the Quest APK.

## Stack under test

- Docker Desktop (linux/aarch64 VM) running a `--platform linux/386` Debian
  container → i386 userland emulated by Docker's bundled qemu-i386 binfmt.
  This is the compat baseline; box64+box32 (dynarec) is the perf variant,
  built separately if qemu perf is unacceptable.
- Game dir bind-mounted read-write into the container.
- Boot headless: Xvfb (arm64-native or i386-in-qemu) + Mesa llvmpipe;
  `game -n -f1360x768`. Success = process stays up, SysLog shows boot
  sequence, framebuffer changes (screenshot via xwd/imagemagick).

## Steps

1. `docker run --rm --platform linux/386 -v "$PWD/<gamedir>:/pm" debian:bookworm-slim uname -m` → expect `i686`.
2. Apt-install in a derived image: libSDL-1.2debian, libgl1-mesa-dri
   (llvmpipe), libglu1-mesa, freeglut3, libtheora0, libogg0,
   libvorbisfile3, libudev1, libx11-6, xvfb, x11-apps. (i386 variants — the
   game needs real i386 .so's under qemu-user.)
3. Stub `libPhysXLoader.so.1` (empty .so, i386) into the rootfs.
4. Boot: `xvfb-run ./game -n -f1360x768`; capture logs; screenshot Xvfb.
5. FPS measure: count WndSwap/glXSwapBuffers via shim counter or frame dumps
   over 60 s.
6. VR variant: build vr/shim with `gcc -m32` inside the container,
   `JPVR_ENABLE=1 xvfb-run vr/run_vr.sh`, verify mode==2 + SBS in dumps.

## Decision gate

- ≥30 fps under qemu-i386 llvmpipe → emulation viable, go box64 for more.
- <30 fps qemu but gl4es/virgl path works → try box64+box32 dynarec.
- Everything fails → reconsider: PC-render+stream, or static recompilation.

## Notes / results

(running log appended below)

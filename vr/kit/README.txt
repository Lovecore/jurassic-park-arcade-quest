JP2015 KIT
==========

Runs the Raw Thrills "Jurassic Park" (2015, v1.08) Linux arcade build on an
AArch64 Linux host with nothing installed on that host: the kit carries its
own i386 emulator, i386 runtime libraries and the compatibility shim.

Contents
  run.sh          launcher (POSIX sh: needs sh, sha256sum, cp, dd, ln, grep)
  patch_game.sh   applies the 52 known binary patches with dd
  bin/qemu-i386   static AArch64 qemu-user (Debian bookworm qemu-user-static)
  rootfs/         minimal i386 Debian bookworm runtime + rootfs/jpdeps/vr_shim.so

The kit contains no game files. Point it at your own TP dump:

  DISPLAY=:0 ./run.sh "/path/to/Jurassic Park (1.08)(2015)[Raw Thrills Linux PC][TP]"

First run copies the pristine binary into ./state/game and patches it
(input and output sha256 are verified). The game folder is used in place.
If g5/dino/pmuser is missing, run.sh links it to ../../pmuser as on the
cabinet, so the dump's saved settings are used; the game writes its logs
and saves there.

What the shim provides (details: ai_docs/jp2015-q0-boot/ in the project)
  - GOT repair for the dump's frozen arcade library addresses
  - HASP HL dongle emulation; encrypted programs answered from programs_dec/
  - RIO I/O board emulation (connected, no buttons pressed)
  - dongle memory seeded with cabinet type 4 (upright); JPVR_CABTYPE=n
  - /pm/... mapped to the game folder for all file access (no root needed)
  - realtime-thread, /proc/cpuinfo, /etc/udev, fclose(NULL), copy-reloc fixes
  - shell-outs skipped (JPVR_ALLOW_SYSTEM=1 to run them)
  - GL compatibility for NVIDIA-era GLSL; fps logging (JPVR_FPS_EVERY)

Useful env
  GALLIUM_DRIVER=softpipe   default; llvmpipe's JIT is unreliable under qemu
  JPVR_LOG=path             shim log (default state/vr_shim.log)
  JPVR_GLDIAG=1             log failed shader compiles/links
  JPVR_SAMPLE=ms            sample every thread (where is time going)
  JPKIT_STATE=dir           where the patched binary and log go

Limits (2026-10-19)
  - Software rendering under emulation is far too slow to play; the Quest
    build needs a GPU path (GL->GLES/Vulkan) and a faster emulator (box64).
  - No PhysX runtime: NxCreatePhysicsSDK returns NULL.
  - No audio device (SDL dummy), no gun input yet.
  - Reaches attract mode ("PRESS START TO BEGIN"); 3D frames take ~16 s
    each with software GL under emulation.

Personal interoperability use only. Do not redistribute game files or a
patched game binary.

# Boot fixes in vr_shim (beyond GOT repair)

Each fix exists because the dump assumes the arcade cabinet (root, NVIDIA,
Ubuntu 14.04 x86, dongle, I/O board). All are needed on Quest too.

| Problem | Symptom | Fix (vr/shim) |
|---|---|---|
| HASP HL dongle absent | `Failed aldeen: 2`, assert game_sys.c:760 | `install_hasp_emulation()`: jmp-patch `hasp_login/logout/encrypt/decrypt/read/write/get_size/get_rtc/get_sessioninfo/free` to in-process stand-ins. Crypto is a no-op (`DongleIsPresent` only checks status). Dongle memory backed by `$JPVR_STATE/hasp_mem.bin`. `JPVR_HASPID` sets the reported id. |
| `SCHED_FIFO` threads need root | `Unable to start dongle write thread: 1` (EPERM) | GOT override of `pthread_create`: retry with `PTHREAD_INHERIT_SCHED` |
| `/proc/cpuinfo` is the host's (AArch64) | `strchr(strstr(buf,"MHz"))` NULL deref in `xSysInit` | GOT override of `open`: serve a synthetic i5-4570 cpuinfo from an unlinked temp file |
| Writes `/etc/udev/rules.d/10-usb-automount.rules` as root | fopen NULL, then game calls `fclose(NULL)` | `fopen` redirect of `/etc/udev/rules.d/` into the state dir; NULL-safe `fclose` |
| `R_386_COPY` objects keep dumped values | `fwrite(..., stderr=0xb72eb960)` crash in `xicoOpenPort` | `repair_copy_relocs()`: copy stdin/stdout/stderr, libstdc++ typeinfo/empty-rep, `glutBitmap8By13` from their owning libs |
| Save dir link lost in the dump | game wrote fresh defaults into an empty `g5/dino/pmuser/`; real saves sat unused in `/pm/pmuser/` | `run.sh` restores `g5/dino/pmuser -> ../../pmuser` (cabinet layout). Saved adjustments carry digests and load as valid. |
| qemu `-L` only redirects existing paths | files the game creates under `/pm/...` (logs) silently fail | `paths.c`: every path-taking import (`open/open64/__open_2/fopen/fopen64/__xstat/__xstat64/access/mkdir/opendir/remove/rmdir/unlink/rename`) maps `/pm/` to `JPVR_ROOT` (default cwd = game dir). Same module serves `/proc/cpuinfo` and the udev redirect. |
| Dongle memory all zero | after boot: `GameDiagStart()` (operator setup) instead of attract; screen stuck on "Loading" | HASP emulator seeds dongle main memory (fileid 0xfff2): `[0x1e]=1` initialized, `[0x1d]` cabinet type = 4 (upright; matches the TP cabinet's saved `cabType`). `JPVR_CABTYPE` overrides. |
| RIO I/O board absent | "RIO Disconnected" banner, `InputInit(): RIO not connected` | `io_emu.c`: `RIO_Connect/ConnectEx` set the connected flag (0x8e7d61c) without USB or reader thread; `RIO_SendReport/GetReport` succeed; disconnect clears. Inputs read from the zeroed report buffer = nothing pressed. |
| `glXSwapIntervalSGI` under Xvfb | `X Error GLXBadContextTag` (VendorPrivate) -> game assert | `gl_compat.c`: swap interval calls are no-ops (OpenXR paces frames on Quest) |
| NVIDIA-lenient GLSL | 4 shaders: "writes to both gl_FragData and out_Bloom" | `gl_compat.c` rewrites, per fragment shader, code-only (comments stripped) `gl_FragData[0/1]`/`gl_FragColor` -> declared `jpvr_FragData0/1` when user `out vec4` also exist; declarations go after any `#version`/`#extension`; locations bound 0/1 at link |
| llvmpipe JIT under qemu-i386 | AVX2 detection, then crashes in JIT'd `glGenerateMipmap`, mass FBO/shader failures | test bed uses softpipe (`GALLIUM_DRIVER=softpipe`). Irrelevant on Quest (GPU path). |
| Shell-outs | `ifconfig`, `udevadm`, `mkdir /mnt/usbflash` via the host shell | skipped by default (`system` -> 0, `popen` -> empty stream, `pclose` aware); `JPVR_ALLOW_SYSTEM=1` runs them |
| Encrypted programs | `Silent failure loading program 'CircleFail'` x 878964 | `hasp_decrypt` swaps in `programs_dec` plaintext for known `programs_enc` ciphertext (1516 pairs, len + FNV-1a 64) |

Shell-outs the game makes (logged by the `system`/`popen` overrides):
`ifconfig eth0 | grep 'inet addr:' ...`, `udevadm control --reload-rules`,
`mkdir /mnt/usbflash`. Harmless when they fail; the Quest build should stub
them.

## Copy protection note

The HASP layer reproduces what the TeknoParrot loader does for this TP dump
(the dump was prepared for it). Personal interoperability use on owned
hardware only; do not redistribute game files or the patched binary.

## Diagnostics built into the shim

- Crash reporter (default on, `JPVR_NOCRASHLOG=1` disables): EIP, fault
  address, registers, game-text return addresses. The game installs its
  own SIGSEGV handler in `xSysInit`, which logs a symbolized stack to
  `g5/dino/pmuser/gamelog.txt`; that one wins after `xSysInit`.
- `JPVR_SAMPLE=<ms>`: SIGPROF sampling of the main thread (EIP + nearest
  game return address). Symbolize with `vr/q0/tools/symbolize.py <addr>`.
- `JPVR_GLDIAG=1` (gl_compat.c): GL strings + every failed shader
  compile/link with its info log, for both GOT and GLEW-fetched pointers.

# Arcade library layout (stamped GOT repair)

The TP dump froze each bound import as an absolute address from the arcade
cabinet's process, and the Sentinel envelope overwrote every PLT reloc
offset with `0xdeadbeef` (`push 0xdeadbeef` in each PLT stub), so import
names exist nowhere in the binary. We recover which library sat where, then
name each slot by offset. Generator: `vr/q0/solve_got2.py` -> `got_fixups.h`
(copy into `vr/shim/`).

## Recovered layout (ld.so maps top-down, back to back)

| Range | Library | Build | Evidence |
|---|---|---|---|
| b7141000-b72ef000 | libc-2.19.so | libc6 2.19-0ubuntu6.6 | 218/218 exact (+libc6-dbg for IFUNC impls) |
| b72ef000-b730c000 | libgcc_s.so.1 | libgcc1 4.9-20140406-0ubuntu1 (trusty release) | fills gap exactly; `_Unwind_Resume` |
| b730c000-b731f000 | libudev.so.1.3.5 | libudev1 204-5ubuntu20 | 23/23 |
| b7320000-b7366000 | libm-2.19.so | libc6 2.19-0ubuntu6.6 | 37/37 |
| b736f000-b738b000 | libpthread-2.19.so | libc6 2.19-0ubuntu6.x | 54/54 |
| b738b000-b73c5000 | libglut.so.3.9.0 | freeglut3 2.8.1-1 | starts at pthread end; `glutBitmapCharacter` |
| b73c5000-b7438000 | libGLU.so.1.3.1 | libglu1-mesa 9.0.0-2 | starts at glut end; `gluErrorString`, `gluBuild2DMipmaps` |
| b7439000-b7558000 | libGL.so (NVIDIA 346.x) | nvidia-346 346.96 as reference | ends exactly at SDL; 164/164 call-site enum args valid |
| b7558000-b75f0000 | libSDL-1.2 | 1.2.15-8ubuntu1.1 | 42/42; ends at libdl |
| b75f0000-b75f5000 | libdl-2.19.so | libc6 2.19 | ends at libstdc++ |
| b75f5000-b76de000 | libstdc++.so.6.0.19 | libstdc++6 4.8.2-19ubuntu1 (trusty release) | 29/29 |
| b76dd000-b76ea000 | libPhysXLoader.so.1 | **missing** | 0xd000 hole; slot 0x8add910 = `NxCreatePhysicsSDK(0x02080100)` (SDK 2.8.1) |
| b76ea000-b76f3000 | libogg | 1.3.1-1ubuntu1 | 14/14; ends at libtheora |
| b76f3000-b7746000 | libtheora | 1.1.1+dfsg.1-3.2 | 19/19 |

`run_game.sh` calls `nvidia-settings`, independently confirming the NVIDIA
driver.

## Why v1 was wrong

- Uniform-stride exports (GL dispatch stubs) give near-equal votes to bases
  shifted by whole pages, and the stamped GL values are themselves dense at
  32-byte stride, so any library with aligned symbols collected chance votes
  there (~10 libs "agreed" on base b74fc000). v1 named GL slots after Mesa
  stubs (`glClearColorIiEXT`, `glColor3b`, ...), plus ALSA/pulse/gconv/xcb
  names that the game never imports.
- 4.8.4 libstdc++ at a non-aligned base gave 19 coincidental hits, several
  wrong (`_ZNSs6assignERKSs` for what is the string copy ctor).

## Validation methods

- **Coverage**: every stale value in a library's range resolves exactly.
- **Contiguity**: each library starts where the previous ends (page aligned).
- **Call-site semantics** (`vr/q0/tools/glcheck.py`, `tools/slotctx.py`):
  constant args at call sites must be valid for the named function
  (glEnable caps, glMatrixMode 0x1700.., GLEW passing strings to
  glXGetProcAddressARB, 4-arg `_Rb_tree_insert_and_rebalance`).

## Hand-named slots (`SEMANTIC` in solve_got2.py)

The arcade's NVIDIA glX section sits -0x150 from 346.96; all four agree:

| Slot | Name | Evidence |
|---|---|---|
| 0x8add588 | glXGetProcAddressARB | 1234 GLEW call sites with string arg |
| 0x8add904 | glXGetClientString | glxewGetExtension: (dpy, GLX_EXTENSIONS=3) |
| 0x8add264 | glXQueryVersion | glxewContextInit: (dpy, &maj, &min) |
| 0x8add70c | glXSwapIntervalSGI | SM_SetVSync(0/1) |

Data words without a PLT stub (`0x8adb128`, `0x8adb644`, and their
`0x8adc...` twins) hold non-aligned values and are left untouched.

## Reference packages

Downloaded from Launchpad into `vr/q0/debs/` (extracted under `debs/x/`,
NVIDIA under `debs/nv/x/`). Discovery tool for new gaps: `tools/gapscan.py`.

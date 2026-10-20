# vr/win — VR patches for the game under TeknoParrot (Windows)

`jpvr.c` ports the VR part of `vr/shim/shim.c` to Windows. It is built as
a proxy `version.dll` that goes next to TeknoParrot's ELF loader
(`ElfLdr2/BudgieLoader.exe`, 32-bit). The loader imports VERSION.dll and
it is not a KnownDLL, so this DLL loads at process start, before the game
is mapped. All 17 version.dll exports jump to the system copy
(`thunks.S`, `version.def`, generated; see build.sh history).

On load it waits for the game image at 0x8048000 (TP maps it unrelocated),
checks the expected bytes at every patch site and only then patches:
mode default 0 -> 2 (OVR) and the seven VidOVR stubs. Any mismatch leaves
the game untouched, so other ElfLdr2 games are safe.

    vr/win/build.sh            # -> vr/win/out/version.dll (llvm-mingw from ext/)
    vr/win/build.sh install    # also copies it into ext/TeknoParrot/ElfLdr2/

Remove `ElfLdr2/version.dll` (or set `JPVR_DISABLE=1`) to go back to stock.
Log: `ElfLdr2/jpvr.log` (or `JPVR_LOG`). `JPVR_WARP=1`: DK2 barrel warp.

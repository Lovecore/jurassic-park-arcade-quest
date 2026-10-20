/* Shared between vr_shim translation units. */
#ifndef JPVR_SHIM_H
#define JPVR_SHIM_H

#define HIDDEN __attribute__((visibility("hidden")))

HIDDEN void logmsg(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

/* Real library resolution (dlsym, legacy aliases, glXGetProcAddressARB). */
HIDDEN void *sym_resolve_real(const char *name);

/* gl_compat.c: wrapper for a GL/GLX entry point the shim intercepts, or NULL.
 * Resolves and remembers the real function on first use. */
HIDDEN void *gl_hook(const char *name, void *(*resolve_real)(const char *));

/* shim.c: write a 5-byte near jmp at site (game .text is RWX). */
HIDDEN void patch_jmp(unsigned int site, void *target);

/* shim.c: writable dir for emulator state (JPVR_STATE). */
HIDDEN const char *state_dir(void);

/* paths.c: wrapper for a path-taking libc import (/pm mapping, synthetic
 * /proc/cpuinfo, /etc/udev redirect), or NULL. */
HIDDEN void *path_hook(const char *name);

/* io_emu.c: RIO I/O board emulation (JPVR_RIO=0 disables). */
HIDDEN void install_io_emulation(void);

#endif

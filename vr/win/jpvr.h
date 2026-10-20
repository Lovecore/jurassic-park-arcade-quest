/* jpvr.h — shared declarations for the jpvr proxy DLL (see jpvr.c). */
#ifndef JPVR_H
#define JPVR_H
#include <windows.h>
#include <stdint.h>

#define CB __attribute__((force_align_arg_pointer))

/* jpvr.ini next to the DLL, section [jpvr]; env JPVR_<KEY> overrides. */
typedef struct {
    int enable;     /* VR patches (mode 2 + VidOVR stubs)              */
    int warp;       /* DK2 barrel warp instead of identity             */
    int diag;       /* diagnostics: tracing, GL checks, shots, console */
    int shot_every; /* framebuffer screenshot every N frames (0: off)  */
    int shot_max;   /* stop after this many screenshots                */
} jpvr_cfg;
extern jpvr_cfg g_cfg;
extern char g_dir[MAX_PATH];   /* directory of this DLL, with trailing '\' */

void logmsg(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
int  readable(uint32_t addr, size_t len);
int  write_code(uint32_t addr, const void *bytes, size_t len);
int  patch_call(uint32_t site, uint32_t expected_target, void *fn, const char *name);

/* hooks.S */
uint32_t jpvr_call(void *fn, int n, const uint32_t *args);
extern void *jpvr_tramp[8];

/* diag.c */
void diag_install(void);       /* after the image is mapped, before boot   */
void diag_console_thread(void);
#endif

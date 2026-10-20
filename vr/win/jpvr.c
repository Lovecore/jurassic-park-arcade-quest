/*
 * jpvr.c — Windows port of the shim's VR patches, for the game running
 * under TeknoParrot's ElfLdr2 loader (BudgieLoader.exe, 32-bit).
 *
 * Built as a proxy VERSION.dll that sits next to BudgieLoader.exe: the
 * loader imports VERSION.dll directly and it is not a KnownDLL, so this
 * file loads before the game ELF is mapped. Every version.dll export is
 * forwarded to the system copy (thunks.S). On attach a thread waits for the
 * game image at its link address, verifies the expected bytes, and applies
 * the same patches as vr/shim/shim.c:
 *   1. GameSysStart mode default 0 -> 2 (SCREENTYPE_OVR) at 0x81bde10
 *   2. seven VidOVR stubs (0x835dfa0-0x835e000) -> jmp to our impls
 * TP maps the ELF unrelocated (non-PIE), so the addresses are the Linux
 * ones; the engine calls the stubs with i386 cdecl, which matches.
 *
 * Patches only when every expected byte matches, so other ElfLdr2 games
 * (or another JP version) are left alone.
 *
 * Config: jpvr.ini next to this DLL, section [jpvr] (keys below); an env
 * var JPVR_<KEY> (inherited from TeknoParrotUi) overrides the ini.
 *   enable=1      VR patches (0: forwarding + diagnostics only)
 *   warp=0        1: DK2-style barrel warp instead of identity
 *   diag=1        diagnostics (diag.c): render-path tracing, GL checks,
 *                 framebuffer screenshots, console mirror
 *   shot_every=600, shot_max=12   screenshot cadence in frames
 *   disable=1     do nothing (forwarding only)
 *   JPVR_LOG=path (env only): log file (default: jpvr.log next to this DLL)
 */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "jpvr.h"

jpvr_cfg g_cfg;
char g_dir[MAX_PATH];

/* ---- fixed addresses in the game binary (see vr/shim/shim.c) ---- */
#define A_MODE_INSTR          0x81bde0a
#define A_MODE_IMM            0x81bde10
#define A_VIDOVR_INIT         0x835dfa0
#define A_VIDOVR_SHUTDOWN     0x835dfb0
#define A_VIDOVR_GETRES       0x835dfc0
#define A_VIDOVR_GETPROJ      0x835dfd0
#define A_VIDOVR_GETIPD       0x835dfe0
#define A_VIDOVR_GETORIENT    0x835dff0
#define A_VIDOVR_GETSCONFIG   0x835e000
#define A_DISPLAY_MODE        0x8e7b3e4   /* VidDisp+0x6a4, runtime mode */

/* ---- logging ---- */
static CRITICAL_SECTION g_log_cs;
static FILE *g_log;

void logmsg(const char *fmt, ...)
{
    va_list ap;
    if (!g_log)
        return;
    EnterCriticalSection(&g_log_cs);
    fprintf(g_log, "[%8lu] ", GetTickCount());
    va_start(ap, fmt);
    vfprintf(g_log, fmt, ap);
    va_end(ap);
    fflush(g_log);
    LeaveCriticalSection(&g_log_cs);
}

static int log_throttled(const char *name, long *counter)
{
    long c = InterlockedIncrement(counter);
    if (c <= 5 || (c % 500) == 0) {
        logmsg("%s call #%ld\n", name, c);
        return 1;
    }
    return 0;
}

static int cfg_int(const char *key, int def)
{
    char ini[MAX_PATH], env[48], b[16];
    DWORD n;
    snprintf(env, sizeof(env), "JPVR_%s", key);
    CharUpperA(env);
    n = GetEnvironmentVariableA(env, b, sizeof(b));
    if (n > 0 && n < sizeof(b))
        return atoi(b);
    snprintf(ini, sizeof(ini), "%sjpvr.ini", g_dir);
    return (int)GetPrivateProfileIntA("jpvr", key, def, ini);
}

/* ---- VidOVR implementations (DK2 profile, static head; as shim.c) ---- */
static long g_calls_getproj, g_calls_getipd, g_calls_getorient, g_calls_getsconfig;

static CB void VidOVRInit(void)
{
    logmsg("VidOVRInit\n");
}

static CB void VidOVRShutdown(void)
{
    logmsg("VidOVRShutdown\n");
}

static CB void VidOVRGetResolution(int *w, int *h)
{
    /* DK2 panel: 1920x1080, two 960x1080 eyes side by side. */
    *w = 1920;
    *h = 1080;
    logmsg("VidOVRGetResolution -> %dx%d\n", *w, *h);
}

static CB void VidOVRGetProjParams(float *yfov, float *aspect, float *offset)
{
    *yfov   = 90.0f;
    *aspect = 960.0f / 1080.0f;
    *offset = 0.15f;
    if (log_throttled("VidOVRGetProjParams", &g_calls_getproj))
        logmsg("  yfov=%.3f aspect=%.4f offset=%.4f\n", *yfov, *aspect, *offset);
}

static CB float VidOVRGetIPDistance(void)
{
    log_throttled("VidOVRGetIPDistance", &g_calls_getipd);
    return 0.064f;
}

static CB void VidOVRGetOrientation(float *quat)
{
    /* Identity quaternion (x,y,z,w): static head for now. */
    quat[0] = 0.0f;
    quat[1] = 0.0f;
    quat[2] = 0.0f;
    quat[3] = 1.0f;
    log_throttled("VidOVRGetOrientation", &g_calls_getorient);
}

static CB void VidOVRGetSConfigParams(int screen, float *LensCenter,
                                      float *ScreenCenter, float *Scale,
                                      float *ScaleIn, float *HmdWarpParam,
                                      float *ChromAbParam, float *view,
                                      float *texm)
{
    static const float ident4x4[16] = {
        1, 0, 0, 0,
        0, 1, 0, 0,
        0, 0, 1, 0,
        0, 0, 0, 1
    };

    LensCenter[0] = (screen == 0) ? 0.575f : 0.425f;
    LensCenter[1] = 0.5f;
    ScreenCenter[0] = 0.5f;
    ScreenCenter[1] = 0.5f;
    ScaleIn[0] = 2.0f;
    ScaleIn[1] = 2.0f;
    Scale[0] = 0.5f;
    Scale[1] = 0.5f;

    if (g_cfg.warp) {
        HmdWarpParam[0] = 1.0f;
        HmdWarpParam[1] = 0.22f;
        HmdWarpParam[2] = 0.24f;
        HmdWarpParam[3] = 0.0f;
        ChromAbParam[0] = 0.996f;
        ChromAbParam[1] = -0.004f;
        ChromAbParam[2] = 1.014f;
        ChromAbParam[3] = 0.0f;
    } else {
        HmdWarpParam[0] = 1.0f;
        HmdWarpParam[1] = 0.0f;
        HmdWarpParam[2] = 0.0f;
        HmdWarpParam[3] = 0.0f;
        ChromAbParam[0] = 1.0f;
        ChromAbParam[1] = 0.0f;
        ChromAbParam[2] = 1.0f;
        ChromAbParam[3] = 0.0f;
    }

    memcpy(view, ident4x4, sizeof(ident4x4));
    memcpy(texm, ident4x4, sizeof(ident4x4));

    if (log_throttled("VidOVRGetSConfigParams", &g_calls_getsconfig))
        logmsg("  screen=%d warp=%d LensCenter=(%.3f,%.3f)\n",
               screen, g_cfg.warp, LensCenter[0], LensCenter[1]);
}

/* ---- patch engine ---- */
typedef struct {
    uint32_t       addr;
    const uint8_t *expected;
    uint8_t        len;
    void          *impl;     /* NULL: mode patch */
    const char    *name;
} patch_entry;

static const uint8_t EXP_MODE[]  = { 0xC7, 0x05, 0x6C, 0x4E, 0xDB, 0x08,
                                     0x00, 0x00, 0x00, 0x00 };
static const uint8_t EXP_STUB[]  = { 0xF3, 0xC3 };          /* repz ret  */
static const uint8_t EXP_STUBF[] = { 0xD9, 0xEE, 0xC3 };    /* fldz; ret */

static const patch_entry g_patches[] = {
    { A_MODE_INSTR,        EXP_MODE,  sizeof(EXP_MODE), NULL,                   "mode default -> SCREENTYPE_OVR" },
    { A_VIDOVR_INIT,       EXP_STUB,  2, (void *)VidOVRInit,             "VidOVRInit" },
    { A_VIDOVR_SHUTDOWN,   EXP_STUB,  2, (void *)VidOVRShutdown,         "VidOVRShutdown" },
    { A_VIDOVR_GETRES,     EXP_STUB,  2, (void *)VidOVRGetResolution,    "VidOVRGetResolution" },
    { A_VIDOVR_GETPROJ,    EXP_STUB,  2, (void *)VidOVRGetProjParams,    "VidOVRGetProjParams" },
    { A_VIDOVR_GETIPD,     EXP_STUBF, 3, (void *)VidOVRGetIPDistance,    "VidOVRGetIPDistance" },
    { A_VIDOVR_GETORIENT,  EXP_STUB,  2, (void *)VidOVRGetOrientation,   "VidOVRGetOrientation" },
    { A_VIDOVR_GETSCONFIG, EXP_STUB,  2, (void *)VidOVRGetSConfigParams, "VidOVRGetSConfigParams" },
};
#define N_PATCHES (sizeof(g_patches) / sizeof(g_patches[0]))

int readable(uint32_t addr, size_t len)
{
    MEMORY_BASIC_INFORMATION mbi;
    if (!VirtualQuery((void *)(uintptr_t)addr, &mbi, sizeof(mbi)))
        return 0;
    if (mbi.State != MEM_COMMIT || (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)))
        return 0;
    return (uintptr_t)mbi.BaseAddress + mbi.RegionSize >= addr + len;
}

static int matches(const patch_entry *p)
{
    return readable(p->addr, p->len) &&
           memcmp((const void *)(uintptr_t)p->addr, p->expected, p->len) == 0;
}

int write_code(uint32_t addr, const void *bytes, size_t len)
{
    DWORD old;
    void *a = (void *)(uintptr_t)addr;
    if (!VirtualProtect(a, len, PAGE_EXECUTE_READWRITE, &old))
        return 0;
    memcpy(a, bytes, len);
    VirtualProtect(a, len, old, &old);
    FlushInstructionCache(GetCurrentProcess(), a, len);
    return 1;
}

/* Redirect a `call rel32` at site (verified to target expected_target). */
int patch_call(uint32_t site, uint32_t expected_target, void *fn, const char *name)
{
    uint8_t b[5];
    int32_t rel;
    if (!readable(site, 5) || *(uint8_t *)(uintptr_t)site != 0xE8 ||
        site + 5 + *(int32_t *)(uintptr_t)(site + 1) != expected_target) {
        logmsg("call site %08x (%s): not a call to %08x, left alone\n",
               site, name, expected_target);
        return 0;
    }
    rel = (int32_t)((uintptr_t)fn - site - 5);
    b[0] = 0xE8;
    memcpy(b + 1, &rel, 4);
    rel = write_code(site, b, 5);
    logmsg("call site %08x (%s) -> %s\n", site, name, rel ? "ok" : "FAILED");
    return rel;
}

static void apply(const patch_entry *p)
{
    uint8_t b[5];
    int ok;
    if (!p->impl) {
        uint32_t mode = 2;
        ok = write_code(A_MODE_IMM, &mode, 4);
    } else {
        int32_t rel = (int32_t)((uintptr_t)p->impl - p->addr - 5);
        b[0] = 0xE9;
        memcpy(b + 1, &rel, 4);
        ok = write_code(p->addr, b, 5);
    }
    logmsg("patch %-32s @%08x -> %s\n", p->name, p->addr, ok ? "ok" : "FAILED");
}

static DWORD WINAPI patch_thread(LPVOID arg)
{
    (void)arg;
    DWORD t0 = GetTickCount();
    size_t i;

    /* Spin until the game image is mapped (the loader maps it soon after
     * start; GameSysStart runs well after that). Give up after 2 min. */
    while (!readable(A_MODE_INSTR, sizeof(EXP_MODE))) {
        if (GetTickCount() - t0 > 120000) {
            logmsg("game image never appeared at %08x; giving up\n", A_MODE_INSTR);
            return 0;
        }
        SwitchToThread();
    }
    logmsg("game image mapped after %lu ms\n", GetTickCount() - t0);

    /* The mapping may still be filling: wait briefly for the bytes. */
    for (;;) {
        int all = 1;
        for (i = 0; i < N_PATCHES; i++)
            all &= matches(&g_patches[i]);
        if (all)
            break;
        if (GetTickCount() - t0 > 10000) {
            for (i = 0; i < N_PATCHES; i++)
                if (!matches(&g_patches[i]))
                    logmsg("unexpected bytes at %08x (%s): not JP 1.08 or "
                           "patched too late; leaving the game alone\n",
                           g_patches[i].addr, g_patches[i].name);
            return 0;
        }
        SwitchToThread();
    }

    if (g_cfg.diag)
        diag_install();
    if (g_cfg.enable) {
        for (i = 0; i < N_PATCHES; i++)
            apply(&g_patches[i]);
        logmsg("all patches applied %lu ms after attach\n", GetTickCount() - t0);
    } else {
        logmsg("enable=0: VR patches not applied (stock mode 0)\n");
        return 0;
    }

    /* Report whether GameSysStart took the OVR branch. */
    for (i = 0; i < 60; i++) {
        Sleep(1000);
        if (readable(A_DISPLAY_MODE, 4)) {
            uint32_t m = *(volatile uint32_t *)(uintptr_t)A_DISPLAY_MODE;
            if (m) {
                logmsg("runtime display mode = %u\n", m);
                return 0;
            }
        }
    }
    logmsg("runtime display mode still 0 after 60 s (patched too late?)\n");
    return 0;
}

/* ---- version.dll forwarding (see thunks.S) ---- */
extern void *jpvr_version_ptrs[];
extern const char *const jpvr_version_names[];
extern const int jpvr_version_count;

static int load_real_version(void)
{
    char path[MAX_PATH];
    UINT n = GetSystemDirectoryA(path, MAX_PATH);  /* SysWOW64 for a WOW64 process */
    int i;
    if (!n || n > MAX_PATH - 16)
        return 0;
    strcat(path, "\\version.dll");
    HMODULE real = LoadLibraryA(path);
    if (!real)
        return 0;
    for (i = 0; i < jpvr_version_count; i++)
        jpvr_version_ptrs[i] = (void *)GetProcAddress(real, jpvr_version_names[i]);
    return 1;
}

static void open_log(void)
{
    char path[MAX_PATH];
    DWORD n = GetEnvironmentVariableA("JPVR_LOG", path, MAX_PATH);
    if (!n || n >= MAX_PATH)
        snprintf(path, sizeof(path), "%sjpvr.log", g_dir);
    g_log = fopen(path, "w");
}

static DWORD WINAPI console_thread(LPVOID arg)
{
    (void)arg;
    diag_console_thread();
    return 0;
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID reserved)
{
    (void)reserved;
    if (reason != DLL_PROCESS_ATTACH)
        return TRUE;
    DisableThreadLibraryCalls(inst);
    if (!load_real_version())
        return FALSE;
    {
        char *slash;
        GetModuleFileNameA(inst, g_dir, MAX_PATH);
        slash = strrchr(g_dir, '\\');
        if (slash)
            slash[1] = 0;
    }
    if (cfg_int("disable", 0))
        return TRUE;
    g_cfg.enable     = cfg_int("enable", 1);
    g_cfg.warp       = cfg_int("warp", 0);
    g_cfg.diag       = cfg_int("diag", 1);
    g_cfg.shot_every = cfg_int("shot_every", 600);
    g_cfg.shot_max   = cfg_int("shot_max", 12);

    InitializeCriticalSection(&g_log_cs);
    open_log();
    {
        char exe[MAX_PATH];
        GetModuleFileNameA(NULL, exe, MAX_PATH);
        logmsg("jpvr loaded into %s (pid %lu): enable=%d warp=%d diag=%d shots=%d/%d\n",
               exe, GetCurrentProcessId(), g_cfg.enable, g_cfg.warp, g_cfg.diag,
               g_cfg.shot_every, g_cfg.shot_max);
    }
    HANDLE t = CreateThread(NULL, 0, patch_thread, NULL, 0, NULL);
    if (t)
        CloseHandle(t);
    else
        logmsg("CreateThread failed: %lu\n", GetLastError());
    if (g_cfg.diag && (t = CreateThread(NULL, 0, console_thread, NULL, 0, NULL)))
        CloseHandle(t);
    return TRUE;
}

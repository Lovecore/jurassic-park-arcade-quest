/*
 * vr_shim.c — Phase 1: re-enable the g5 engine's compiled-out OVR render path
 * in the Raw Thrills Jurassic Park 1.08 Linux build.
 *
 * Loaded via LD_PRELOAD into the 32-bit game process. A constructor-spawned
 * thread waits for the Sentinel Envelope to finish unpacking (.text bytes match
 * known plaintext), then applies runtime patches:
 *
 *   1. GameSysStart mode default 0 -> 2 (SCREENTYPE_OVR) at 0x81bde10
 *   2. Seven gutted Vid-level VR stubs (0x835dfa0-0x835e000) -> near jmp to
 *      implementations below.
 *
 * It also always repairs the dump's stamped GOT: the TP patch froze absolute
 * 0xb7xxxxxx library addresses (arcade Ubuntu 14.04 layout) into 580 GOT
 * slots and neutered PLT lazy resolution. Under any other lib layout those
 * are wild jumps; the constructor re-resolves them with dlsym
 * (vr/q0/solve_got2.py generates got_fixups.h from the recovered arcade
 * library layout; see ai_docs/jp2015-q0-boot/).
 *
 * All LOAD segments in this binary are RWX (p_flags=7), so no mprotect needed
 * for .text; the GOT spans are mprotect'ed defensively (RELRO).
 * Non-PIE: all addresses are absolute and stable.
 *
 * Env:
 *   JPVR_ENABLE=1   VR patch thread (GOT repair runs regardless)
 *   JPVR_WARP=1     use DK2-style barrel warp params instead of identity
 *   JPVR_DUMP=1     dump runtime-decrypted protected function bodies to /tmp
 *   JPVR_LOG=path   log file (default /tmp/vr_shim.log)
 *   JPVR_HASP=0     disable the in-process HASP HL emulation (default on)
 *   JPVR_HASPID=n   dongle id reported by hasp_get_sessioninfo
 *   JPVR_CABTYPE=n  cabinet type in dongle memory (default 4 = upright)
 *   JPVR_STATE=dir  writable dir for hasp_mem.bin (default g5/dino/pmuser)
 *   JPVR_RIO=0      disable the RIO I/O board emulation (io_emu.c)
 *   JPVR_ALLOW_SYSTEM=1  really run the game's system()/popen() shell-outs
 *   JPVR_PROGRAMS_ENC / JPVR_PROGRAMS_DEC  ciphertext / plaintext program
 *                   trees for hasp_decrypt (defaults: g5/dino/data/
 *                   programs_enc, programs_dec)
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <pthread.h>
#include <dlfcn.h>
#include <sys/mman.h>
#include <time.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <ucontext.h>
#include <ftw.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <dirent.h>

#include "got_fixups.h"
#include "shim.h"

/* ------------------------------------------------------------------ */
/* Fixed addresses in the game binary                                  */
/* ------------------------------------------------------------------ */

/* GameSysStart: c7 05 6c 4e db 08 00 00 00 00  = mov dword [0x8db4e6c], 0
 * at 0x81bde0a; the imm32 mode value sits at 0x81bde10. */
#define A_MODE_INSTR   0x81bde0a
#define A_MODE_IMM     0x81bde10

/* Vid-level VR stubs (16-byte stride slots). */
#define A_VIDOVR_INIT         0x835dfa0   /* void (void)                          */
#define A_VIDOVR_SHUTDOWN     0x835dfb0   /* void (void)                          */
#define A_VIDOVR_GETRES       0x835dfc0   /* void (int* w, int* h)                */
#define A_VIDOVR_GETPROJ      0x835dfd0   /* void (float* yfov, float* aspect,
                                              float* offset)                     */
#define A_VIDOVR_GETIPD       0x835dfe0   /* float (void)  -- fldz;ret in stub    */
#define A_VIDOVR_GETORIENT    0x835dff0   /* void (float* quat)                   */
#define A_VIDOVR_GETSCONFIG   0x835e000   /* void (int screen, float* LensCenter,
                                              float* ScreenCenter, float* Scale,
                                              float* ScaleIn, float* HmdWarpParam,
                                              float* ChromAbParam, float* view,
                                              float* texm)                       */

/* Envelope-encrypted functions we want to dump post-boot for analysis. */
#define A_DUMP1_START  0x82ffd30  /* RenderLayerSetScreenCam */
#define A_DUMP1_LEN    0x300
#define A_DUMP2_START  0x8358a50  /* VidNumScreens */
#define A_DUMP2_LEN    0x40

/* ------------------------------------------------------------------ */
/* Logging                                                             */
/* ------------------------------------------------------------------ */

static FILE *g_log;
static int g_log_ready;

HIDDEN void logmsg(const char *fmt, ...)
{
    va_list ap;
    if (!g_log_ready)
        return;
    va_start(ap, fmt);
    vfprintf(g_log, fmt, ap);
    va_end(ap);
    fflush(g_log);
}

/* Log the first few calls, then every Nth, for per-frame functions. */
static int log_throttled(const char *name, long *counter)
{
    long c = ++*counter;
    if (c <= 5 || (c % 500) == 0) {
        logmsg("[vr_shim] %s call #%ld\n", name, c);
        return 1;
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* VidOVR implementations (DK2-style profile, static head for Phase 1) */
/* ------------------------------------------------------------------ */

static long g_calls_getproj, g_calls_getipd, g_calls_getorient, g_calls_getsconfig;

static void VidOVRInit(void)
{
    logmsg("[vr_shim] VidOVRInit\n");
}

static void VidOVRShutdown(void)
{
    logmsg("[vr_shim] VidOVRShutdown\n");
}

static void VidOVRGetResolution(int *w, int *h)
{
    /* DK2 panel: 1920x1080, two 960x1080 eyes side by side. */
    *w = 1920;
    *h = 1080;
    logmsg("[vr_shim] VidOVRGetResolution -> %dx%d\n", *w, *h);
}

static void VidOVRGetProjParams(float *yfov, float *aspect, float *offset)
{
    /* Caller converts yfov degrees -> radians. Start conservative; tune
     * against what UpdateCam produces per eye. */
    *yfov   = 90.0f;
    *aspect = 960.0f / 1080.0f;
    *offset = 0.15f;
    if (log_throttled("VidOVRGetProjParams", &g_calls_getproj))
        logmsg("[vr_shim]   yfov=%.3f aspect=%.4f offset=%.4f\n",
               *yfov, *aspect, *offset);
}

static float VidOVRGetIPDistance(void)
{
    log_throttled("VidOVRGetIPDistance", &g_calls_getipd);
    return 0.064f;
}

static void VidOVRGetOrientation(float *quat)
{
    /* Identity quaternion (x,y,z,w): static head for Phase 1. */
    quat[0] = 0.0f;
    quat[1] = 0.0f;
    quat[2] = 0.0f;
    quat[3] = 1.0f;
    log_throttled("VidOVRGetOrientation", &g_calls_getorient);
}

static void VidOVRGetSConfigParams(int screen, float *LensCenter,
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
    int warp = getenv("JPVR_WARP") && atoi(getenv("JPVR_WARP"));

    /* screen 0 = left eye, 1 = right eye (assumed; verify visually).
     * NOTE (from shaderOVRSetupFunc decompilation): the engine's warp-pass
     * setup calls this with screen=0 hardcoded for both eyes, so the
     * per-eye LensCenter mirror below never actually varies at runtime.
     * Moot for the default identity-warp path; Phase 3 must revisit how
     * per-eye distortion is selected (possibly via the texm matrix). */
    LensCenter[0] = (screen == 0) ? 0.575f : 0.425f;
    LensCenter[1] = 0.5f;
    ScreenCenter[0] = 0.5f;
    ScreenCenter[1] = 0.5f;
    ScaleIn[0] = 2.0f;
    ScaleIn[1] = 2.0f;
    Scale[0] = 0.5f;
    Scale[1] = 0.5f;

    if (warp) {
        /* DK2-style polynomial barrel warp + chromatic aberration. */
        HmdWarpParam[0] = 1.0f;
        HmdWarpParam[1] = 0.22f;
        HmdWarpParam[2] = 0.24f;
        HmdWarpParam[3] = 0.0f;
        ChromAbParam[0] = 0.996f;
        ChromAbParam[1] = -0.004f;
        ChromAbParam[2] = 1.014f;
        ChromAbParam[3] = 0.0f;
    } else {
        /* Identity warp: output = input, raw SBS image. */
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
        logmsg("[vr_shim]   screen=%d warp=%d LensCenter=(%.3f,%.3f)\n",
               screen, warp, LensCenter[0], LensCenter[1]);
}

/* ------------------------------------------------------------------ */
/* Patch engine                                                        */
/* ------------------------------------------------------------------ */

static void emit_jmp(uint32_t site, void *target)
{
    volatile uint8_t *p = (volatile uint8_t *)(uintptr_t)site;
    int32_t rel = (int32_t)((uintptr_t)target - (uintptr_t)site - 5);
    p[0] = 0xE9;
    p[1] = (uint8_t)(rel & 0xFF);
    p[2] = (uint8_t)((rel >> 8) & 0xFF);
    p[3] = (uint8_t)((rel >> 16) & 0xFF);
    p[4] = (uint8_t)((rel >> 24) & 0xFF);
}

HIDDEN void patch_jmp(unsigned int site, void *target)
{
    emit_jmp(site, target);
}

static void apply_mode_patch(uint32_t site)
{
    /* site == A_MODE_IMM; instruction starts at A_MODE_INSTR. */
    volatile uint32_t *imm = (volatile uint32_t *)(uintptr_t)site;
    *imm = 2; /* SCREENTYPE_OVR */
}

static void apply_stub_patch(uint32_t site, void *impl)
{
    emit_jmp(site, impl);
}

typedef void (*stub_fn)(void);

typedef struct {
    uint32_t        verify_addr;   /* where expected bytes start */
    const uint8_t  *expected;
    uint8_t         expected_len;
    uint32_t        patch_addr;    /* where to write */
    stub_fn         impl;          /* NULL for the mode patch */
    const char     *name;
    int             done;
} patch_entry;

static const uint8_t EXP_MODE[]  = { 0xC7, 0x05, 0x6C, 0x4E, 0xDB, 0x08,
                                     0x00, 0x00, 0x00, 0x00 };
static const uint8_t EXP_STUB[]  = { 0xF3, 0xC3 };              /* repz ret */
static const uint8_t EXP_STUBF[] = { 0xD9, 0xEE, 0xC3 };        /* fldz; ret */

static patch_entry g_patches[] = {
    { A_MODE_INSTR, EXP_MODE,  sizeof(EXP_MODE),  A_MODE_IMM,         NULL,
      "mode default -> SCREENTYPE_OVR", 0 },
    { A_VIDOVR_INIT,       EXP_STUB,  2, A_VIDOVR_INIT,       (stub_fn)VidOVRInit,
      "VidOVRInit", 0 },
    { A_VIDOVR_SHUTDOWN,   EXP_STUB,  2, A_VIDOVR_SHUTDOWN,   (stub_fn)VidOVRShutdown,
      "VidOVRShutdown", 0 },
    { A_VIDOVR_GETRES,     EXP_STUB,  2, A_VIDOVR_GETRES,     (stub_fn)VidOVRGetResolution,
      "VidOVRGetResolution", 0 },
    { A_VIDOVR_GETPROJ,    EXP_STUB,  2, A_VIDOVR_GETPROJ,    (stub_fn)VidOVRGetProjParams,
      "VidOVRGetProjParams", 0 },
    { A_VIDOVR_GETIPD,     EXP_STUBF, 3, A_VIDOVR_GETIPD,     (stub_fn)VidOVRGetIPDistance,
      "VidOVRGetIPDistance", 0 },
    { A_VIDOVR_GETORIENT,  EXP_STUB,  2, A_VIDOVR_GETORIENT,  (stub_fn)VidOVRGetOrientation,
      "VidOVRGetOrientation", 0 },
    { A_VIDOVR_GETSCONFIG, EXP_STUB,  2, A_VIDOVR_GETSCONFIG, (stub_fn)VidOVRGetSConfigParams,
      "VidOVRGetSConfigParams", 0 },
};

#define N_PATCHES (sizeof(g_patches) / sizeof(g_patches[0]))

/* ------------------------------------------------------------------ */
/* Post-boot memory dumps (decrypted Envelope bodies)                  */
/* ------------------------------------------------------------------ */

static void dump_region(uint32_t vaddr, size_t len, const char *path)
{
    FILE *f = fopen(path, "wb");
    if (!f) {
        logmsg("[vr_shim] dump: cannot open %s\n", path);
        return;
    }
    fwrite((const void *)(uintptr_t)vaddr, 1, len, f);
    fclose(f);
    logmsg("[vr_shim] dumped 0x%08x..+0x%zx -> %s\n", vaddr, len, path);
}

/* ------------------------------------------------------------------ */
/* Stamped-GOT repair (Q0)                                              */
/*                                                                      */
/* The TP dump captured post-run process memory: .got.plt entries were  */
/* frozen with absolute 0xb7xxxxxx addresses from the arcade's Ubuntu   */
/* 14.04 library layout, and PLT slow paths were neutered. Under any    */
/* other library layout those jumps go into the void. got_fixups.h      */
/* (generated by vr/q0/solve_got.py) maps each affected slot to the     */
/* symbol it should hold; we re-resolve with dlsym at load time.        */
/* Slots we could not identify are routed to a trap that logs and       */
/* aborts instead of jumping into the void.                             */
/* ------------------------------------------------------------------ */

/* Some stamped slots resolve to glibc-private or legacy names that modern
 * dlsym no longer returns. Map them to their public equivalents. */
static const struct { const char *stale_sym, *alias; } SYM_ALIASES[] = {
    { "cfree",                      "free" },
    { "__clock_getres",             "clock_getres" },
    { "__pthread_mutex_destroy",    "pthread_mutex_destroy" },
    { "__pthread_mutex_lock",       "pthread_mutex_lock" },
    { "__pthread_mutex_trylock",    "pthread_mutex_trylock" },
    { "__pthread_mutexattr_destroy","pthread_mutexattr_destroy" },
    { "__pthread_rwlock_destroy",   "pthread_rwlock_destroy" },
    { "__pthread_once",             "pthread_once" },
};

/* The arcade ran as root; game threads ask for SCHED_FIFO with explicit
 * scheduling, which an unprivileged process (container, Android app) is
 * refused with EPERM. Retry those with inherited scheduling. */
static int game_pthread_create(pthread_t *t, const pthread_attr_t *attr,
                               void *(*fn)(void *), void *arg)
{
    int rc = pthread_create(t, attr, fn, arg);
    if (rc == EPERM && attr) {
        pthread_attr_t a2 = *attr;
        pthread_attr_setinheritsched(&a2, PTHREAD_INHERIT_SCHED);
        rc = pthread_create(t, &a2, fn, arg);
        static int logged;
        if (!logged++)
            logmsg("[vr_shim] pthread_create: realtime sched refused, "
                   "using inherited (rc=%d)\n", rc);
    }
    return rc;
}

/* Writable dir for emulator state (hasp_mem.bin, redirected root-only
 * files). Default is the game's own save dir. */
HIDDEN const char *state_dir(void)
{
    const char *s = getenv("JPVR_STATE");
    return s ? s : "g5/dino/pmuser";
}

/* xSysInit calls fclose() on a failed fopen() result; glibc crashes on NULL. */
static int game_fclose(FILE *f)
{
    return f ? fclose(f) : EOF;
}

/* Shell-outs are arcade housekeeping (ifconfig, udevadm, mkdir /mnt/usbflash).
 * There is no guest /bin/sh on Quest, so by default they are logged and
 * answered without running: system() succeeds, popen() yields an empty
 * stream. JPVR_ALLOW_SYSTEM=1 runs them for real. */
static int allow_system(void)
{
    const char *v = getenv("JPVR_ALLOW_SYSTEM");
    return v && atoi(v);
}

static int game_system(const char *cmd)
{
    logmsg("[vr_shim] system(\"%s\")%s\n", cmd ? cmd : "(null)",
           allow_system() ? "" : " skipped");
    return allow_system() ? system(cmd) : 0;
}

static FILE *g_fake_popen[16];

static FILE *game_popen(const char *cmd, const char *mode)
{
    logmsg("[vr_shim] popen(\"%s\", %s)%s\n", cmd ? cmd : "(null)", mode,
           allow_system() ? "" : " skipped");
    if (allow_system())
        return popen(cmd, mode);
    FILE *f = tmpfile();
    for (int i = 0; f && i < 16; i++)
        if (!g_fake_popen[i]) {
            g_fake_popen[i] = f;
            break;
        }
    return f;
}

static int game_pclose(FILE *f)
{
    for (int i = 0; i < 16; i++)
        if (f && g_fake_popen[i] == f) {
            g_fake_popen[i] = NULL;
            fclose(f);
            return 0;
        }
    return pclose(f);
}

HIDDEN int game_sigaction(int sig, const struct sigaction *act,
                          struct sigaction *old);

/* Imports the shim answers itself instead of the system library. */
static const struct { const char *sym; void *impl; } SYM_OVERRIDES[] = {
    { "sigaction",      (void *)game_sigaction },
    { "pthread_create", (void *)game_pthread_create },
    { "fclose",         (void *)game_fclose },
    { "system",         (void *)game_system },
    { "popen",          (void *)game_popen },
    { "pclose",         (void *)game_pclose },
};

static void *sym_resolve(const char *name)
{
    void *gl = gl_hook(name, sym_resolve_real);
    if (gl)
        return gl;
    void *fs = path_hook(name);
    if (fs)
        return fs;
    for (size_t i = 0; i < sizeof(SYM_OVERRIDES) / sizeof(SYM_OVERRIDES[0]); i++)
        if (strcmp(name, SYM_OVERRIDES[i].sym) == 0)
            return SYM_OVERRIDES[i].impl;
    return sym_resolve_real(name);
}

HIDDEN void *sym_resolve_real(const char *name)
{
    void *p = dlsym(RTLD_DEFAULT, name);
    if (p)
        return p;
    for (size_t i = 0; i < sizeof(SYM_ALIASES) / sizeof(SYM_ALIASES[0]); i++)
        if (strcmp(name, SYM_ALIASES[i].stale_sym) == 0)
            return dlsym(RTLD_DEFAULT, SYM_ALIASES[i].alias);
    /* The arcade linked NVIDIA's libGL, which exports every GL/GLX entry
     * point. glvnd/Mesa libGL exports only the core ABI; the rest must come
     * from glXGetProcAddressARB. */
    if (strncmp(name, "gl", 2) == 0) {
        typedef void *(*gpa_fn)(const unsigned char *);
        gpa_fn gpa = (gpa_fn)dlsym(RTLD_DEFAULT, "glXGetProcAddressARB");
        if (gpa)
            return gpa((const unsigned char *)name);
    }
    return NULL;
}

/* Per-slot trap stubs: emit "push slotaddr; jmp common_asm". common_asm
 * preserves all registers, calls the C logger, optionally aborts, else
 * returns 0 to the original caller (cdecl: caller cleans args). */
static uint8_t *g_stub_pool;
static size_t   g_stub_used;
static int      g_trap_abort;

/* Only referenced from trap_common_asm: keep it, and hidden so the asm call
 * binds locally (no PLT, no TEXTREL). */
__attribute__((used, visibility("hidden")))
void trap_log(uint32_t slot_addr, uint32_t caller)
{
    static long hits[1024];
    long n = ++hits[(slot_addr >> 2) & 1023];
    if (n <= 20 || (n % 1000) == 0)
        logmsg("[vr_shim] TRAP: unresolved slot 0x%08x called "
               "(caller=0x%08x, hit #%ld)%s\n",
               slot_addr, caller, n, g_trap_abort ? " — aborting" : "");
    if (g_trap_abort)
        abort();
}

__attribute__((naked))
static void trap_common_asm(void)
{
    __asm__ volatile(
        "pusha\n"
        "movl 32(%esp), %eax\n"     /* slotaddr pushed by stub */
        "movl 36(%esp), %edx\n"     /* original return address */
        "pushl %edx\n"
        "pushl %eax\n"
        "call trap_log\n"
        "addl $8, %esp\n"
        "popa\n"
        "addl $4, %esp\n"           /* drop stub's slotaddr */
        "xorl %eax, %eax\n"         /* return 0 */
        "ret\n");
}

static void *make_slot_trap(uint32_t slot_addr)
{
    /* 10 bytes: 68 <imm32>  e9 <rel32> */
    uint8_t *s = g_stub_pool + g_stub_used;
    g_stub_used += 16;
    s[0] = 0x68;
    memcpy(s + 1, &slot_addr, 4);
    s[5] = 0xE9;
    int32_t rel = (int32_t)((uintptr_t)trap_common_asm -
                            (uintptr_t)(s + 5) - 5);
    memcpy(s + 6, &rel, 4);
    return s;
}

static void apply_got_fixups(void)
{
    /* spans: GOT 0x8adb000..0x8ade000 and envelope table 0x90e7000..0x90e8000 */
    static const struct { uintptr_t lo, hi; } spans[] = {
        { 0x8adb000, 0x8ade000 },
        { 0x90e7000, 0x90e8000 },
    };
    for (size_t i = 0; i < sizeof(spans) / sizeof(spans[0]); i++)
        mprotect((void *)spans[i].lo, spans[i].hi - spans[i].lo,
                 PROT_READ | PROT_WRITE | PROT_EXEC);

    /* pool for per-slot trap stubs (16 bytes each, 1024 max) */
    g_stub_pool = mmap(NULL, 1024 * 16, PROT_READ | PROT_WRITE | PROT_EXEC,
                       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    g_stub_used = 0;
    g_trap_abort = getenv("JPVR_TRAPABORT") &&
                   atoi(getenv("JPVR_TRAPABORT")) != 0;

    int fixed = 0, skipped = 0, failed = 0;
    for (size_t i = 0; i < sizeof(GOT_FIXUPS) / sizeof(GOT_FIXUPS[0]); i++) {
        volatile uint32_t *slot =
            (volatile uint32_t *)(uintptr_t)GOT_FIXUPS[i].addr;
        uint32_t cur = *slot;
        /* accept the stamped value, or (descriptor slots we file-patched)
         * the envelope slow-path stub range */
        if (cur != GOT_FIXUPS[i].stale &&
            !(cur >= 0x9047b00 && cur < 0x9048200)) {
            skipped++;   /* someone else (ld.so) already wrote it — keep */
            continue;
        }
        void *sym = sym_resolve(GOT_FIXUPS[i].sym);
        if (!sym) {
            logmsg("[vr_shim] got_fixup: dlsym(%s) FAILED for slot 0x%08x"
                   " — routing to trap\n", GOT_FIXUPS[i].sym,
                   GOT_FIXUPS[i].addr);
            *slot = (uint32_t)(uintptr_t)make_slot_trap(GOT_FIXUPS[i].addr);
            failed++;
            continue;
        }
        *slot = (uint32_t)(uintptr_t)sym;
        fixed++;
    }
    logmsg("[vr_shim] got_fixups: %d fixed, %d skipped (ld.so-owned), "
           "%d dlsym failures\n", fixed, skipped, failed);

    /* No PhysX 2.8.1 runtime in the rootfs yet: NxCreatePhysicsSDK returns
     * NULL through the trap and the game runs without physics. */
    for (size_t i = 0; i < sizeof(GOT_PHYSX) / sizeof(GOT_PHYSX[0]); i++) {
        volatile uint32_t *slot =
            (volatile uint32_t *)(uintptr_t)GOT_PHYSX[i].addr;
        *slot = (uint32_t)(uintptr_t)make_slot_trap(GOT_PHYSX[i].addr);
        logmsg("[vr_shim] got_fixups: %s (slot 0x%08x) stubbed to return 0\n",
               GOT_PHYSX[i].sym, GOT_PHYSX[i].addr);
    }

    int trapped = 0;
    for (size_t i = 0; i < sizeof(GOT_UNRESOLVED) / sizeof(GOT_UNRESOLVED[0]); i++) {
        uint32_t a = GOT_UNRESOLVED[i];
        if (a == 0)
            continue;   /* list terminator */
        if (a >= 0x90e7000 && a < 0x90e8000)
            continue;   /* descriptor slots keep the envelope slow path */
        volatile uint32_t *slot = (volatile uint32_t *)(uintptr_t)a;
        if (*slot >= 0xb7000000 && *slot < 0xb8000000) {
            *slot = (uint32_t)(uintptr_t)make_slot_trap(a);
            trapped++;
        }
    }
    logmsg("[vr_shim] got_fixups: %d unresolved slots trapped\n", trapped);
}

/* R_386_COPY data imports (.rel.dyn). The dump holds the arcade values
 * (stderr = 0xb72eb960, ...) and they are not reliably re-copied at load,
 * so copy each object from the library that defines it. Looked up via that
 * library's handle: RTLD_DEFAULT would find the game's own copy first. */
static const struct { uint32_t addr; const char *lib, *sym; uint32_t size; } COPY_RELOCS[] = {
    { 0x8b4b96c, "libc.so.6",       "stderr", 4 },
    { 0x8b4b9c0, "libc.so.6",       "stdout", 4 },
    { 0x8b4b9e0, "libc.so.6",       "stdin",  4 },
    { 0x8b4b920, "libstdc++.so.6",  "_ZNSs4_Rep20_S_empty_rep_storageE", 16 },
    { 0x8b4b9b0, "libstdc++.so.6",  "_ZNSbIwSt11char_traitsIwESaIwEE4_Rep20_S_empty_rep_storageE", 16 },
    { 0x8b4b930, "libstdc++.so.6",  "_ZTISt9exception", 8 },
    { 0x8b4b940, "libstdc++.so.6",  "_ZTVN10__cxxabiv117__class_type_infoE", 44 },
    { 0x8b4b980, "libstdc++.so.6",  "_ZTVN10__cxxabiv120__si_class_type_infoE", 44 },
    { 0x8b4b9d0, "libglut.so.3",    "glutBitmap8By13", 16 },
};

static void repair_copy_relocs(void)
{
    for (size_t i = 0; i < sizeof(COPY_RELOCS) / sizeof(COPY_RELOCS[0]); i++) {
        void *h = dlopen(COPY_RELOCS[i].lib, RTLD_NOW | RTLD_NOLOAD);
        if (!h)
            h = dlopen(COPY_RELOCS[i].lib, RTLD_NOW | RTLD_GLOBAL);
        void *src = h ? dlsym(h, COPY_RELOCS[i].sym) : NULL;
        uint32_t *dst = (uint32_t *)(uintptr_t)COPY_RELOCS[i].addr;
        uint32_t before = *dst;
        if (!src || src == (void *)dst) {
            logmsg("[vr_shim] copy reloc %s: no source (%s)\n",
                   COPY_RELOCS[i].sym, h ? "dlsym" : dlerror());
            continue;
        }
        memcpy(dst, src, COPY_RELOCS[i].size);
        logmsg("[vr_shim] copy reloc %s @0x%08x: %08x -> %08x\n",
               COPY_RELOCS[i].sym, COPY_RELOCS[i].addr, before, *dst);
    }
}

/* ------------------------------------------------------------------ */
/* Poll thread: wait for unpack, then patch                            */
/* ------------------------------------------------------------------ */

#define POLL_US        100
#define TIMEOUT_MS     30000
#define DUMP_DELAY_SEC 20

static void *patch_thread(void *unused)
{
    size_t remaining = N_PATCHES;
    long waited_us = 0;

    logmsg("[vr_shim] patch thread started\n");

    while (remaining > 0 && waited_us < TIMEOUT_MS * 1000L) {
        for (size_t i = 0; i < N_PATCHES; i++) {
            patch_entry *p = &g_patches[i];
            if (p->done)
                continue;
            volatile uint8_t *site =
                (volatile uint8_t *)(uintptr_t)p->verify_addr;
            if (memcmp((const void *)site, p->expected, p->expected_len) == 0) {
                if (p->impl)
                    apply_stub_patch(p->patch_addr, (void *)p->impl);
                else
                    apply_mode_patch(p->patch_addr);
                p->done = 1;
                remaining--;
                logmsg("[vr_shim] patched: %s (0x%08x)\n",
                       p->name, p->patch_addr);
            }
        }
        usleep(POLL_US);
        waited_us += POLL_US;
    }

    if (remaining > 0) {
        logmsg("[vr_shim] ERROR: %zu patch site(s) never matched expected "
               "bytes after %d ms — protector interference?\n",
               remaining, TIMEOUT_MS);
        for (size_t i = 0; i < N_PATCHES; i++) {
            patch_entry *p = &g_patches[i];
            if (!p->done) {
                volatile uint8_t *site =
                    (volatile uint8_t *)(uintptr_t)p->verify_addr;
                logmsg("[vr_shim]   pending: %s at 0x%08x, bytes=%02x %02x "
                       "%02x %02x\n", p->name, p->verify_addr,
                       site[0], site[1], site[2], site[3]);
            }
        }
        return NULL;
    }

    logmsg("[vr_shim] all patches applied\n");

    if (getenv("JPVR_DUMP") && atoi(getenv("JPVR_DUMP"))) {
        sleep(DUMP_DELAY_SEC);
        dump_region(A_DUMP1_START, A_DUMP1_LEN, "/tmp/rt_dump_screencam.bin");
        dump_region(A_DUMP2_START, A_DUMP2_LEN, "/tmp/rt_dump_numscreens.bin");
    }

    return NULL;
}

/* ------------------------------------------------------------------ */
/* HASP HL emulation                                                   */
/*                                                                      */
/* The Sentinel HASP HL client API is statically linked into the game.  */
/* The TP dump was made to run under a loader that answers it in        */
/* process; we do the same by jumping each entry point to the stand-ins */
/* below. Crypto is a no-op: DongleIsPresent only checks the status     */
/* code. Dongle memory (HASP_FILEID_RW etc.) is backed by a file so     */
/* audit data written by DongleWriteLoop survives restarts.             */
/* ------------------------------------------------------------------ */

#define A_HASP_LOGIN        0x8529650
#define A_HASP_LOGOUT       0x85295f0
#define A_HASP_ENCRYPT      0x8529550
#define A_HASP_DECRYPT      0x85294d0
#define A_HASP_READ         0x8528520
#define A_HASP_WRITE        0x8528470
#define A_HASP_GET_SIZE     0x85283d0
#define A_HASP_GET_RTC      0x8528350
#define A_HASP_SESSIONINFO  0x8528670
#define A_HASP_FREE         0x85282a0

#define HASP_MEM_SIZE       4096
#define HASP_FILES          4      /* RW 0xfff4, RO 0xfff5, other ids */

static uint8_t  g_hasp_mem[HASP_FILES][HASP_MEM_SIZE];
static char     g_hasp_path[512];
static pthread_mutex_t g_hasp_lock = PTHREAD_MUTEX_INITIALIZER;

static int hasp_file_index(uint32_t fileid)
{
    switch (fileid) {
    case 0xfff4: return 0;
    case 0xfff5: return 1;
    case 0:      return 2;
    case 0xfff2: return 3;
    default:     return 3;
    }
}

static void hasp_mem_save(void)
{
    FILE *f = fopen(g_hasp_path, "wb");
    if (!f)
        return;
    fwrite(g_hasp_mem, 1, sizeof(g_hasp_mem), f);
    fclose(f);
}

static int hasp_login(uint32_t feature, const void *vendor_code, uint32_t *handle)
{
    logmsg("[vr_shim] hasp_login(feature=%u)\n", feature);
    if (handle)
        *handle = 0x100 + (feature & 0xff);
    return 0;
}

static int hasp_logout(uint32_t handle)
{
    return 0;
}

/* hasp_decrypt cannot be computed without the key's AES secret, but the TP
 * dump ships the answers: programs_dec/ holds the plaintext of every
 * data/programs_enc/ file (same relative path, same length). Index the
 * ciphertexts by (length, FNV-1a 64) and swap in the plaintext when the
 * game decrypts a buffer we know. Anything else stays a no-op. */
typedef struct { uint32_t len; uint64_t hash; char *plain_path; } CipherEntry;
static CipherEntry *g_cipher;
static size_t g_ncipher, g_cipher_cap;
static char g_enc_root[256], g_dec_root[256];

static uint64_t fnv1a64(const uint8_t *p, size_t n)
{
    uint64_t h = 1469598103934665603ULL;
    for (size_t i = 0; i < n; i++)
        h = (h ^ p[i]) * 1099511628211ULL;
    return h;
}

static int index_cipher(const char *path, const struct stat *st, int type,
                        struct FTW *ftw)
{
    if (type != FTW_F || st->st_size <= 0)
        return 0;
    FILE *f = fopen(path, "rb");
    if (!f)
        return 0;
    uint8_t *buf = malloc((size_t)st->st_size);
    size_t n = buf ? fread(buf, 1, (size_t)st->st_size, f) : 0;
    fclose(f);
    if (n == (size_t)st->st_size) {
        if (g_ncipher == g_cipher_cap) {
            g_cipher_cap = g_cipher_cap ? g_cipher_cap * 2 : 2048;
            g_cipher = realloc(g_cipher, g_cipher_cap * sizeof(*g_cipher));
        }
        char plain[512];
        snprintf(plain, sizeof(plain), "%s%s", g_dec_root, path + strlen(g_enc_root));
        g_cipher[g_ncipher++] = (CipherEntry){ (uint32_t)n, fnv1a64(buf, n), strdup(plain) };
    }
    free(buf);
    return 0;
}

static int hasp_encrypt(uint32_t handle, void *buf, uint32_t len)
{
    return 0;
}

static int hasp_decrypt(uint32_t handle, void *buf, uint32_t len)
{
    if (!buf || !len || !g_ncipher)
        return 0;
    uint64_t h = fnv1a64(buf, len);
    for (size_t i = 0; i < g_ncipher; i++) {
        if (g_cipher[i].len != len || g_cipher[i].hash != h)
            continue;
        FILE *f = fopen(g_cipher[i].plain_path, "rb");
        if (f) {
            size_t n = fread(buf, 1, len, f);
            fclose(f);
            if (n == len)
                return 0;
        }
        logmsg("[vr_shim] hasp_decrypt: plaintext unreadable: %s\n",
               g_cipher[i].plain_path);
        return 0;
    }
    static int misses;
    if (misses++ < 10)
        logmsg("[vr_shim] hasp_decrypt: no plaintext for %u-byte buffer\n", len);
    return 0;
}

static int hasp_read(uint32_t handle, uint32_t fileid, uint32_t off,
                     uint32_t len, void *buf)
{
    if (!buf || off > HASP_MEM_SIZE || len > HASP_MEM_SIZE - off)
        return 9;   /* HASP_MEM_RANGE */
    pthread_mutex_lock(&g_hasp_lock);
    memcpy(buf, g_hasp_mem[hasp_file_index(fileid)] + off, len);
    pthread_mutex_unlock(&g_hasp_lock);
    return 0;
}

static int hasp_write(uint32_t handle, uint32_t fileid, uint32_t off,
                      uint32_t len, const void *buf)
{
    if (!buf || off > HASP_MEM_SIZE || len > HASP_MEM_SIZE - off)
        return 9;
    pthread_mutex_lock(&g_hasp_lock);
    memcpy(g_hasp_mem[hasp_file_index(fileid)] + off, buf, len);
    hasp_mem_save();
    pthread_mutex_unlock(&g_hasp_lock);
    return 0;
}

static int hasp_get_size(uint32_t handle, uint32_t fileid, uint32_t *size)
{
    if (size)
        *size = HASP_MEM_SIZE;
    return 0;
}

static int hasp_get_rtc(uint32_t handle, uint64_t *t)
{
    if (t)
        *t = (uint64_t)time(NULL);
    return 0;
}

static int hasp_get_sessioninfo(uint32_t handle, const char *fmt, char **info)
{
    const char *id = getenv("JPVR_HASPID");
    char buf[256];
    snprintf(buf, sizeof(buf),
             "<?xml version=\"1.0\" encoding=\"UTF-8\" ?>\n<hasp_info>\n"
             "  <keyspec>\n    <hasp>\n      <haspid>%s</haspid>\n"
             "      <typename>HASP HL</typename>\n    </hasp>\n"
             "  </keyspec>\n</hasp_info>\n", id ? id : "1234567890");
    if (!info)
        return 501;  /* HASP_INV_PARAM-ish */
    *info = strdup(buf);
    return 0;
}

static void hasp_free(char *info)
{
    free(info);
}

static void install_hasp_emulation(void)
{
    snprintf(g_hasp_path, sizeof(g_hasp_path), "%s/hasp_mem.bin", state_dir());
    FILE *f = fopen(g_hasp_path, "rb");
    if (f) {
        size_t n = fread(g_hasp_mem, 1, sizeof(g_hasp_mem), f);
        fclose(f);
        logmsg("[vr_shim] hasp: loaded %zu bytes from %s\n", n, g_hasp_path);
    }

    /* Dongle main memory (fileid 0xfff2): [0x1e] = 1 marks it initialized,
     * [0x1d] = cabinet type (0 none .. 4 upright .. 6 namcokit; see
     * data/adj/game/luts/cabType.txt). Type 0 sends BootProcKill to the
     * operator setup menu instead of attract; the TP cabinet was upright. */
    uint8_t *main_mem = g_hasp_mem[hasp_file_index(0xfff2)];
    const char *cab = getenv("JPVR_CABTYPE");
    if (cab || main_mem[0x1d] == 0) {
        main_mem[0x1e] = 1;
        main_mem[0x1d] = (uint8_t)(cab ? atoi(cab) : 4);
        logmsg("[vr_shim] hasp: cabinet type set to %d\n", main_mem[0x1d]);
    }

    const char *enc = getenv("JPVR_PROGRAMS_ENC"), *dec = getenv("JPVR_PROGRAMS_DEC");
    snprintf(g_enc_root, sizeof(g_enc_root), "%s", enc ? enc : "g5/dino/data/programs_enc");
    snprintf(g_dec_root, sizeof(g_dec_root), "%s", dec ? dec : "programs_dec");
    nftw(g_enc_root, index_cipher, 16, FTW_PHYS);
    logmsg("[vr_shim] hasp: %zu encrypted program files indexed (%s -> %s)\n",
           g_ncipher, g_enc_root, g_dec_root);

    static const struct { uint32_t addr; void *impl; const char *name; } H[] = {
        { A_HASP_LOGIN,       (void *)hasp_login,           "hasp_login" },
        { A_HASP_LOGOUT,      (void *)hasp_logout,          "hasp_logout" },
        { A_HASP_ENCRYPT,     (void *)hasp_encrypt,         "hasp_encrypt" },
        { A_HASP_DECRYPT,     (void *)hasp_decrypt,         "hasp_decrypt" },
        { A_HASP_READ,        (void *)hasp_read,            "hasp_read" },
        { A_HASP_WRITE,       (void *)hasp_write,           "hasp_write" },
        { A_HASP_GET_SIZE,    (void *)hasp_get_size,        "hasp_get_size" },
        { A_HASP_GET_RTC,     (void *)hasp_get_rtc,         "hasp_get_rtc" },
        { A_HASP_SESSIONINFO, (void *)hasp_get_sessioninfo, "hasp_get_sessioninfo" },
        { A_HASP_FREE,        (void *)hasp_free,            "hasp_free" },
    };
    for (size_t i = 0; i < sizeof(H) / sizeof(H[0]); i++)
        emit_jmp(H[i].addr, H[i].impl);
    logmsg("[vr_shim] hasp: %zu entry points emulated (state %s)\n",
           sizeof(H) / sizeof(H[0]), g_hasp_path);
}

/* ------------------------------------------------------------------ */
/* Crash reporter                                                      */
/*                                                                      */
/* Logs EIP, fault address, registers and every stack word that points  */
/* into game .text (symbolize offline with symbols_original.txt), then  */
/* re-raises with the default action so cores still happen.            */
/* ------------------------------------------------------------------ */


#define GAME_TEXT_LO 0x08048000u
#define GAME_TEXT_HI 0x08adc000u

static struct sigaction g_game_sa[32];

static void crash_handler(int sig, siginfo_t *si, void *uc_)
{
    ucontext_t *uc = (ucontext_t *)uc_;
    greg_t *r = uc->uc_mcontext.gregs;
    logmsg("[vr_shim] CRASH sig=%d addr=%p eip=%08x esp=%08x ebp=%08x\n"
           "[vr_shim]   eax=%08x ebx=%08x ecx=%08x edx=%08x esi=%08x edi=%08x\n",
           sig, si->si_addr, (unsigned)r[REG_EIP], (unsigned)r[REG_ESP],
           (unsigned)r[REG_EBP], (unsigned)r[REG_EAX], (unsigned)r[REG_EBX],
           (unsigned)r[REG_ECX], (unsigned)r[REG_EDX], (unsigned)r[REG_ESI],
           (unsigned)r[REG_EDI]);
    uint32_t *sp = (uint32_t *)(uintptr_t)r[REG_ESP];
    int shown = 0;
    for (int i = 0; i < 4096 && shown < 24; i++) {
        uint32_t v = sp[i];
        if (v >= GAME_TEXT_LO && v < GAME_TEXT_HI) {
            logmsg("[vr_shim]   stack[%d] = %08x\n", i, v);
            shown++;
        }
    }
    if (sig >= 0 && sig < 32 && g_game_sa[sig].sa_sigaction &&
        (g_game_sa[sig].sa_flags & SA_SIGINFO)) {
        g_game_sa[sig].sa_sigaction(sig, si, uc_);   /* game's own crash log */
        return;
    }
    signal(sig, SIG_DFL);
    raise(sig);
}

/* The game installs its own fatal-signal handler in xSysInit. Keep ours
 * first (it can scan past JIT frames) and chain to the game's. */
HIDDEN int game_sigaction(int sig, const struct sigaction *act,
                          struct sigaction *old)
{
    int fatal = sig == SIGSEGV || sig == SIGBUS || sig == SIGILL || sig == SIGFPE;
    if (!fatal || getenv("JPVR_NOCRASHLOG"))
        return sigaction(sig, act, old);
    if (old)
        *old = g_game_sa[sig];
    if (act)
        g_game_sa[sig] = *act;
    return 0;
}

/* Poor-man's profiler: JPVR_SAMPLE=<ms> makes a thread SIGPROF the main
 * thread every <ms>; the handler logs EIP and the innermost return address
 * in game .text. Tells where boot is stuck and where frame time goes. */
static pthread_t g_main_thread;
static int g_sample_ms;

static void sample_handler(int sig, siginfo_t *si, void *uc_)
{
    greg_t *r = ((ucontext_t *)uc_)->uc_mcontext.gregs;
    uint32_t eip = (uint32_t)r[REG_EIP], ret = 0;
    uint32_t *sp = (uint32_t *)(uintptr_t)r[REG_ESP];
    for (int i = 0; i < 2048 && !ret; i++)
        if (sp[i] >= GAME_TEXT_LO && sp[i] < GAME_TEXT_HI)
            ret = sp[i];
    logmsg("[vr_shim] SAMPLE tid=%ld eip=%08x game_ret=%08x\n",
           (long)syscall(SYS_gettid), eip, ret);
}

/* Samples every thread of the process (loader/worker stalls are invisible
 * from the main thread), skipping the sampler itself. */
static void *sampler_thread(void *unused)
{
    long self = (long)syscall(SYS_gettid);
    for (;;) {
        usleep((useconds_t)g_sample_ms * 1000);
        DIR *d = opendir("/proc/self/task");
        if (!d) {
            pthread_kill(g_main_thread, SIGPROF);
            continue;
        }
        struct dirent *e;
        while ((e = readdir(d))) {
            long tid = atol(e->d_name);
            if (tid > 0 && tid != self) {
                syscall(SYS_tgkill, (long)getpid(), tid, SIGPROF);
                usleep(2000);
            }
        }
        closedir(d);
    }
    return NULL;
}

static void install_sampler(void)
{
    const char *ms = getenv("JPVR_SAMPLE");
    if (!ms || (g_sample_ms = atoi(ms)) <= 0)
        return;
    g_main_thread = pthread_self();
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = sample_handler;
    sa.sa_flags = SA_SIGINFO | SA_RESTART;
    sigaction(SIGPROF, &sa, NULL);
    pthread_t t;
    pthread_create(&t, NULL, sampler_thread, NULL);
    logmsg("[vr_shim] sampler: every %d ms\n", g_sample_ms);
}

static void install_crash_reporter(void)
{
    static uint8_t altstack[64 * 1024];
    stack_t ss = { .ss_sp = altstack, .ss_size = sizeof(altstack) };
    sigaltstack(&ss, NULL);
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = crash_handler;
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
    int sigs[] = { SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGABRT };
    for (size_t i = 0; i < sizeof(sigs) / sizeof(sigs[0]); i++)
        sigaction(sigs[i], &sa, NULL);
}

/* ------------------------------------------------------------------ */
/* Entry                                                               */
/* ------------------------------------------------------------------ */

__attribute__((constructor))
static void vr_shim_ctor(void)
{
    const char *logpath = getenv("JPVR_LOG");
    if (!logpath)
        logpath = "/tmp/vr_shim.log";
    g_log = fopen(logpath, "w");
    if (g_log)
        g_log_ready = 1;

    if (!getenv("JPVR_NOCRASHLOG"))
        install_crash_reporter();
    install_sampler();

    /* Boot-critical under any non-arcade library layout — always apply. */
    apply_got_fixups();
    repair_copy_relocs();

    const char *hasp = getenv("JPVR_HASP");
    if (!hasp || atoi(hasp))
        install_hasp_emulation();
    install_io_emulation();

    const char *en = getenv("JPVR_ENABLE");
    if (!en || !atoi(en))
        return;

    logmsg("[vr_shim] loaded; spawning patch thread\n");

    pthread_t tid;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    if (pthread_create(&tid, &attr, patch_thread, NULL) != 0)
        logmsg("[vr_shim] ERROR: pthread_create failed\n");
    pthread_attr_destroy(&attr);
}

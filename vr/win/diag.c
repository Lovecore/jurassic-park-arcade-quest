/*
 * diag.c — diagnostics for the engine's OVR (mode 2) render path under
 * TeknoParrot. Everything is read-only with respect to game behaviour:
 *
 *  - trace hooks (entry detours, hooks.S) on render-path functions:
 *    per-frame call counts, RenderLayerRender per layer/screen, the
 *    arguments of RenderState_SetRenderTarget; init-time functions are
 *    logged call by call
 *  - frame end: the three WndSwap call sites go through frame_end(), which
 *    logs a summary whenever the frame's render signature changes (page
 *    transitions), drains glGetError, and takes framebuffer screenshots
 *  - GLEW wrappers (installed right after the game's glewInit returns):
 *    shader compile / program link status + info logs, framebuffer
 *    completeness, framebuffer binds and attachments
 *  - console mirror: TeknoParrot's console (game stdout) copied to
 *    jpvr_console.txt every 5 s
 *
 * Output goes next to the DLL: jpvr.log, jpvr_console.txt, shots\*.bmp.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "jpvr.h"

/* ---- game addresses ---- */
#define A_GLEWINIT           0x8527a90
#define A_WNDSWAP            0x835c800
#define A_RENDER_LAYERS      0x8e249e0
#define RENDER_LAYER_SIZE    0x9bec
#define N_LAYERS             8
#define A_DISPLAY_MODE       0x8e7b3e4

/* GLEW function-pointer slots in the game's .bss */
#define G_CompileShader            0x8bc493c
#define G_GetProgramInfoLog        0x8bc4970
#define G_GetProgramiv             0x8bc4974
#define G_GetShaderInfoLog         0x8bc4978
#define G_GetShaderiv              0x8bc4980
#define G_LinkProgram              0x8bc49a8
#define G_ShaderSource             0x8bc49ac
#define G_CompileShaderARB         0x8bc4c6c
#define G_LinkProgramARB           0x8bc4ca8
#define G_ShaderSourceARB          0x8bc4cac
#define G_BindFramebufferEXT       0x8bc5118
#define G_CheckFramebufferStatusEXT 0x8bc5120
#define G_FramebufferTexture2DEXT  0x8bc5134

#define SLOT(a) (*(void *volatile *)(uintptr_t)(a))

/* ---- trace hooks ---- */
enum {
    H_RENDERLAYERRENDER, H_SETSCREENCAM, H_UPDATECAM, H_SETRENDERTARGET,
    H_SHADEROVRSETUP, H_RTINIT, H_LAYERSINIT, N_HOOKS
};

typedef struct {
    uint32_t    addr;
    uint8_t     steal;
    const char *bytes;     /* expected prologue, hex */
    const char *name;
    int         per_frame; /* 0: log every call (init-time function) */
    int         nargs;     /* args to log */
} hook_def;

static const hook_def HOOKS[N_HOOKS] = {
    { 0x8302210,  7, "5557565383ec4c",       "RenderLayerRender",           1, 2 },
    { 0x82ffd30,  7, "5557565383ec7c",       "RenderLayerSetScreenCam",     1, 3 },
    { 0x825ed30,  7, "5557565383ec7c",       "UpdateCam",                   1, 2 },
    { 0x830d750,  5, "565383ec34",           "RenderState_SetRenderTarget", 1, 3 },
    { 0x831f7f0,  9, "5531c057b910000000",   "shaderOVRSetupFunc",          1, 2 },
    { 0x830dd30, 10, "5557565381ec8c030000", "RenderTargetInitialize",      0, 4 },
    { 0x8300030,  5, "5531ed5756",           "RenderLayersInit",            0, 2 },
};

extern void jpvr_hook_stub_0(void), jpvr_hook_stub_1(void), jpvr_hook_stub_2(void),
            jpvr_hook_stub_3(void), jpvr_hook_stub_4(void), jpvr_hook_stub_5(void),
            jpvr_hook_stub_6(void);
static void (*const STUBS[N_HOOKS])(void) = {
    jpvr_hook_stub_0, jpvr_hook_stub_1, jpvr_hook_stub_2, jpvr_hook_stub_3,
    jpvr_hook_stub_4, jpvr_hook_stub_5, jpvr_hook_stub_6,
};

/* per-frame counters (render thread only; diagnostic, no locking) */
static long     f_calls[N_HOOKS];
static long     f_layer[N_LAYERS][4];  /* RenderLayerRender by layer, screen 0..2 / other */
#define MAX_RT 12
static uint32_t f_rt[MAX_RT][3];
static int      f_nrt;
static long     f_binds;
#define MAX_FB 12
static uint32_t f_fb[MAX_FB];
static int      f_nfb;
static long     g_init_logged;

void CB jpvr_diag_record(int id, const uint32_t *args)
{
    int i;
    f_calls[id]++;
    if (id == H_RENDERLAYERRENDER) {
        uint32_t l = (args[0] - A_RENDER_LAYERS) / RENDER_LAYER_SIZE;
        uint32_t s = args[1];
        if (l < N_LAYERS)
            f_layer[l][s < 3 ? s : 3]++;
    } else if (id == H_SETRENDERTARGET) {
        for (i = 0; i < f_nrt; i++)
            if (!memcmp(f_rt[i], args, 12))
                break;
        if (i == f_nrt && f_nrt < MAX_RT)
            memcpy(f_rt[f_nrt++], args, 12);
    }
    if (!HOOKS[id].per_frame && g_init_logged++ < 200) {
        char buf[128];
        int n = 0;
        for (i = 0; i < HOOKS[id].nargs; i++)
            n += snprintf(buf + n, sizeof(buf) - n, " %08x", args[i]);
        logmsg("call %s(%s )\n", HOOKS[id].name, buf);
    }
}

static int hexbytes(const char *hex, uint8_t *out)
{
    int n = 0;
    while (hex[0] && hex[1]) {
        unsigned v;
        sscanf(hex, "%2x", &v);
        out[n++] = (uint8_t)v;
        hex += 2;
    }
    return n;
}

static void install_hooks(void)
{
    uint8_t *pool = VirtualAlloc(NULL, 4096, MEM_COMMIT | MEM_RESERVE,
                                 PAGE_EXECUTE_READWRITE);
    int i;
    if (!pool) {
        logmsg("diag: VirtualAlloc for trampolines failed\n");
        return;
    }
    for (i = 0; i < N_HOOKS; i++) {
        const hook_def *h = &HOOKS[i];
        uint8_t exp[16], jmp[5], *t = pool + i * 32;
        int n = hexbytes(h->bytes, exp);
        int32_t rel;
        if (n != h->steal || !readable(h->addr, n) ||
            memcmp((void *)(uintptr_t)h->addr, exp, n)) {
            logmsg("diag: hook %s @%08x: prologue differs, not hooked\n", h->name, h->addr);
            continue;
        }
        /* trampoline: stolen bytes (all position-independent) + jmp back */
        memcpy(t, exp, n);
        t[n] = 0xE9;
        rel = (int32_t)(h->addr + n - (uintptr_t)(t + n + 5));
        memcpy(t + n + 1, &rel, 4);
        jpvr_tramp[i] = t;
        /* detour */
        jmp[0] = 0xE9;
        rel = (int32_t)((uintptr_t)STUBS[i] - h->addr - 5);
        memcpy(jmp + 1, &rel, 4);
        logmsg("diag: hook %-28s @%08x %s\n", h->name, h->addr,
               write_code(h->addr, jmp, 5) ? "ok" : "FAILED");
    }
}

/* ---- GL helpers (real opengl32 for core GL 1.x queries) ---- */
typedef unsigned (WINAPI *PFN_glGetError)(void);
typedef void (WINAPI *PFN_glGetIntegerv)(unsigned, int *);
typedef void (WINAPI *PFN_glReadPixels)(int, int, int, int, unsigned, unsigned, void *);
typedef void (WINAPI *PFN_glReadBuffer)(unsigned);
typedef void (WINAPI *PFN_glPixelStorei)(unsigned, int);
static PFN_glGetError    p_glGetError;
static PFN_glGetIntegerv p_glGetIntegerv;
static PFN_glReadPixels  p_glReadPixels;
static PFN_glReadBuffer  p_glReadBuffer;
static PFN_glPixelStorei p_glPixelStorei;
typedef HDC (WINAPI *PFN_wglGetCurrentDC)(void);
static PFN_wglGetCurrentDC p_wglGetCurrentDC;

static void resolve_gl(void)
{
    HMODULE gl = GetModuleHandleA("opengl32.dll");
    if (!gl)
        return;
    p_glGetError    = (PFN_glGetError)(void *)GetProcAddress(gl, "glGetError");
    p_glGetIntegerv = (PFN_glGetIntegerv)(void *)GetProcAddress(gl, "glGetIntegerv");
    p_glReadPixels  = (PFN_glReadPixels)(void *)GetProcAddress(gl, "glReadPixels");
    p_glReadBuffer  = (PFN_glReadBuffer)(void *)GetProcAddress(gl, "glReadBuffer");
    p_glPixelStorei = (PFN_glPixelStorei)(void *)GetProcAddress(gl, "glPixelStorei");
    p_wglGetCurrentDC = (PFN_wglGetCurrentDC)(void *)GetProcAddress(gl, "wglGetCurrentDC");
}

/* ---- GLEW wrappers ---- */
static void *o_CompileShader, *o_CompileShaderARB, *o_LinkProgram, *o_LinkProgramARB,
            *o_ShaderSource, *o_ShaderSourceARB, *o_GetShaderiv, *o_GetShaderInfoLog,
            *o_GetProgramiv, *o_GetProgramInfoLog, *o_BindFramebufferEXT,
            *o_CheckFramebufferStatusEXT, *o_FramebufferTexture2DEXT;

#define MAX_SH 4096
static char g_shname[MAX_SH][72];
static long g_compiles, g_compile_fail, g_links, g_link_fail, g_fbo_incomplete, g_attach_logged;

#define CALL(fn, ...) ({ uint32_t a_[] = { __VA_ARGS__ }; \
                         jpvr_call(fn, sizeof(a_) / 4, a_); })

static void stash_source(uint32_t sh, int count, const char *const *strs)
{
    const char *s;
    int n = 0;
    if (sh >= MAX_SH || count <= 0 || !strs || !(s = strs[count - 1]))
        return;
    /* first non-blank text of the last string (the shader body) */
    for (; *s && n < 70; s++) {
        if (*s == '\r' || *s == '\n' || *s == '\t')
            { if (n && g_shname[sh][n - 1] != ' ') g_shname[sh][n++] = ' '; }
        else if (*s != ' ' || (n && g_shname[sh][n - 1] != ' '))
            g_shname[sh][n++] = *s;
    }
    g_shname[sh][n] = 0;
}

static CB void w_ShaderSource(uint32_t sh, int count, const char *const *strs, const int *len)
{
    stash_source(sh, count, strs);
    CALL(o_ShaderSource, sh, (uint32_t)count, (uint32_t)(uintptr_t)strs, (uint32_t)(uintptr_t)len);
}

static CB void w_ShaderSourceARB(uint32_t sh, int count, const char *const *strs, const int *len)
{
    stash_source(sh, count, strs);
    CALL(o_ShaderSourceARB, sh, (uint32_t)count, (uint32_t)(uintptr_t)strs, (uint32_t)(uintptr_t)len);
}

static void check_shader(uint32_t sh)
{
    int ok = 1;
    char info[2048];
    int len = 0;
    g_compiles++;
    CALL(o_GetShaderiv, sh, 0x8B81 /* COMPILE_STATUS */, (uint32_t)(uintptr_t)&ok);
    if (ok)
        return;
    g_compile_fail++;
    info[0] = 0;
    CALL(o_GetShaderInfoLog, sh, sizeof(info), (uint32_t)(uintptr_t)&len, (uint32_t)(uintptr_t)info);
    logmsg("GL compile FAILED shader %u [%s]:\n%s\n", sh,
           sh < MAX_SH ? g_shname[sh] : "?", info);
}

static CB void w_CompileShader(uint32_t sh)
{
    CALL(o_CompileShader, sh);
    check_shader(sh);
}

static CB void w_CompileShaderARB(uint32_t sh)
{
    CALL(o_CompileShaderARB, sh);
    check_shader(sh);
}

static void check_program(uint32_t p)
{
    int ok = 1, len = 0;
    char info[2048];
    g_links++;
    CALL(o_GetProgramiv, p, 0x8B82 /* LINK_STATUS */, (uint32_t)(uintptr_t)&ok);
    if (ok)
        return;
    g_link_fail++;
    info[0] = 0;
    CALL(o_GetProgramInfoLog, p, sizeof(info), (uint32_t)(uintptr_t)&len, (uint32_t)(uintptr_t)info);
    logmsg("GL link FAILED program %u:\n%s\n", p, info);
}

static CB void w_LinkProgram(uint32_t p)
{
    CALL(o_LinkProgram, p);
    check_program(p);
}

static CB void w_LinkProgramARB(uint32_t p)
{
    CALL(o_LinkProgramARB, p);
    check_program(p);
}

static CB void w_BindFramebufferEXT(uint32_t target, uint32_t fb)
{
    int i;
    f_binds++;
    for (i = 0; i < f_nfb && f_fb[i] != fb; i++)
        ;
    if (i == f_nfb && f_nfb < MAX_FB)
        f_fb[f_nfb++] = fb;
    CALL(o_BindFramebufferEXT, target, fb);
}

static CB uint32_t w_CheckFramebufferStatusEXT(uint32_t target)
{
    uint32_t r = CALL(o_CheckFramebufferStatusEXT, target);
    if (r != 0x8CD5 && g_fbo_incomplete++ < 50)
        logmsg("GL framebuffer INCOMPLETE target %04x status %04x\n", target, r);
    return r;
}

static CB void w_FramebufferTexture2DEXT(uint32_t target, uint32_t att, uint32_t tt,
                                         uint32_t tex, int level)
{
    if (g_attach_logged++ < 64)
        logmsg("GL fbo attach target %04x att %04x textarget %04x tex %u level %d\n",
               target, att, tt, tex, level);
    CALL(o_FramebufferTexture2DEXT, target, att, tt, tex, (uint32_t)level);
}

static void wrap(uint32_t slot, void **orig, void *fn, const char *name)
{
    *orig = SLOT(slot);
    if (!*orig) {
        logmsg("diag: GLEW %s is NULL, not wrapped\n", name);
        return;
    }
    SLOT(slot) = fn;
}

static CB uint32_t my_glewInit(void)
{
    uint32_t r = jpvr_call((void *)A_GLEWINIT, 0, NULL);
    static int done;
    logmsg("glewInit -> %u\n", r);
    if (done++)
        return r;
    resolve_gl();
    /* queries first: wrappers call these */
    o_GetShaderiv       = SLOT(G_GetShaderiv);
    o_GetShaderInfoLog  = SLOT(G_GetShaderInfoLog);
    o_GetProgramiv      = SLOT(G_GetProgramiv);
    o_GetProgramInfoLog = SLOT(G_GetProgramInfoLog);
    if (!o_GetShaderiv || !o_GetShaderInfoLog || !o_GetProgramiv || !o_GetProgramInfoLog) {
        logmsg("diag: GLEW query functions missing; GL wrappers off\n");
        return r;
    }
    wrap(G_ShaderSource,     &o_ShaderSource,     (void *)w_ShaderSource,     "ShaderSource");
    wrap(G_ShaderSourceARB,  &o_ShaderSourceARB,  (void *)w_ShaderSourceARB,  "ShaderSourceARB");
    wrap(G_CompileShader,    &o_CompileShader,    (void *)w_CompileShader,    "CompileShader");
    wrap(G_CompileShaderARB, &o_CompileShaderARB, (void *)w_CompileShaderARB, "CompileShaderARB");
    wrap(G_LinkProgram,      &o_LinkProgram,      (void *)w_LinkProgram,      "LinkProgram");
    wrap(G_LinkProgramARB,   &o_LinkProgramARB,   (void *)w_LinkProgramARB,   "LinkProgramARB");
    wrap(G_BindFramebufferEXT, &o_BindFramebufferEXT, (void *)w_BindFramebufferEXT,
         "BindFramebufferEXT");
    wrap(G_CheckFramebufferStatusEXT, &o_CheckFramebufferStatusEXT,
         (void *)w_CheckFramebufferStatusEXT, "CheckFramebufferStatusEXT");
    wrap(G_FramebufferTexture2DEXT, &o_FramebufferTexture2DEXT,
         (void *)w_FramebufferTexture2DEXT, "FramebufferTexture2DEXT");
    logmsg("diag: GLEW wrappers installed\n");
    return r;
}

/* ---- screenshots ---- */
static void write_bmp(const char *path, int w, int h, const uint8_t *bgr)
{
    FILE *f = fopen(path, "wb");
    int stride = (w * 3 + 3) & ~3, y;
    uint32_t size = 54 + stride * h;
    uint8_t hd[54] = { 'B', 'M' };
    if (!f)
        return;
    memcpy(hd + 2, &size, 4);
    hd[10] = 54;
    hd[14] = 40;
    memcpy(hd + 18, &w, 4);
    memcpy(hd + 22, &h, 4);
    hd[26] = 1;
    hd[28] = 24;
    fwrite(hd, 1, 54, f);
    for (y = 0; y < h; y++) {      /* GL rows are bottom-up, like BMP */
        static const uint8_t pad[3];
        fwrite(bgr + (size_t)y * w * 3, 1, w * 3, f);
        fwrite(pad, 1, stride - w * 3, f);
    }
    fclose(f);
}

static int g_shots;

static void shot(long frame)
{
    int fb = 0, pbo = 0, align = 4, w, h;
    RECT rc;
    HWND wnd = p_wglGetCurrentDC ? WindowFromDC(p_wglGetCurrentDC()) : NULL;
    uint8_t *buf;
    char path[MAX_PATH];
    if (!p_glReadPixels || !p_glGetIntegerv || !o_BindFramebufferEXT || !wnd)
        return;
    GetClientRect(wnd, &rc);
    w = rc.right - rc.left;
    h = rc.bottom - rc.top;
    if (w <= 0 || h <= 0)
        return;
    p_glGetIntegerv(0x88ED /* PIXEL_PACK_BUFFER_BINDING */, &pbo);
    if (pbo)
        return;
    buf = malloc((size_t)w * h * 3);
    if (!buf)
        return;
    p_glGetIntegerv(0x8CA6 /* FRAMEBUFFER_BINDING */, &fb);
    p_glGetIntegerv(0x0D05 /* PACK_ALIGNMENT */, &align);
    p_glPixelStorei(0x0D05, 1);
    CALL(o_BindFramebufferEXT, 0x8D40, 0);
    p_glReadBuffer(0x0405 /* BACK */);
    p_glReadPixels(0, 0, w, h, 0x80E0 /* BGR */, 0x1401, buf);
    CALL(o_BindFramebufferEXT, 0x8D40, (uint32_t)fb);
    p_glPixelStorei(0x0D05, align);
    snprintf(path, sizeof(path), "%sshots\\f%06ld_%dx%d.bmp", g_dir, frame, w, h);
    write_bmp(path, w, h, buf);
    free(buf);
    logmsg("shot frame %ld -> %s (bound fb was %d)\n", frame, path, fb);
    g_shots++;
}

/* ---- frame end ---- */
static long     g_frame;
static uint32_t g_last_sig;
static long     g_sig_logs;

static uint32_t frame_sig(void)
{
    uint32_t h = 2166136261u;
    int l, s, i;
    for (l = 0; l < N_LAYERS; l++)
        for (s = 0; s < 4; s++)
            h = (h ^ (f_layer[l][s] ? 1 + (f_layer[l][s] > 1) : 0)) * 16777619u;
    for (i = 0; i < N_HOOKS; i++)
        h = (h ^ (f_calls[i] != 0)) * 16777619u;
    for (i = 0; i < f_nrt; i++)
        h = (h ^ f_rt[i][0]) * 16777619u;
    return h;
}

static void frame_summary(const char *why)
{
    char b[512];
    int n = 0, l, s, i;
    n += snprintf(b + n, sizeof(b) - n, "layers:");
    for (l = 0; l < N_LAYERS; l++)
        for (s = 0; s < 4; s++)
            if (f_layer[l][s])
                n += snprintf(b + n, sizeof(b) - n, " L%d/s%d x%ld", l, s == 3 ? -1 : s, f_layer[l][s]);
    logmsg("frame %ld (%s) mode=%u %s\n", g_frame, why,
           *(volatile uint32_t *)A_DISPLAY_MODE, b);
    logmsg("   calls: screencam %ld updatecam %ld setrt %ld ovrsetup %ld | fb binds %ld\n",
           f_calls[H_SETSCREENCAM], f_calls[H_UPDATECAM], f_calls[H_SETRENDERTARGET],
           f_calls[H_SHADEROVRSETUP], f_binds);
    n = 0;
    for (i = 0; i < f_nrt; i++)
        n += snprintf(b + n, sizeof(b) - n, " (%x,%x,%x)", f_rt[i][0], f_rt[i][1], f_rt[i][2]);
    if (f_nrt)
        logmsg("   SetRenderTarget args:%s\n", b);
    n = 0;
    for (i = 0; i < f_nfb; i++)
        n += snprintf(b + n, sizeof(b) - n, " %u", f_fb[i]);
    if (f_nfb)
        logmsg("   framebuffers bound:%s\n", b);
}

static void dump_layers(void)
{
    int l;
    for (l = 0; l < N_LAYERS; l++) {
        uint8_t *p = (uint8_t *)(A_RENDER_LAYERS + l * RENDER_LAYER_SIZE);
        logmsg("layer %d: flags %08x rt %08x scale %.2f %.2f\n", l,
               *(uint32_t *)(p + 0x145c), *(uint32_t *)(p + 0x1460),
               *(float *)(p + 0x1464), *(float *)(p + 0x1468));
    }
}

static CB uint32_t frame_end(uint32_t wnd)
{
    uint32_t sig, e;
    int k;
    g_frame++;
    if (g_frame == 1) {
        logmsg("first frame; GL: compiles %ld (failed %ld), links %ld (failed %ld)\n",
               g_compiles, g_compile_fail, g_links, g_link_fail);
        dump_layers();
    }
    sig = frame_sig();
    if (g_frame <= 3)
        frame_summary("start");
    else if (sig != g_last_sig && g_sig_logs++ < 300)
        frame_summary("changed");
    else if (g_frame % 1800 == 0)
        frame_summary("periodic");
    g_last_sig = sig;

    if (p_glGetError)
        for (k = 0; k < 8 && (e = p_glGetError()) != 0; k++) {
            static long errs;
            if (errs++ < 100)
                logmsg("glGetError %04x at frame %ld\n", e, g_frame);
        }
    if (g_cfg.shot_every > 0 && g_frame % g_cfg.shot_every == 0 && g_shots < g_cfg.shot_max)
        shot(g_frame);
    if (g_frame % 1800 == 0)
        logmsg("GL totals: compiles %ld (failed %ld), links %ld (failed %ld), incomplete fbo %ld\n",
               g_compiles, g_compile_fail, g_links, g_link_fail, g_fbo_incomplete);

    memset(f_calls, 0, sizeof(f_calls));
    memset(f_layer, 0, sizeof(f_layer));
    f_nrt = f_nfb = 0;
    f_binds = 0;
    return CALL((void *)A_WNDSWAP, wnd);
}

/* ---- console mirror ---- */
void diag_console_thread(void)
{
    char path[MAX_PATH];
    snprintf(path, sizeof(path), "%sjpvr_console.txt", g_dir);
    for (;;) {
        HANDLE con = CreateFileA("CONOUT$", GENERIC_READ | GENERIC_WRITE,
                                 FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                                 OPEN_EXISTING, 0, NULL);
        CONSOLE_SCREEN_BUFFER_INFO ci;
        if (con != INVALID_HANDLE_VALUE && GetConsoleScreenBufferInfo(con, &ci)) {
            FILE *f = fopen(path, "w");
            SHORT y;
            char *line = malloc(ci.dwSize.X + 2);
            for (y = 0; f && line && y <= ci.dwCursorPosition.Y; y++) {
                DWORD got = 0;
                COORD at = { 0, y };
                ReadConsoleOutputCharacterA(con, line, ci.dwSize.X, at, &got);
                while (got && line[got - 1] == ' ')
                    got--;
                line[got] = 0;
                fprintf(f, "%s\n", line);
            }
            free(line);
            if (f)
                fclose(f);
        }
        if (con != INVALID_HANDLE_VALUE)
            CloseHandle(con);
        Sleep(5000);
    }
}

void diag_install(void)
{
    char dir[MAX_PATH];
    snprintf(dir, sizeof(dir), "%sshots", g_dir);
    CreateDirectoryA(dir, NULL);
    install_hooks();
    patch_call(0x837aedb, A_GLEWINIT, (void *)my_glewInit, "glewInit call #1");
    patch_call(0x837b184, A_GLEWINIT, (void *)my_glewInit, "glewInit call #2");
    patch_call(0x83065e6, A_WNDSWAP, (void *)frame_end, "WndSwap (RenderLoop)");
    patch_call(0x8305912, A_WNDSWAP, (void *)frame_end, "WndSwap #2");
    patch_call(0x829816f, A_WNDSWAP, (void *)frame_end, "WndSwap #3");
}

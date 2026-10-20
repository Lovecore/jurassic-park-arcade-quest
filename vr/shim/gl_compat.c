/*
 * gl_compat.c — GL compatibility layer for the NVIDIA-era renderer.
 *
 * The arcade ran NVIDIA's driver, which accepts version-less GLSL that uses
 * 1.30+ features and mixes gl_FragData with user outputs. Mesa (and any
 * GLES/Zink path on Quest) is strict. GL entry points reach the game two
 * ways: GOT imports (shim.c sym_resolve) and GLEW function pointers fetched
 * through glXGetProcAddressARB. Both go through gl_hook(), so one table
 * covers core and ARB names wherever they are fetched.
 *
 * Fixups (always on): mixed built-in/user fragment outputs (see below).
 *
 * Env:
 *   JPVR_FPS_EVERY=s  log frames/fps every s seconds (default 10, 0 = off)
 *   JPVR_GLDIAG=1   log GL strings and every failed compile/link with its
 *                   info log
 */

#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "shim.h"

typedef unsigned int GLuint;
typedef int GLint;
typedef int GLsizei;
typedef unsigned int GLenum;
typedef char GLchar;

typedef const unsigned char *(*glGetString_fn)(GLenum);
typedef void (*glu1_fn)(GLuint);
typedef void (*gliv_fn)(GLuint, GLenum, GLint *);
typedef void (*gllog_fn)(GLuint, GLsizei, GLsizei *, GLchar *);
typedef void *(*gpa_fn)(const unsigned char *);
typedef void (*glbfdl_fn)(GLuint, GLuint, const GLchar *);

static void *(*g_resolve)(const char *);
static glu1_fn real_compile, real_link;
static gpa_fn real_gpa;

static int gldiag(void)
{
    static int v = -1;
    if (v < 0)
        v = getenv("JPVR_GLDIAG") && atoi(getenv("JPVR_GLDIAG"));
    return v;
}

static void log_status(const char *what, GLuint obj, GLenum pname,
                       const char *ivname, const char *logname)
{
    gliv_fn iv = (gliv_fn)g_resolve(ivname);
    gllog_fn lg = (gllog_fn)g_resolve(logname);
    GLint ok = -1;
    char buf[1024] = "";
    if (iv)
        iv(obj, pname, &ok);
    if (ok == 1)
        return;
    if (lg)
        lg(obj, sizeof(buf), NULL, buf);
    logmsg("[gl_compat] %s(%u) failed: %.900s\n", what, obj, buf);
}

static void hook_compile(GLuint sh)
{
    static int once;
    if (!once++) {
        glGetString_fn gs = (glGetString_fn)g_resolve("glGetString");
        if (gs)
            logmsg("[gl_compat] GL %s | %s | %s | GLSL %s\n", gs(0x1F00),
                   gs(0x1F01), gs(0x1F02), gs(0x8B8C));
    }
    real_compile(sh);
    if (gldiag())
        log_status("compile", sh, 0x8B81, "glGetShaderiv", "glGetShaderInfoLog");
}

static void hook_link(GLuint pr)
{
    /* Locations for rewritten built-in outputs; harmless if unused. */
    glbfdl_fn bind = (glbfdl_fn)g_resolve("glBindFragDataLocation");
    if (bind) {
        bind(pr, 0, "jpvr_FragData0");
        bind(pr, 1, "jpvr_FragData1");
    }
    real_link(pr);
    if (gldiag())
        log_status("link", pr, 0x8B82, "glGetProgramiv", "glGetProgramInfoLog");
}

typedef void (*glcti_fn)(GLenum, GLint, GLenum, GLsizei, GLsizei, GLint,
                         GLsizei, const void *);
typedef GLenum (*glcfs_fn)(GLenum);
typedef void (*gliv2_fn)(GLenum, GLint, GLenum, GLint *);
typedef GLenum (*glge_fn)(void);
static glcti_fn real_cti;
static glcfs_fn real_cfs;

static GLenum gl_error(void)
{
    glge_fn ge = (glge_fn)g_resolve("glGetError");
    return ge ? ge() : 0;
}

static void hook_cti(GLenum target, GLint level, GLenum ifmt, GLsizei w,
                     GLsizei h, GLint border, GLsizei size, const void *data)
{
    real_cti(target, level, ifmt, w, h, border, size, data);
    GLenum e = gl_error();
    if (gldiag()) {
        static GLenum seen[32];
        static int nseen;
        int known = 0;
        for (int i = 0; i < nseen; i++)
            known |= seen[i] == ifmt;
        if (!known && nseen < 32) {
            seen[nseen++] = ifmt;
            logmsg("[gl_compat] compressed format 0x%x first used (%dx%d)\n", ifmt, w, h);
        }
    }
    /* imgLoad uploads to GL_PROXY_TEXTURE_2D first and calls SysLockup(1169)
     * when the proxy reports width/height/format 0: log what was refused. */
    if (target == 0x8064) {
        gliv2_fn tlp = (gliv2_fn)g_resolve("glGetTexLevelParameteriv");
        GLint pw = -1, ph = -1, pf = -1;
        if (tlp) {
            tlp(0x8064, level, 0x1000, &pw);    /* WIDTH */
            tlp(0x8064, level, 0x1001, &ph);    /* HEIGHT */
            tlp(0x8064, level, 0x1003, &pf);    /* INTERNAL_FORMAT */
        }
        static int n;
        if ((pw == 0 || ph == 0 || pf == 0) && n++ < 20)
            logmsg("[gl_compat] proxy refused: fmt=0x%x lvl=%d %dx%d size=%d err=0x%x "
                   "-> proxy %dx%d fmt=0x%x\n", ifmt, level, w, h, size, e, pw, ph, pf);
    } else if (e && gldiag()) {
        static int n;
        if (n++ < 40)
            logmsg("[gl_compat] glCompressedTexImage2D(target=0x%x lvl=%d fmt=0x%x "
                   "%dx%d size=%d) -> err 0x%x\n", target, level, ifmt, w, h, size, e);
    }
}

static GLenum hook_cfs(GLenum target)
{
    GLenum st = real_cfs(target);
    static int n;
    if (gldiag() && st != 0x8CD5 && n++ < 40)   /* GL_FRAMEBUFFER_COMPLETE */
        logmsg("[gl_compat] glCheckFramebufferStatus(0x%x) = 0x%x\n", target, st);
    return st;
}

/* ------------------------------------------------------------------ */
/* GLSL source fixups                                                  */
/*                                                                      */
/* NVIDIA lets a fragment shader write gl_FragData/gl_FragColor while   */
/* also declaring user `out` variables (frag_bloom.gls declares         */
/* out_Bloom in shaders that never call ComputeBloom). Strict GLSL      */
/* forbids the mix. When both appear, built-ins become user outputs     */
/* jpvr_FragData0/1, bound to locations 0/1 at link time.               */
/* ------------------------------------------------------------------ */

typedef void (*glss_fn)(GLuint, GLsizei, const GLchar *const *, const GLint *);
static glss_fn real_shader_source;

/* Copy of src with // and block comments blanked (same length, so offsets
 * still line up); detection must ignore commented-out gl_FragColor lines. */
static char *strip_comments(const char *src)
{
    size_t n = strlen(src);
    char *o = malloc(n + 1);
    int line = 0, block = 0;
    for (size_t i = 0; i < n; i++) {
        char c = src[i];
        if (block) {
            if (c == '*' && src[i + 1] == '/') {
                o[i] = o[i + 1] = ' ';
                i++;
                block = 0;
                continue;
            }
            o[i] = c == '\n' ? '\n' : ' ';
            continue;
        }
        if (line) {
            if (c == '\n')
                line = 0;
            o[i] = c == '\n' ? '\n' : ' ';
            continue;
        }
        if (c == '/' && src[i + 1] == '/')
            line = 1;
        else if (c == '/' && src[i + 1] == '*')
            block = 1;
        o[i] = (line || block) ? ' ' : c;
    }
    o[n] = 0;
    return o;
}

static int declares_user_out(const char *src)
{
    for (const char *p = strstr(src, "out "); p; p = strstr(p + 4, "out ")) {
        int line_start = (p == src || p[-1] == '\n');
        if (line_start && strncmp(p + 4, "vec4", 4) == 0)
            return 1;
    }
    return 0;
}

/* Replace every `from` with `to`; returns a malloc'd string. */
static char *replace_all(const char *src, const char *from, const char *to)
{
    size_t fl = strlen(from), tl = strlen(to), n = 0;
    for (const char *p = strstr(src, from); p; p = strstr(p + fl, from))
        n++;
    char *out = malloc(strlen(src) + n * (tl > fl ? tl - fl : 0) + 1);
    char *o = out;
    for (const char *p = src;;) {
        const char *q = strstr(p, from);
        if (!q) {
            strcpy(o, p);
            break;
        }
        memcpy(o, p, (size_t)(q - p));
        o += q - p;
        memcpy(o, to, tl);
        o += tl;
        p = q + fl;
    }
    return out;
}

static void hook_shader_source(GLuint sh, GLsizei count,
                               const GLchar *const *str, const GLint *len)
{
    size_t total = 0;
    for (GLsizei i = 0; i < count; i++)
        total += (len && len[i] >= 0) ? (size_t)len[i] : strlen(str[i]);
    char *src = malloc(total + 1), *o = src;
    for (GLsizei i = 0; i < count; i++) {
        size_t l = (len && len[i] >= 0) ? (size_t)len[i] : strlen(str[i]);
        memcpy(o, str[i], l);
        o += l;
    }
    *o = 0;

    gliv_fn iv = (gliv_fn)g_resolve("glGetShaderiv");
    GLint type = 0;
    if (iv)
        iv(sh, 0x8B4F, &type);                 /* GL_SHADER_TYPE */
    int frag = type == 0x8B30;                 /* GL_FRAGMENT_SHADER */
    char *code = frag ? strip_comments(src) : NULL;
    int use0 = code && (strstr(code, "gl_FragData[0]") || strstr(code, "gl_FragColor"));
    int use1 = code && strstr(code, "gl_FragData[1]");
    if ((use0 || use1) && declares_user_out(code)) {
        /* Rewrite the commented copy so dead gl_Frag* text cannot leak back. */
        char *a = replace_all(code, "gl_FragData[0]", "jpvr_FragData0");
        char *b = replace_all(a, "gl_FragData[1]", "jpvr_FragData1");
        char *c = replace_all(b, "gl_FragColor", "jpvr_FragData0");
        free(a);
        free(b);
        char decl[96];
        snprintf(decl, sizeof(decl), "%s%s", use0 ? "out vec4 jpvr_FragData0;\n" : "",
                 use1 ? "out vec4 jpvr_FragData1;\n" : "");
        /* Declarations go after any leading #version/#extension lines. */
        size_t at = 0;
        for (const char *ln = c; *ln;) {
            const char *nl = strchr(ln, '\n');
            const char *t = ln;
            while (*t == ' ' || *t == '\t')
                t++;
            if (strncmp(t, "#version", 8) && strncmp(t, "#extension", 10) &&
                *t != '\n' && *t != '\r' && *t)
                break;
            if (!strncmp(t, "#version", 8) || !strncmp(t, "#extension", 10))
                at = nl ? (size_t)(nl - c) + 1 : strlen(c);
            if (!nl)
                break;
            ln = nl + 1;
        }
        char *d = malloc(strlen(decl) + strlen(c) + 1);
        memcpy(d, c, at);
        strcpy(d + at, decl);
        strcat(d, c + at);
        free(c);
        free(src);
        src = d;
        static int n;
        if (n++ < 20)
            logmsg("[gl_compat] shader %u: gl_FragData -> jpvr_FragData (mixed outputs)\n", sh);
    }
    free(code);
    const GLchar *one = src;
    real_shader_source(sh, 1, &one, NULL);
    free(src);
}

/* ------------------------------------------------------------------ */
/* Frame pacing                                                        */
/*                                                                      */
/* SDL_GL_SwapBuffers is the game's present call: count frames and log  */
/* fps every JPVR_FPS_EVERY seconds (default 10). This is the Q0 perf   */
/* gate measurement, and later the hook point for VR frame submission.  */
/* ------------------------------------------------------------------ */

#include <time.h>

typedef void (*swap_fn)(void);
static swap_fn real_swap;

static double now_s(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec * 1e-9;
}

static void hook_swap(void)
{
    static double t0, tlast;
    static long frames, window;
    static int every = -1;
    if (every < 0) {
        const char *e = getenv("JPVR_FPS_EVERY");
        every = e ? atoi(e) : 10;
        t0 = tlast = now_s();
    }
    real_swap();
    frames++;
    window++;
    double t = now_s();
    if (every > 0 && t - tlast >= every) {
        logmsg("[gl_compat] frames=%ld fps=%.2f (avg %.2f over %.0fs)\n",
               frames, window / (t - tlast), frames / (t - t0), t - t0);
        tlast = t;
        window = 0;
    }
}

/* Swap interval is the display's business (Xvfb has no vsync, and on Quest
 * the OpenXR runtime paces frames). Under Xvfb the direct-rendering context
 * has no server tag, so the GLX request fails with GLXBadContextTag and the
 * game's X error handler asserts. */
static int hook_swap_interval(int interval)
{
    static int once;
    if (!once++)
        logmsg("[gl_compat] glXSwapInterval(%d) ignored\n", interval);
    return 0;
}

static void *noop_real;

static void *hook_gpa(const unsigned char *name);

static const struct {
    const char *name;
    void *wrap;
    void **real;
} HOOKS[] = {
    { "glShaderSource",       (void *)hook_shader_source, (void **)&real_shader_source },
    { "glShaderSourceARB",    (void *)hook_shader_source, (void **)&real_shader_source },
    { "glCompileShader",      (void *)hook_compile, (void **)&real_compile },
    { "glCompileShaderARB",   (void *)hook_compile, (void **)&real_compile },
    { "glLinkProgram",        (void *)hook_link,    (void **)&real_link },
    { "glLinkProgramARB",     (void *)hook_link,    (void **)&real_link },
    { "glCompressedTexImage2D",    (void *)hook_cti, (void **)&real_cti },
    { "glCompressedTexImage2DARB", (void *)hook_cti, (void **)&real_cti },
    { "glCheckFramebufferStatus",    (void *)hook_cfs, (void **)&real_cfs },
    { "glCheckFramebufferStatusEXT", (void *)hook_cfs, (void **)&real_cfs },
    { "SDL_GL_SwapBuffers",   (void *)hook_swap,    (void **)&real_swap },
    { "glXSwapIntervalSGI",   (void *)hook_swap_interval, &noop_real },
    { "glXSwapIntervalMESA",  (void *)hook_swap_interval, &noop_real },
    { "glXSwapIntervalEXT",   (void *)hook_swap_interval, &noop_real },
    { "glXGetProcAddressARB", (void *)hook_gpa,     (void **)&real_gpa },
    { "glXGetProcAddress",    (void *)hook_gpa,     (void **)&real_gpa },
};

HIDDEN void *gl_hook(const char *name, void *(*resolve_real)(const char *))
{
    g_resolve = resolve_real;
    for (size_t i = 0; i < sizeof(HOOKS) / sizeof(HOOKS[0]); i++) {
        if (strcmp(name, HOOKS[i].name) != 0)
            continue;
        if (!*HOOKS[i].real)
            *HOOKS[i].real = resolve_real(name);
        return *HOOKS[i].real ? HOOKS[i].wrap : NULL;
    }
    return NULL;
}

/* GLEW fetches most entry points here; hand back our wrappers for hooked
 * names so both paths see the same layer. */
static void *hook_gpa(const unsigned char *name)
{
    void *w = gl_hook((const char *)name, g_resolve);
    if (w)
        return w;
    return real_gpa(name);
}

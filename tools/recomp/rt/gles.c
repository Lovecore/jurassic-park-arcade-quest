/*
 * gles.c — desktop OpenGL (what the game calls) on an OpenGL ES 3.2
 * context: the Quest's only API, also testable on the PC through an ES
 * profile context (JPRT_GLES=1).
 *
 * The game's GL is small (89 entry points through gameplay) and mostly
 * GL 3 core that ES 3.2 has verbatim. This layer handles the rest:
 *   - shaders: glsl_es.c translation at glShaderSource; fragment outputs
 *     get the program's glBindFragDataLocation locations at link time
 *   - fixed-function leftovers (light model, shade model, matrix mode,
 *     current color, desktop-only enables/hints): dropped
 *   - double-precision and renamed calls (glClearDepth, glDepthRange,
 *     glDrawBuffer, glGetActiveUniformName, glAreTexturesResident,
 *     glGetTexImage)
 *   - texture formats ES lacks (BGRA, unsized/legacy internal formats,
 *     GL_CLAMP, S3TC when the device has no DXT support)
 *   - glMapBuffer read-back through glMapBufferRange
 *
 * JPRT_GLTRACE=1 also takes an argument census (formats, modes, caps) in
 * either mode, which is how the tables here were chosen.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "rt.h"
#include "kernel.h"
#include "gl_host.h"
#include "glsl_es.h"
#include "texdecode.h"
#include "video.h"

#define ARG(i) rd32(cpu->esp + 4 + 4 * (i))
#define ARGD(i) rdf64(cpu->esp + 4 + 4 * (i))     /* double spanning slots i, i+1 */
#define RET(v) do { cpu->eax = (uint32_t)(v); cpu->esp += 4; return 1; } while (0)

uint32_t guest_strdup(const char *s);
int g_gles;                                     /* the host context is OpenGL ES */

typedef unsigned int GLenum;
#define P(idx, name, type) ((type)gl_proc(idx, name))

/* ---- census (JPRT_GLTRACE) ---- */
static int census_on(void)
{
    static int on = -1;
    if (on < 0) on = getenv("JPRT_GLTRACE") != NULL;
    return on;
}

static void census(const char *what, uint32_t a, uint32_t b, uint32_t c)
{
    static struct { const char *w; uint32_t a, b, c; } seen[512];
    static int n;
    int i;
    if (!census_on()) return;
    for (i = 0; i < n; i++)
        if (seen[i].w == what && seen[i].a == a && seen[i].b == b && seen[i].c == c) return;
    if (n < 512) { seen[n].w = what; seen[n].a = a; seen[n].b = b; seen[n].c = c; n++; }
    klog("glcensus: %s %#x %#x %#x\n", what, a, b, c);
}

/* calls observed in either mode (desktop: census only) */
void gl_census(rt_cpu *cpu, const char *n)
{
    if (!census_on()) return;
    if (!strcmp(n, "glTexImage2D")) census("TexImage2D(internal,format,type)", ARG(2), ARG(6), ARG(7));
    else if (!strcmp(n, "glCompressedTexImage2D")) census("CompressedTexImage2D(internal)", ARG(2), 0, 0);
    else if (!strcmp(n, "glTexSubImage2D")) census("TexSubImage2D(format,type)", ARG(6), ARG(7), 0);
    else if (!strcmp(n, "glDrawArrays")) census("DrawArrays(mode)", ARG(0), 0, 0);
    else if (!strcmp(n, "glDrawRangeElements")) census("DrawRangeElements(mode,type)", ARG(0), ARG(4), 0);
    else if (!strcmp(n, "glDrawElements")) census("DrawElements(mode,type)", ARG(0), ARG(2), 0);
    else if (!strcmp(n, "glEnable") || !strcmp(n, "glDisable")) census("Enable/Disable(cap)", ARG(0), 0, 0);
    else if (!strcmp(n, "glHint")) census("Hint(target,mode)", ARG(0), ARG(1), 0);
    else if (!strcmp(n, "glTexParameteri")) census("TexParameteri(target,pname,param)", ARG(0), ARG(1), ARG(2));
    else if (!strcmp(n, "glTexParameterfv")) census("TexParameterfv(target,pname)", ARG(0), ARG(1), 0);
    else if (!strcmp(n, "glGetIntegerv")) census("GetIntegerv(pname)", ARG(0), 0, 0);
    else if (!strcmp(n, "glGetString")) census("GetString(name)", ARG(0), 0, 0);
    else if (!strcmp(n, "glBindFramebuffer")) census("BindFramebuffer(target,fb)", ARG(0), ARG(1) ? 1 : 0, 0);
    else if (!strcmp(n, "glDrawBuffer")) census("DrawBuffer(mode)", ARG(0), 0, 0);
    else if (!strcmp(n, "glReadBuffer")) census("ReadBuffer(mode)", ARG(0), 0, 0);
    else if (!strcmp(n, "glMapBuffer")) census("MapBuffer(target,access)", ARG(0), ARG(1), 0);
    else if (!strcmp(n, "glGetTexImage")) census("GetTexImage(target,level,format,type)", ARG(0), ARG(2), ARG(3));
    else if (!strcmp(n, "glGetTexLevelParameteriv")) census("GetTexLevelParameteriv(pname)", ARG(2), 0, 0);
    else if (!strcmp(n, "glPixelStorei")) census("PixelStorei(pname,param)", ARG(0), ARG(1), 0);
    else if (!strcmp(n, "glBlendFunc")) census("BlendFunc", ARG(0), ARG(1), 0);
    else if (!strcmp(n, "glFramebufferTexture2D")) census("FramebufferTexture2D(attach,textarget)", ARG(1), ARG(2), 0);
    else if (!strcmp(n, "glVertexAttribPointer")) census("VertexAttribPointer(size,type,norm)", ARG(1), ARG(2), ARG(3));
}

/* ---- ES translation (only when g_gles) ---- */
typedef void (GLAPI_CALL *pf_v1)(uint32_t);
typedef void (GLAPI_CALL *pf_v2)(uint32_t, uint32_t);
typedef void (GLAPI_CALL *pf_v3)(uint32_t, uint32_t, uint32_t);
typedef void (GLAPI_CALL *pf_f1)(float);
typedef void (GLAPI_CALL *pf_f2)(float, float);
typedef void (GLAPI_CALL *pf_geti)(uint32_t, int32_t *);
typedef void (GLAPI_CALL *pf_getshi)(uint32_t, uint32_t, int32_t *);
typedef void (GLAPI_CALL *pf_shsrc)(uint32_t, int32_t, const char *const *, const int32_t *);
typedef void (GLAPI_CALL *pf_info)(uint32_t, int32_t, int32_t *, char *);
typedef void (GLAPI_CALL *pf_teximg)(uint32_t, int32_t, int32_t, int32_t, int32_t, int32_t, uint32_t, uint32_t, const void *);
typedef void (GLAPI_CALL *pf_ctex)(uint32_t, int32_t, uint32_t, int32_t, int32_t, int32_t, int32_t, const void *);
typedef void (GLAPI_CALL *pf_texlvl)(uint32_t, int32_t, uint32_t, int32_t *);
typedef void (GLAPI_CALL *pf_drawel)(uint32_t, int32_t, uint32_t, const void *, int32_t);
typedef void (GLAPI_CALL *pf_drawarr)(uint32_t, int32_t, int32_t);
typedef void (GLAPI_CALL *pf_bufdata)(uint32_t, intptr_t, const void *, uint32_t);
typedef void (GLAPI_CALL *pf_gen)(int32_t, uint32_t *);
typedef void (GLAPI_CALL *pf_fbtex)(uint32_t, uint32_t, uint32_t, uint32_t, int32_t);
typedef void (GLAPI_CALL *pf_readpx)(int32_t, int32_t, int32_t, int32_t, uint32_t, uint32_t, void *);
typedef void (GLAPI_CALL *pf_actuni)(uint32_t, uint32_t, int32_t, int32_t *, int32_t *, uint32_t *, char *);
typedef void (GLAPI_CALL *pf_drawbufs)(int32_t, const uint32_t *);
typedef const char *(GLAPI_CALL *pf_str)(uint32_t);
typedef void *(GLAPI_CALL *pf_maprange)(uint32_t, intptr_t, intptr_t, uint32_t);
typedef uint8_t (GLAPI_CALL *pf_unmap)(uint32_t);

/* indices for gl_proc's cache: past gen_gl's commands and hostlibs' 4000s */
enum {
    X_GetIntegerv = 4020, X_GetShaderiv, X_ShaderSource, X_CompileShader, X_GetShaderInfoLog,
    X_LinkProgram, X_GetProgramiv, X_GetProgramInfoLog, X_TexImage2D, X_CompressedTexImage2D,
    X_TexParameteri, X_GenerateMipmap, X_GetTexLevelParameteriv, X_DrawElementsBaseVertex,
    X_BindBuffer, X_GenBuffers, X_BufferData, X_GenFramebuffers, X_BindFramebuffer,
    X_FramebufferTexture2D, X_ReadPixels, X_GetActiveUniform, X_DrawBuffers, X_ClearDepthf,
    X_DepthRangef, X_GetString, X_MapBufferRange, X_UnmapBuffer, X_Enable, X_Disable, X_Hint,
    X_ReadBuffer, X_DrawArrays, X_TexParameteriv, X_PixelStorei, X_CheckFramebufferStatus, X_IsTexture, X_BindTexture, X_GetError, X_DrawElements
};

#define MAXOBJ 16384
static char *g_sh_src[MAXOBJ];                 /* original desktop source per shader */
static uint8_t g_sh_frag[MAXOBJ];
static struct gl_prog { uint32_t sh[8]; int nsh; glsl_es_binds b; } *g_prog[MAXOBJ];
static int g_has_s3tc, g_has_rgtc, g_exts_checked;

static void check_exts(void)
{
    const char *e;
    if (g_exts_checked) return;
    g_exts_checked = 1;
    e = P(X_GetString, "glGetString", pf_str)(0x1F03);
    /* decoded by default (as on the Quest, which has no S3TC): the game reads textures back */
    g_has_s3tc = e && strstr(e, "GL_EXT_texture_compression_s3tc") != NULL && getenv("JPRT_NATIVE_DXT");
    g_has_rgtc = e && strstr(e, "GL_EXT_texture_compression_rgtc") != NULL && getenv("JPRT_NATIVE_DXT");
    klog("gles: S3TC %s, RGTC %s\n", g_has_s3tc ? "native" : "decoded", g_has_rgtc ? "native" : "decoded");
}

static struct gl_prog *prog_get(uint32_t p)
{
    if (p >= MAXOBJ) return NULL;
    if (!g_prog[p]) g_prog[p] = calloc(1, sizeof(struct gl_prog));
    return g_prog[p];
}

static void compile_es(uint32_t sh, const glsl_es_binds *b)
{
    char *es = glsl_es_translate(g_sh_src[sh], g_sh_frag[sh], b);
    const char *s = es;
    P(X_ShaderSource, "glShaderSource", pf_shsrc)(sh, 1, &s, NULL);
    free(es);
}

static void check_shader(uint32_t sh)
{
    int32_t ok = 0;
    P(X_GetShaderiv, "glGetShaderiv", pf_getshi)(sh, 0x8B81 /* COMPILE_STATUS */, &ok);
    if (!ok) {
        char log[2048] = "";
        static int dumped;
        P(X_GetShaderInfoLog, "glGetShaderInfoLog", pf_info)(sh, sizeof(log), NULL, log);
        klog("gles: shader %u (%s) failed to compile:\n%s\n", sh, g_sh_frag[sh] ? "frag" : "vert", log);
        if (dumped++ < 8 && sh < MAXOBJ && g_sh_src[sh]) {
            char *es = glsl_es_translate(g_sh_src[sh], g_sh_frag[sh], NULL), path[64];
            FILE *f;
            snprintf(path, sizeof(path), "gles_fail_%u.%s", sh, g_sh_frag[sh] ? "frag" : "vert");
            if ((f = fopen(path, "wb"))) { fputs(es, f); fclose(f); }
            free(es);
        }
    }
}

/* desktop texture upload -> ES internal format / format / type */
static void fix_tex_formats(uint32_t *internal, uint32_t *format, uint32_t *type)
{
    switch (*internal) {                         /* compressed internal format with plain data */
    case TEX_DXT1_RGB: *internal = 0x1907; break;                 /* RGB */
    case TEX_DXT1_RGBA: case TEX_DXT3: case TEX_DXT5: *internal = 0x1908; break;
    case TEX_LATC1: *internal = 0x1909; break;                    /* LUMINANCE */
    case TEX_LATC2: *internal = 0x190A; break;                    /* LUMINANCE_ALPHA */
    case 1: *internal = 0x1909; break;
    case 2: *internal = 0x190A; break;
    case 3: *internal = 0x1907; break;
    case 4: *internal = 0x1908; break;
    }
    /* unsized internal formats must equal the format in ES */
    if ((*internal == 0x1907 || *internal == 0x1908 || *internal == 0x1909 || *internal == 0x190A) &&
        *internal != *format)
        *internal = *format;
    if (*format == 0x80E1 && *type == 0x1401) {   /* BGRA: EXT_texture_format_BGRA8888 */
        *internal = 0x80E1;
    }
    if (*format == 0x1902) {                      /* depth: ES wants UNSIGNED_INT/SHORT/FLOAT */
        if (*type == 0x1404) *type = 0x1405;
        if (*internal == 0x1902) *internal = 0x81A6;
    }
}

/* GL_GENERATE_MIPMAP (texture parameter, gone in ES): remembered per
 * texture, honoured with glGenerateMipmap after a level-0 upload */
static uint8_t g_automip[MAXOBJ];
static uint8_t g_lum[MAXOBJ];          /* decoded LATC stored as R8 (1) / RG8 (2) + swizzle */

static uint32_t bound_tex(uint32_t target)
{
    int32_t t = 0;
    P(X_GetIntegerv, "glGetIntegerv", pf_geti)(target == 0x8513 ? 0x8514 : 0x8069, &t);
    return (uint32_t)t;
}

static void after_upload(uint32_t target, int32_t level)
{
    uint32_t t;
    if (level != 0 || target != 0x0DE1) return;
    t = bound_tex(target);
    if (t < MAXOBJ && g_automip[t]) P(X_GenerateMipmap, "glGenerateMipmap", pf_v1)(target);
}

/* GL_PROXY_TEXTURE_2D (capability probes, gone in ES): answer "supported"
 * with the probed size */
static struct { int32_t w, h; uint32_t internal; } g_proxy;

static void upload_decoded(uint32_t target, int32_t level, uint32_t fmt, int32_t w, int32_t h,
                           const uint8_t *data, uint32_t size)
{
    int bpp = tex_decoded_bpp(fmt);
    uint8_t *px = malloc((size_t)(w > 0 ? w : 1) * (h > 0 ? h : 1) * bpp);
    /* luminance formats as R8 / RG8 + swizzle: same sampling, but
     * colour-renderable, so glGetTexImage can read them back */
    uint32_t f = bpp == 4 ? 0x1908 : bpp == 2 ? 0x8227 /* RG */ : 0x1903 /* RED */;
    uint32_t internal = bpp == 4 ? 0x8058 /* RGBA8 */ : bpp == 2 ? 0x822B /* RG8 */ : 0x8229 /* R8 */;
    int32_t align = 4;
    if (!px) return;
    if (data && tex_decode(fmt, data, size, w, h, px) < 0) memset(px, 0, (size_t)w * h * bpp);
    if (!data) memset(px, 0, (size_t)w * h * bpp);
    P(X_GetIntegerv, "glGetIntegerv", pf_geti)(0x0CF5 /* UNPACK_ALIGNMENT */, &align);
    P(X_PixelStorei, "glPixelStorei", pf_v2)(0x0CF5, 1);
    P(X_TexImage2D, "glTexImage2D", pf_teximg)(target, level, (int32_t)internal, w, h, 0, f, 0x1401, px);
    P(X_PixelStorei, "glPixelStorei", pf_v2)(0x0CF5, (uint32_t)align);
    if (bpp < 4 && level == 0) {
        uint32_t t = bound_tex(target);
        P(X_TexParameteri, "glTexParameteri", pf_v3)(target, 0x8E42 /* SWIZZLE_R */, 0x1903);
        P(X_TexParameteri, "glTexParameteri", pf_v3)(target, 0x8E43 /* SWIZZLE_G */, 0x1903);
        P(X_TexParameteri, "glTexParameteri", pf_v3)(target, 0x8E44 /* SWIZZLE_B */, 0x1903);
        P(X_TexParameteri, "glTexParameteri", pf_v3)(target, 0x8E45 /* SWIZZLE_A */, bpp == 2 ? 0x1904 /* GREEN */ : 1 /* ONE */);
        if (t < MAXOBJ) g_lum[t] = (uint8_t)bpp;
    }
    free(px);
}

/* GL_QUADS via a growing quad index buffer + glDrawElementsBaseVertex */
static uint32_t g_quad_ibo, g_quad_cap;
uint32_t gl_bound_element_buffer(void);           /* hostlibs.c */

static void draw_quads(int32_t first, int32_t count)
{
    uint32_t nq = (uint32_t)count / 4, prev = gl_bound_element_buffer();
    if (!nq) return;
    if (!g_quad_ibo) P(X_GenBuffers, "glGenBuffers", pf_gen)(1, &g_quad_ibo);
    P(X_BindBuffer, "glBindBuffer", pf_v2)(0x8893, g_quad_ibo);
    if (nq > g_quad_cap) {
        uint32_t cap = nq < 4096 ? 4096 : nq * 2, q, *ix = malloc(sizeof(uint32_t) * 6 * cap);
        for (q = 0; q < cap; q++) {
            ix[6 * q + 0] = 4 * q; ix[6 * q + 1] = 4 * q + 1; ix[6 * q + 2] = 4 * q + 2;
            ix[6 * q + 3] = 4 * q; ix[6 * q + 4] = 4 * q + 2; ix[6 * q + 5] = 4 * q + 3;
        }
        P(X_BufferData, "glBufferData", pf_bufdata)(0x8893, (intptr_t)(sizeof(uint32_t) * 6 * cap), ix, 0x88E4);
        free(ix);
        g_quad_cap = cap;
    }
    if (glsl_es_version >= 320)
        P(X_DrawElementsBaseVertex, "glDrawElementsBaseVertex", pf_drawel)(4 /* TRIANGLES */, (int32_t)(6 * nq),
                                                                           0x1405, NULL, first);
    else if (first == 0)
        ((void (GLAPI_CALL *)(uint32_t, int32_t, uint32_t, const void *))gl_proc(X_DrawElements, "glDrawElements"))
            (4, (int32_t)(6 * nq), 0x1405, NULL);
    else {                                        /* ES 3.0/3.1: offset indices in a scratch buffer */
        static uint32_t ibo2;
        uint32_t q, *ix = malloc(sizeof(uint32_t) * 6 * nq);
        for (q = 0; q < nq; q++) {
            uint32_t b = (uint32_t)first + 4 * q;
            ix[6 * q] = b; ix[6 * q + 1] = b + 1; ix[6 * q + 2] = b + 2;
            ix[6 * q + 3] = b; ix[6 * q + 4] = b + 2; ix[6 * q + 5] = b + 3;
        }
        if (!ibo2) P(X_GenBuffers, "glGenBuffers", pf_gen)(1, &ibo2);
        P(X_BindBuffer, "glBindBuffer", pf_v2)(0x8893, ibo2);
        P(X_BufferData, "glBufferData", pf_bufdata)(0x8893, (intptr_t)(sizeof(uint32_t) * 6 * nq), ix, 0x88E0);
        ((void (GLAPI_CALL *)(uint32_t, int32_t, uint32_t, const void *))gl_proc(X_DrawElements, "glDrawElements"))
            (4, (int32_t)(6 * nq), 0x1405, NULL);
        free(ix);
    }
    P(X_BindBuffer, "glBindBuffer", pf_v2)(0x8893, prev);
}

/* glGetTexImage: attach the level to a scratch FBO and read it back */
static void get_tex_image(uint32_t target, int32_t level, uint32_t format, uint32_t type, uint32_t pixels)
{
    static uint32_t fbo;
    int32_t w = 0, h = 0, prev_read = 0, tex = 0, i;
    uint8_t *rgba;
    if (target != 0x0DE1 || type != 0x1401 || !pixels) return;
    P(X_GetTexLevelParameteriv, "glGetTexLevelParameteriv", pf_texlvl)(target, level, 0x1000, &w);
    P(X_GetTexLevelParameteriv, "glGetTexLevelParameteriv", pf_texlvl)(target, level, 0x1001, &h);
    P(X_GetIntegerv, "glGetIntegerv", pf_geti)(0x8069 /* TEXTURE_BINDING_2D */, &tex);
    P(X_GetIntegerv, "glGetIntegerv", pf_geti)(0x8CAA /* READ_FRAMEBUFFER_BINDING */, &prev_read);
    if (w <= 0 || h <= 0) return;
    if (!fbo) P(X_GenFramebuffers, "glGenFramebuffers", pf_gen)(1, &fbo);
    P(X_BindFramebuffer, "glBindFramebuffer", pf_v2)(0x8CA8, fbo);
    P(X_FramebufferTexture2D, "glFramebufferTexture2D", pf_fbtex)(0x8CA8, 0x8CE0, target, (uint32_t)tex, level);
    {
        static int logged;
        uint32_t st = ((uint32_t (GLAPI_CALL *)(uint32_t))gl_proc(X_CheckFramebufferStatus, "glCheckFramebufferStatus"))(0x8CA8);
        if (st != 0x8CD5 && logged++ < 4) {
            int32_t fmt = 0;
            P(X_GetTexLevelParameteriv, "glGetTexLevelParameteriv", pf_texlvl)(target, level, 0x1003, &fmt);
            klog("gles: glGetTexImage tex %d level %d (%dx%d, internal %#x): framebuffer status %#x\n",
                 tex, level, w, h, fmt, st);
        }
    }
    rgba = malloc((size_t)w * h * 4);
    P(X_ReadPixels, "glReadPixels", pf_readpx)(0, 0, w, h, 0x1908, 0x1401, rgba);
    if (tex >= 0 && tex < MAXOBJ && g_lum[tex])           /* R8/RG8 luminance: (R,R,R,1|G) */
        for (i = 0; i < w * h; i++) {
            uint8_t *q = rgba + 4 * i;
            q[3] = g_lum[tex] == 2 ? q[1] : 255;
            q[1] = q[2] = q[0];
        }
    if (format == 0x1908) memcpy(RT_P(pixels), rgba, (size_t)w * h * 4);
    else if (format == 0x1907) {
        int32_t pack = 4, stride;
        P(X_GetIntegerv, "glGetIntegerv", pf_geti)(0x0D05 /* PACK_ALIGNMENT */, &pack);
        stride = (w * 3 + pack - 1) / pack * pack;
        for (i = 0; i < w * h; i++) {
            uint8_t *d = (uint8_t *)RT_P(pixels) + (size_t)(i / w) * stride + (size_t)(i % w) * 3;
            memcpy(d, rgba + 4 * i, 3);
        }
    }
    free(rgba);
    P(X_FramebufferTexture2D, "glFramebufferTexture2D", pf_fbtex)(0x8CA8, 0x8CE0, target, 0, 0);
    P(X_BindFramebuffer, "glBindFramebuffer", pf_v2)(0x8CA8, (uint32_t)prev_read);
}

static int es_cap(uint32_t cap)
{
    switch (cap) {
    case 0x0BE2: case 0x0B44: case 0x0B71: case 0x0BD0: case 0x8037: case 0x809E: case 0x80A0:
    case 0x0C11: case 0x0B90: case 0x8D69: case 0x8C89: case 0x8E51: case 0x92E0: case 0x8242:
        return 1;
    }
    return 0;
}

static const char ES_EXTENSIONS[] =
    "GL_ARB_multitexture GL_ARB_texture_compression GL_EXT_texture_compression_s3tc "
    "GL_EXT_texture_compression_dxt1 GL_EXT_texture_compression_latc GL_EXT_texture_compression_rgtc "
    "GL_ARB_texture_cube_map GL_ARB_texture_border_clamp GL_ARB_texture_mirrored_repeat "
    "GL_EXT_texture_edge_clamp GL_ARB_texture_non_power_of_two GL_ARB_depth_texture GL_ARB_shadow "
    "GL_ARB_vertex_buffer_object GL_ARB_pixel_buffer_object GL_EXT_pixel_buffer_object "
    "GL_ARB_shader_objects GL_ARB_vertex_shader GL_ARB_fragment_shader GL_ARB_shading_language_100 "
    "GL_ARB_draw_buffers GL_ARB_texture_float GL_ARB_half_float_pixel GL_ARB_color_buffer_float "
    "GL_EXT_framebuffer_object GL_ARB_framebuffer_object GL_EXT_framebuffer_blit "
    "GL_EXT_framebuffer_multisample GL_EXT_packed_depth_stencil GL_EXT_texture_filter_anisotropic "
    "GL_EXT_blend_func_separate GL_EXT_blend_equation_separate GL_EXT_blend_minmax "
    "GL_EXT_blend_subtract GL_EXT_texture_lod_bias GL_SGIS_generate_mipmap GL_EXT_bgra "
    "GL_ARB_uniform_buffer_object GL_ARB_texture_rg GL_EXT_texture_sRGB GL_ARB_occlusion_query "
    "GL_ARB_multisample GL_ARB_point_sprite GL_EXT_draw_range_elements GL_ARB_vertex_program "
    "GL_ARB_fragment_program";

int gles_manual(rt_cpu *cpu, const char *n)
{
    if (!g_gles) return 0;
    check_exts();
    switch (n[2]) {                               /* cheap first-letter filter after "gl" */
    case 'S':
        if (!strcmp(n, "glShaderSource") || !strcmp(n, "glShaderSourceARB")) {
            uint32_t sh = ARG(0), cnt = ARG(1), strs = ARG(2), lens = ARG(3), i;
            size_t total = 0;
            int32_t type = 0;
            char *src;
            for (i = 0; i < cnt; i++) {
                int32_t l = lens ? (int32_t)rd32(lens + 4 * i) : -1;
                total += l >= 0 ? (size_t)l : strlen(gstr(rd32(strs + 4 * i)));
            }
            src = malloc(total + 1);
            total = 0;
            for (i = 0; i < cnt; i++) {
                int32_t l = lens ? (int32_t)rd32(lens + 4 * i) : -1;
                const char *s = gstr(rd32(strs + 4 * i));
                size_t k = l >= 0 ? (size_t)l : strlen(s);
                memcpy(src + total, s, k);
                total += k;
            }
            src[total] = 0;
            if (sh >= MAXOBJ) kfatal(cpu, "gles: shader id %u too large", sh);
            free(g_sh_src[sh]);
            g_sh_src[sh] = src;
            P(X_GetShaderiv, "glGetShaderiv", pf_getshi)(sh, 0x8B4F /* SHADER_TYPE */, &type);
            g_sh_frag[sh] = type == 0x8B30;
            compile_es(sh, NULL);
            RET(0);
        }
        if (!strcmp(n, "glShadeModel")) RET(0);
        break;
    case 'C':
        if (!strcmp(n, "glCompileShader")) {
            uint32_t sh = ARG(0);
            P(X_CompileShader, "glCompileShader", pf_v1)(sh);
            if (sh < MAXOBJ && g_sh_src[sh]) check_shader(sh);
            RET(0);
        }
        if (!strcmp(n, "glColor4f") || !strcmp(n, "glColor4fv") || !strcmp(n, "glColor3f") ||
            !strcmp(n, "glColor4ub")) RET(0);
        if (!strcmp(n, "glClearDepth")) { P(X_ClearDepthf, "glClearDepthf", pf_f1)((float)ARGD(0)); RET(0); }
        if (!strcmp(n, "glCompressedTexImage2D")) {
            uint32_t target = ARG(0), fmt = ARG(2), size = ARG(6), data = ARG(7);
            int32_t level = (int32_t)ARG(1), w = (int32_t)ARG(3), h = (int32_t)ARG(4);
            int native = ((fmt >= TEX_DXT1_RGB && fmt <= TEX_DXT5) && g_has_s3tc);
            if (target == 0x8064) {                           /* PROXY_TEXTURE_2D */
                g_proxy.w = w; g_proxy.h = h; g_proxy.internal = fmt;
                RET(0);
            }
            if (gl_bound_UNPACK()) break;                     /* PBO source: pass through */
            if (native) {
                P(X_CompressedTexImage2D, "glCompressedTexImage2D", pf_ctex)(target, level, fmt, w, h, 0,
                                                                             (int32_t)size, GUEST_PTR(data));
                after_upload(target, level);
                RET(0);
            }
            if (tex_block_bytes(fmt)) {
                upload_decoded(target, level, fmt, w, h, data ? (const uint8_t *)RT_P(data) : NULL, size);
                after_upload(target, level);
                RET(0);
            }
            break;
        }
        break;
    case 'A':
        if (!strcmp(n, "glAttachShader")) {
            struct gl_prog *p = prog_get(ARG(0));
            if (p && p->nsh < 8) p->sh[p->nsh++] = ARG(1);
            break;                                            /* and pass through */
        }
        if (!strcmp(n, "glAreTexturesResident")) {
            uint32_t k, cnt = ARG(0), out = ARG(2);
            for (k = 0; out && k < cnt; k++) wr8(out + k, 1);
            RET(1);
        }
        break;
    case 'B':
        if (!strcmp(n, "glBindFramebuffer") || !strcmp(n, "glBindFramebufferEXT")) {
            /* the game's default framebuffer is an offscreen target under OpenXR */
            uint32_t fb = ARG(1);
            if (!fb) fb = vid_default_fbo();
            P(X_BindFramebuffer, "glBindFramebuffer", pf_v2)(ARG(0), fb);
            RET(0);
        }
        if (!strcmp(n, "glBindFragDataLocation") || !strcmp(n, "glBindFragDataLocationEXT")) {
            struct gl_prog *p = prog_get(ARG(0));
            const char *name = gstr(ARG(2));
            int i;
            if (p && name) {
                for (i = 0; i < p->b.n && strcmp(p->b.name[i], name); i++) ;
                if (i < 8) {
                    snprintf(p->b.name[i], sizeof(p->b.name[i]), "%s", name);
                    p->b.loc[i] = (int)ARG(1);
                    if (i == p->b.n) p->b.n++;
                }
            }
            RET(0);
        }
        break;
    case 'L':
        if (!strcmp(n, "glLinkProgram")) {
            uint32_t prog = ARG(0);
            struct gl_prog *p = prog < MAXOBJ ? g_prog[prog] : NULL;
            int32_t ok = 0, i;
            if (p && p->b.n)                                  /* outputs need their locations */
                for (i = 0; i < p->nsh; i++)
                    if (p->sh[i] < MAXOBJ && g_sh_src[p->sh[i]] && g_sh_frag[p->sh[i]]) {
                        compile_es(p->sh[i], &p->b);
                        P(X_CompileShader, "glCompileShader", pf_v1)(p->sh[i]);
                        check_shader(p->sh[i]);
                    }
            P(X_LinkProgram, "glLinkProgram", pf_v1)(prog);
            P(X_GetProgramiv, "glGetProgramiv", pf_getshi)(prog, 0x8B82 /* LINK_STATUS */, &ok);
            if (!ok) {
                char log[2048] = "";
                P(X_GetProgramInfoLog, "glGetProgramInfoLog", pf_info)(prog, sizeof(log), NULL, log);
                klog("gles: program %u failed to link:\n%s\n", prog, log);
            }
            RET(0);
        }
        if (!strcmp(n, "glLightModeli") || !strcmp(n, "glLightModelfv") || !strcmp(n, "glLightModelf"))
            RET(0);
        if (!strcmp(n, "glLoadIdentity") || !strcmp(n, "glLoadMatrixf")) RET(0);
        break;
    case 'M':
        if (!strcmp(n, "glMatrixMode")) RET(0);
        if (!strcmp(n, "glMapBuffer") || !strcmp(n, "glMapBufferARB")) break;   /* hostlibs (shadow) */
        break;
    case 'D':
        if (!strcmp(n, "glDepthRange")) { P(X_DepthRangef, "glDepthRangef", pf_f2)((float)ARGD(0), (float)ARGD(2)); RET(0); }
        if (!strcmp(n, "glDisable") || !strcmp(n, "glDisablei")) {
            if (!es_cap(ARG(0))) RET(0);
            break;
        }
        if (!strcmp(n, "glDrawBuffer")) {
            int32_t fb = 0;
            uint32_t mode = ARG(0);
            P(X_GetIntegerv, "glGetIntegerv", pf_geti)(0x8CA6 /* DRAW_FRAMEBUFFER_BINDING */, &fb);
            if (fb && (mode == 0x0405 || mode == 0x0404 || mode == 0x0408)) mode = 0x8CE0;
            if (!fb && mode != 0) mode = 0x0405;
            P(X_DrawBuffers, "glDrawBuffers", pf_drawbufs)(1, &mode);
            RET(0);
        }
        if (!strcmp(n, "glDrawArrays") && ARG(0) == 7) { draw_quads((int32_t)ARG(1), (int32_t)ARG(2)); RET(0); }
        if (!strcmp(n, "glDrawBuffers") || !strcmp(n, "glDrawBuffersARB")) {
            /* ES: an FBO takes COLOR_ATTACHMENTi (or NONE) in slot i; the
             * default framebuffer takes BACK only */
            int32_t fb = 0, cnt = (int32_t)ARG(0), i;
            uint32_t bufs[8], src = ARG(1);
            if (cnt > 8) cnt = 8;
            P(X_GetIntegerv, "glGetIntegerv", pf_geti)(0x8CA6, &fb);
            for (i = 0; i < cnt; i++) {
                uint32_t b = src ? rd32(src + 4 * (uint32_t)i) : 0;
                if (fb && (b == 0x0405 || b == 0x0404 || b == 0x0408 || b == 0x0402)) b = 0x8CE0 + (uint32_t)i;
                else if (!fb && b >= 0x8CE0 && b < 0x8CF0) b = 0x0405;
                if (fb && b >= 0x8CE0 && b < 0x8CF0 && b != 0x8CE0 + (uint32_t)i) {
                    static int warned;
                    if (!warned++) klog("gles: glDrawBuffers slot %d -> attachment %u (unsupported in ES)\n", i, b - 0x8CE0);
                }
                bufs[i] = b;
            }
            P(X_DrawBuffers, "glDrawBuffers", pf_drawbufs)(cnt, bufs);
            RET(0);
        }
        break;
    case 'E':
        if (!strcmp(n, "glEnable") || !strcmp(n, "glEnablei")) {
            if (!es_cap(ARG(0))) RET(0);
            break;
        }
        break;
    case 'H':
        if (!strcmp(n, "glHint")) {
            uint32_t t = ARG(0);
            if (t != 0x8192 /* GENERATE_MIPMAP */ && t != 0x8B8B /* FRAGMENT_SHADER_DERIVATIVE */) RET(0);
        }
        break;
    case 'G':
        if (!strcmp(n, "glGetIntegerv") && vid_default_fbo()) {
            uint32_t pname = ARG(0), out = ARG(1);
            int32_t v = 0;
            if (pname == 0x8CA6 || pname == 0x8CAA) {     /* (DRAW_)FRAMEBUFFER / READ_FRAMEBUFFER_BINDING */
                P(X_GetIntegerv, "glGetIntegerv", pf_geti)(pname, &v);
                if ((uint32_t)v == vid_default_fbo()) v = 0;
                if (out) wr32(out, (uint32_t)v);
                RET(0);
            }
            break;
        }
        if (!strcmp(n, "glGetString")) {
            static uint32_t vendor, version, slv, exts;
            switch (ARG(0)) {
            case 0x1F00: if (!vendor) vendor = guest_strdup("NVIDIA Corporation"); RET(vendor);
            case 0x1F02: if (!version) version = guest_strdup("4.5.0 NVIDIA (OpenGL ES 3.2 backend)"); RET(version);
            case 0x8B8C: if (!slv) slv = guest_strdup("4.50 NVIDIA"); RET(slv);
            case 0x1F03: if (!exts) exts = guest_strdup(ES_EXTENSIONS); RET(exts);
            }
            break;                                    /* renderer: the real one */
        }
        if (!strcmp(n, "glGetActiveUniformName")) {
            int32_t size, len = 0;
            uint32_t type;
            uint32_t prog = ARG(0), idx = ARG(1), buf = ARG(2), plen = ARG(3), name = ARG(4);
            P(X_GetActiveUniform, "glGetActiveUniform", pf_actuni)(prog, idx, (int32_t)buf, &len, &size, &type,
                                                                   name ? (char *)RT_P(name) : NULL);
            if (plen) wr32(plen, (uint32_t)len);
            RET(0);
        }
        if (!strcmp(n, "glGetTexImage")) {
            get_tex_image(ARG(0), (int32_t)ARG(1), ARG(2), ARG(3), ARG(4));
            RET(0);
        }
        if (!strcmp(n, "glGetTexLevelParameteriv")) {
            uint32_t pname = ARG(2), out = ARG(3);
            if (ARG(0) == 0x8064) {                       /* proxy: the probed values */
                int32_t v = pname == 0x1000 ? g_proxy.w : pname == 0x1001 ? g_proxy.h :
                            pname == 0x1003 ? (int32_t)g_proxy.internal :
                            pname == 0x86A1 ? tex_block_bytes(g_proxy.internal) != 0 : 0;
                if (out) wr32(out, (uint32_t)v);
                RET(0);
            }
            if (pname == 0x86A0 || pname == 0x86A1) {     /* COMPRESSED_IMAGE_SIZE / TEXTURE_COMPRESSED */
                if (out) wr32(out, 0);
                RET(0);
            }
            break;
        }
        break;
    case 'T':
        if (!strcmp(n, "glTexImage2D")) {
            uint32_t target = ARG(0), internal = ARG(2), format = ARG(6), type = ARG(7), data = ARG(8);
            int32_t level = (int32_t)ARG(1), w = (int32_t)ARG(3), h = (int32_t)ARG(4);
            if (target == 0x8064) {                       /* PROXY_TEXTURE_2D */
                g_proxy.w = w; g_proxy.h = h; g_proxy.internal = internal;
                RET(0);
            }
            fix_tex_formats(&internal, &format, &type);
            P(X_TexImage2D, "glTexImage2D", pf_teximg)(target, level, (int32_t)internal, w, h, 0, format, type,
                gl_bound_UNPACK() ? (const void *)(uintptr_t)data : GUEST_PTR(data));
            after_upload(target, level);
            RET(0);
        }
        if (!strcmp(n, "glTexParameteri") || !strcmp(n, "glTexParameterf")) {
            uint32_t target = ARG(0), pname = ARG(1), v = ARG(2);
            if (n[14] == 'f') {                           /* float param: only wrap/filter enums matter */
                float f = rdf32(cpu->esp + 12);
                v = (uint32_t)f;
            }
            if (pname == 0x8191) {                        /* GENERATE_MIPMAP */
                uint32_t t = bound_tex(target);
                if (t < MAXOBJ) g_automip[t] = v != 0;
                RET(0);
            }
            if (pname == 0x884B /* DEPTH_TEXTURE_MODE */) RET(0);
            if ((pname == 0x2802 || pname == 0x2803 || pname == 0x8072) && v == 0x2900) v = 0x812F;   /* CLAMP */
            P(X_TexParameteri, "glTexParameteri", pf_v3)(target, pname, v);
            RET(0);
        }
        break;
    case 'F':
        if (!strcmp(n, "glFramebufferTexture2D") || !strcmp(n, "glFramebufferTexture2DEXT")) {
            /* ES needs the texture object to exist: a name from glGenTextures
             * that was never bound is created here (desktop drivers allow it) */
            uint32_t tex = ARG(3), target = ARG(2);
            if (tex && !((uint8_t (GLAPI_CALL *)(uint32_t))gl_proc(X_IsTexture, "glIsTexture"))(tex)) {
                uint32_t bt = target == 0x0DE1 ? 0x0DE1 : 0x8513, prev = bound_tex(bt);
                P(X_BindTexture, "glBindTexture", pf_v2)(bt, tex);
                P(X_BindTexture, "glBindTexture", pf_v2)(bt, prev);
                klog("gles: created never-bound texture %u for an FBO attachment\n", tex);
            }
            P(X_FramebufferTexture2D, "glFramebufferTexture2D", pf_fbtex)(ARG(0), ARG(1), target, tex, (int32_t)ARG(4));
            if (getenv("JPRT_GLCHECK")) {
                uint32_t e = ((uint32_t (GLAPI_CALL *)(void))gl_proc(X_GetError, "glGetError"))();
                if (e) {
                    uint32_t prev = bound_tex(0x0DE1), e2;
                    int32_t w = -1, h = -1, f = -1;
                    P(X_BindTexture, "glBindTexture", pf_v2)(0x0DE1, tex);
                    e2 = ((uint32_t (GLAPI_CALL *)(void))gl_proc(X_GetError, "glGetError"))();
                    P(X_GetTexLevelParameteriv, "glGetTexLevelParameteriv", pf_texlvl)(0x0DE1, (int32_t)ARG(4), 0x1000, &w);
                    P(X_GetTexLevelParameteriv, "glGetTexLevelParameteriv", pf_texlvl)(0x0DE1, (int32_t)ARG(4), 0x1001, &h);
                    P(X_GetTexLevelParameteriv, "glGetTexLevelParameteriv", pf_texlvl)(0x0DE1, (int32_t)ARG(4), 0x1003, &f);
                    P(X_BindTexture, "glBindTexture", pf_v2)(0x0DE1, prev);
                    klog("gles: FramebufferTexture2D(tex %u) error %#x; bind-2D error %#x, level %u: %dx%d internal %#x\n",
                         tex, e, e2, ARG(4), w, h, f);
                }
            }
            RET(0);
        }
        break;
    case 'R':
        if (!strcmp(n, "glReadBuffer")) {
            int32_t fb = 0;
            uint32_t mode = ARG(0);
            P(X_GetIntegerv, "glGetIntegerv", pf_geti)(0x8CAA, &fb);
            if (fb && (mode == 0x0405 || mode == 0x0404)) mode = 0x8CE0;
            P(X_ReadBuffer, "glReadBuffer", pf_v1)(mode);
            RET(0);
        }
        break;
    }
    return 0;
}

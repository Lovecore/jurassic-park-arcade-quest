/*
 * video_android.c — video.h on the Quest: EGL (OpenGL ES 3.2) + OpenXR.
 *
 * The game renders, as on the cabinet, into its "default framebuffer" —
 * here an offscreen FBO (vid_default_fbo; gles.c maps the game's binds of
 * framebuffer 0 to it). Each SDL_GL_SwapBuffers ends an OpenXR frame: the
 * FBO's colour texture is copied (glCopyImageSubData, raw texels: the
 * game's output is already display-encoded, the swapchain is sRGB) into a
 * swapchain image shown as a large quad layer in front of the player — the
 * cabinet screen in VR. All OpenXR calls happen on the game's render
 * thread (the one that owns the GL context).
 *
 * Controllers (Touch): the right aim ray hits the screen quad -> SDL mouse
 * motion (the game's io_sdl driver turns it into P1 gun aim); right
 * trigger -> mouse button 1 (P1 fire); left trigger -> mouse button 3;
 * A -> '4' (P1 start), X -> '5' (P2 start), B -> 'q', Y -> 'w',
 * left thumbstick click -> recentre the screen.
 *
 * Flat mode (no OpenXR runtime, e.g. the Android emulator, or
 * JPRT_FLAT=1): an EGL window surface on the activity's window; the
 * framebuffer is letterboxed onto it, touch/mouse aims and fires.
 */
#if defined(__ANDROID__)
#define XR_USE_PLATFORM_ANDROID
#define XR_USE_GRAPHICS_API_OPENGL_ES
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl32.h>
#include <android/log.h>
#include <android/native_window.h>
#include <dlfcn.h>
#include <jni.h>
#include <math.h>
#include <time.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>
#include "rt.h"
#include "kernel.h"
#include "video.h"
#include "glsl_es.h"

extern int g_gles;
extern JavaVM *g_android_vm;               /* android_main.c */
extern jobject g_android_activity;
extern ANativeWindow *volatile g_android_window;

static int g_flat;
static EGLSurface g_win = EGL_NO_SURFACE;
static int g_lb_x, g_lb_y, g_lb_w, g_lb_h;       /* letterbox rect on the window (flat mode) */

static EGLDisplay g_dpy = EGL_NO_DISPLAY;
static EGLContext g_ctx = EGL_NO_CONTEXT;
static EGLConfig g_cfg;
static EGLSurface g_pbuf = EGL_NO_SURFACE;
static GLuint g_fbo, g_color, g_depth;
static int g_w, g_h, g_ready;

static XrInstance g_inst = XR_NULL_HANDLE;
static XrSystemId g_sys;
static XrSession g_sess = XR_NULL_HANDLE;
static XrSpace g_local = XR_NULL_HANDLE, g_view = XR_NULL_HANDLE;
static XrSwapchain g_sc = XR_NULL_HANDLE;
static XrSwapchainImageOpenGLESKHR g_sc_img[8];
static uint32_t g_sc_n;
static int g_running;
static XrSessionState g_state = XR_SESSION_STATE_UNKNOWN;
static XrPosef g_screen;                      /* screen quad pose in LOCAL space */
/* metres; 2.4 m wide at 2.4 m is ~53 deg, about one game pixel per display
 * pixel on the Quest 3 (wider magnifies it and looks soft) */
static float g_screen_w = 2.4f, g_screen_h;
static int g_layer_settings;                  /* XR_FB_composition_layer_settings */
static uint32_t g_sc_mips = 1;

/* input */
static XrActionSet g_aset = XR_NULL_HANDLE;
static XrAction g_a_aim, g_a_trigger, g_a_btn[5], g_a_size;
static XrPath g_hand[2];
static XrSpace g_aim_space[2];

#define XRCHK(x) do { XrResult r_ = (x); if (XR_FAILED(r_)) { klog("openxr: %s failed (%d)\n", #x, (int)r_); return -1; } } while (0)

static int xr_init_instance(void)
{
    PFN_xrInitializeLoaderKHR init = NULL;
    XrLoaderInitInfoAndroidKHR li = { XR_TYPE_LOADER_INIT_INFO_ANDROID_KHR };
    XrInstanceCreateInfoAndroidKHR ia = { XR_TYPE_INSTANCE_CREATE_INFO_ANDROID_KHR };
    XrInstanceCreateInfo ci = { XR_TYPE_INSTANCE_CREATE_INFO };
    XrSystemGetInfo sgi = { XR_TYPE_SYSTEM_GET_INFO };
    const char *exts[3] = { XR_KHR_OPENGL_ES_ENABLE_EXTENSION_NAME, XR_KHR_ANDROID_CREATE_INSTANCE_EXTENSION_NAME };
    uint32_t next = 2, ne = 0, i;
    XrExtensionProperties *props;
    XRCHK(xrGetInstanceProcAddr(XR_NULL_HANDLE, "xrInitializeLoaderKHR", (PFN_xrVoidFunction *)&init));
    li.applicationVM = g_android_vm;
    li.applicationContext = g_android_activity;
    XRCHK(init((const XrLoaderInitInfoBaseHeaderKHR *)&li));
    /* optional: the Quest compositor's supersampling / sharpening of layers
     * (a 1360x768 flat screen is soft without it) */
    if (XR_SUCCEEDED(xrEnumerateInstanceExtensionProperties(NULL, 0, &ne, NULL)) && ne &&
        (props = calloc(ne, sizeof *props))) {
        for (i = 0; i < ne; i++) props[i].type = XR_TYPE_EXTENSION_PROPERTIES;
        if (XR_SUCCEEDED(xrEnumerateInstanceExtensionProperties(NULL, ne, &ne, props)))
            for (i = 0; i < ne; i++)
                if (!strcmp(props[i].extensionName, XR_FB_COMPOSITION_LAYER_SETTINGS_EXTENSION_NAME)) {
                    exts[next++] = XR_FB_COMPOSITION_LAYER_SETTINGS_EXTENSION_NAME;
                    g_layer_settings = 1;
                }
        free(props);
    }
    klog("openxr: layer settings (supersample/sharpen) %s\n", g_layer_settings ? "on" : "unavailable");
    ia.applicationVM = g_android_vm;
    ia.applicationActivity = g_android_activity;
    ci.next = &ia;
    snprintf(ci.applicationInfo.applicationName, sizeof(ci.applicationInfo.applicationName), "JP2015 VR");
    ci.applicationInfo.applicationVersion = 1;
    snprintf(ci.applicationInfo.engineName, sizeof(ci.applicationInfo.engineName), "jprt");
    ci.applicationInfo.apiVersion = XR_MAKE_VERSION(1, 0, 34);
    ci.enabledExtensionCount = next;
    ci.enabledExtensionNames = exts;
    XRCHK(xrCreateInstance(&ci, &g_inst));
    sgi.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
    XRCHK(xrGetSystem(g_inst, &sgi, &g_sys));
    return 0;
}

static int egl_init(void)
{
    const EGLint cfg_attr[] = { EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, g_flat ? 0 : 8,
                                EGL_DEPTH_SIZE, 0, EGL_RENDERABLE_TYPE, 0x40 /* EGL_OPENGL_ES3_BIT */,
                                EGL_SURFACE_TYPE, g_flat ? EGL_WINDOW_BIT : EGL_PBUFFER_BIT, EGL_NONE };
    const EGLint ctx_attr[] = { EGL_CONTEXT_CLIENT_VERSION, 3, 0x30FB /* MINOR */, 2, EGL_NONE };
    const EGLint pb_attr[] = { EGL_WIDTH, 16, EGL_HEIGHT, 16, EGL_NONE };
    EGLint n = 0;
    g_dpy = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (!eglInitialize(g_dpy, NULL, NULL)) return -1;
    if (!eglChooseConfig(g_dpy, cfg_attr, &g_cfg, 1, &n) || n < 1) { klog("egl: no config\n"); return -1; }
    {   /* ES 3.2 (the Quest); 3.1 / 3.0 on lesser GPUs such as the emulator's */
        EGLint attr[5];
        int minor;
        memcpy(attr, ctx_attr, sizeof(attr));
        for (minor = 2; minor >= 0; minor--) {
            attr[3] = minor;
            g_ctx = eglCreateContext(g_dpy, g_cfg, EGL_NO_CONTEXT, attr);
            if (g_ctx != EGL_NO_CONTEXT) break;
        }
        if (g_ctx == EGL_NO_CONTEXT) { klog("egl: eglCreateContext failed\n"); return -1; }
        glsl_es_version = 300 + 10 * minor;
        if (minor < 2) klog("egl: OpenGL ES 3.%d context (3.2 unavailable)\n", minor);
    }
    if (g_flat) {
        int i;
        for (i = 0; i < 1000 && !g_android_window; i++) {      /* up to 10 s for the window */
            struct timespec ts = { 0, 10000000 };
            nanosleep(&ts, NULL);
        }
        if (!g_android_window) return -1;
        {
            EGLint vid = 0;
            eglGetConfigAttrib(g_dpy, g_cfg, EGL_NATIVE_VISUAL_ID, &vid);
            ANativeWindow_setBuffersGeometry(g_android_window, 0, 0, vid);
        }
        g_win = eglCreateWindowSurface(g_dpy, g_cfg, g_android_window, NULL);
        if (g_win == EGL_NO_SURFACE) { klog("egl: eglCreateWindowSurface failed (%#x)\n", eglGetError()); return -1; }
        return eglMakeCurrent(g_dpy, g_win, g_win, g_ctx) ? 0 : -1;
    }
    g_pbuf = eglCreatePbufferSurface(g_dpy, g_cfg, pb_attr);
    if (!eglMakeCurrent(g_dpy, g_pbuf, g_pbuf, g_ctx)) return -1;
    return 0;
}

static XrPath path(const char *s)
{
    XrPath p = XR_NULL_PATH;
    xrStringToPath(g_inst, s, &p);
    return p;
}

static int xr_make_action(XrActionType type, const char *name, int both_hands, XrAction *out)
{
    XrActionCreateInfo ai = { XR_TYPE_ACTION_CREATE_INFO };
    ai.actionType = type;
    snprintf(ai.actionName, sizeof(ai.actionName), "%s", name);
    snprintf(ai.localizedActionName, sizeof(ai.localizedActionName), "%s", name);
    if (both_hands) { ai.countSubactionPaths = 2; ai.subactionPaths = g_hand; }
    XRCHK(xrCreateAction(g_aset, &ai, out));
    return 0;
}

static int xr_init_input(void)
{
    XrActionSetCreateInfo asi = { XR_TYPE_ACTION_SET_CREATE_INFO };
    static const char *const btn_names[5] = { "a", "b", "x", "y", "recenter" };
    static const char *const btn_paths[5] = { "/user/hand/right/input/a/click", "/user/hand/right/input/b/click",
                                              "/user/hand/left/input/x/click", "/user/hand/left/input/y/click",
                                              "/user/hand/left/input/thumbstick/click" };
    XrActionSuggestedBinding b[11];
    XrInteractionProfileSuggestedBinding sb = { XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING };
    XrSessionActionSetsAttachInfo at = { XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO };
    int i, n = 0;
    snprintf(asi.actionSetName, sizeof(asi.actionSetName), "game");
    snprintf(asi.localizedActionSetName, sizeof(asi.localizedActionSetName), "Game");
    XRCHK(xrCreateActionSet(g_inst, &asi, &g_aset));
    g_hand[0] = path("/user/hand/left");
    g_hand[1] = path("/user/hand/right");
    if (xr_make_action(XR_ACTION_TYPE_POSE_INPUT, "aim", 1, &g_a_aim) < 0) return -1;
    if (xr_make_action(XR_ACTION_TYPE_FLOAT_INPUT, "trigger", 1, &g_a_trigger) < 0) return -1;
    for (i = 0; i < 5; i++)
        if (xr_make_action(XR_ACTION_TYPE_BOOLEAN_INPUT, btn_names[i], 0, &g_a_btn[i]) < 0) return -1;
    if (xr_make_action(XR_ACTION_TYPE_FLOAT_INPUT, "screen_size", 0, &g_a_size) < 0) return -1;
    b[n].action = g_a_aim; b[n++].binding = path("/user/hand/left/input/aim/pose");
    b[n].action = g_a_aim; b[n++].binding = path("/user/hand/right/input/aim/pose");
    b[n].action = g_a_trigger; b[n++].binding = path("/user/hand/left/input/trigger/value");
    b[n].action = g_a_trigger; b[n++].binding = path("/user/hand/right/input/trigger/value");
    for (i = 0; i < 5; i++) { b[n].action = g_a_btn[i]; b[n++].binding = path(btn_paths[i]); }
    b[n].action = g_a_size; b[n++].binding = path("/user/hand/right/input/thumbstick/y");
    sb.interactionProfile = path("/interaction_profiles/oculus/touch_controller");
    sb.countSuggestedBindings = (uint32_t)n;
    sb.suggestedBindings = b;
    XRCHK(xrSuggestInteractionProfileBindings(g_inst, &sb));
    {   /* generic controllers (other runtimes, emulators): aim, select = fire, menu = start */
        XrActionSuggestedBinding g[6];
        XrInteractionProfileSuggestedBinding gs = { XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING };
        int k = 0;
        g[k].action = g_a_aim; g[k++].binding = path("/user/hand/left/input/aim/pose");
        g[k].action = g_a_aim; g[k++].binding = path("/user/hand/right/input/aim/pose");
        g[k].action = g_a_trigger; g[k++].binding = path("/user/hand/left/input/select/click");
        g[k].action = g_a_trigger; g[k++].binding = path("/user/hand/right/input/select/click");
        g[k].action = g_a_btn[0]; g[k++].binding = path("/user/hand/right/input/menu/click");
        g[k].action = g_a_btn[2]; g[k++].binding = path("/user/hand/left/input/menu/click");
        gs.interactionProfile = path("/interaction_profiles/khr/simple_controller");
        gs.countSuggestedBindings = (uint32_t)k;
        gs.suggestedBindings = g;
        if (XR_FAILED(xrSuggestInteractionProfileBindings(g_inst, &gs))) klog("openxr: simple_controller bindings refused\n");
    }
    for (i = 0; i < 2; i++) {
        XrActionSpaceCreateInfo sci = { XR_TYPE_ACTION_SPACE_CREATE_INFO };
        sci.action = g_a_aim;
        sci.subactionPath = g_hand[i];
        sci.poseInActionSpace.orientation.w = 1;
        XRCHK(xrCreateActionSpace(g_sess, &sci, &g_aim_space[i]));
    }
    at.countActionSets = 1;
    at.actionSets = &g_aset;
    XRCHK(xrAttachSessionActionSets(g_sess, &at));
    return 0;
}

static int xr_init_session(void)
{
    PFN_xrGetOpenGLESGraphicsRequirementsKHR req_fn = NULL;
    XrGraphicsRequirementsOpenGLESKHR req = { XR_TYPE_GRAPHICS_REQUIREMENTS_OPENGL_ES_KHR };
    XrGraphicsBindingOpenGLESAndroidKHR gb = { XR_TYPE_GRAPHICS_BINDING_OPENGL_ES_ANDROID_KHR };
    XrSessionCreateInfo sci = { XR_TYPE_SESSION_CREATE_INFO };
    XrReferenceSpaceCreateInfo rs = { XR_TYPE_REFERENCE_SPACE_CREATE_INFO };
    XrSwapchainCreateInfo swi = { XR_TYPE_SWAPCHAIN_CREATE_INFO };
    int64_t *fmts;
    uint32_t nf = 0, i;
    int64_t fmt = 0;
    XRCHK(xrGetInstanceProcAddr(g_inst, "xrGetOpenGLESGraphicsRequirementsKHR", (PFN_xrVoidFunction *)&req_fn));
    XRCHK(req_fn(g_inst, g_sys, &req));
    gb.display = g_dpy;
    gb.config = g_cfg;
    gb.context = g_ctx;
    sci.next = &gb;
    sci.systemId = g_sys;
    XRCHK(xrCreateSession(g_inst, &sci, &g_sess));
    rs.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
    rs.poseInReferenceSpace.orientation.w = 1;
    XRCHK(xrCreateReferenceSpace(g_sess, &rs, &g_local));
    rs.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW;
    XRCHK(xrCreateReferenceSpace(g_sess, &rs, &g_view));
    /* two-call idiom: the Quest runtime lists well over 64 formats */
    XRCHK(xrEnumerateSwapchainFormats(g_sess, 0, &nf, NULL));
    fmts = calloc(nf ? nf : 1, sizeof *fmts);
    XRCHK(xrEnumerateSwapchainFormats(g_sess, nf, &nf, fmts));
    for (i = 0; i < nf; i++) if (fmts[i] == GL_SRGB8_ALPHA8) fmt = fmts[i];
    if (!fmt) for (i = 0; i < nf; i++) if (fmts[i] == GL_RGBA8) fmt = fmts[i];
    free(fmts);
    if (!fmt) { klog("openxr: no RGBA8 swapchain format\n"); return -1; }
    swi.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT;
    swi.format = fmt;
    swi.sampleCount = 1;
    swi.width = (uint32_t)g_w;
    swi.height = (uint32_t)g_h;
    swi.faceCount = 1;
    swi.arraySize = 1;
    /* a full mip chain (rebuilt each frame) so the compositor minifies the
     * screen cleanly when it is small or far; plain level 0 if refused */
    for (g_sc_mips = 1; (g_w >> g_sc_mips) || (g_h >> g_sc_mips); g_sc_mips++) {}
    swi.mipCount = g_sc_mips;
    if (XR_FAILED(xrCreateSwapchain(g_sess, &swi, &g_sc))) {
        klog("openxr: mipmapped swapchain refused, using one level\n");
        swi.mipCount = g_sc_mips = 1;
        XRCHK(xrCreateSwapchain(g_sess, &swi, &g_sc));
    }
    for (i = 0; i < 8; i++) g_sc_img[i].type = XR_TYPE_SWAPCHAIN_IMAGE_OPENGL_ES_KHR;
    XRCHK(xrEnumerateSwapchainImages(g_sc, 0, &g_sc_n, NULL));
    if (g_sc_n > 8) { klog("openxr: %u swapchain images (max 8)\n", g_sc_n); return -1; }
    XRCHK(xrEnumerateSwapchainImages(g_sc, g_sc_n, &g_sc_n, (XrSwapchainImageBaseHeader *)g_sc_img));
    /* the cabinet screen: 2.4 m in front at eye height of the start pose */
    g_screen.orientation.w = 1;
    g_screen.position.z = -2.4f;
    if (getenv("JPRT_SCREEN_W") && atof(getenv("JPRT_SCREEN_W")) > 0.1) g_screen_w = (float)atof(getenv("JPRT_SCREEN_W"));
    g_screen_h = g_screen_w * (float)g_h / (float)g_w;
    klog("openxr: session up, swapchain %ux%u format %#llx, %u images, %u mips, screen %.2f m\n", g_w, g_h,
         (long long)fmt, g_sc_n, g_sc_mips, g_screen_w);
    return xr_init_input();
}

int vid_create(int w, int h)
{
    if (g_ready) return 0;
    g_w = w;
    g_h = h;
    g_flat = getenv("JPRT_FLAT") && atoi(getenv("JPRT_FLAT"));
    if (!g_flat && xr_init_instance() < 0) {
        klog("openxr: no runtime: flat mode\n");
        g_flat = 1;
    }
    if (egl_init() < 0) { klog("egl: context creation failed (%#x)\n", eglGetError()); return -1; }
    g_gles = 1;
    /* the game's default framebuffer */
    glGenTextures(1, &g_color);
    glBindTexture(GL_TEXTURE_2D, g_color);
    glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA8, w, h);
    glBindTexture(GL_TEXTURE_2D, 0);
    glGenRenderbuffers(1, &g_depth);
    glBindRenderbuffer(GL_RENDERBUFFER, g_depth);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, w, h);
    glBindRenderbuffer(GL_RENDERBUFFER, 0);
    glGenFramebuffers(1, &g_fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, g_fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, g_color, 0);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, g_depth);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) klog("egl: default FBO incomplete\n");
    glViewport(0, 0, w, h);
    vid_log_context();
    if (!g_flat && xr_init_session() < 0) return -1;
    g_ready = 1;
    return 0;
}

void vid_log_context(void)
{
    klog("GL: %s / %s / %s\n", (const char *)glGetString(GL_VENDOR), (const char *)glGetString(GL_RENDERER),
         (const char *)glGetString(GL_VERSION));
}

int vid_ready(void) { return g_ready; }
uint32_t vid_default_fbo(void) { return g_fbo; }
void vid_set_title(const char *t) { (void)t; }
void vid_swap_interval(int n) { (void)n; }    /* xrWaitFrame paces */
void vid_screen_size(int *w, int *h) { *w = g_w ? g_w : 1360; *h = g_h ? g_h : 768; }

void *vid_gl_proc(const char *name)
{
    static void *gles;
    void *p = (void *)eglGetProcAddress(name);
    if (!p) {
        if (!gles) gles = dlopen("libGLESv3.so", RTLD_NOW);
        if (gles) p = dlsym(gles, name);
    }
    return p;
}

void vid_pump(void) {}   /* OS events arrive through OpenXR in vid_present */

/* ---- input ---- */
static void key(uint32_t sym, int down)
{
    uint8_t ev[20] = { 0 };
    ev[0] = down ? 2 : 3;
    ev[2] = (uint8_t)down;
    memcpy(ev + 8, &sym, 4);
    host_push_event(ev);
}

static void mouse_button(int button, int down, int x, int y)
{
    uint8_t ev[20] = { 0 };
    uint16_t ux = (uint16_t)x, uy = (uint16_t)y;
    ev[0] = down ? 5 : 6;
    ev[2] = (uint8_t)button;
    ev[3] = (uint8_t)down;
    memcpy(ev + 4, &ux, 2);
    memcpy(ev + 6, &uy, 2);
    host_push_event(ev);
}

static void mouse_motion(int x, int y)
{
    static int lx, ly;
    uint8_t ev[20] = { 0 };
    uint16_t ux = (uint16_t)x, uy = (uint16_t)y;
    int16_t dx = (int16_t)(x - lx), dy = (int16_t)(y - ly);
    if (x == lx && y == ly) return;
    lx = x; ly = y;
    ev[0] = 4;
    memcpy(ev + 4, &ux, 2);
    memcpy(ev + 6, &uy, 2);
    memcpy(ev + 8, &dx, 2);
    memcpy(ev + 10, &dy, 2);
    host_push_event(ev);
}

static void quat_rotate(const XrQuaternionf *q, const XrVector3f *v, XrVector3f *o)
{
    /* o = q * v * q^-1 */
    float tx = 2 * (q->y * v->z - q->z * v->y), ty = 2 * (q->z * v->x - q->x * v->z), tz = 2 * (q->x * v->y - q->y * v->x);
    o->x = v->x + q->w * tx + (q->y * tz - q->z * ty);
    o->y = v->y + q->w * ty + (q->z * tx - q->x * tz);
    o->z = v->z + q->w * tz + (q->x * ty - q->y * tx);
}

/* aim ray of hand i against the screen plane -> game pixels; 0 if it misses */
static int aim_hit(int i, XrTime t, int *px, int *py)
{
    XrSpaceLocation loc = { XR_TYPE_SPACE_LOCATION };
    XrVector3f fwd = { 0, 0, -1 }, d, o;
    float k, u, v;
    if (XR_FAILED(xrLocateSpace(g_aim_space[i], g_local, t, &loc))) return 0;
    if (!(loc.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT) ||
        !(loc.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT)) return 0;
    quat_rotate(&loc.pose.orientation, &fwd, &d);
    o = loc.pose.position;
    /* screen plane z = g_screen.position.z (the screen faces +z, unrotated) */
    if (d.z > -1e-4f) return 0;
    k = (g_screen.position.z - o.z) / d.z;
    u = (o.x + k * d.x - g_screen.position.x) / g_screen_w + 0.5f;
    v = 0.5f - (o.y + k * d.y - g_screen.position.y) / g_screen_h;
    if (u < -0.1f || u > 1.1f || v < -0.1f || v > 1.1f) return 0;
    *px = (int)(fminf(fmaxf(u, 0.f), 1.f) * (float)(g_w - 1));
    *py = (int)(fminf(fmaxf(v, 0.f), 1.f) * (float)(g_h - 1));
    return 1;
}

static void recenter(XrTime t)
{
    XrSpaceLocation loc = { XR_TYPE_SPACE_LOCATION };
    if (XR_FAILED(xrLocateSpace(g_view, g_local, t, &loc)) ||
        !(loc.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT)) return;
    g_screen.position.x = loc.pose.position.x;
    g_screen.position.y = loc.pose.position.y;
    g_screen.position.z = loc.pose.position.z - 2.4f;
}

static void poll_input(XrTime t)
{
    static int trig_down[2], btn_down[5], mx = -1, my = -1;
    static const uint32_t btn_key[4] = { '4', 'q', '5', 'w' };
    XrActiveActionSet aas = { g_aset, XR_NULL_PATH };
    XrActionsSyncInfo si = { XR_TYPE_ACTIONS_SYNC_INFO };
    int i, x, y;
    si.countActiveActionSets = 1;
    si.activeActionSets = &aas;
    if (XR_FAILED(xrSyncActions(g_sess, &si))) return;
    if (aim_hit(1, t, &x, &y)) { mouse_motion(x, y); mx = x; my = y; }
    for (i = 0; i < 2; i++) {
        XrActionStateGetInfo gi = { XR_TYPE_ACTION_STATE_GET_INFO };
        XrActionStateFloat st = { XR_TYPE_ACTION_STATE_FLOAT };
        int down;
        gi.action = g_a_trigger;
        gi.subactionPath = g_hand[i];
        if (XR_FAILED(xrGetActionStateFloat(g_sess, &gi, &st))) continue;
        down = st.isActive && st.currentState > (trig_down[i] ? 0.35f : 0.6f);   /* hysteresis */
        if (down != trig_down[i]) {
            trig_down[i] = down;
            mouse_button(i == 1 ? 1 : 3, down, mx < 0 ? g_w / 2 : mx, my < 0 ? g_h / 2 : my);
        }
    }
    for (i = 0; i < 5; i++) {
        XrActionStateGetInfo gi = { XR_TYPE_ACTION_STATE_GET_INFO };
        XrActionStateBoolean st = { XR_TYPE_ACTION_STATE_BOOLEAN };
        int down;
        gi.action = g_a_btn[i];
        if (XR_FAILED(xrGetActionStateBoolean(g_sess, &gi, &st))) continue;
        down = st.isActive && st.currentState;
        if (down == btn_down[i]) continue;
        btn_down[i] = down;
        if (i < 4) key(btn_key[i], down);
        else if (down) recenter(t);
    }
    {   /* right thumbstick up / down: screen bigger / smaller (1.2 .. 4.8 m) */
        XrActionStateGetInfo gi = { XR_TYPE_ACTION_STATE_GET_INFO };
        XrActionStateFloat st = { XR_TYPE_ACTION_STATE_FLOAT };
        static XrTime last;
        gi.action = g_a_size;
        if (XR_SUCCEEDED(xrGetActionStateFloat(g_sess, &gi, &st)) && st.isActive && fabsf(st.currentState) > 0.25f) {
            float dt = last ? (float)(t - last) * 1e-9f : 0.f;
            if (dt > 0.1f) dt = 0.1f;
            g_screen_w *= 1.f + st.currentState * 0.5f * dt;
            g_screen_w = fminf(fmaxf(g_screen_w, 1.2f), 4.8f);
            g_screen_h = g_screen_w * (float)g_h / (float)g_w;
        }
        last = t;
    }
}

/* ---- session state ---- */
static void poll_events(void)
{
    XrEventDataBuffer e;
    for (;;) {
        e.type = XR_TYPE_EVENT_DATA_BUFFER;
        e.next = NULL;
        if (xrPollEvent(g_inst, &e) != XR_SUCCESS) return;
        if (e.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED) {
            const XrEventDataSessionStateChanged *sc = (const XrEventDataSessionStateChanged *)&e;
            g_state = sc->state;
            klog("openxr: session state %d\n", (int)g_state);
            if (g_state == XR_SESSION_STATE_READY) {
                XrSessionBeginInfo bi = { XR_TYPE_SESSION_BEGIN_INFO };
                bi.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
                if (XR_SUCCEEDED(xrBeginSession(g_sess, &bi))) g_running = 1;
            } else if (g_state == XR_SESSION_STATE_STOPPING) {
                xrEndSession(g_sess);
                g_running = 0;
            } else if (g_state == XR_SESSION_STATE_EXITING || g_state == XR_SESSION_STATE_LOSS_PENDING) {
                klog("openxr: session ending, exiting\n");
                exit(0);
            }
        } else if (e.type == XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING) {
            exit(0);
        }
    }
}

/* ---- flat mode ---- */
static void flat_present(void)
{
    EGLint ww = 0, wh = 0;
    GLint prev_draw = 0, prev_read = 0;
    eglQuerySurface(g_dpy, g_win, EGL_WIDTH, &ww);
    eglQuerySurface(g_dpy, g_win, EGL_HEIGHT, &wh);
    if (ww <= 0 || wh <= 0) return;
    if ((int64_t)ww * g_h > (int64_t)wh * g_w) { g_lb_h = wh; g_lb_w = (int)((int64_t)wh * g_w / g_h); }
    else { g_lb_w = ww; g_lb_h = (int)((int64_t)ww * g_h / g_w); }
    g_lb_x = (ww - g_lb_w) / 2;
    g_lb_y = (wh - g_lb_h) / 2;
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &prev_draw);
    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &prev_read);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, g_fbo);
    glReadBuffer(GL_COLOR_ATTACHMENT0);
    glDisable(GL_SCISSOR_TEST);
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    glBlitFramebuffer(0, 0, g_w, g_h, g_lb_x, g_lb_y, g_lb_x + g_lb_w, g_lb_y + g_lb_h,
                      GL_COLOR_BUFFER_BIT, GL_LINEAR);
    eglSwapBuffers(g_dpy, g_win);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, (GLuint)prev_draw);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, (GLuint)prev_read);
}

/* android_main.c (activity thread): touch / mouse and keys in flat mode */
void vid_flat_touch(float x, float y, int down)
{
    static int was_down;
    int gx, gy;
    if (!g_flat || g_lb_w <= 0) return;
    gx = (int)((x - (float)g_lb_x) * (float)g_w / (float)g_lb_w);
    /* window y is top-down, as are SDL mouse coordinates */
    gy = (int)((y - (float)g_lb_y) * (float)g_h / (float)g_lb_h);
    if (gx < 0) gx = 0;
    if (gx >= g_w) gx = g_w - 1;
    if (gy < 0) gy = 0;
    if (gy >= g_h) gy = g_h - 1;
    mouse_motion(gx, gy);
    if (down != was_down) { mouse_button(1, down, gx, gy); was_down = down; }
}

void vid_flat_key(int keycode, int down)
{
    uint32_t sym = 0;
    if (keycode >= 7 && keycode <= 16) sym = (uint32_t)('0' + keycode - 7);          /* KEYCODE_0..9 */
    else if (keycode >= 29 && keycode <= 54) sym = (uint32_t)('a' + keycode - 29);   /* KEYCODE_A..Z */
    else if (keycode == 66) sym = 13;                                                /* ENTER */
    else if (keycode == 111) sym = 27;                                               /* ESCAPE */
    else if (keycode == 96) sym = '4';                                               /* gamepad A: P1 start */
    else if (keycode == 99) sym = '5';                                               /* gamepad X: P2 start */
    else if (keycode == 97) sym = 'q';
    else if (keycode == 100) sym = 'w';
    if (sym) key(sym, down);
}

void vid_present(void)
{
    XrFrameWaitInfo wi = { XR_TYPE_FRAME_WAIT_INFO };
    XrFrameState fs = { XR_TYPE_FRAME_STATE };
    XrFrameBeginInfo bi = { XR_TYPE_FRAME_BEGIN_INFO };
    XrFrameEndInfo ei = { XR_TYPE_FRAME_END_INFO };
    XrCompositionLayerQuad quad = { XR_TYPE_COMPOSITION_LAYER_QUAD };
    const XrCompositionLayerBaseHeader *layers[1];
    static int centred;
    if (!g_ready) return;
    if (g_flat) { flat_present(); return; }
    poll_events();
    if (!g_running) {
        struct timespec ts = { 0, 10000000 };
        nanosleep(&ts, NULL);              /* not visible: keep the game ticking slowly */
        return;
    }
    if (XR_FAILED(xrWaitFrame(g_sess, &wi, &fs))) return;
    if (XR_FAILED(xrBeginFrame(g_sess, &bi))) return;
    if (!centred) { recenter(fs.predictedDisplayTime); centred = 1; }
    poll_input(fs.predictedDisplayTime);
    ei.displayTime = fs.predictedDisplayTime;
    ei.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
    if (fs.shouldRender) {
        XrSwapchainImageAcquireInfo ai = { XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO };
        XrSwapchainImageWaitInfo swi = { XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO };
        XrSwapchainImageReleaseInfo ri = { XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO };
        uint32_t idx = 0;
        swi.timeout = XR_INFINITE_DURATION;
        if (XR_SUCCEEDED(xrAcquireSwapchainImage(g_sc, &ai, &idx)) &&
            XR_SUCCEEDED(xrWaitSwapchainImage(g_sc, &swi))) {
            /* raw texel copy: OpenXR GL swapchains are bottom-up like the
             * game's framebuffer, and the game's pixels are already
             * display-encoded, which an sRGB swapchain expects verbatim */
            while (glGetError()) {}             /* only the copy's own errors below */
            if (glsl_es_version >= 320) {
                glCopyImageSubData(g_color, GL_TEXTURE_2D, 0, 0, 0, 0,
                                   g_sc_img[idx].image, GL_TEXTURE_2D, 0, 0, 0, 0, g_w, g_h, 1);
            } else {                        /* ES 3.0/3.1 (emulators): a blit (sRGB-encodes) */
                static GLuint dst_fbo;
                GLint prev_draw = 0, prev_read = 0;
                glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &prev_draw);
                glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &prev_read);
                if (!dst_fbo) glGenFramebuffers(1, &dst_fbo);
                glBindFramebuffer(GL_READ_FRAMEBUFFER, g_fbo);
                glReadBuffer(GL_COLOR_ATTACHMENT0);
                glBindFramebuffer(GL_DRAW_FRAMEBUFFER, dst_fbo);
                glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                                       g_sc_img[idx].image, 0);
                glBlitFramebuffer(0, 0, g_w, g_h, 0, 0, g_w, g_h, GL_COLOR_BUFFER_BIT, GL_NEAREST);
                glBindFramebuffer(GL_DRAW_FRAMEBUFFER, (GLuint)prev_draw);
                glBindFramebuffer(GL_READ_FRAMEBUFFER, (GLuint)prev_read);
            }
            if (g_sc_mips > 1) {
                GLint prev_tex = 0;
                glGetIntegerv(GL_TEXTURE_BINDING_2D, &prev_tex);
                glBindTexture(GL_TEXTURE_2D, g_sc_img[idx].image);
                glGenerateMipmap(GL_TEXTURE_2D);
                glBindTexture(GL_TEXTURE_2D, (GLuint)prev_tex);
            }
            {
                static int checked;
                GLenum e = glGetError();
                if (e && checked++ < 3) klog("openxr: frame copy GL error %#x\n", e);
            }
            xrReleaseSwapchainImage(g_sc, &ri);
            quad.space = g_local;
            quad.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
            quad.subImage.swapchain = g_sc;
            quad.subImage.imageRect.extent.width = g_w;
            quad.subImage.imageRect.extent.height = g_h;
            quad.pose = g_screen;
            quad.size.width = g_screen_w;
            quad.size.height = g_screen_h;
            if (g_layer_settings) {
                static XrCompositionLayerSettingsFB ls = { XR_TYPE_COMPOSITION_LAYER_SETTINGS_FB };
                ls.layerFlags = XR_COMPOSITION_LAYER_SETTINGS_QUALITY_SUPER_SAMPLING_BIT_FB |
                                XR_COMPOSITION_LAYER_SETTINGS_QUALITY_SHARPENING_BIT_FB;
                quad.next = &ls;
            }
            layers[0] = (const XrCompositionLayerBaseHeader *)&quad;
            ei.layerCount = 1;
            ei.layers = layers;
        }
    }
    xrEndFrame(g_sess, &ei);
}
#endif

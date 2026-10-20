/*
 * hostlibs.c — host implementations of the libraries the recompiled
 * process does not run itself: SDL 1.2 (subset the game uses), OpenGL
 * (generated thunks in out/gl_thunks.c + the hand-written ones here), GLX
 * proc lookup, GLU, glut, udev, X11. Windows host: Win32 window + WGL.
 *
 * Called from host_call() with the guest stack as on i386: [esp] is the
 * return address, arguments above it; results go to eax (or st0), and the
 * handler pops the return address.
 *
 * Bring-up state: video/GL/events/timers are real; audio, joystick, udev
 * and X11 are stubs that report failure, so the game takes its own
 * "no device" paths.
 */
#include <windows.h>
#include <GL/gl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "rt.h"
#include "kernel.h"
#include "gl_host.h"

#define ARG(i) rd32(cpu->esp + 4 + 4 * (i))
#define ARGF(i) rdf32(cpu->esp + 4 + 4 * (i))
#define RET(v) do { cpu->eax = (uint32_t)(v); cpu->esp += 4; return 1; } while (0)

/* ---- guest memory owned by the host (SDL surfaces, strings) ---- */
#define HOSTHEAP_BASE 0xb6000000u
#define HOSTHEAP_SIZE 0x00400000u
static uint32_t g_hh_next;

/* Reserved at startup, before the guest maps anything (mapping it later
 * would zero guest pages the guest's mmap may have put there). */
void host_heap_init(void)
{
    mem_map(HOSTHEAP_BASE, HOSTHEAP_SIZE);
    g_hh_next = HOSTHEAP_BASE;
}

uint32_t guest_alloc(uint32_t n)
{
    uint32_t a;
    n = (n + 15) & ~15u;
    if (g_hh_next + n > HOSTHEAP_BASE + HOSTHEAP_SIZE) kfatal(NULL, "host heap exhausted");
    a = g_hh_next;
    g_hh_next += n;
    return a;
}

uint32_t guest_strdup(const char *s)
{
    uint32_t n = (uint32_t)strlen(s) + 1, a = guest_alloc(n);
    memcpy(RT_P(a), s, n);
    return a;
}

/* ---- window + GL context ---- */
static HWND g_wnd;
static HDC g_dc;
static HGLRC g_rc;
static int g_w = 1360, g_h = 768;
static uint32_t g_surface;            /* guest SDL_Surface */
static uint32_t g_errstr;
static LARGE_INTEGER g_t0, g_freq;

/* SDL event queue (guest SDL_Event layout, 20 bytes) */
#define QN 256
static uint8_t g_evq[QN][20];
static int g_evh, g_evt;

static SRWLOCK g_evlock = SRWLOCK_INIT;   /* window/render thread pushes, io thread polls */

static void push_event(const uint8_t *ev)
{
    int n;
    AcquireSRWLockExclusive(&g_evlock);
    n = (g_evt + 1) % QN;
    if (n != g_evh) {
        memcpy(g_evq[g_evt], ev, 20);
        g_evt = n;
    }
    ReleaseSRWLockExclusive(&g_evlock);
}

/* JPRT_KEYS="frame+sym,frame-sym,...": scripted SDL key down (+) / up (-)
 * events at swap counts, for testing input without a person at the window;
 * sym is a decimal SDLK value (2+ digits) or a single character. */
static void scripted_keys(unsigned frame)
{
    static const char *p;
    static int init;
    if (!init) { init = 1; p = getenv("JPRT_KEYS"); }
    while (p && *p) {
        char *e;
        unsigned long f = strtoul(p, &e, 10);
        uint8_t ev[20] = { 0 };
        uint32_t sym;
        if (f > frame) return;
        if (*e != '+' && *e != '-') { p = NULL; return; }
        ev[0] = *e == '+' ? 2 : 3;
        ev[2] = *e == '+';
        e++;
        if (e[0] >= '0' && e[0] <= '9' && e[1] >= '0' && e[1] <= '9') sym = (uint32_t)strtoul(e, &e, 10);
        else sym = (uint8_t)*e++;
        memcpy(ev + 8, &sym, 4);
        push_event(ev);
        klog("JPRT_KEYS: frame %u key %u %s\n", frame, sym, ev[0] == 2 ? "down" : "up");
        p = *e == ',' ? e + 1 : e;
    }
}

static uint32_t sdl_key(WPARAM vk)
{
    if (vk >= 'A' && vk <= 'Z') return (uint32_t)(vk - 'A' + 'a');
    if (vk >= '0' && vk <= '9') return (uint32_t)vk;
    switch (vk) {
    case VK_ESCAPE: return 27; case VK_RETURN: return 13; case VK_SPACE: return 32;
    case VK_BACK: return 8; case VK_TAB: return 9;
    case VK_UP: return 273; case VK_DOWN: return 274; case VK_RIGHT: return 275; case VK_LEFT: return 276;
    case VK_F1: return 282; case VK_F2: return 283; case VK_F3: return 284; case VK_F4: return 285;
    case VK_F5: return 286; case VK_F6: return 287; case VK_F7: return 288; case VK_F8: return 289;
    case VK_F9: return 290; case VK_F10: return 291; case VK_F11: return 292; case VK_F12: return 293;
    case VK_SHIFT: return 304; case VK_CONTROL: return 306; case VK_MENU: return 308;
    default: return 0;
    }
}

static LRESULT CALLBACK wndproc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    uint8_t ev[20] = { 0 };
    switch (m) {
    case WM_CLOSE:
        ev[0] = 12;                                   /* SDL_QUIT */
        push_event(ev);
        return 0;
    case WM_KEYDOWN: case WM_KEYUP: case WM_SYSKEYDOWN: case WM_SYSKEYUP: {
        /* SDL_KeyboardEvent (i386): type, which, state, pad, keysym {
         * scancode @4, sym @8, mod @12, unicode @16 } */
        uint32_t sym = sdl_key(w);
        if (!sym || (m == WM_KEYDOWN && (l & 0x40000000))) break;   /* no auto-repeat */
        ev[0] = (m == WM_KEYDOWN || m == WM_SYSKEYDOWN) ? 2 : 3;   /* SDL_KEYDOWN / UP */
        ev[2] = ev[0] == 2 ? 1 : 0;
        ev[4] = (uint8_t)((l >> 16) & 0xff);
        memcpy(ev + 8, &sym, 4);
        push_event(ev);
        return 0;
    }
    case WM_MOUSEMOVE: case WM_LBUTTONDOWN: case WM_LBUTTONUP: case WM_RBUTTONDOWN: case WM_RBUTTONUP:
    case WM_MBUTTONDOWN: case WM_MBUTTONUP: {
        /* SDL_MouseMotionEvent: type 4, which, state @2, x @4, y @6, xrel @8, yrel @10
         * SDL_MouseButtonEvent: type 5/6, which, button @2, state @3, x @4, y @6
         * (the game's io_sdl_loop drives the gun's analog x/y from these) */
        static int lx, ly;
        uint16_t x = (uint16_t)(short)LOWORD(l), y = (uint16_t)(short)HIWORD(l);
        int16_t dx = (int16_t)(x - lx), dy = (int16_t)(y - ly);
        lx = x; ly = y;
        if (m == WM_MOUSEMOVE) {
            ev[0] = 4;
            ev[2] = (uint8_t)(((w & MK_LBUTTON) ? 1 : 0) | ((w & MK_MBUTTON) ? 2 : 0) | ((w & MK_RBUTTON) ? 4 : 0));
            memcpy(ev + 8, &dx, 2);
            memcpy(ev + 10, &dy, 2);
        } else {
            ev[0] = (m == WM_LBUTTONDOWN || m == WM_RBUTTONDOWN || m == WM_MBUTTONDOWN) ? 5 : 6;
            ev[2] = (m == WM_LBUTTONDOWN || m == WM_LBUTTONUP) ? 1 : (m == WM_MBUTTONDOWN || m == WM_MBUTTONUP) ? 2 : 3;
            ev[3] = ev[0] == 5 ? 1 : 0;
        }
        memcpy(ev + 4, &x, 2);
        memcpy(ev + 6, &y, 2);
        push_event(ev);
        return 0;
    }
    }
    return DefWindowProcA(h, m, w, l);
}

static void pump(void)
{
    MSG msg;
    while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE)) {
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
    }
}

extern int g_gles;                    /* gles.c: the context is OpenGL ES */
static int g_swap_interval = 1;

static void apply_swap_interval(void)
{
    BOOL (WINAPI *si)(int);
    if (!g_rc) return;                         /* applied once the context exists */
    si = (void *)wglGetProcAddress("wglSwapIntervalEXT");
    if (si) si(g_swap_interval);
    klog("swap interval %d\n", g_swap_interval);
}

static int create_window(int w, int h, int gl)
{
    WNDCLASSA wc = { 0 };
    RECT r = { 0, 0, w, h };
    PIXELFORMATDESCRIPTOR pfd = { sizeof(pfd), 1, PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER,
                                  PFD_TYPE_RGBA, 32, 0, 0, 0, 0, 0, 0, 8, 0, 0, 0, 0, 0, 0, 24, 8, 0,
                                  PFD_MAIN_PLANE, 0, 0, 0, 0 };
    if (g_wnd) return 0;
    wc.lpfnWndProc = wndproc;
    wc.hInstance = GetModuleHandleA(NULL);
    wc.lpszClassName = "jp2015";
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    RegisterClassA(&wc);
    AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
    g_wnd = CreateWindowA("jp2015", "Jurassic Park (recompiled)", WS_OVERLAPPEDWINDOW | WS_VISIBLE,
                          CW_USEDEFAULT, CW_USEDEFAULT, r.right - r.left, r.bottom - r.top,
                          NULL, NULL, wc.hInstance, NULL);
    if (!g_wnd) return -1;
    g_dc = GetDC(g_wnd);
    if (gl) {
        int pf = ChoosePixelFormat(g_dc, &pfd);
        SetPixelFormat(g_dc, pf, &pfd);
        g_rc = wglCreateContext(g_dc);
        if (!g_rc || !wglMakeCurrent(g_dc, g_rc)) return -1;
        if (getenv("JPRT_GLES") && atoi(getenv("JPRT_GLES"))) {
            /* OpenGL ES 3.2 profile context (WGL_EXT_create_context_es2_profile):
             * the Quest's API, exercised on the PC */
            HGLRC (WINAPI *cca)(HDC, HGLRC, const int *) = (void *)wglGetProcAddress("wglCreateContextAttribsARB");
            const int attr[] = { 0x2091, 3, 0x2092, 2, 0x9126, 0x4, 0 };
            HGLRC es = cca ? cca(g_dc, NULL, attr) : NULL;
            if (!es) kfatal(NULL, "JPRT_GLES: no OpenGL ES 3.2 context on this host");
            wglMakeCurrent(NULL, NULL);
            wglDeleteContext(g_rc);
            g_rc = es;
            if (!wglMakeCurrent(g_dc, g_rc)) return -1;
            g_gles = 1;
        }
        apply_swap_interval();
        klog("GL: %s / %s / %s\n", (const char *)glGetString(GL_VENDOR),
             (const char *)glGetString(GL_RENDERER), (const char *)glGetString(GL_VERSION));
    }
    pump();
    return 0;
}

/* ---- GL host pieces ---- */
static void *g_procs[4096];
static HMODULE g_ogl;

void *gl_proc(int idx, const char *name)
{
    void *p = idx < 4096 ? g_procs[idx] : NULL;
    if (p) return p;
    p = (void *)wglGetProcAddress(name);
    if ((uintptr_t)p <= 3 || (intptr_t)p == -1) {
        if (!g_ogl) g_ogl = LoadLibraryA("opengl32.dll");
        p = (void *)GetProcAddress(g_ogl, name);
    }
    if (!p) kfatal(NULL, "GL: %s not available on this host", name);
    if (idx < 4096) g_procs[idx] = p;
    return p;
}

static uint32_t g_bind[4];            /* ARRAY ELEMENT PACK UNPACK */
int gl_bound_ARRAY(void) { return g_bind[0] != 0; }
int gl_bound_ELEMENT(void) { return g_bind[1] != 0; }
int gl_bound_PACK(void) { return g_bind[2] != 0; }
int gl_bound_UNPACK(void) { return g_bind[3] != 0; }
uint32_t gl_bound_element_buffer(void) { return g_bind[1]; }


/* guest-memory shadow for glMapBuffer, one per bind target */
static struct { uint32_t addr, cap, size; int write, mapped; } g_map[4];

static int gl_manual(rt_cpu *cpu, const char *name)
{
    if (!strcmp(name, "glBindBuffer") || !strcmp(name, "glBindBufferARB")) {
        uint32_t t = ARG(0), b = ARG(1);
        int k = t == 0x8892 ? 0 : t == 0x8893 ? 1 : t == 0x88eb ? 2 : t == 0x88ec ? 3 : -1;
        if (k >= 0) g_bind[k] = b;
        ((void (GLAPI_CALL *)(uint32_t, uint32_t))gl_proc(4000, "glBindBuffer"))(t, b);
        RET(0);
    }
    if (!strcmp(name, "glGetString") || !strcmp(name, "glGetStringi")) {
        static struct { uint32_t key; uint32_t g; } cache[64];
        static int n;
        uint32_t e = ARG(0), idx = name[11] == 'i' ? ARG(1) : 0xffffffffu, key = e * 4099u + idx;
        const char *s;
        int i;
        for (i = 0; i < n; i++) if (cache[i].key == key) RET(cache[i].g);
        s = idx == 0xffffffffu ? (const char *)glGetString(e)
                               : ((const char *(GLAPI_CALL *)(uint32_t, uint32_t))gl_proc(4001, "glGetStringi"))(e, idx);
        if (!s) RET(0);
        if (n < 64) { cache[n].key = key; cache[n].g = guest_strdup(s); RET(cache[n++].g); }
        RET(guest_strdup(s));
    }
    if (!strcmp(name, "glMapBuffer") || !strcmp(name, "glMapBufferARB")) {
        /* the guest gets a shadow in guest memory (one per bind target,
         * reused); glUnmapBuffer uploads it */
        uint32_t target = ARG(0), access = ARG(1);
        int32_t size = 0;
        int k = target == 0x8892 ? 0 : target == 0x8893 ? 1 : target == 0x88eb ? 2 : target == 0x88ec ? 3 : -1;
        if (k < 0) RET(0);
        ((void (GLAPI_CALL *)(uint32_t, uint32_t, int32_t *))gl_proc(4003, "glGetBufferParameteriv"))
            (target, 0x8764 /* GL_BUFFER_SIZE */, &size);
        if (size <= 0) RET(0);
        if ((uint32_t)size > g_map[k].cap) {
            uint32_t cap = ((uint32_t)size + 0xffff) & ~0xffffu, a;
            if (g_map[k].cap) mem_unmap(g_map[k].addr, g_map[k].cap);
            a = mem_find(cap, 0xb0000000u);
            if (!a || mem_map(a, cap) < 0) RET(0);
            g_map[k].addr = a;
            g_map[k].cap = cap;
        }
        g_map[k].size = (uint32_t)size;
        g_map[k].write = access != 0x88B8;            /* not READ_ONLY */
        g_map[k].mapped = 1;
        if (access != 0x88B9 && !g_gles)              /* not WRITE_ONLY: current contents */
            ((void (GLAPI_CALL *)(uint32_t, intptr_t, intptr_t, void *))gl_proc(4004, "glGetBufferSubData"))
                (target, 0, size, RT_P(g_map[k].addr));
        else if (access != 0x88B9) {                  /* ES: no glGetBufferSubData */
            void *m = ((void *(GLAPI_CALL *)(uint32_t, intptr_t, intptr_t, uint32_t))gl_proc(4006, "glMapBufferRange"))
                (target, 0, size, 0x1 /* MAP_READ_BIT */);
            if (m) memcpy(RT_P(g_map[k].addr), m, (size_t)size);
            ((uint8_t (GLAPI_CALL *)(uint32_t))gl_proc(4007, "glUnmapBuffer"))(target);
        }
        RET(g_map[k].addr);
    }
    if (!strcmp(name, "glUnmapBuffer") || !strcmp(name, "glUnmapBufferARB")) {
        uint32_t target = ARG(0);
        int k = target == 0x8892 ? 0 : target == 0x8893 ? 1 : target == 0x88eb ? 2 : target == 0x88ec ? 3 : -1;
        if (k >= 0 && g_map[k].mapped) {
            if (g_map[k].write)
                ((void (GLAPI_CALL *)(uint32_t, intptr_t, intptr_t, const void *))gl_proc(4005, "glBufferSubData"))
                    (target, 0, g_map[k].size, RT_P(g_map[k].addr));
            g_map[k].mapped = 0;
        }
        RET(1);
    }
    if (!strcmp(name, "glShaderSource") || !strcmp(name, "glShaderSourceARB")) {
        uint32_t sh = ARG(0), cnt = ARG(1), strs = ARG(2), lens = ARG(3), i;
        const char **hs = malloc(sizeof(char *) * (cnt ? cnt : 1));
        for (i = 0; i < cnt; i++) hs[i] = gstr(rd32(strs + 4 * i));
        if (getenv("JPRT_SHADER_DUMP")) {   /* dir: one file per glShaderSource */
            static int k;
            char path[512];
            FILE *f;
            snprintf(path, sizeof(path), "%s/sh_%04d_%u.glsl", getenv("JPRT_SHADER_DUMP"), k++, sh);
            if ((f = fopen(path, "wb"))) {
                for (i = 0; i < cnt; i++) {
                    int32_t l = lens ? (int32_t)rd32(lens + 4 * i) : -1;
                    fwrite(hs[i], 1, l >= 0 ? (size_t)l : strlen(hs[i]), f);
                }
                fclose(f);
            }
        }
        ((void (GLAPI_CALL *)(uint32_t, uint32_t, const char **, const int32_t *))gl_proc(4002, "glShaderSource"))
            (sh, cnt, hs, lens ? (const int32_t *)RT_P(lens) : NULL);
        free(hs);
        RET(0);
    }
    return 0;
}

/* JPRT_GLTRACE=1: log every distinct GL entry point on first use (the
 * surface a GLES backend has to cover) */
static void gl_trace(const char *name)
{
    static int on = -1, n;
    static const char *seen[1024];
    int i;
    if (on < 0) on = getenv("JPRT_GLTRACE") != NULL;
    if (!on) return;
    for (i = 0; i < n; i++) if (seen[i] == name || !strcmp(seen[i], name)) return;
    if (n < 1024) seen[n++] = name;
    klog("gltrace: %s\n", name);
}

void gl_census(rt_cpu *cpu, const char *n);      /* gles.c */
int gles_manual(rt_cpu *cpu, const char *n);

static int gl_dispatch_inner(rt_cpu *cpu, const char *name)
{
    int lo, hi;
    gl_trace(name);
    gl_census(cpu, name);
    if (gles_manual(cpu, name)) return 1;
    if (gl_manual(cpu, name)) return 1;
    {
        static const struct gl_thunk **sorted;
        static int ns;
        if (!sorted) {
            int i, j;
            sorted = malloc(sizeof(*sorted) * gl_thunk_count);
            for (i = 0; i < gl_thunk_count; i++) sorted[i] = &gl_thunks[i];
            for (i = 1; i < gl_thunk_count; i++) {           /* insertion sort, once */
                const struct gl_thunk *t = sorted[i];
                for (j = i - 1; j >= 0 && strcmp(sorted[j]->name, t->name) > 0; j--) sorted[j + 1] = sorted[j];
                sorted[j + 1] = t;
            }
            ns = gl_thunk_count;
        }
        lo = 0; hi = ns;
        while (lo < hi) {
            int mid = (lo + hi) / 2, c = strcmp(sorted[mid]->name, name);
            if (!c) return sorted[mid]->fn(cpu);
            if (c < 0) lo = mid + 1; else hi = mid;
        }
    }
    return 0;
}

/* JPRT_GLCHECK=1: glGetError after every call, first error per entry point
 * logged (the game's own glGetError calls then see GL_NO_ERROR) */
static int gl_dispatch(rt_cpu *cpu, const char *name)
{
    static int check = -1;
    uint32_t a[5], k, ra = rd32(cpu->esp);
    int r;
    for (k = 0; k < 5; k++) a[k] = rd32(cpu->esp + 4 + 4 * k);
    r = gl_dispatch_inner(cpu, name);
    if (check < 0) check = getenv("JPRT_GLCHECK") != NULL;
    if (check && r && strcmp(name, "glGetError")) {
        static struct { const char *n; int c; } seen[256];
        static int ns;
        uint32_t e = glGetError();
        int i;
        if (e) {
            for (i = 0; i < ns && strcmp(seen[i].n, name); i++) {}
            if (i == ns && ns < 256) seen[ns++].n = name;
            if (i < 256 && seen[i].c++ < 4) {
                int32_t dfb = 0, rfb = 0;
                glGetIntegerv(0x8CA6, &dfb);
                glGetIntegerv(0x8CAA, &rfb);
                klog("glcheck: %s(%#x, %#x, %#x, %#x, %#x) -> error %#x [from %08x, draw fb %d, read fb %d]\n",
                     name, a[0], a[1], a[2], a[3], a[4], e, ra, dfb, rfb);
                if (!strcmp(name, "glDrawBuffers") && a[1])
                    for (k = 0; k < a[0] && k < 8; k++) klog("glcheck:   bufs[%u] = %#x\n", k, rd32(a[1] + 4 * k));
            }
        }
    }
    return r;
}

/* ---- SDL 1.2 ---- */
int sdl_audio(rt_cpu *cpu, const char *n);      /* hostaudio.c */
void audio_pump(rt_cpu *cpu);
void game_select_input_maps(void);         /* hostgame.c */

static uint32_t g_sdl_inited;

static int sdl(rt_cpu *cpu, const char *n)
{
    if (sdl_audio(cpu, n)) return 1;
    if (!strcmp(n, "SDL_Init")) {
        QueryPerformanceFrequency(&g_freq);
        QueryPerformanceCounter(&g_t0);
        klog("SDL_Init(%#x)\n", ARG(0));
        g_sdl_inited |= ARG(0) & 0xffffu;
        RET(0);
    }
    if (!strcmp(n, "SDL_InitSubSystem")) { g_sdl_inited |= ARG(0) & 0xffffu; RET(0); }
    /* subsystems only: io_sdl_init asks WasInit(SDL_INIT_NOPARACHUTE) and
     * enables the keyboard/mouse driver only when that is 0 */
    if (!strcmp(n, "SDL_WasInit")) RET(g_sdl_inited & ARG(0));
    if (!strcmp(n, "SDL_Quit")) RET(0);
    if (!strcmp(n, "SDL_GetTicks")) {
        LARGE_INTEGER t;
        if (!g_freq.QuadPart) { QueryPerformanceFrequency(&g_freq); QueryPerformanceCounter(&g_t0); }
        QueryPerformanceCounter(&t);
        RET((uint32_t)((t.QuadPart - g_t0.QuadPart) * 1000 / g_freq.QuadPart));
    }
    if (!strcmp(n, "SDL_Delay")) { Sleep(ARG(0)); pump(); audio_pump(cpu); RET(0); }
    if (!strcmp(n, "SDL_GetError") || !strcmp(n, "SDL_Error")) {
        if (!g_errstr) g_errstr = guest_strdup("unsupported in the recompiled build");
        RET(g_errstr);
    }
    if (!strcmp(n, "SDL_SetError")) RET(0);
    if (!strcmp(n, "SDL_GL_SetAttribute")) RET(0);
    if (!strcmp(n, "SDL_SetVideoMode") || !strcmp(n, "SDL_GetVideoSurface")) {
        if (!strcmp(n, "SDL_SetVideoMode")) {
            uint32_t w = ARG(0), h = ARG(1), flags = ARG(3);
            klog("SDL_SetVideoMode(%u, %u, %u, %#x) on thread %lu\n", w, h, ARG(2), flags, GetCurrentThreadId());
            if (w) g_w = (int)w;
            if (h) g_h = (int)h;
            if (create_window(g_w, g_h, (flags & 2) != 0) < 0) RET(0);
        }
        if (!g_surface && g_wnd) {
            /* SDL_Surface (i386): flags, format*, w, h, pitch, pixels ... */
            g_surface = guest_alloc(64);
            memset(RT_P(g_surface), 0, 64);
            wr32(g_surface + 0, 0x2 | 0x40000000u);
            wr32(g_surface + 8, (uint32_t)g_w);
            wr32(g_surface + 12, (uint32_t)g_h);
            wr16(g_surface + 16, (uint16_t)(g_w * 4));
        }
        RET(g_surface);
    }
    if (!strcmp(n, "SDL_GetVideoInfo")) {
        static uint32_t vi;
        if (!vi) {
            /* SDL_VideoInfo: 2 words of bitfields, video_mem, vfmt*, current_w, current_h */
            vi = guest_alloc(32);
            memset(RT_P(vi), 0, 32);
            wr32(vi + 4, 1u << 9);
            wr32(vi + 8, 1 << 20);
            wr32(vi + 16, (uint32_t)GetSystemMetrics(SM_CXSCREEN));
            wr32(vi + 20, (uint32_t)GetSystemMetrics(SM_CYSCREEN));
        }
        RET(vi);
    }
    if (!strcmp(n, "SDL_GL_SwapBuffers") || !strcmp(n, "SDL_Flip")) {
        static unsigned frames;
        static int shot_every = -1, shots;
        if (shot_every < 0) shot_every = getenv("JPRT_SHOT_EVERY") ? atoi(getenv("JPRT_SHOT_EVERY")) : 0;
        if (shot_every > 0 && frames % (unsigned)shot_every == 0 && shots < 64 && g_rc) {
            /* the game's own back buffer -> BMP (never the screen) */
            int w = g_w, h = g_h, stride = (w * 3 + 3) & ~3, y;
            uint8_t *px = malloc((size_t)stride * h);
            char path[64];
            FILE *f;
            uint8_t *rgba = malloc((size_t)w * h * 4);
            int x;
            glPixelStorei(GL_PACK_ALIGNMENT, 4);
            glReadBuffer(GL_BACK);
            glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, rgba);   /* BGR is not in ES */
            for (y = 0; y < h; y++)
                for (x = 0; x < w; x++) {
                    const uint8_t *s = rgba + ((size_t)y * w + x) * 4;
                    uint8_t *d = px + (size_t)y * stride + (size_t)x * 3;
                    d[0] = s[2]; d[1] = s[1]; d[2] = s[0];
                }
            free(rgba);
            snprintf(path, sizeof(path), "frame_%06u.bmp", frames);
            if ((f = fopen(path, "wb"))) {
                uint8_t hd[54] = { 'B', 'M' };
                uint32_t sz = 54 + (uint32_t)stride * h;
                memcpy(hd + 2, &sz, 4); hd[10] = 54; hd[14] = 40;
                memcpy(hd + 18, &w, 4); memcpy(hd + 22, &h, 4); hd[26] = 1; hd[28] = 24;
                fwrite(hd, 1, 54, f);
                for (y = 0; y < h; y++) fwrite(px + (size_t)y * stride, 1, stride, f);
                fclose(f);
                shots++;
                klog("shot %s\n", path);
            }
            free(px);
        }
        if (frames < 3)
            klog("swap on thread %lu (GL context current: %p)\n", GetCurrentThreadId(), (void *)wglGetCurrentContext());
        if (g_dc) SwapBuffers(g_dc);
        {   /* frame pacing: the cabinet ran at the display rate (60 Hz);
             * drivers may ignore the swap interval, so cap here too */
            static double next;
            static int cap = -1;
            LARGE_INTEGER t, fq;
            double now;
            if (cap < 0) {
                cap = getenv("JPRT_FPSCAP") ? atoi(getenv("JPRT_FPSCAP")) : 60;
                timeBeginPeriod(1);
            }
            if (cap > 0) {
                QueryPerformanceCounter(&t);
                QueryPerformanceFrequency(&fq);
                now = (double)t.QuadPart / (double)fq.QuadPart;
                if (next == 0 || now > next + 0.25) next = now;
                next += 1.0 / cap;
                while (now < next) {
                    double left = next - now;
                    if (left > 0.002) Sleep((DWORD)((left - 0.001) * 1000));
                    QueryPerformanceCounter(&t);
                    now = (double)t.QuadPart / (double)fq.QuadPart;
                }
            }
        }
        pump();
        audio_pump(cpu);
        scripted_keys(frames);
        if ((++frames % 600) == 1) klog("frame %u\n", frames);
        RET(0);
    }
    if (!strcmp(n, "SDL_PollEvent")) {
        uint32_t ev = ARG(0);
        int have;
        pump();
        audio_pump(cpu);
        AcquireSRWLockExclusive(&g_evlock);
        have = g_evh != g_evt;
        if (have && ev) { memcpy(RT_P(ev), g_evq[g_evh], 20); g_evh = (g_evh + 1) % QN; }
        ReleaseSRWLockExclusive(&g_evlock);
        RET(have);
    }
    if (!strcmp(n, "SDL_PushEvent")) { push_event(RT_P(ARG(0))); RET(0); }
    if (!strcmp(n, "SDL_ShowCursor")) { game_select_input_maps(); RET(0); }
    if (!strcmp(n, "SDL_WM_SetCaption")) {
        if (g_wnd && ARG(0)) SetWindowTextA(g_wnd, gstr(ARG(0)));
        RET(0);
    }
    if (!strcmp(n, "SDL_EnableKeyRepeat")) RET(0);
    if (!strcmp(n, "SDL_GetMouseState")) {
        if (ARG(0)) wr32(ARG(0), 0);
        if (ARG(1)) wr32(ARG(1), 0);
        RET(0);
    }
    if (!strcmp(n, "SDL_NumJoysticks")) RET(0);
    if (!strcmp(n, "SDL_JoystickOpen")) RET(0);
    if (!strcmp(n, "SDL_JoystickEventState")) RET(0);
    if (!strcmp(n, "SDL_LoadObject")) {
        const char *lib = gstr(ARG(0));
        klog("SDL_LoadObject(%s): host replacement\n", lib ? lib : "(null)");
        RET(0xf0000f00u);
    }
    if (!strcmp(n, "SDL_LoadFunction")) RET(host_thunk_addr(gstr(ARG(1))));
    if (!strcmp(n, "SDL_UnloadObject")) RET(0);
    if (!strcmp(n, "SDL_FreeSurface")) RET(0);
    return 0;
}

/* ---- entry from host_call ---- */
int host_impl(rt_cpu *cpu, const char *name)
{
    if (!strncmp(name, "SDL_", 4)) return sdl(cpu, name);
    if (!strcmp(name, "glXSwapIntervalSGI") || !strcmp(name, "glXSwapIntervalMESA")) {
        /* the cabinet ran with vsync; JPRT_VSYNC=0 to run uncapped */
        g_swap_interval = getenv("JPRT_VSYNC") ? atoi(getenv("JPRT_VSYNC")) : (int)ARG(0);
        apply_swap_interval();
        RET(0);
    }
    if (!strcmp(name, "glXGetProcAddressARB") || !strcmp(name, "glXGetProcAddress"))
        RET(host_thunk_addr(gstr(ARG(0))));
    if (!strncmp(name, "gl", 2) && name[2] >= 'A' && name[2] <= 'Z') return gl_dispatch(cpu, name);
    if (!strcmp(name, "gluErrorString")) RET(guest_strdup("GLU error"));
    if (!strcmp(name, "gluBuild2DMipmaps")) {
        static HMODULE glu;
        int (GLAPI_CALL *f)(uint32_t, int32_t, int32_t, int32_t, uint32_t, uint32_t, const void *);
        if (!glu) glu = LoadLibraryA("glu32.dll");
        f = (void *)GetProcAddress(glu, "gluBuild2DMipmaps");
        RET(f(ARG(0), (int32_t)ARG(1), (int32_t)ARG(2), (int32_t)ARG(3), ARG(4), ARG(5), GUEST_PTR(ARG(6))));
    }
    if (!strcmp(name, "glutBitmapCharacter")) RET(0);
    if (!strncmp(name, "udev_", 5)) RET(0);       /* no udev: gun/IO discovery finds nothing */
    if (!strcmp(name, "XOpenDisplay")) RET(0);
    return 0;
}

/* video_win.c — video.h on Windows: Win32 window, WGL context (desktop GL,
 * or OpenGL ES 3.2 with JPRT_GLES=1), keyboard/mouse as SDL 1.2 events. */
#if defined(_WIN32)
#include <windows.h>
#include <GL/gl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "rt.h"
#include "kernel.h"
#include "video.h"

extern int g_gles;                      /* gles.c */
static HWND g_wnd;
static HDC g_dc;
static HGLRC g_rc;
static int g_swap_interval = 1;

static uint32_t sdl_key(WPARAM vk)
{
    if (vk >= 'A' && vk <= 'Z') return (uint32_t)(vk - 'A' + 'a');
    if (vk >= '0' && vk <= '9') return (uint32_t)vk;
    switch (vk) {
    case VK_ESCAPE: return 27; case VK_RETURN: return 13; case VK_SPACE: return 32;
    case VK_BACK: return 8; case VK_TAB: return 9;
    case VK_OEM_COMMA: return ','; case VK_OEM_PERIOD: return '.';
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
        host_push_event(ev);
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
        host_push_event(ev);
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
        host_push_event(ev);
        return 0;
    }
    }
    return DefWindowProcA(h, m, w, l);
}

void vid_pump(void)
{
    MSG msg;
    while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE)) {
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
    }
}

void vid_swap_interval(int n)
{
    BOOL (WINAPI *si)(int);
    g_swap_interval = n;
    if (!g_rc) return;                         /* applied once the context exists */
    si = (void *)wglGetProcAddress("wglSwapIntervalEXT");
    if (si) si(g_swap_interval);
    klog("swap interval %d\n", g_swap_interval);
}

int vid_create(int w, int h)
{
    WNDCLASSA wc = { 0 };
    RECT r = { 0, 0, w, h };
    PIXELFORMATDESCRIPTOR pfd = { sizeof(pfd), 1, PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER,
                                  PFD_TYPE_RGBA, 32, 0, 0, 0, 0, 0, 0, 8, 0, 0, 0, 0, 0, 0, 24, 8, 0,
                                  PFD_MAIN_PLANE, 0, 0, 0, 0 };
    int pf;
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
    pf = ChoosePixelFormat(g_dc, &pfd);
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
    vid_swap_interval(g_swap_interval);
    vid_log_context();
    vid_pump();
    return 0;
}

void vid_log_context(void)
{
    klog("GL: %s / %s / %s (thread %lu)\n", (const char *)glGetString(GL_VENDOR),
         (const char *)glGetString(GL_RENDERER), (const char *)glGetString(GL_VERSION), GetCurrentThreadId());
}

int vid_ready(void) { return g_rc != NULL; }
void vid_present(void) { if (g_dc) SwapBuffers(g_dc); }
uint32_t vid_default_fbo(void) { return 0; }

void *vid_gl_proc(const char *name)
{
    static HMODULE ogl;
    void *p = (void *)wglGetProcAddress(name);
    if ((uintptr_t)p <= 3 || (intptr_t)p == -1) {
        if (!ogl) ogl = LoadLibraryA("opengl32.dll");
        p = (void *)GetProcAddress(ogl, name);
    }
    return p;
}

void vid_set_title(const char *t) { if (g_wnd && t) SetWindowTextA(g_wnd, t); }

void vid_screen_size(int *w, int *h)
{
    *w = GetSystemMetrics(SM_CXSCREEN);
    *h = GetSystemMetrics(SM_CYSCREEN);
}

/* stereo: no head tracking on the desktop */
void vid_head_orientation(float q[4]) { q[0] = q[1] = q[2] = 0.0f; q[3] = 1.0f; }
#endif

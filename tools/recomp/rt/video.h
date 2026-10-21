/* video.h — the window / GL context / input backend under hostlibs.c's SDL:
 * video_win.c (Win32 + WGL, desktop GL or ES 3.2) and video_android.c
 * (EGL + OpenXR on the Quest). */
#ifndef VIDEO_H
#define VIDEO_H
#include <stdint.h>

/* create the window and a GL context current on the calling thread; sets
 * g_gles when the context is OpenGL ES. 0 on success. */
int vid_create(int w, int h);
int vid_ready(void);
void vid_present(void);                 /* end of a game frame (SDL_GL_SwapBuffers) */
void vid_pump(void);                    /* OS events -> host_push_event */
void *vid_gl_proc(const char *name);    /* NULL if unavailable */
void vid_set_title(const char *title);
void vid_swap_interval(int n);
void vid_screen_size(int *w, int *h);
/* the framebuffer the game's "default framebuffer" (0) maps to: 0 on a
 * window, the eye/cinema render target under OpenXR */
uint32_t vid_default_fbo(void);
void vid_log_context(void);

/* stereo (hostgame.c, JPRT_STEREO): the engine renders both eyes side by
 * side into the default framebuffer, each eye w x h */
int stereo_enabled(void);
void stereo_eye_size(int *w, int *h);
void vid_eye_size(int *w, int *h);      /* one eye's image */
float vid_eye_half_yfov(void);          /* radians, symmetric; the width follows the eye aspect */
void vid_head_orientation(float q[4]);  /* x y z w, the engine's camera frame */

/* hostlibs.c: queue a guest SDL_Event (20 bytes) */
void host_push_event(const uint8_t *ev);

#endif

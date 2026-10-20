/* gl_host.h — host side of the GL thunks (gen_gl.py output + hostlibs.c). */
#ifndef GL_HOST_H
#define GL_HOST_H
#include <stdint.h>
#include "rt.h"

#if defined(_WIN32)
#define GLAPI_CALL __stdcall
#else
#define GLAPI_CALL
#endif

/* guest pointer -> host pointer (0 stays NULL) */
#define GUEST_PTR(a) ((a) ? (void *)RT_P(a) : (void *)0)

struct gl_thunk { const char *name; int (*fn)(rt_cpu *); };
extern const struct gl_thunk gl_thunks[];
extern const int gl_thunk_count;

/* host entry point for GL command #idx (resolved and cached on first use) */
void *gl_proc(int idx, const char *name);

/* buffer bindings that turn pointer arguments into offsets */
int gl_bound_ARRAY(void);
int gl_bound_ELEMENT(void);
int gl_bound_UNPACK(void);
int gl_bound_PACK(void);
#endif

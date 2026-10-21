/* glsl_es.h — desktop GLSL 1.50 (game shaders) -> GLSL ES 3.20 */
#ifndef GLSL_ES_H
#define GLSL_ES_H

/* fragment output locations recorded from glBindFragDataLocation */
typedef struct {
    int n;
    char name[8][64];
    int loc[8];
} glsl_es_binds;

/* GLSL ES version emitted (320 default; 310/300 on lesser contexts) */
extern int glsl_es_version;

/* returns a malloc'ed ES 3.x source; binds may be NULL */
char *glsl_es_translate(const char *src, int frag, const glsl_es_binds *binds);

#endif

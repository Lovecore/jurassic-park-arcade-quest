/*
 * glsl_es.c — the game's desktop GLSL (#version 150 core, written for the
 * NVIDIA compatibility profile) -> GLSL ES 3.20 for the GLES backend.
 *
 * The engine's shaders select features with simple preprocessor
 * conditionals (#if (USE_X==1), #ifdef). Inactive branches still mention
 * compatibility built-ins (gl_LightSource, attribute/varying, ...), and the
 * ES compiler rejects them even unused only after preprocessing, so the
 * conditionals are evaluated here first (object-like macros; #define lines
 * are kept for the driver, function-like ones included). The active text
 * is then rewritten:
 *   - #version 320 es + default precisions
 *   - texture2D / texture2DLod / textureCube / texture2DProj -> texture*
 *   - attribute / varying -> in / out
 *   - gl_FragData[N] / gl_FragColor -> explicit location outputs
 *   - fragment `out` declarations get layout(location) from the program's
 *     glBindFragDataLocation calls (ES has no such call)
 */
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "glsl_es.h"

/* ---- growable string ---- */
typedef struct { char *p; size_t n, cap; } sbuf;

static void sb_put(sbuf *b, const char *s, size_t n)
{
    if (b->n + n + 1 > b->cap) {
        b->cap = (b->n + n + 1) * 2;
        b->p = realloc(b->p, b->cap);
    }
    memcpy(b->p + b->n, s, n);
    b->n += n;
    b->p[b->n] = 0;
}
static void sb_puts(sbuf *b, const char *s) { sb_put(b, s, strlen(s)); }

/* ---- object-like macros for #if evaluation ---- */
#define NMAC 512
typedef struct { char name[64]; long val; int has_val; } macro;
typedef struct { macro m[NMAC]; int n; } macros;

static macro *mac_find(macros *ms, const char *name, size_t len)
{
    int i;
    for (i = 0; i < ms->n; i++)
        if (strlen(ms->m[i].name) == len && !strncmp(ms->m[i].name, name, len)) return &ms->m[i];
    return NULL;
}

static void mac_define(macros *ms, const char *line)
{
    const char *p = line, *nm;
    size_t len;
    macro *m;
    while (isspace((unsigned char)*p)) p++;
    nm = p;
    while (isalnum((unsigned char)*p) || *p == '_') p++;
    len = (size_t)(p - nm);
    if (!len || len >= sizeof(ms->m[0].name)) return;
    m = mac_find(ms, nm, len);
    if (!m) {
        if (ms->n == NMAC) return;
        m = &ms->m[ms->n++];
        memcpy(m->name, nm, len);
        m->name[len] = 0;
    }
    m->has_val = 0;
    m->val = 0;
    if (*p == '(') return;                         /* function-like: defined, no value */
    while (*p == ' ' || *p == '\t') p++;
    if (*p == '(' || *p == '-' || isdigit((unsigned char)*p)) {
        char *e;
        const char *q = p;
        int neg = 0;
        while (*q == '(') q++;
        if (*q == '-') { neg = 1; q++; }
        m->val = strtol(q, &e, 0);
        if (neg) m->val = -m->val;
        if (e != q) m->has_val = 1;
    } else if (!*p || *p == '\n' || *p == '\r' || (*p == '/' && p[1] == '/'))
        m->has_val = 1;                            /* empty: 0 in expressions, still defined */
}

static void mac_undef(macros *ms, const char *line)
{
    const char *p = line, *nm;
    macro *m;
    while (isspace((unsigned char)*p)) p++;
    nm = p;
    while (isalnum((unsigned char)*p) || *p == '_') p++;
    m = mac_find(ms, nm, (size_t)(p - nm));
    if (m) { *m = ms->m[--ms->n]; }
}

/* ---- #if expression evaluator (recursive descent) ---- */
typedef struct { const char *p; macros *ms; } ex;

static void ex_ws(ex *e) { while (*e->p == ' ' || *e->p == '\t') e->p++; }
static long ex_or(ex *e);

static long ex_prim(ex *e)
{
    ex_ws(e);
    if (*e->p == '(') {
        long v;
        e->p++;
        v = ex_or(e);
        ex_ws(e);
        if (*e->p == ')') e->p++;
        return v;
    }
    if (*e->p == '!') { e->p++; return !ex_prim(e); }
    if (*e->p == '-') { e->p++; return -ex_prim(e); }
    if (isdigit((unsigned char)*e->p)) { char *end; long v = strtol(e->p, &end, 0); e->p = end; while (isalpha((unsigned char)*e->p)) e->p++; return v; }
    if (isalpha((unsigned char)*e->p) || *e->p == '_') {
        const char *nm = e->p;
        size_t len;
        macro *m;
        while (isalnum((unsigned char)*e->p) || *e->p == '_') e->p++;
        len = (size_t)(e->p - nm);
        if (len == 7 && !strncmp(nm, "defined", 7)) {
            int paren;
            ex_ws(e);
            paren = *e->p == '(';
            if (paren) e->p++;
            ex_ws(e);
            nm = e->p;
            while (isalnum((unsigned char)*e->p) || *e->p == '_') e->p++;
            m = mac_find(e->ms, nm, (size_t)(e->p - nm));
            ex_ws(e);
            if (paren && *e->p == ')') e->p++;
            return m != NULL;
        }
        m = mac_find(e->ms, nm, len);
        return m ? m->val : 0;
    }
    return 0;
}

static long ex_mul(ex *e)
{
    long v = ex_prim(e);
    for (;;) {
        ex_ws(e);
        if (*e->p == '*') { e->p++; v *= ex_prim(e); }
        else if (*e->p == '/') { long d; e->p++; d = ex_prim(e); v = d ? v / d : 0; }
        else if (*e->p == '%') { long d; e->p++; d = ex_prim(e); v = d ? v % d : 0; }
        else return v;
    }
}
static long ex_add(ex *e)
{
    long v = ex_mul(e);
    for (;;) {
        ex_ws(e);
        if (*e->p == '+') { e->p++; v += ex_mul(e); }
        else if (*e->p == '-') { e->p++; v -= ex_mul(e); }
        else return v;
    }
}
static long ex_rel(ex *e)
{
    long v = ex_add(e);
    for (;;) {
        ex_ws(e);
        if (e->p[0] == '<' && e->p[1] == '=') { e->p += 2; v = v <= ex_add(e); }
        else if (e->p[0] == '>' && e->p[1] == '=') { e->p += 2; v = v >= ex_add(e); }
        else if (e->p[0] == '<') { e->p++; v = v < ex_add(e); }
        else if (e->p[0] == '>') { e->p++; v = v > ex_add(e); }
        else return v;
    }
}
static long ex_eq(ex *e)
{
    long v = ex_rel(e);
    for (;;) {
        ex_ws(e);
        if (e->p[0] == '=' && e->p[1] == '=') { e->p += 2; v = v == ex_rel(e); }
        else if (e->p[0] == '!' && e->p[1] == '=') { e->p += 2; v = v != ex_rel(e); }
        else return v;
    }
}
static long ex_and(ex *e)
{
    long v = ex_eq(e);
    for (;;) {
        ex_ws(e);
        if (e->p[0] == '&' && e->p[1] == '&') { long r; e->p += 2; r = ex_eq(e); v = v && r; }
        else return v;
    }
}
static long ex_or(ex *e)
{
    long v = ex_and(e);
    for (;;) {
        ex_ws(e);
        if (e->p[0] == '|' && e->p[1] == '|') { long r; e->p += 2; r = ex_and(e); v = v || r; }
        else return v;
    }
}

/* ---- pass 1: keep the active lines ---- */
static char *preprocess(const char *src)
{
    macros *ms = calloc(1, sizeof(*ms));
    sbuf out = { 0 };
    /* stack: active = this level emits; taken = some branch already taken */
    int act[64], taken[64], sp = 0;
    const char *p = src;
    act[0] = 1;
    taken[0] = 1;
    while (*p) {
        const char *eol = strchr(p, '\n'), *q = p;
        size_t len = eol ? (size_t)(eol - p + 1) : strlen(p);
        char dir[16] = { 0 };
        int i = 0, on = act[sp];
        while (*q == ' ' || *q == '\t') q++;
        if (*q == '#') {
            q++;
            while (*q == ' ' || *q == '\t') q++;
            while (isalpha((unsigned char)*q) && i < 15) dir[i++] = *q++;
            if (!strcmp(dir, "if") || !strcmp(dir, "ifdef") || !strcmp(dir, "ifndef")) {
                long v = 0;
                if (on) {
                    if (dir[2] == 0) { ex e = { q, ms }; v = ex_or(&e); }
                    else {
                        const char *nm;
                        while (*q == ' ' || *q == '\t') q++;
                        nm = q;
                        while (isalnum((unsigned char)*q) || *q == '_') q++;
                        v = mac_find(ms, nm, (size_t)(q - nm)) != NULL;
                        if (dir[2] == 'n') v = !v;
                    }
                }
                if (sp < 63) { sp++; act[sp] = on && v; taken[sp] = !on || v; }
            } else if (!strcmp(dir, "elif")) {
                int parent = sp > 0 ? act[sp - 1] : 1;
                if (parent && !taken[sp]) { ex e = { q, ms }; act[sp] = ex_or(&e) != 0; taken[sp] = act[sp]; }
                else act[sp] = 0;
            } else if (!strcmp(dir, "else")) {
                int parent = sp > 0 ? act[sp - 1] : 1;
                act[sp] = parent && !taken[sp];
                taken[sp] = 1;
            } else if (!strcmp(dir, "endif")) {
                if (sp > 0) sp--;
            } else if (on) {
                if (!strcmp(dir, "define")) mac_define(ms, q);
                else if (!strcmp(dir, "undef")) mac_undef(ms, q);
                sb_put(&out, p, len);              /* #define/#version/#extension/... */
                p += len;
                continue;
            }
            sb_puts(&out, "\n");                   /* keep line numbers for driver messages */
        } else if (on)
            sb_put(&out, p, len);
        else
            sb_puts(&out, "\n");
        p += len;
    }
    free(ms);
    if (!out.p) sb_puts(&out, "");
    return out.p;
}


#include "glsl_es_rewrite.inc"

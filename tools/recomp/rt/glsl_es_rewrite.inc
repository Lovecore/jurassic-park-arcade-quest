/* glsl_es_rewrite.inc — pass 2 of glsl_es.c (included at its end):
 * tokenize the active text and rewrite it as GLSL ES 3.20. */

enum { T_ID, T_NUM, T_OP, T_WS, T_DIR };     /* T_WS: spaces/newlines/comments; T_DIR: a # line */
typedef struct { int k; const char *s; int n; } tok;

static int is_id(int c) { return isalnum(c) || c == '_'; }

static tok *tokenize(const char *p, int *count)
{
    int cap = 4096, n = 0;
    tok *t = malloc(sizeof(tok) * (size_t)cap);
    const char *base = p;
    while (*p) {
        const char *s = p;
        int k;
        if (n + 1 >= cap) t = realloc(t, sizeof(tok) * (size_t)(cap *= 2));
        if (*p == '#') {
            const char *q = p;
            while (q > base && (q[-1] == ' ' || q[-1] == '\t')) q--;
            if (q == base || q[-1] == '\n') {
                while (*p && *p != '\n') p++;
                k = T_DIR;
                goto done;
            }
        }
        if (p[0] == '/' && p[1] == '/') { while (*p && *p != '\n') p++; k = T_WS; }
        else if (p[0] == '/' && p[1] == '*') { const char *e = strstr(p + 2, "*/"); p = e ? e + 2 : p + strlen(p); k = T_WS; }
        else if (isspace((unsigned char)*p)) { while (isspace((unsigned char)*p)) p++; k = T_WS; }
        else if (isalpha((unsigned char)*p) || *p == '_') { while (is_id((unsigned char)*p)) p++; k = T_ID; }
        else if (isdigit((unsigned char)*p) || (*p == '.' && isdigit((unsigned char)p[1]))) {
            while (isalnum((unsigned char)*p) || *p == '.' ||
                   ((*p == '+' || *p == '-') && (p[-1] == 'e' || p[-1] == 'E') && isdigit((unsigned char)s[0])))
                p++;
            k = T_NUM;
        } else {
            static const char *const two[] = { "<=", ">=", "==", "!=", "&&", "||", "++", "--", "+=", "-=", "*=", "/=", NULL };
            int i;
            k = T_OP;
            for (i = 0; two[i]; i++) if (p[0] == two[i][0] && p[1] == two[i][1]) break;
            p += two[i] ? 2 : 1;
        }
    done:
        t[n].k = k; t[n].s = s; t[n].n = (int)(p - s);
        n++;
    }
    *count = n;
    return t;
}

static int tok_is(const tok *t, const char *s) { return (int)strlen(s) == t->n && !strncmp(t->s, s, (size_t)t->n); }
static int next_sig(const tok *t, int n, int i) { for (i++; i < n && t[i].k == T_WS; i++) {} return i; }
static int prev_sig(const tok *t, int i) { for (i--; i >= 0 && t[i].k == T_WS; i--) {} return i; }

/* ---- declared variable types (for int-literal promotion) ---- */
enum { TY_UNK, TY_FLT, TY_INT };
#define NSYM 4096
typedef struct { const char *s; int n, ty; } sym;

static int type_class(const tok *t)
{
    static const char *const fl[] = { "float", "vec2", "vec3", "vec4", "mat2", "mat3", "mat4", NULL };
    static const char *const in[] = { "int", "ivec2", "ivec3", "ivec4", "uint", "uvec2", "uvec3", "uvec4", "bool", NULL };
    int i;
    if (t->k != T_ID) return TY_UNK;
    for (i = 0; fl[i]; i++) if (tok_is(t, fl[i])) return TY_FLT;
    for (i = 0; in[i]; i++) if (tok_is(t, in[i])) return TY_INT;
    return TY_UNK;
}

static int collect_syms(const tok *t, int n, sym *ss)
{
    int i, ns = 0;
    for (i = 0; i < n; i++) {
        int ty = type_class(&t[i]), j;
        if (ty == TY_UNK) continue;
        j = next_sig(t, n, i);
        while (j < n && t[j].k == T_ID && ns < NSYM) {   /* type a, b[3], c = ... */
            ss[ns].s = t[j].s; ss[ns].n = t[j].n; ss[ns].ty = ty; ns++;
            j = next_sig(t, n, j);
            if (j < n && tok_is(&t[j], "[")) { while (j < n && !tok_is(&t[j], "]")) j++; j = next_sig(t, n, j); }
            if (j < n && tok_is(&t[j], ",")) j = next_sig(t, n, j);
            else break;
        }
    }
    return ns;
}

static int sym_type(const sym *ss, int ns, const tok *t)
{
    int i;
    for (i = ns - 1; i >= 0; i--)
        if (ss[i].n == t->n && !strncmp(ss[i].s, t->s, (size_t)t->n)) return ss[i].ty;
    return TY_UNK;
}

static int is_swizzle(const tok *t)
{
    int i;
    if (t->k != T_ID || t->n > 4) return 0;
    for (i = 0; i < t->n; i++) if (!strchr("xyzwrgbastpq", t->s[i])) return 0;
    return 1;
}

static int num_type(const tok *t)
{
    return memchr(t->s, '.', (size_t)t->n) || memchr(t->s, 'e', (size_t)t->n) ? TY_FLT : TY_UNK;
}

/* a parenthesized expression ending at `)` (index i) is float if any
 * operand inside it is: (x * 2.0) - 1 */
static int paren_type(const tok *t, int i, const sym *ss, int ns)
{
    int d = 0, j;
    for (j = i; j >= 0; j--) {
        if (tok_is(&t[j], ")")) d++;
        else if (tok_is(&t[j], "(") && --d == 0) break;
        else if (d == 1 && t[j].k == T_NUM && num_type(&t[j]) == TY_FLT) return TY_FLT;
        else if (d == 1 && t[j].k == T_ID && sym_type(ss, ns, &t[j]) == TY_FLT) return TY_FLT;
    }
    return TY_UNK;
}

/* type of the operand that ends at token i (walking left) */
static int operand_left(const tok *t, int i, const sym *ss, int ns)
{
    int j;
    if (i < 0) return TY_UNK;
    if (tok_is(&t[i], ")")) return paren_type(t, i, ss, ns);
    if (t[i].k == T_NUM) return num_type(&t[i]);
    if (t[i].k != T_ID) return TY_UNK;
    j = prev_sig(t, i);
    if (j >= 0 && tok_is(&t[j], ".")) {             /* a.xyz / a[i].xyz / member */
        int base = prev_sig(t, j);
        if (base >= 0 && tok_is(&t[base], "]")) {
            int d = 0;
            for (; base >= 0; base--) {
                if (tok_is(&t[base], "]")) d++;
                else if (tok_is(&t[base], "[") && --d == 0) break;
            }
            base = prev_sig(t, base);
        }
        if (is_swizzle(&t[i]) && base >= 0 && t[base].k == T_ID)
            return sym_type(ss, ns, &t[base]) == TY_INT ? TY_INT : TY_FLT;
        return TY_UNK;
    }
    return sym_type(ss, ns, &t[i]);
}

/* type of the operand that starts at token i (walking right) */
static int operand_right(const tok *t, int n, int i, const sym *ss, int ns)
{
    int j;
    if (i >= n) return TY_UNK;
    if (t[i].k == T_NUM) return num_type(&t[i]);
    if (t[i].k != T_ID) return TY_UNK;
    j = next_sig(t, n, i);
    if (j < n && tok_is(&t[j], "(")) return type_class(&t[i]);   /* constructor float(...) */
    if (j < n && tok_is(&t[j], "[")) { while (j < n && !tok_is(&t[j], "]")) j++; j = next_sig(t, n, j); }
    if (j < n && tok_is(&t[j], ".")) {
        int m = next_sig(t, n, j);
        if (m < n && is_swizzle(&t[m])) return sym_type(ss, ns, &t[i]) == TY_INT ? TY_INT : TY_FLT;
        return TY_UNK;
    }
    return sym_type(ss, ns, &t[i]);
}

static int is_arith(const tok *t)
{
    static const char *const ops[] = { "<", ">", "<=", ">=", "==", "!=", "+", "-", "*", "/", "+=", "-=", "*=", "/=", "=", NULL };
    int i;
    if (t->k != T_OP) return 0;
    for (i = 0; ops[i]; i++) if (tok_is(t, ops[i])) return 1;
    return 0;
}

static int int_literal(const tok *t)
{
    int i;
    if (t->k != T_NUM) return 0;
    for (i = 0; i < t->n; i++) if (!isdigit((unsigned char)t->s[i])) return 0;
    return 1;
}

static const char *const RENAME[][2] = {
    { "texture2DLod", "textureLod" }, { "texture2DProjLod", "textureProjLod" },
    { "texture2DProj", "textureProj" }, { "texture2D", "texture" },
    { "textureCubeLod", "textureLod" }, { "textureCube", "texture" },
    { "texture3D", "texture" }, { "shadow2DProj", "textureProj" }, { "shadow2D", "texture" },
    /* identifiers reserved in GLSL ES 3.20 */
    { "sample", "jp_sample" }, { "filter", "jp_filter" }, { "input", "jp_input" },
    { "output", "jp_output" }, { "active", "jp_active" }, { "common", "jp_common" },
    { "partition", "jp_partition" }, { "patch", "jp_patch" }, { "precise", "jp_precise" },
    { "resource", "jp_resource" },
    { NULL, NULL }
};

static int out_loc(const glsl_es_binds *b, const char *name, int len)
{
    int i;
    for (i = 0; b && i < b->n; i++)
        if ((int)strlen(b->name[i]) == len && !strncmp(b->name[i], name, (size_t)len)) return b->loc[i];
    return -1;
}

static const char ES_HEADER[] =
    "#version 320 es\n"
    "precision highp float; precision highp int;\n"
    "precision highp sampler2D; precision highp sampler2DShadow;\n"
    "precision highp samplerCube; precision highp sampler3D;\n"
    "precision highp sampler2DArray;";

char *glsl_es_translate(const char *src, int frag, const glsl_es_binds *binds)
{
    char *pp = preprocess(src);
    sbuf out = { 0 };
    int n, ns, i, depth = 0, paren = 0, used, fragdata = 0, fragcolor = 0, fn_body = 0;
    tok *t = tokenize(pp, &n);
    sym *ss = malloc(sizeof(sym) * NSYM);
    ns = collect_syms(t, n, ss);
    /* gl_FragData slots / gl_FragColor written by the active code */
    for (i = 0; i < n; i++) {
        if (tok_is(&t[i], "gl_FragData")) {
            int j = next_sig(t, n, i);
            if (j < n && tok_is(&t[j], "[")) { j = next_sig(t, n, j); if (j < n) fragdata |= 1 << (atoi(t[j].s) & 7); }
        }
        if (tok_is(&t[i], "gl_FragColor")) fragcolor = 1;
    }
    used = fragdata | (fragcolor ? 1 : 0);
    for (i = 0; i < n; i++) {
        tok *k = &t[i];
        if (k->k == T_DIR) {
            const char *d = k->s + strspn(k->s, " \t#");
            if (!strncmp(d, "version", 7)) {
                int s;
                sb_puts(&out, ES_HEADER);
                if (frag) {
                    for (s = 0; s < 8; s++)
                        if (fragdata & (1 << s)) {
                            char b[96];
                            snprintf(b, sizeof(b), "%clayout(location=%d) out vec4 jp_FragData%d;", 10, s, s);
                            sb_puts(&out, b);
                        }
                    if (fragcolor) { char b[64]; snprintf(b, sizeof(b), "%clayout(location=0) out vec4 jp_FragColor;", 10); sb_puts(&out, b); }
                }
                continue;
            }
            sb_put(&out, k->s, (size_t)k->n);
            continue;
        }
        if (k->k == T_OP) {
            if (tok_is(k, "{")) {
                int j = prev_sig(t, i);
                if (depth++ == 0) fn_body = j >= 0 && tok_is(&t[j], ")");
            } else if (tok_is(k, "}")) depth--;
            else if (tok_is(k, "(")) paren++;
            else if (tok_is(k, ")")) paren--;
            else if (tok_is(k, ";") && depth == 0 && fn_body) {   /* `};` after a function: extraneous in ES */
                int j = prev_sig(t, i);
                if (j >= 0 && tok_is(&t[j], "}")) continue;
            }
            sb_put(&out, k->s, (size_t)k->n);
            continue;
        }
        if (k->k == T_NUM && int_literal(k)) {
            /* int literal beside a float operand: ES has no implicit conversion */
            int pj = prev_sig(t, i), nj = next_sig(t, n, i), promote = 0;
            if (pj >= 0 && is_arith(&t[pj]) && operand_left(t, prev_sig(t, pj), ss, ns) == TY_FLT) promote = 1;
            if (nj < n && is_arith(&t[nj]) && !tok_is(&t[nj], "=") &&
                operand_right(t, n, next_sig(t, n, nj), ss, ns) == TY_FLT) promote = 1;
            if (nj < n && (tok_is(&t[nj], ")") || tok_is(&t[nj], ",")) && pj >= 0 &&
                (tok_is(&t[pj], ",") || tok_is(&t[pj], "("))) {
                /* argument of a call: textureLod(.., 0) / clamp(v, 0, 1) with a float argument */
                static const char *const fl_fns[] = { "clamp", "min", "max", "mix", "pow", "step",
                                                      "smoothstep", "mod", "atan", "distance", NULL };
                int d = 0, j, open, f, fl_arg = 0;
                for (j = pj; j >= 0; j--) {
                    if (tok_is(&t[j], ")")) d++;
                    else if (tok_is(&t[j], "(") && d-- == 0) break;
                }
                open = j;
                j = prev_sig(t, open);
                if (j >= 0 && (tok_is(&t[j], "texture2DLod") || tok_is(&t[j], "textureLod") ||
                               tok_is(&t[j], "textureCubeLod")) && tok_is(&t[nj], ")"))
                    promote = 1;
                for (f = 0; j >= 0 && fl_fns[f]; f++) if (tok_is(&t[j], fl_fns[f])) break;
                if (j >= 0 && fl_fns[f]) {
                    /* any argument starting with a float operand makes the call float */
                    int a = next_sig(t, n, open);
                    d = 0;
                    while (a < n) {
                        if (d == 0 && operand_right(t, n, a, ss, ns) == TY_FLT) fl_arg = 1;
                        while (a < n) {
                            if (tok_is(&t[a], "(")) d++;
                            else if (tok_is(&t[a], ")")) { if (d-- == 0) break; }
                            else if (tok_is(&t[a], ",") && d == 0) break;
                            a++;
                        }
                        if (a >= n || tok_is(&t[a], ")")) break;
                        a = next_sig(t, n, a);
                    }
                    if (fl_arg) promote = 1;
                }
            }
            sb_put(&out, k->s, (size_t)k->n);
            if (promote) sb_puts(&out, ".0");
            continue;
        }
        if (k->k == T_ID) {
            int r;
            for (r = 0; RENAME[r][0]; r++) if (tok_is(k, RENAME[r][0])) break;
            if (RENAME[r][0]) { sb_puts(&out, RENAME[r][1]); continue; }
            if (tok_is(k, "attribute")) { sb_puts(&out, "in"); continue; }
            if (tok_is(k, "varying")) { sb_puts(&out, frag ? "in" : "out"); continue; }
            if (tok_is(k, "gl_FragColor")) { sb_puts(&out, "jp_FragColor"); continue; }
            if (tok_is(k, "gl_FragData")) {
                int j = next_sig(t, n, i), m;
                if (j < n && tok_is(&t[j], "[")) {
                    char b[32];
                    m = next_sig(t, n, j);
                    snprintf(b, sizeof(b), "jp_FragData%d", m < n ? atoi(t[m].s) & 7 : 0);
                    sb_puts(&out, b);
                    while (i < n && !tok_is(&t[i], "]")) i++;
                    continue;
                }
            }
            /* global fragment output `out vec4 name;` -> layout(location=L) out ... */
            if (frag && depth == 0 && paren == 0 && tok_is(k, "out")) {
                int ty = next_sig(t, n, i), nm = next_sig(t, n, ty), loc;
                if (nm < n && t[nm].k == T_ID) {
                    char b[48];
                    loc = out_loc(binds, t[nm].s, t[nm].n);
                    if (loc < 0 || loc > 7 || (used & (1 << loc))) for (loc = 0; loc < 7 && (used & (1 << loc)); loc++) ;
                    used |= 1 << loc;
                    snprintf(b, sizeof(b), "layout(location=%d) out", loc);
                    sb_puts(&out, b);
                    continue;
                }
            }
        }
        sb_put(&out, k->s, (size_t)k->n);
    }
    free(t);
    free(ss);
    free(pp);
    if (!out.p) sb_puts(&out, "");
    return out.p;
}

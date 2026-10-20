/*
 * paths.c — filesystem view the game expects, without root or bind mounts.
 *
 *  - /pm/...            the arcade install root. qemu -L only redirects a
 *                       guest path when the target already exists, so files
 *                       the game creates (logs, saves) would miss; map /pm
 *                       to JPVR_ROOT (default: the cwd at load, i.e. the game
 *                       folder) for every path-taking import.
 *  - /proc/cpuinfo      the host's (AArch64 under emulation) lacks the x86
 *                       fields xSysInit parses ("cpu MHz"); serve the arcade
 *                       PC's view.
 *  - /etc/udev/rules.d/ written as root on the cabinet; goes to the state dir.
 *  - JPVR_OVERLAY=dir   files under dir shadow /pm/... (same relative path).
 *
 * Wrappers call the real libc entry point resolved by sym_resolve_real.
 */

#define _GNU_SOURCE
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "shim.h"

static char g_root[512];

static char g_overlay[512];

static const char *map_path(const char *path, char *buf, size_t n)
{
    if (!path)
        return path;
    if (strncmp(path, "/pm/", 4) == 0 && g_root[0]) {
        /* JPVR_OVERLAY: a file at the same relative path wins (asset
         * overrides without touching the game folder). */
        if (g_overlay[0]) {
            snprintf(buf, n, "%s/%s", g_overlay, path + 4);
            if (access(buf, F_OK) == 0)
                return buf;
        }
        snprintf(buf, n, "%s/%s", g_root, path + 4);
        return buf;
    }
    if (strncmp(path, "/etc/udev/rules.d/", 18) == 0) {
        snprintf(buf, n, "%s/%s", state_dir(), path + 18);
        return buf;
    }
    return path;
}

#define MAPPED(var, path) char var##_buf[1024]; \
    const char *var = map_path(path, var##_buf, sizeof(var##_buf))

static const char FAKE_CPUINFO[] =
    "processor\t: %d\nvendor_id\t: GenuineIntel\ncpu family\t: 6\n"
    "model\t\t: 60\nmodel name\t: Intel(R) Core(TM) i5-4570 CPU @ 3.20GHz\n"
    "stepping\t: 3\ncpu MHz\t\t: 3200.000\ncache size\t: 6144 KB\n"
    "physical id\t: 0\nsiblings\t: 4\ncore id\t\t: %d\ncpu cores\t: 4\n"
    "flags\t\t: fpu vme de pse tsc msr pae mce cx8 apic sep mtrr pge mca cmov "
    "pat pse36 clflush mmx fxsr sse sse2 ss ht syscall nx lm constant_tsc "
    "pni ssse3 cx16 sse4_1 sse4_2 popcnt\nbogomips\t: 6400.00\n\n";

/* From an unlinked temp file so read()/lseek() behave like the real one. */
static int fake_cpuinfo_fd(void)
{
    char path[] = "/tmp/jpvr_cpuinfo_XXXXXX";
    int fd = mkstemp(path);
    if (fd < 0)
        return -1;
    unlink(path);
    char buf[1024];
    for (int cpu = 0; cpu < 4; cpu++) {
        int n = snprintf(buf, sizeof(buf), FAKE_CPUINFO, cpu, cpu);
        if (write(fd, buf, (size_t)n) != n)
            break;
    }
    lseek(fd, 0, SEEK_SET);
    return fd;
}

typedef int (*open_fn)(const char *, int, ...);
typedef int (*open2_fn)(const char *, int);
typedef FILE *(*fopen_fn)(const char *, const char *);
typedef int (*xstat_fn)(int, const char *, void *);
typedef int (*path_fn)(const char *);
typedef int (*path_int_fn)(const char *, int);
typedef int (*mkdir_fn)(const char *, unsigned int);
typedef void *(*opendir_fn)(const char *);
typedef int (*rename_fn)(const char *, const char *);

/* Real function for this call site, resolved once (GNU statement expr). */
#define REAL(type, name) \
    ({ static type f_; if (!f_) { f_ = (type)sym_resolve_real(name); } f_; })

static int open_common(const char *name, const char *path, int flags, va_list ap)
{
    int mode = (flags & O_CREAT) ? va_arg(ap, int) : 0;
    if (path && strcmp(path, "/proc/cpuinfo") == 0)
        return fake_cpuinfo_fd();
    MAPPED(p, path);
    return REAL(open_fn, name)(p, flags, mode);
}

static int w_open(const char *path, int flags, ...)
{
    va_list ap;
    va_start(ap, flags);
    int r = open_common("open", path, flags, ap);
    va_end(ap);
    return r;
}

static int w_open64(const char *path, int flags, ...)
{
    va_list ap;
    va_start(ap, flags);
    int r = open_common("open64", path, flags, ap);
    va_end(ap);
    return r;
}

static int w_open_2(const char *path, int flags)
{
    if (path && strcmp(path, "/proc/cpuinfo") == 0)
        return fake_cpuinfo_fd();
    MAPPED(p, path);
    return REAL(open2_fn, "__open_2")(p, flags);
}

static FILE *w_fopen(const char *path, const char *mode)
{
    MAPPED(p, path);
    return REAL(fopen_fn, "fopen")(p, mode);
}

static FILE *w_fopen64(const char *path, const char *mode)
{
    MAPPED(p, path);
    return REAL(fopen_fn, "fopen64")(p, mode);
}

static int w_xstat(int ver, const char *path, void *st)
{
    MAPPED(p, path);
    return REAL(xstat_fn, "__xstat")(ver, p, st);
}

static int w_xstat64(int ver, const char *path, void *st)
{
    MAPPED(p, path);
    return REAL(xstat_fn, "__xstat64")(ver, p, st);
}

static int w_access(const char *path, int m)
{
    MAPPED(p, path);
    return REAL(path_int_fn, "access")(p, m);
}

static int w_mkdir(const char *path, unsigned int m)
{
    MAPPED(p, path);
    return REAL(mkdir_fn, "mkdir")(p, m);
}

static void *w_opendir(const char *path)
{
    MAPPED(p, path);
    return REAL(opendir_fn, "opendir")(p);
}

static int w_remove(const char *path)
{
    MAPPED(p, path);
    return REAL(path_fn, "remove")(p);
}

static int w_rmdir(const char *path)
{
    MAPPED(p, path);
    return REAL(path_fn, "rmdir")(p);
}

static int w_unlink(const char *path)
{
    MAPPED(p, path);
    return REAL(path_fn, "unlink")(p);
}

static int w_rename(const char *from, const char *to)
{
    MAPPED(a, from);
    MAPPED(b, to);
    return REAL(rename_fn, "rename")(a, b);
}

static const struct { const char *name; void *wrap; } PATH_HOOKS[] = {
    { "open",      (void *)w_open },
    { "open64",    (void *)w_open64 },
    { "__open_2",  (void *)w_open_2 },
    { "fopen",     (void *)w_fopen },
    { "fopen64",   (void *)w_fopen64 },
    { "__xstat",   (void *)w_xstat },
    { "__xstat64", (void *)w_xstat64 },
    { "access",    (void *)w_access },
    { "mkdir",     (void *)w_mkdir },
    { "opendir",   (void *)w_opendir },
    { "remove",    (void *)w_remove },
    { "rmdir",     (void *)w_rmdir },
    { "unlink",    (void *)w_unlink },
    { "rename",    (void *)w_rename },
};

HIDDEN void *path_hook(const char *name)
{
    if (!g_root[0]) {
        const char *o = getenv("JPVR_OVERLAY");
        if (o)
            snprintf(g_overlay, sizeof(g_overlay), "%s", o);
        const char *r = getenv("JPVR_ROOT");
        if (r)
            snprintf(g_root, sizeof(g_root), "%s", r);
        else if (!getcwd(g_root, sizeof(g_root)))
            g_root[0] = 0;
    }
    for (size_t i = 0; i < sizeof(PATH_HOOKS) / sizeof(PATH_HOOKS[0]); i++)
        if (strcmp(name, PATH_HOOKS[i].name) == 0)
            return PATH_HOOKS[i].wrap;
    return NULL;
}

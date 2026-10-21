/*
 * syscalls.c — Linux i386 system calls for the recompiled process, on a
 * Windows x64 host, plus the virtual file system, threads (clone, TLS,
 * futex) and the host side of overridden / replaced library functions.
 *
 * Errors follow Linux: return -errno in eax.
 */
#include <fcntl.h>
#include <sys/stat.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <sched.h>
#include "rt.h"
#include "kernel.h"
#include "plat.h"

#if !defined(_WIN32)
#define strtok_s strtok_r
#endif

enum { L_EPERM = 1, L_ENOENT = 2, L_EINTR = 4, L_EIO = 5, L_EBADF = 9, L_EAGAIN = 11, L_ENOMEM = 12,
       L_EACCES = 13, L_EFAULT = 14, L_EEXIST = 17, L_ENOTDIR = 20, L_EISDIR = 21, L_EINVAL = 22,
       L_EMFILE = 24, L_ENOTTY = 25, L_ESPIPE = 29, L_ERANGE = 34, L_ENOSYS = 38, L_ENOTEMPTY = 39,
       L_EAFNOSUPPORT = 97, L_ETIMEDOUT = 110 };

static int lerrno(int e)
{
    switch (e) {
    case ENOENT: return -L_ENOENT;
    case EACCES: return -L_EACCES;
    case EEXIST: return -L_EEXIST;
    case EBADF: return -L_EBADF;
    case EINVAL: return -L_EINVAL;
    case ENOTDIR: return -L_ENOTDIR;
    case EISDIR: return -L_EISDIR;
    case EMFILE: return -L_EMFILE;
    case ENOTEMPTY: return -L_ENOTEMPTY;
    default: return -L_EIO;
    }
}

/* ---------------------------------------------------------------- VFS */
static char g_game_dir[1024];       /* host dir backing /pm */
static char g_root_dir[1024];       /* host dir backing every other absolute path */
static char g_lib_dirs[7][1024];    /* host dirs with the i386 libraries */
static char g_cwd[1024] = "/pm";

/* library sonames -> real file names in the extracted packages */
static const char *const LIBALIAS[][2] = {
    { "ld-linux.so.2", "ld-2.19.so" }, { "libc.so.6", "libc-2.19.so" }, { "libm.so.6", "libm-2.19.so" },
    { "libpthread.so.0", "libpthread-2.19.so" }, { "libdl.so.2", "libdl-2.19.so" },
    { "librt.so.1", "librt-2.19.so" }, { "libstdc++.so.6", "libstdc++.so.6.0.19" },
    { "libogg.so.0", "libogg.so.0.8.1" }, { "libtheora.so.0", "libtheora.so.0.3.10" },
    { "libnss_files.so.2", "libnss_files-2.19.so" },
    { "libPhysXCore.so.1", "libPhysXCore.so" }, { "libNxCooking.so.1", "libNxCooking.so" }, { NULL, NULL }
};
/* where each library goes (arcade layout, got-layout.md; librt free) */
static const struct { const char *file; uint32_t base; } LIBBASE[] = {
    { "libc-2.19.so", 0xb7141000u }, { "libgcc_s.so.1", 0xb72ef000u }, { "libm-2.19.so", 0xb7320000u },
    { "libpthread-2.19.so", 0xb736f000u }, { "libdl-2.19.so", 0xb75f0000u },
    { "libstdc++.so.6.0.19", 0xb75f5000u }, { "librt-2.19.so", 0xb7748000u },
    { "libogg.so.0.8.1", 0xb76ea000u }, { "libtheora.so.0.3.10", 0xb76f3000u },
    { "libnss_files-2.19.so", 0xb7790000u },
    /* PhysX 2.8 i386 runtime from TeknoParrot: the loader is the arcade's
     * build (its frozen GOT slot = NxGetCookingLib at b76de000+0x88e4);
     * core/cooking/character get free bases */
    { "libPhysXLoader.so.1", 0xb76de000u }, { "libPhysXCore.so", 0xa0000000u },
    { "libNxCooking.so", 0xa0400000u }, { "libNxCharacter.so.1", 0xa0500000u }, { NULL, 0 }
};

static int host_exists(const char *p) { return p_access(p, 0) == 0; }

/* /etc files the guest libc reads: NSS limited to `files` (only
 * libnss_files is lifted), the arcade ran as root */
static void write_etc_files(void)
{
    static const char *const files[][2] = {
        { "etc/nsswitch.conf", "passwd: files\ngroup: files\nshadow: files\nhosts: files\n" },
        { "etc/passwd", "root:x:0:0:root:/root:/bin/sh\n" },
        { "etc/group", "root:x:0:\n" },
        { "etc/hosts", "127.0.0.1 localhost\n" },
        { "proc/cpuinfo", "processor\t: 0\nvendor_id\t: GenuineIntel\ncpu family\t: 6\nmodel\t\t: 42\n"
                          "model name\t: Intel(R) Core(TM) i3-2120 CPU @ 3.30GHz\ncpu MHz\t\t: 3300.000\n"
                          "flags\t\t: fpu vme de pse tsc msr pae mce cx8 apic sep mtrr pge mca cmov pat pse36 "
                          "clflush mmx fxsr sse sse2 ht pni ssse3 cx16 sse4_1 sse4_2 popcnt\nbogomips\t: 6600.00\n\n" },
    };
    static const char *const dirs[] = { "etc", "root", "tmp", "proc", "etc/udev", "etc/udev/rules.d",
                                        "mnt", "mnt/usbflash", "media", "var", "var/log", "dev" };
    char p[1100];
    int i;
    for (i = 0; i < (int)(sizeof(dirs) / sizeof(dirs[0])); i++) {
        snprintf(p, sizeof(p), "%s/%s", g_root_dir, dirs[i]);
        p_mkdir(p);
    }
    for (i = 0; i < 5; i++) {
        FILE *f;
        snprintf(p, sizeof(p), "%s/%s", g_root_dir, files[i][0]);
        if ((f = fopen(p, "wb"))) { fputs(files[i][1], f); fclose(f); }
    }
}

void vfs_init(const char *game_dir)
{
    const char *libdir = getenv("JPRT_LIBDIR");
    const char *root = getenv("JPRT_ROOT");
    char exe[1024];
    plat_exe_dir(exe, sizeof(exe));
    snprintf(g_game_dir, sizeof(g_game_dir), "%s", game_dir);
    if (root) snprintf(g_root_dir, sizeof(g_root_dir), "%s", root);
    else snprintf(g_root_dir, sizeof(g_root_dir), "%s/rootfs", exe);
    p_mkdir(g_root_dir);
    if (!libdir) libdir = "ext/debs";
    snprintf(g_lib_dirs[0], 1024, "%s/x_libc6_2.19-0ubuntu6.6_i386/lib/i386-linux-gnu", libdir);
    snprintf(g_lib_dirs[1], 1024, "%s/x_libgcc1_4.9-20140406-0ubuntu1_i386/lib/i386-linux-gnu", libdir);
    snprintf(g_lib_dirs[2], 1024, "%s/x_libstdc++6_4.8.2-19ubuntu1_i386/usr/lib/i386-linux-gnu", libdir);
    snprintf(g_lib_dirs[3], 1024, "%s/x_libogg0_1.3.1-1ubuntu1_i386/usr/lib/i386-linux-gnu", libdir);
    snprintf(g_lib_dirs[4], 1024, "%s/x_libtheora0_1.1.1+dfsg.1-3.2_i386/usr/lib/i386-linux-gnu", libdir);
    /* PhysX 2.8 runtime: JPRT_TPLIBS, default <libdir>/../TeknoParrot/ElfLdr2/libs */
    if (getenv("JPRT_TPLIBS")) snprintf(g_lib_dirs[5], 1024, "%s", getenv("JPRT_TPLIBS"));
    else snprintf(g_lib_dirs[5], 1024, "%s/../TeknoParrot/ElfLdr2/libs", libdir);
    g_lib_dirs[6][0] = 0;
    if (getenv("JPRT_LIBS")) {              /* all i386 libraries in one flat dir (the Quest) */
        snprintf(g_lib_dirs[0], 1024, "%s", getenv("JPRT_LIBS"));
        g_lib_dirs[1][0] = 0;
    }
    write_etc_files();
    klog("vfs: /pm -> %s, / -> %s, libs in %s\n", g_game_dir, g_root_dir, libdir);
}

static void normalize(const char *in, char *out, size_t n)
{
    char tmp[1024], *parts[128], *tok, *ctx = NULL;
    int np = 0, i;
    if (in[0] == '/') snprintf(tmp, sizeof(tmp), "%s", in);
    else snprintf(tmp, sizeof(tmp), "%s/%s", g_cwd, in);
    for (tok = strtok_s(tmp, "/", &ctx); tok; tok = strtok_s(NULL, "/", &ctx)) {
        if (!strcmp(tok, ".") || !*tok) continue;
        if (!strcmp(tok, "..")) { if (np) np--; continue; }
        if (np < 128) parts[np++] = tok;
    }
    out[0] = 0;
    for (i = 0; i < np; i++) {
        strncat(out, "/", n - strlen(out) - 1);
        strncat(out, parts[i], n - strlen(out) - 1);
    }
    if (!out[0]) snprintf(out, n, "/");
}

/* guest path -> host path; returns 0 if it cannot exist (e.g. replaced libs) */
int vfs_host_path(const char *guest, char *host, size_t n)
{
    char g[1024];
    const char *base;
    int i, d;
    normalize(guest, g, sizeof(g));
    base = strrchr(g, '/') + 1;
    if (!strncmp(g, "/pm/g5/dino/pmuser", 18) && (g[18] == 0 || g[18] == '/')) {
        /* on the cabinet g5/dino/pmuser links to /pm/pmuser (saves, logs) */
        snprintf(host, n, "%s/pmuser%s", g_game_dir, g + 18);
        return 1;
    }
    if (!strncmp(g, "/pm/", 4) || !strcmp(g, "/pm")) {
        snprintf(host, n, "%s%s", g_game_dir, g + 3);
        return 1;
    }
    if (!strncmp(g, "/lib/", 5) || !strncmp(g, "/usr/lib/", 9)) {
        /* only the flat library dirs; hwcap subdirs (tls/i686/...) miss */
        const char *rest = g + (g[1] == 'l' ? 5 : 9);
        if (!strncmp(rest, "i386-linux-gnu/", 15)) rest += 15;
        if (strchr(rest, '/')) return 0;
        for (i = 0; LIBALIAS[i][0]; i++)
            if (!strcmp(rest, LIBALIAS[i][0])) { rest = LIBALIAS[i][1]; break; }
        for (d = 0; d < 7 && g_lib_dirs[d][0]; d++) {
            snprintf(host, n, "%s/%s", g_lib_dirs[d], rest);
            if (host_exists(host)) return 1;
        }
        return 0;
    }
#if defined(_WIN32)
    if (!strcmp(g, "/dev/null")) { snprintf(host, n, "NUL"); return 1; }
#else
    if (!strcmp(g, "/dev/null")) { snprintf(host, n, "/dev/null"); return 1; }
#endif
    (void)base;
    snprintf(host, n, "%s%s", g_root_dir, g);
    return 1;
}

/* ---------------------------------------------------------------- fds */
#define MAXFD 256
typedef struct {
    int used, hfd, is_dir;
    char gpath[512];
    char hpath[1024];
    plat_dir *dir;                 /* directory listing state */
    int dir_done;
    char pending[256];             /* entry that did not fit the last getdents buffer */
    int pending_isdir;
} gfd;
static gfd g_fd[MAXFD];
static plat_mutex g_fd_cs;
static int g_fd_init;

static void fd_init(void)
{
    int i;
    if (g_fd_init) return;
    g_fd_init = 1;
    plat_mutex_init(&g_fd_cs);
    for (i = 0; i < 3; i++) { g_fd[i].used = 1; g_fd[i].hfd = i; snprintf(g_fd[i].gpath, 512, "<std%d>", i); }
}

static int fd_alloc(int from)
{
    int i;
    for (i = from; i < MAXFD; i++)
        if (!g_fd[i].used) { memset(&g_fd[i], 0, sizeof(gfd)); g_fd[i].used = 1; return i; }
    return -L_EMFILE;
}

static gfd *fd_get(uint32_t fd) { return (fd < MAXFD && g_fd[fd].used) ? &g_fd[fd] : NULL; }

static int is_host_dir(const char *p) { return plat_is_dir(p); }

static int sys_open(const char *path, int flags, int mode)
{
    char host[1024];
    int hf = 0, fd, h;
    if (!path) return -L_EFAULT;
    if (!strcmp(path, "/etc/ld.so.cache") || !strcmp(path, "/etc/ld.so.preload")) return -L_ENOENT;
    if (!vfs_host_path(path, host, sizeof(host))) return -L_ENOENT;
    plat_mutex_lock(&g_fd_cs);
    fd = fd_alloc(3);
    if (fd < 0) { plat_mutex_unlock(&g_fd_cs); return fd; }
    if (is_host_dir(host)) {
        g_fd[fd].is_dir = 1;
        g_fd[fd].hfd = -1;
    } else {
        if ((flags & 3) == 0) hf = P_O_RDONLY; else if ((flags & 3) == 1) hf = P_O_WRONLY; else hf = P_O_RDWR;
        if (flags & 0100) hf |= P_O_CREAT;
        if (flags & 01000) hf |= P_O_TRUNC;
        if (flags & 02000) hf |= P_O_APPEND;
        if (flags & 0200) hf |= P_O_EXCL;
        if (flags & 0200000) { g_fd[fd].used = 0; plat_mutex_unlock(&g_fd_cs); return -L_ENOTDIR; } /* O_DIRECTORY */
        h = p_open(host, hf, P_MODE);
        if (h < 0) { int e = errno; g_fd[fd].used = 0; plat_mutex_unlock(&g_fd_cs); return lerrno(e); }
        g_fd[fd].hfd = h;
    }
    snprintf(g_fd[fd].gpath, sizeof(g_fd[fd].gpath), "%s", path);
    snprintf(g_fd[fd].hpath, sizeof(g_fd[fd].hpath), "%s", host);
    plat_mutex_unlock(&g_fd_cs);
    if (g_verbose) klog("open(%s, %#x) = %d [%s]\n", path, flags, fd, host);
    return fd;
}

static int sys_close(uint32_t fd)
{
    gfd *f = fd_get(fd);
    if (!f) return -L_EBADF;
    if (fd < 3) return 0;
    if (f->hfd >= 0) p_close(f->hfd);
    if (f->dir) plat_closedir(f->dir);
    f->used = 0;
    return 0;
}

/* struct stat64 (i386, 96 bytes) */
static void fill_stat64(uint32_t buf, int is_dir, int64_t size, int64_t mtime, uint32_t ino)
{
    memset(RT_MEM + buf, 0, 96);
    wr64(buf + 0, 0x801);
    wr32(buf + 12, ino);
    wr32(buf + 16, is_dir ? 040755 : 0100644);
    wr32(buf + 20, 1);
    wr64(buf + 44, (uint64_t)size);
    wr32(buf + 52, 4096);
    wr64(buf + 56, (uint64_t)((size + 511) / 512));
    wr32(buf + 64, (uint32_t)mtime);
    wr32(buf + 72, (uint32_t)mtime);
    wr32(buf + 80, (uint32_t)mtime);
    wr64(buf + 88, ino);
}

static uint32_t path_ino(const char *s)
{
    uint32_t h = 2166136261u;
    while (*s) h = (h ^ (uint8_t)*s++) * 16777619u;
    return h | 1;
}

static int stat_host(const char *host, uint32_t buf)
{
    p_stat_t st;
    if (p_stat(host, &st) < 0) return lerrno(errno);
    fill_stat64(buf, P_S_ISDIR(st.st_mode), st.st_size, st.st_mtime, path_ino(host));
    return 0;
}

static int sys_stat64(const char *path, uint32_t buf)
{
    char host[1024];
    if (!path) return -L_EFAULT;
    if (!vfs_host_path(path, host, sizeof(host))) return -L_ENOENT;
    return stat_host(host, buf);
}

static int sys_fstat64(uint32_t fd, uint32_t buf)
{
    gfd *f = fd_get(fd);
    if (!f) return -L_EBADF;
    if (fd < 3) {   /* a character device (pipe-ish for the CRT) */
        fill_stat64(buf, 0, 0, 0, 100 + fd);
        wr32(buf + 16, 020620);
        return 0;
    }
    if (f->is_dir) return stat_host(f->hpath, buf);
    {
        p_stat_t st;
        if (p_fstat(f->hfd, &st) < 0) return lerrno(errno);
        fill_stat64(buf, 0, st.st_size, st.st_mtime, path_ino(f->hpath));
    }
    return 0;
}

static int sys_getdents(uint32_t fd, uint32_t buf, uint32_t len, int old)
{
    gfd *f = fd_get(fd);
    uint32_t used = 0;
    if (!f) return -L_EBADF;
    if (!f->is_dir) return -L_ENOTDIR;
    if (!f->dir && !f->dir_done) {
        f->dir = plat_opendir(f->hpath);
        if (!f->dir) { f->dir_done = 1; return 0; }
    }
    for (;;) {
        const char *name;
        int isdir = 0;
        uint32_t nl, rec;
        if (f->pending[0]) { name = f->pending; isdir = f->pending_isdir; }
        else {
            if (f->dir_done) return (int)used;
            name = plat_readdir(f->dir, &isdir);
            if (!name) { plat_closedir(f->dir); f->dir = NULL; f->dir_done = 1; return (int)used; }
        }
        /* linux_dirent64: ino u64, off s64, reclen u16, type u8, name
         * linux_dirent (old): ino u32, off u32, reclen u16, name, 0, type */
        nl = (uint32_t)strlen(name);
        rec = old ? ((10 + nl + 2 + 3) & ~3u) : ((19 + nl + 1 + 7) & ~7u);
        if (used + rec > len) {
            if (!used) return -L_EINVAL;
            if (name != f->pending) {         /* keep it for the next call */
                snprintf(f->pending, sizeof(f->pending), "%s", name);
                f->pending_isdir = isdir;
            }
            return (int)used;
        }
        if (old) {
            memset(RT_MEM + buf + used, 0, rec);
            wr32(buf + used, path_ino(name));
            wr32(buf + used + 4, used + rec);
            wr16(buf + used + 8, (uint16_t)rec);
            memcpy(RT_MEM + buf + used + 10, name, nl + 1);
            wr8(buf + used + rec - 1, isdir ? 4 : 8);
        } else {
            wr64(buf + used, path_ino(name));
            wr64(buf + used + 8, used + rec);
            wr16(buf + used + 16, (uint16_t)rec);
            wr8(buf + used + 18, isdir ? 4 : 8);
            memcpy(RT_MEM + buf + used + 19, name, nl + 1);
        }
        used += rec;
        f->pending[0] = 0;
    }
}

/* ---------------------------------------------------------------- threads, TLS */
static int32_t g_next_tid = 1000;
static __thread kthread *t_self;
static int32_t g_threads = 1;

#define SHADOW_DEPTH 65536

rt_cpu *thread_new_cpu(void)
{
#if defined(_WIN32)
    kthread *k = _aligned_malloc(sizeof(kthread), 64);
#else
    kthread *k = aligned_alloc(64, (sizeof(kthread) + 63) & ~(size_t)63);
#endif
    memset(k, 0, sizeof(*k));
    k->cpu.shadow = malloc(sizeof(uint32_t) * SHADOW_DEPTH);
    k->cpu.shcap = SHADOW_DEPTH;
    k->tid = (uint32_t)plat_atomic_inc(&g_next_tid) - 1;
    if (!t_self) t_self = k;
    return &k->cpu;
}

static kthread *self_of(rt_cpu *cpu) { return (kthread *)cpu; }

/* guest CPU of the calling host thread (for crash reports) */
rt_cpu *current_cpu(void) { return t_self ? &t_self->cpu : NULL; }

void rt_load_gs(rt_cpu *cpu)
{
    kthread *k = self_of(cpu);
    uint32_t e = cpu->gs_sel >> 3;
    cpu->gs_base = (e >= 6 && e <= 8) ? k->tls_base[e - 6] : 0;
}

static int sys_set_thread_area(rt_cpu *cpu, uint32_t ud)
{
    kthread *k = self_of(cpu);
    int32_t e = (int32_t)rd32(ud);
    if (e == -1) {
        for (e = 6; e <= 8 && k->tls_base[e - 6]; e++) ;
        if (e > 8) return -3;                     /* ESRCH: no free entry */
        wr32(ud, (uint32_t)e);
    }
    if (e < 6 || e > 8) return -L_EINVAL;
    k->tls_base[e - 6] = rd32(ud + 4);
    if ((cpu->gs_sel >> 3) == (uint32_t)e) rt_load_gs(cpu);
    return 0;
}

/* ---- trampoline: non-local transfers discard host frames and continue
 * here. The whole guest state is in cpu + guest memory, so dropping the
 * host stack is safe; the guest's own stack (and its copying coroutines)
 * is untouched. */
void rt_bounce(rt_cpu *cpu, uint32_t to)
{
    kthread *k = self_of(cpu);
    k->bounce_to = to;
    plat_longjmp(k->bounce_jb, 1);
}

void run_guest(rt_cpu *cpu_in, uint32_t target, int resume)
{
    volatile uint32_t t = target;
    volatile int res = resume;
    t_self = (kthread *)cpu_in;
    if (plat_setjmp(t_self->bounce_jb)) {
        /* back from rt_bounce: host frames above were abandoned */
        t = t_self->bounce_to;
        res = 1;
    }
    {
        rt_cpu *cpu = &t_self->cpu;
        lifted_fn fn = res ? find_resume(t) : NULL;
        cpu->sh = 0;
        if (fn) {
            cpu->resume_at = t;
        } else {
            fn = find_lifted(t);            /* a bounce to a function entry */
            if (!fn) kfatal(cpu, "bounce to %08x: neither a resume point nor a function", t);
        }
        fn(cpu);
        kfatal(cpu, "guest code returned to the trampoline (from %08x, ret to %08x)", t,
               mem_ok(cpu->esp - 4, 4) ? rd32(cpu->esp - 4) : 0);
    }
}

typedef struct { kthread *k; uint32_t resume; } child_start;

void plat_guest_thread_init(void);          /* weak hook: thread affinity on the Quest */

static void *child_main(void *p)
{
    child_start cs = *(child_start *)p;
    free(p);
    t_self = cs.k;
    plat_guest_thread_init();
    run_guest(&cs.k->cpu, cs.resume, 1);
}

static int sys_clone(rt_cpu *cpu, uint32_t next)
{
    uint32_t flags = cpu->ebx, stack = cpu->ecx, ptid = cpu->edx, tls = cpu->esi, ctid = cpu->edi;
    kthread *k;
    child_start *cs;
    if (!(flags & 0x100)) {                 /* !CLONE_VM: fork */
        klog("clone without CLONE_VM (fork) refused\n");
        return -L_ENOSYS;
    }
    if (!next) kfatal(cpu, "clone through the vsyscall entry: no resume point");
    k = (kthread *)thread_new_cpu();
    {   /* copy the parent registers, keep the child's own shadow stack */
        uint32_t *sh = k->cpu.shadow, cap = k->cpu.shcap;
        k->cpu = *cpu;
        k->cpu.shadow = sh;
        k->cpu.shcap = cap;
        k->cpu.sh = 0;
    }
    k->cpu.eax = 0;
    k->cpu.esp = stack;
    memcpy(k->tls_base, self_of(cpu)->tls_base, sizeof(k->tls_base));
    if (flags & 0x80000) {                  /* CLONE_SETTLS: user_desc* */
        int32_t e = (int32_t)rd32(tls);
        if (e >= 6 && e <= 8) {
            k->tls_base[e - 6] = rd32(tls + 4);
            k->cpu.gs_sel = (uint32_t)(e << 3) | 3;
        }
        rt_load_gs(&k->cpu);
    }
    if (flags & 0x100000) wr32(ptid, k->tid);        /* CLONE_PARENT_SETTID */
    if (flags & 0x1000000) wr32(ctid, k->tid);       /* CLONE_CHILD_SETTID */
    if (flags & 0x200000) k->clear_child_tid = ctid; /* CLONE_CHILD_CLEARTID */
    cs = malloc(sizeof(*cs));
    cs->k = k;
    cs->resume = next;
    plat_atomic_inc(&g_threads);
    if (plat_thread_start(child_main, cs, 64u << 20) < 0) { plat_atomic_dec(&g_threads); return -L_EAGAIN; }
    klog("clone: thread %u (stack %08x, resume %08x)\n", k->tid, stack, next);
    return (int)k->tid;
}

static void futex_wake_addr(uint32_t a)
{
    plat_wake32((volatile uint32_t *)(RT_MEM + a), 1);
}

static void thread_exit(rt_cpu *cpu, int code)
{
    kthread *k = self_of(cpu);
    if (k->clear_child_tid) {
        wr32(k->clear_child_tid, 0);
        futex_wake_addr(k->clear_child_tid);
    }
    if (plat_atomic_dec(&g_threads) == 0) {
        klog("last thread exited (%d)\n", code);
        exit(code);
    }
    plat_thread_exit();
}

static int sys_futex(rt_cpu *cpu, uint32_t uaddr, int op, uint32_t val, uint32_t timeout, uint32_t uaddr2, uint32_t val3)
{
    int cmd = op & 0x7f;
    (void)cpu;
    switch (cmd) {
    case 0: case 9: {                       /* WAIT, WAIT_BITSET */
        int64_t tns = -1;
        if (timeout) {
            int64_t s = (int32_t)rd32(timeout), ns = (int32_t)rd32(timeout + 4);
            tns = s * 1000000000ll + ns;
            if (cmd == 9) {                 /* absolute: FUTEX_CLOCK_REALTIME or monotonic */
                int64_t now = (op & 256) ? plat_real_ns() : (int64_t)plat_mono_ns();
                tns -= now;
                if (tns < 0) tns = 0;
            }
        }
        if (rd32(uaddr) != val) return -L_EAGAIN;
        return plat_wait32((volatile uint32_t *)(RT_MEM + uaddr), val, tns) ? -L_ETIMEDOUT : 0;
    }
    case 1: case 10:                        /* WAKE, WAKE_BITSET */
        plat_wake32((volatile uint32_t *)(RT_MEM + uaddr), val != 1);
        return (int)val;
    case 3: case 4:                         /* REQUEUE / CMP_REQUEUE: wake everyone */
        if (cmd == 4 && rd32(uaddr) != val3) return -L_EAGAIN;
        plat_wake32((volatile uint32_t *)(RT_MEM + uaddr), 1);
        plat_wake32((volatile uint32_t *)(RT_MEM + uaddr2), 1);
        return 1;
    case 5: {                               /* WAKE_OP */
        uint32_t o = val3, oparg = (o >> 12) & 0xfff, cmparg = o & 0xfff, opn = (o >> 28) & 7;
        uint32_t cmp = (o >> 24) & 15, old = rd32(uaddr2), nv = old;
        int ok;
        if (o & 0x80000000u) oparg = 1u << oparg;
        switch (opn) { case 0: nv = oparg; break; case 1: nv = old + oparg; break; case 2: nv = old | oparg; break;
                       case 3: nv = old & ~oparg; break; case 4: nv = old ^ oparg; break; }
        wr32(uaddr2, nv);
        switch (cmp) { case 0: ok = old == cmparg; break; case 1: ok = old != cmparg; break;
                       case 2: ok = (int32_t)old < (int32_t)cmparg; break; case 3: ok = (int32_t)old <= (int32_t)cmparg; break;
                       case 4: ok = (int32_t)old > (int32_t)cmparg; break; default: ok = (int32_t)old >= (int32_t)cmparg; }
        plat_wake32((volatile uint32_t *)(RT_MEM + uaddr), 1);
        if (ok) plat_wake32((volatile uint32_t *)(RT_MEM + uaddr2), 1);
        return 1;
    }
    default:
        klog("futex op %d unsupported\n", op);
        return -L_ENOSYS;
    }
}

/* ---------------------------------------------------------------- time */
static void now_mono(int64_t *s, int64_t *ns)
{
    uint64_t t = plat_mono_ns();
    *s = (int64_t)(t / 1000000000ull);
    *ns = (int64_t)(t % 1000000000ull);
}

static void now_real(int64_t *s, int64_t *ns)
{
    int64_t t = plat_real_ns();
    *s = t / 1000000000ll;
    *ns = t % 1000000000ll;
}

static void sleep_ns(int64_t s, int64_t ns)
{
    int64_t ms = s * 1000 + ns / 1000000;
    if (ms <= 0) { plat_yield(); return; }
    plat_sleep_ms((uint32_t)ms);
}

/* ---------------------------------------------------------------- mmap */
static uint32_t lib_base_for(const char *gpath)
{
    const char *b = strrchr(gpath, '/');
    int i;
    b = b ? b + 1 : gpath;
    for (i = 0; LIBALIAS[i][0]; i++)
        if (!strcmp(b, LIBALIAS[i][0])) { b = LIBALIAS[i][1]; break; }
    for (i = 0; LIBBASE[i].file; i++)
        if (!strcmp(b, LIBBASE[i].file)) return LIBBASE[i].base;
    return 0;
}

static int64_t sys_mmap2(uint32_t addr, uint32_t len, uint32_t prot, uint32_t flags, int32_t fd, uint32_t pgoff)
{
    gfd *f = NULL;
    (void)prot;
    if (!len) return -L_EINVAL;
    if (!(flags & 0x20)) {                  /* file-backed */
        f = fd_get((uint32_t)fd);
        if (!f || f->hfd < 0) return -L_EBADF;
    }
    if (!(flags & 0x10)) {                  /* not MAP_FIXED: choose */
        uint32_t lb = (f && pgoff == 0) ? lib_base_for(f->gpath) : 0;
        if (lb && mem_free(lb, len)) addr = lb;
        else if (addr && !(addr & 0xfffu) && mem_free(addr, len)) { /* hint honoured only if wholly free */ }
        else addr = mem_find(len, MMAP_TOP);
        if (!addr) return -L_ENOMEM;
    }
    if (mem_map(addr, len) < 0) return -L_ENOMEM;
    if (f) {
        int64_t cur = p_lseek(f->hfd, 0, SEEK_CUR);
        int n;
        p_lseek(f->hfd, (int64_t)pgoff * 4096, SEEK_SET);
        n = p_read(f->hfd, RT_MEM + addr, len);
        p_lseek(f->hfd, cur, SEEK_SET);
        if (n < 0) n = 0;
    }
    if (g_verbose) klog("mmap2(%08x, %#x, fd %d %s) = %08x\n", addr, len, fd, f ? f->gpath : "anon", addr);
    return addr;
}

/* ---------------------------------------------------------------- misc */
static void put_utsname(uint32_t b)
{
    const char *f[6] = { "Linux", "arcade", "3.13.0-24-generic", "#46-Ubuntu SMP", "i686", "(none)" };
    int i;
    memset(RT_MEM + b, 0, 6 * 65);
    for (i = 0; i < 6; i++) memcpy(RT_MEM + b + 65 * i, f[i], strlen(f[i]));
}

static int sys_readlink(const char *path, uint32_t buf, uint32_t n)
{
    const char *r = NULL;
    if (path && !strcmp(path, "/proc/self/exe")) r = "/pm/game";
    if (!r) return -L_EINVAL;
    {
        uint32_t l = (uint32_t)strlen(r);
        if (l > n) l = n;
        memcpy(RT_MEM + buf, r, l);
        return (int)l;
    }
}

static int sys_rw(int is_write, uint32_t fd, uint32_t buf, uint32_t n)
{
    gfd *f = fd_get(fd);
    int r;
    if (!f) return -L_EBADF;
    if (f->is_dir) return -L_EISDIR;
    if (!n) return 0;
    if (is_write && (fd == 1 || fd == 2)) {
        fwrite(RT_MEM + buf, 1, n, fd == 1 ? stdout : stderr);
        fflush(fd == 1 ? stdout : stderr);
        klog("[guest %s] %.*s%s", fd == 1 ? "stdout" : "stderr", (int)n, (char *)(RT_MEM + buf),
             ((char *)RT_MEM)[buf + n - 1] == '\n' ? "" : "\n");
        return (int)n;
    }
    r = is_write ? p_write(f->hfd, RT_MEM + buf, n) : p_read(f->hfd, RT_MEM + buf, n);
    return r < 0 ? lerrno(errno) : r;
}

/* ---------------------------------------------------------------- dispatcher */
static uint32_t g_sig_handlers[65];

void rt_syscall(rt_cpu *cpu, uint32_t next)
{
    uint32_t nr = cpu->eax, a = cpu->ebx, b = cpu->ecx, c = cpu->edx, d = cpu->esi, e = cpu->edi, f6 = cpu->ebp;
    int64_t r = -L_ENOSYS;
    int64_t s, ns;
    fd_init();
    watch_poll();
    switch (nr) {
    case 1:   thread_exit(cpu, (int)a); break;                                  /* exit */
    case 252: klog("exit_group(%d)\n", (int)a); fflush(NULL); exit((int)a);   /* exit_group */
    case 3:   r = sys_rw(0, a, b, c); break;
    case 4:   r = sys_rw(1, a, b, c); break;
    case 5:   r = sys_open(gstr(a), (int)b, (int)c); break;
    case 295: r = ((int32_t)a == -100) ? sys_open(gstr(b), (int)c, (int)d) : -L_ENOSYS; break;  /* openat(AT_FDCWD) */
    case 6:   r = sys_close(a); break;
    case 10:  { char h[1024]; r = vfs_host_path(gstr(a), h, sizeof(h)) ? (p_unlink(h) < 0 ? lerrno(errno) : 0) : -L_ENOENT; } break;
    case 12:  { char h[1024], g[1024]; if (vfs_host_path(gstr(a), h, sizeof(h)) && is_host_dir(h)) { normalize(gstr(a), g, sizeof(g)); snprintf(g_cwd, sizeof(g_cwd), "%s", g); r = 0; } else r = -L_ENOENT; } break;
    case 13:  now_real(&s, &ns); if (a) wr32(a, (uint32_t)s); r = s; break;    /* time */
    case 19:  { gfd *fd = fd_get(a); r = fd && fd->hfd >= 0 ? p_lseek(fd->hfd, (int32_t)b, (int)c) : -L_EBADF; if (r < 0 && fd) r = -L_ESPIPE; } break;
    case 140: { gfd *fd = fd_get(a); int64_t off = ((int64_t)b << 32) | c;   /* _llseek */
                if (!fd || fd->hfd < 0) { r = -L_EBADF; break; }
                off = p_lseek(fd->hfd, off, (int)e);
                if (off < 0) { r = -L_EINVAL; break; }
                wr64(d, (uint64_t)off); r = 0; } break;
    case 20:  r = 1000; break;                                                  /* getpid */
    case 224: r = self_of(cpu)->tid; break;                                     /* gettid */
    case 64:  r = 1; break;                                                     /* getppid */
    case 24: case 47: case 49: case 50: case 199: case 200: case 201: case 202: r = 0; break; /* uid/gid: root */
    case 33:  { char h[1024]; r = (vfs_host_path(gstr(a), h, sizeof(h)) && p_access(h, 0) == 0) ? 0 : -L_ENOENT; } break;
    case 37: case 270: if ((nr == 37 ? b : c) == 6) kfatal(cpu, "guest abort() (SIGABRT)"); r = 0; break;
    case 38:  { char h1[1024], h2[1024]; r = (vfs_host_path(gstr(a), h1, 1024) && vfs_host_path(gstr(b), h2, 1024)) ? (plat_rename(h1, h2) == 0 ? 0 : -L_EACCES) : -L_ENOENT; } break;
    case 39:  { char h[1024]; r = vfs_host_path(gstr(a), h, sizeof(h)) ? (p_mkdir(h) < 0 ? lerrno(errno) : 0) : -L_EACCES; } break;
    case 40:  { char h[1024]; r = vfs_host_path(gstr(a), h, sizeof(h)) ? (p_rmdir(h) < 0 ? lerrno(errno) : 0) : -L_ENOENT; } break;
    case 41: case 63: { gfd *fd = fd_get(a); int nfd;                          /* dup / dup2 */
                if (!fd) { r = -L_EBADF; break; }
                plat_mutex_lock(&g_fd_cs);
                nfd = nr == 41 ? fd_alloc(0) : (int)b;
                if (nfd >= 0 && nfd < MAXFD) { if (nr == 63 && g_fd[nfd].used && nfd != (int)a) sys_close(nfd);
                    g_fd[nfd] = *fd; g_fd[nfd].hfd = fd->hfd >= 0 ? p_dup(fd->hfd) : -1; g_fd[nfd].dir = NULL; g_fd[nfd].dir_done = 0; g_fd[nfd].pending[0] = 0; }
                plat_mutex_unlock(&g_fd_cs); r = nfd; } break;
    case 43:  r = 0; break;                                                     /* times */
    case 45:  if (a > g_brk) { if (mem_map(g_brk, a - g_brk) == 0) g_brk = a; }                  /* brk */
              else if (a && a >= g_brk_start && a < g_brk) { mem_unmap((a + 0xfff) & ~0xfffu, g_brk - ((a + 0xfff) & ~0xfffu)); g_brk = a; }
              r = g_brk; break;
    case 54:  r = -L_ENOTTY; break;                                             /* ioctl */
    case 55: case 221:                                                          /* fcntl(64) */
        if (!fd_get(a)) { r = -L_EBADF; break; }
        if (b == 0 || b == 1030) {                                              /* F_DUPFD(_CLOEXEC) */
            int nfd;
            plat_mutex_lock(&g_fd_cs);
            nfd = fd_alloc((int)c);
            if (nfd >= 0) {
                g_fd[nfd] = g_fd[a];
                g_fd[nfd].hfd = g_fd[a].hfd >= 0 ? p_dup(g_fd[a].hfd) : -1;
                g_fd[nfd].dir = NULL; g_fd[nfd].dir_done = 0; g_fd[nfd].pending[0] = 0;
            }
            plat_mutex_unlock(&g_fd_cs);
            r = nfd;
        } else {
            r = 0;                                                              /* F_GETFD/SETFD/GETFL/SETFL */
        }
        break;
    case 60:  r = 022; break;                                                   /* umask */
    case 77:  memset(RT_MEM + b, 0, 72); r = 0; break;                          /* getrusage */
    case 78:  now_real(&s, &ns); if (a) { wr32(a, (uint32_t)s); wr32(a + 4, (uint32_t)(ns / 1000)); } if (b) { wr32(b, 0); wr32(b + 4, 0); } r = 0; break;
    case 85:  r = sys_readlink(gstr(a), b, c); break;
    case 90:  r = sys_mmap2(rd32(a), rd32(a + 4), rd32(a + 8), rd32(a + 12), (int32_t)rd32(a + 16), rd32(a + 20) >> 12); break;
    case 91:  mem_unmap(a, b); r = 0; break;                                    /* munmap */
    case 102: r = (a == 1) ? -L_EAFNOSUPPORT : -L_ENOSYS; break;                /* socketcall: no network */
    case 104: r = 0; break;                                                     /* setitimer */
    case 106: case 107: case 195: case 196: r = sys_stat64(gstr(a), b); break;  /* stat/lstat(64) */
    case 108: case 197: r = sys_fstat64(a, b); break;
    case 116: memset(RT_MEM + a, 0, 64); wr32(a + 4, 1u << 30); wr32(a + 52, 1); r = 0; break; /* sysinfo */
    case 120: r = sys_clone(cpu, next); break;
    case 122: put_utsname(a); r = 0; break;
    case 125: case 219: case 144: case 150: case 151: r = 0; break;             /* mprotect madvise msync mlock */
    case 126: case 175: if (c) memset(RT_MEM + c, 0, nr == 175 ? 8 : 4); r = 0; break; /* sigprocmask */
    case 67: case 174: if (a < 65) { if (c) { wr32(c, g_sig_handlers[a]); memset(RT_MEM + c + 4, 0, 16); }
                                     if (b) g_sig_handlers[a] = rd32(b); }
                       r = 0; break;                                            /* (rt_)sigaction */
    case 186: if (b) { wr32(b, 0); wr32(b + 4, 2); wr32(b + 8, 0); } r = 0; break;   /* sigaltstack: SS_DISABLE */
    case 142: case 168: if (nr == 168 && (int32_t)c > 0) plat_sleep_ms(c); r = 0; break; /* select/poll: nothing ready */
    case 145: case 146: { uint32_t i, tot = 0;                                  /* readv / writev */
                for (i = 0; i < c; i++) { int k = sys_rw(nr == 146, a, rd32(b + 8 * i), rd32(b + 8 * i + 4));
                    if (k < 0) { if (!tot) tot = (uint32_t)k; break; } tot += (uint32_t)k; if ((uint32_t)k < rd32(b + 8 * i + 4)) break; }
                r = (int32_t)tot; } break;
    case 158: plat_yield(); r = 0; break;                                   /* sched_yield */
    case 154: case 156: r = 0; break;                                           /* sched_setparam/setscheduler: accepted */
    case 155: if (b) wr32(b, 0); r = 0; break;                                  /* sched_getparam: priority 0 */
    case 157: r = 0; break;                                                     /* sched_getscheduler: SCHED_OTHER */
    case 159: r = (a == 1 || a == 2) ? 99 : 0; break;                           /* sched_get_priority_max */
    case 160: r = (a == 1 || a == 2) ? 1 : 0; break;                            /* sched_get_priority_min */
    case 162: sleep_ns((int32_t)rd32(a), (int32_t)rd32(a + 4)); if (b) { wr32(b, 0); wr32(b + 4, 0); } r = 0; break;
    case 267: if (b & 1) { int64_t cs, cns; (a == 0 ? now_real : now_mono)(&cs, &cns);   /* clock_nanosleep TIMER_ABSTIME */
                  sleep_ns((int32_t)rd32(c) - cs, (int32_t)rd32(c + 4) - cns); }
              else sleep_ns((int32_t)rd32(c), (int32_t)rd32(c + 4));
              r = 0; break;
    case 172: r = 0; break;                                                     /* prctl */
    case 180: case 181: { gfd *fd = fd_get(a); int64_t cur, off = ((int64_t)e << 32) | d;   /* pread64/pwrite64 */
                if (!fd || fd->hfd < 0) { r = -L_EBADF; break; }
                cur = p_lseek(fd->hfd, 0, SEEK_CUR); p_lseek(fd->hfd, off, SEEK_SET);
                r = nr == 180 ? p_read(fd->hfd, RT_MEM + b, c) : p_write(fd->hfd, RT_MEM + b, c);
                p_lseek(fd->hfd, cur, SEEK_SET); } break;
    case 183: { uint32_t l = (uint32_t)strlen(g_cwd) + 1; if (l > b) { r = -L_ERANGE; break; } memcpy(RT_MEM + a, g_cwd, l); r = l; } break;
    case 191: case 76: if (a == 3) { wr32(b, 8u << 20); wr32(b + 4, 0xffffffffu); }   /* ugetrlimit */
                       else if (a == 7) { wr32(b, 1024); wr32(b + 4, 4096); }
                       else { wr32(b, 0xffffffffu); wr32(b + 4, 0xffffffffu); }
                       r = 0; break;
    case 340: if (c) { wr32(d, 0); }                                            /* prlimit64 (get) */
              if (d) { uint64_t cur = b == 3 ? (8u << 20) : b == 7 ? 1024 : ~0ull; wr64(d, cur); wr64(d + 8, ~0ull); }
              r = 0; break;
    case 192: r = sys_mmap2(a, b, c, d, (int32_t)e, f6); break;
    case 193: case 194: { gfd *fd = fd_get(a); r = fd && fd->hfd >= 0 ? (p_ftruncate(fd->hfd, ((int64_t)c << 32) | b) ? -L_EIO : 0) : -L_EBADF; } break;
    case 118: case 148: r = 0; break;                                           /* fsync */
    case 220: r = sys_getdents(a, b, c, 0); break;
    case 141: r = sys_getdents(a, b, c, 1); break;
    case 240: r = sys_futex(cpu, a, (int)b, c, d, e, f6); break;
    case 241: r = 0; break;
    case 242: if (b >= 4) { memset(RT_MEM + c, 0, b); wr32(c, 0xf); r = 4; } else r = -L_EINVAL; break;
    case 243: r = sys_set_thread_area(cpu, a); break;
    case 258: self_of(cpu)->clear_child_tid = a; r = self_of(cpu)->tid; break;
    case 311: r = 0; break;                                                     /* set_robust_list */
    case 263: case 264: r = -L_EPERM; break;
    case 265: if (a == 0 || a == 5) now_real(&s, &ns); else now_mono(&s, &ns);
              wr32(b, (uint32_t)s); wr32(b + 4, (uint32_t)ns); r = 0; break;    /* clock_gettime */
    case 266: if (b) { wr32(b, 0); wr32(b + 4, 1); } r = 0; break;              /* clock_getres */
    case 268: memset(RT_MEM + c, 0, b); wr32(c, 0xef53); wr32(c + 4, 4096); r = 0; break; /* statfs64 */
    case 42: case 331: { int p[2]; int r0, r1;                                 /* pipe / pipe2 */ if (p_pipe(p) < 0) { r = -L_EMFILE; break; }
                plat_mutex_lock(&g_fd_cs); r0 = fd_alloc(3); g_fd[r0].hfd = p[0]; r1 = fd_alloc(3); g_fd[r1].hfd = p[1];
                plat_mutex_unlock(&g_fd_cs); wr32(a, r0); wr32(a + 4, r1); r = 0; } break;
    case 2: case 190: case 11: r = -L_ENOSYS; break;                            /* fork vfork execve */
    case 114: case 7: r = -10; break;                                           /* wait: ECHILD */
    default:
        klog("syscall %u (%08x %08x %08x %08x %08x) not implemented\n", nr, a, b, c, d, e);
        r = -L_ENOSYS;
    }
    if (g_verbose && ((nr != 4 && nr != 3) || getenv("JPRT_TRACEIO")) && nr != 240 && nr != 265 && nr != 162)
        klog("syscall %u(%08x, %08x, %08x) = %lld\n", nr, a, b, c, (long long)r);
    cpu->eax = (uint32_t)r;
}

/* ---------------------------------------------------------------- host functions */
/* dlopen/dlsym of replaced libraries -> fake handles and thunk addresses */
#define MAX_DYN 4096
static struct { uint32_t addr; char name[96]; } g_dyn[MAX_DYN];
static int g_ndyn;
static char g_fake_libs[16][64];
static int g_nfake;
static int g_last_fake;
static plat_mutex g_dyn_cs;
static int g_dyn_init;

const char *dyn_thunk_name(uint32_t a)
{
    if (a >= DYN_THUNK_BASE && a < DYN_THUNK_BASE + 16u * MAX_DYN) {
        int i = (int)((a - DYN_THUNK_BASE) / 16);
        if (i < g_ndyn) return g_dyn[i].name;
    }
    return NULL;
}

uint32_t host_thunk_addr(const char *name)
{
    int i;
    if (!g_dyn_init) { g_dyn_init = 1; plat_mutex_init(&g_dyn_cs); }
    plat_mutex_lock(&g_dyn_cs);
    for (i = 0; i < g_ndyn; i++)
        if (!strcmp(g_dyn[i].name, name)) { plat_mutex_unlock(&g_dyn_cs); return g_dyn[i].addr; }
    if (g_ndyn == MAX_DYN) kfatal(NULL, "too many dlsym thunks");
    g_dyn[g_ndyn].addr = DYN_THUNK_BASE + 16u * (uint32_t)g_ndyn;
    snprintf(g_dyn[g_ndyn].name, sizeof(g_dyn[0].name), "%s", name);
    i = g_ndyn++;
    plat_mutex_unlock(&g_dyn_cs);
    return g_dyn[i].addr;
}

static int lifted_lib(const char *name)
{
    const char *b = strrchr(name, '/');
    int i;
    b = b ? b + 1 : name;
    for (i = 0; LIBALIAS[i][0]; i++)
        if (!strcmp(b, LIBALIAS[i][0]) || !strcmp(b, LIBALIAS[i][1])) return 1;
    return !strcmp(b, "libgcc_s.so.1") || lib_base_for(name) != 0;
}

/* arguments of a called guest function: [esp] is the return address */
#define ARG(i) rd32(cpu->esp + 4 + 4 * (i))
#define RET(v) do { cpu->eax = (uint32_t)(v); cpu->esp += 4; return 1; } while (0)

int host_impl(rt_cpu *cpu, const char *name);     /* hostlibs.c: SDL, GL, ... */

int host_call(rt_cpu *cpu, uint32_t target)
{
    const char *name;
    lifted_fn fn;
    if (!g_dyn_init) { g_dyn_init = 1; plat_mutex_init(&g_dyn_cs); }
    name = thunk_name(target);
    if (!name) return 0;
    fn = find_lifted(target);
    if (name[0] == 'd' && !strcmp(name, "dlopen")) {
        const char *lib = gstr(ARG(0));
        if (lib && !lifted_lib(lib)) {
            int i;
            for (i = 0; i < g_nfake && strcmp(g_fake_libs[i], lib); i++) ;
            if (i == g_nfake && g_nfake < 16) snprintf(g_fake_libs[g_nfake++], 64, "%s", lib);
            klog("dlopen(%s): host replacement, handle %08x\n", lib, 0xf0000010u + 16u * i);
            g_last_fake = 1;
            RET(0xf0000010u + 16u * i);
        }
        g_last_fake = 0;
    } else if (name[0] == 'd' && (!strcmp(name, "dlsym") || !strcmp(name, "dlvsym"))) {
        uint32_t h = ARG(0);
        if (h >= 0xf0000010u && h < 0xf0000110u) {
            const char *sym = gstr(ARG(1));
            uint32_t a = host_thunk_addr(sym);
            if (g_verbose) klog("dlsym(%s, %s) = %08x\n", g_fake_libs[(h - 0xf0000010u) / 16], sym, a);
            g_last_fake = 1;
            RET(a);
        }
        g_last_fake = 0;
    } else if (name[0] == 'f' && !strcmp(name, "fclose")) {
        if (!ARG(0)) RET(0xffffffffu);            /* fclose(NULL): EOF instead of a crash */
    } else if (name[0] == 'd' && !strcmp(name, "dlclose")) {
        if (ARG(0) >= 0xf0000010u && ARG(0) < 0xf0000110u) RET(0);
    } else if (name[0] == 'd' && !strcmp(name, "dlerror")) {
        if (g_last_fake) { g_last_fake = 0; RET(0); }
    } else {
        /* function of a replaced library */
        if (host_impl(cpu, name))
            return 1;
        {
            static char seen[1024][64];
            static int nseen;
            int i;
            for (i = 0; i < nseen && strcmp(seen[i], name); i++) ;
            if (i == nseen && nseen < 1024) {
                snprintf(seen[nseen++], 64, "%s", name);
                klog("host: %s not implemented yet (returns 0)\n", name);
            }
        }
        RET(0);
    }
    /* not intercepted: run the real (lifted) function */
    if (!fn) kfatal(cpu, "%s at %08x: not lifted", name, target);
    fn(cpu);
    return 1;
}

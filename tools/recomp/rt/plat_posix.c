/* plat_posix.c — plat.h on Linux / Android (the Quest: arm64) */
#if !defined(_WIN32)
#define _GNU_SOURCE
#include <dirent.h>
#include <dlfcn.h>
#include <errno.h>
#include <limits.h>
#include <linux/futex.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <time.h>
#include <unwind.h>
#include "plat.h"

uint64_t plat_mono_ns(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec;
}

uint64_t plat_thread_cpu_ns(void)
{
    struct timespec t;
    clock_gettime(CLOCK_THREAD_CPUTIME_ID, &t);
    return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec;
}

int64_t plat_real_ns(void)
{
    struct timespec t;
    clock_gettime(CLOCK_REALTIME, &t);
    return (int64_t)t.tv_sec * 1000000000ll + t.tv_nsec;
}

void plat_sleep_ms(uint32_t ms)
{
    struct timespec t = { (time_t)(ms / 1000), (long)(ms % 1000) * 1000000L };
    while (nanosleep(&t, &t) < 0 && errno == EINTR) {}
}

void plat_timer_resolution(void) {}

/* the guest futex word is host memory too: the native futex does the work */
int plat_wait32(volatile uint32_t *addr, uint32_t expected, int64_t timeout_ns)
{
    struct timespec t, *tp = NULL;
    long r;
    if (timeout_ns >= 0) {
        t.tv_sec = (time_t)(timeout_ns / 1000000000ll);
        t.tv_nsec = (long)(timeout_ns % 1000000000ll);
        tp = &t;
    }
    r = syscall(SYS_futex, addr, FUTEX_WAIT_PRIVATE, expected, tp, NULL, 0);
    return r < 0 && errno == ETIMEDOUT ? 1 : 0;
}

void plat_wake32(volatile uint32_t *addr, int all)
{
    syscall(SYS_futex, addr, FUTEX_WAKE_PRIVATE, all ? INT_MAX : 1, NULL, NULL, 0);
}

struct start { void *(*fn)(void *); void *arg; };

static void *thread_entry(void *p)
{
    struct start s = *(struct start *)p;
    free(p);
    return s.fn(s.arg);
}

int plat_thread_start(void *(*fn)(void *), void *arg, size_t stack_bytes)
{
    pthread_t t;
    pthread_attr_t a;
    struct start *s = malloc(sizeof(*s));
    int r;
    s->fn = fn;
    s->arg = arg;
    pthread_attr_init(&a);
    pthread_attr_setstacksize(&a, stack_bytes);
    pthread_attr_setdetachstate(&a, PTHREAD_CREATE_DETACHED);
    r = pthread_create(&t, &a, thread_entry, s);
    pthread_attr_destroy(&a);
    if (r) { free(s); return -1; }
    return 0;
}

void plat_thread_exit(void) { pthread_exit(NULL); }

/* x86 is TSO; the lifted code's plain loads/stores on arm64 are not. Until
 * the lifter emits ordered accesses where it matters, every guest thread
 * runs on one core (a single core is sequentially consistent with itself).
 * JPRT_CORES=all skips this; JPRT_CORE=<n> picks the core (default: the
 * highest, a big core on the Quest's XR2 Gen 2). */
void plat_guest_thread_init(void)
{
#if defined(__aarch64__) || defined(__arm__)
    const char *c = getenv("JPRT_CORES");
    cpu_set_t set;
    long n = sysconf(_SC_NPROCESSORS_CONF);
    int core = getenv("JPRT_CORE") ? atoi(getenv("JPRT_CORE")) : (int)(n > 0 ? n - 1 : 0);
    if (c && !strcmp(c, "all")) return;
    CPU_ZERO(&set);
    CPU_SET(core, &set);
    sched_setaffinity(0, sizeof(set), &set);
#endif
}
uint64_t plat_thread_id(void) { return (uint64_t)gettid(); }

uint8_t *plat_reserve(uint64_t bytes)
{
    void *p = mmap(NULL, bytes, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    return p == MAP_FAILED ? NULL : p;
}

int plat_commit(void *p, size_t bytes) { return mprotect(p, bytes, PROT_READ | PROT_WRITE); }

void plat_decommit(void *p, size_t bytes)
{
    madvise(p, bytes, MADV_DONTNEED);            /* anonymous pages read back as zero */
    mprotect(p, bytes, PROT_NONE);
}

void plat_protect(void *p, size_t bytes, int writable)
{
    mprotect(p, bytes, writable ? PROT_READ | PROT_WRITE : PROT_READ);
}

void plat_exe_dir(char *out, size_t n)
{
    Dl_info i;
    char *s;
    out[0] = 0;
    if (dladdr((void *)plat_exe_dir, &i) && i.dli_fname) snprintf(out, n, "%s", i.dli_fname);
    s = strrchr(out, '/');
    if (s) *s = 0;
}

int plat_is_dir(const char *path)
{
    struct stat st;
    return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

struct plat_dir { DIR *d; char path[1024]; };

plat_dir *plat_opendir(const char *path)
{
    plat_dir *d = malloc(sizeof(*d));
    if (!(d->d = opendir(path))) { free(d); return NULL; }
    snprintf(d->path, sizeof(d->path), "%s", path);
    return d;
}

const char *plat_readdir(plat_dir *d, int *is_dir)
{
    struct dirent *e;
    while ((e = readdir(d->d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        if (is_dir) {
            if (e->d_type == DT_UNKNOWN) {
                char p[1300];
                snprintf(p, sizeof(p), "%s/%s", d->path, e->d_name);
                *is_dir = plat_is_dir(p);
            } else
                *is_dir = e->d_type == DT_DIR;
        }
        return e->d_name;
    }
    return NULL;
}

void plat_closedir(plat_dir *d) { closedir(d->d); free(d); }

int plat_rename(const char *from, const char *to) { return rename(from, to); }

static plat_fault_fn g_fault;

static void on_signal(int sig, siginfo_t *si, void *uc)
{
    int kind = sig == SIGSEGV || sig == SIGBUS ? PLAT_FAULT_ACCESS : sig == SIGFPE ? PLAT_FAULT_DIV0 : PLAT_FAULT_OTHER;
    void *pc = NULL;
#if defined(__aarch64__)
    pc = (void *)((ucontext_t *)uc)->uc_mcontext.pc;
#elif defined(__x86_64__)
    pc = (void *)((ucontext_t *)uc)->uc_mcontext.gregs[REG_RIP];
#endif
    if (g_fault && g_fault(kind, (uintptr_t)si->si_addr, 0, pc)) return;
    signal(sig, SIG_DFL);
    raise(sig);
}

void plat_install_fault_handler(plat_fault_fn fn)
{
    struct sigaction sa;
    static char altstack[1 << 16];
    stack_t ss;
    g_fault = fn;
    ss.ss_sp = altstack;                     /* report stack overflows too */
    ss.ss_size = sizeof(altstack);
    ss.ss_flags = 0;
    sigaltstack(&ss, NULL);
    memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = on_signal;
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGBUS, &sa, NULL);
    sigaction(SIGFPE, &sa, NULL);
    sigaction(SIGILL, &sa, NULL);
}

struct bt { void (*line)(const char *); int n; };

static _Unwind_Reason_Code bt_frame(struct _Unwind_Context *c, void *p)
{
    struct bt *b = p;
    uintptr_t pc = _Unwind_GetIP(c);
    Dl_info i;
    char buf[400];
    if (!pc || b->n++ >= 48) return _URC_END_OF_STACK;
    if (dladdr((void *)pc, &i) && i.dli_fname)
        snprintf(buf, sizeof(buf), "  host frame %2d: %s+%#lx (%s)\n", b->n - 1,
                 strrchr(i.dli_fname, '/') ? strrchr(i.dli_fname, '/') + 1 : i.dli_fname,
                 (unsigned long)(pc - (uintptr_t)i.dli_fbase), i.dli_sname ? i.dli_sname : "?");
    else
        snprintf(buf, sizeof(buf), "  host frame %2d: %#lx\n", b->n - 1, (unsigned long)pc);
    b->line(buf);
    return _URC_NO_REASON;
}

void plat_backtrace(void (*line)(const char *))
{
    struct bt b = { line, 0 };
    _Unwind_Backtrace(bt_frame, &b);
}
#endif

#if !defined(_WIN32)
void plat_yield(void) { sched_yield(); }
#endif

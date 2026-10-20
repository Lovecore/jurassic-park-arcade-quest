/*
 * plat.h — host primitives for the runtime: Windows x64 (development host)
 * and Linux/Android arm64 (the Quest). Everything the emulated kernel needs
 * from the host OS that is not plain C: locks, threads, futex-style waits,
 * time, the guest address-space reservation, files and directories.
 */
#ifndef PLAT_H
#define PLAT_H

#include <stdint.h>
#include <stddef.h>
#include <setjmp.h>

#if defined(_WIN32)
#include <windows.h>
#include <io.h>
#include <direct.h>
#include <sys/stat.h>

typedef CRITICAL_SECTION plat_mutex;
static inline void plat_mutex_init(plat_mutex *m) { InitializeCriticalSection(m); }
static inline void plat_mutex_lock(plat_mutex *m) { EnterCriticalSection(m); }
static inline void plat_mutex_unlock(plat_mutex *m) { LeaveCriticalSection(m); }

/* the bounce trampoline: CRT _setjmp with a NULL frame (no SEH unwind) */
#define plat_setjmp(b) _setjmp(b, NULL)
#define plat_longjmp(b, v) longjmp(b, v)

/* POSIX file API names on the MSVCRT */
#define p_open(p, f, m) _open(p, (f) | _O_BINARY, m)
#define p_close _close
#define p_dup _dup
#define p_read _read
#define p_write _write
#define p_lseek _lseeki64
#define p_access _access
#define p_unlink _unlink
#define p_rmdir _rmdir
#define p_mkdir(p) _mkdir(p)
typedef struct _stat64 p_stat_t;
#define p_stat _stat64
#define p_fstat _fstat64
#define P_O_RDONLY _O_RDONLY
#define P_O_WRONLY _O_WRONLY
#define P_O_RDWR _O_RDWR
#define P_O_CREAT _O_CREAT
#define P_O_TRUNC _O_TRUNC
#define P_O_APPEND _O_APPEND
#define P_O_EXCL _O_EXCL
#define P_MODE (_S_IREAD | _S_IWRITE)
#define P_S_ISDIR(m) (((m) & _S_IFDIR) != 0)

#else /* Linux / Android */
#include <pthread.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>

typedef pthread_mutex_t plat_mutex;
static inline void plat_mutex_init(plat_mutex *m)
{
    pthread_mutexattr_t a;
    pthread_mutexattr_init(&a);
    pthread_mutexattr_settype(&a, PTHREAD_MUTEX_RECURSIVE);   /* like a CRITICAL_SECTION */
    pthread_mutex_init(m, &a);
    pthread_mutexattr_destroy(&a);
}
static inline void plat_mutex_lock(plat_mutex *m) { pthread_mutex_lock(m); }
static inline void plat_mutex_unlock(plat_mutex *m) { pthread_mutex_unlock(m); }

#define plat_setjmp(b) _setjmp(b)          /* no signal-mask save */
#define plat_longjmp(b, v) _longjmp(b, v)

#define p_open(p, f, m) open(p, f, m)
#define p_close close
#define p_dup dup
#define p_read read
#define p_write write
#define p_lseek lseek
#define p_access access
#define p_unlink unlink
#define p_rmdir rmdir
#define p_mkdir(p) mkdir(p, 0777)
typedef struct stat p_stat_t;
#define p_stat stat
#define p_fstat fstat
#define P_O_RDONLY O_RDONLY
#define P_O_WRONLY O_WRONLY
#define P_O_RDWR O_RDWR
#define P_O_CREAT O_CREAT
#define P_O_TRUNC O_TRUNC
#define P_O_APPEND O_APPEND
#define P_O_EXCL O_EXCL
#define P_MODE 0666
#define P_S_ISDIR(m) S_ISDIR(m)
#endif

/* ---- atomics (compiler builtins on both hosts) ---- */
#define plat_atomic_inc(p) __atomic_add_fetch((p), 1, __ATOMIC_SEQ_CST)
#define plat_atomic_dec(p) __atomic_sub_fetch((p), 1, __ATOMIC_SEQ_CST)
#define plat_atomic_xchg(p, v) __atomic_exchange_n((p), (v), __ATOMIC_SEQ_CST)
static inline int plat_atomic_cas(volatile int32_t *p, int32_t expect, int32_t v)
{
    return __atomic_compare_exchange_n(p, &expect, v, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
}

/* ---- time ---- */
uint64_t plat_mono_ns(void);              /* monotonic clock */
int64_t plat_real_ns(void);               /* wall clock, ns since the Unix epoch */
void plat_sleep_ms(uint32_t ms);
void plat_yield(void);
void plat_timer_resolution(void);         /* 1 ms sleeps where the OS needs asking */

/* ---- futex-style waits on a guest word ---- */
/* returns 0 when woken (or the value differed), 1 on timeout; timeout_ns < 0 waits forever */
int plat_wait32(volatile uint32_t *addr, uint32_t expected, int64_t timeout_ns);
void plat_wake32(volatile uint32_t *addr, int all);

/* ---- threads ---- */
int plat_thread_start(void *(*fn)(void *), void *arg, size_t stack_bytes);
void plat_thread_exit(void);
uint64_t plat_thread_id(void);
/* called on every host thread that runs guest code: on weakly ordered hosts
 * (arm64) all guest threads share one core, which keeps x86 memory
 * ordering for the lifted code (JPRT_CORES=all lifts that) */
void plat_guest_thread_init(void);

/* ---- guest address space ---- */
uint8_t *plat_reserve(uint64_t bytes);                 /* inaccessible reservation */
int plat_commit(void *p, size_t bytes);                /* -> zeroed read/write pages */
void plat_decommit(void *p, size_t bytes);             /* contents dropped (zero on recommit) */
void plat_protect(void *p, size_t bytes, int writable);

/* ---- paths and directories ---- */
void plat_exe_dir(char *out, size_t n);                /* directory of the running binary */
int plat_is_dir(const char *path);
typedef struct plat_dir plat_dir;
plat_dir *plat_opendir(const char *path);
/* next entry name (not "." / ".."), NULL at the end; *is_dir set */
const char *plat_readdir(plat_dir *d, int *is_dir);
void plat_closedir(plat_dir *d);
int plat_rename(const char *from, const char *to);     /* replaces an existing target */

/* ---- faults ---- */
/* handler for host faults: kind, faulting address, write flag, host pc.
 * Returns 0 (not handled), 1 (handled: retry the instruction) or 2 (retry
 * it single-stepped, then report PLAT_FAULT_STEP; Windows only, used by
 * the write watchpoint). */
typedef int (*plat_fault_fn)(int kind, uintptr_t addr, int write, void *pc);
enum { PLAT_FAULT_ACCESS, PLAT_FAULT_STACK, PLAT_FAULT_DIV0, PLAT_FAULT_STEP, PLAT_FAULT_OTHER };
void plat_install_fault_handler(plat_fault_fn fn);
void plat_backtrace(void (*line)(const char *));       /* host frames, one line each */

#endif

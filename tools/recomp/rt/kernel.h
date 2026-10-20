/* kernel.h — shared declarations of the emulated Linux i386 process runtime. */
#ifndef KERNEL_H
#define KERNEL_H
#include <stdint.h>
#include <stdarg.h>
#include <setjmp.h>
#include "rt.h"

#define LD_BASE        0xb7760000u     /* ld-2.19.so (free choice: nothing references it) */
#define SYSCALL_ENTRY  0xffffe400u     /* AT_SYSINFO: `call *%gs:0x10` lands here */
#define MMAP_TOP       0xb7100000u     /* anonymous mmaps go below the arcade libraries */
#define DYN_THUNK_BASE 0xe1000000u     /* addresses handed out by dlsym for replaced libs */

/* per-thread kernel state; the rt_cpu must stay the first member */
typedef struct kthread {
    rt_cpu   cpu;
    uint32_t tid;
    uint32_t clear_child_tid;          /* set_tid_address / CLONE_CHILD_CLEARTID */
    uint32_t tls_base[3];              /* GDT TLS entries 6..8 (per thread, like Linux) */
    jmp_buf  bounce_jb;                /* trampoline context (_setjmp with NULL frame: no SEH unwind) */
    uint32_t bounce_to;
} kthread;

/* Run guest code on this host thread from `target` (a function, or with
 * resume set a resume point) through the bounce trampoline; never returns. */
void run_guest(rt_cpu *cpu, uint32_t target, int resume) __attribute__((noreturn));

extern int g_verbose;
extern uint32_t g_brk_start, g_brk;

void klog(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void kfatal(rt_cpu *cpu, const char *fmt, ...) __attribute__((format(printf, 2, 3), noreturn));

int  mem_ok(uint32_t a, uint32_t len);
int  mem_map(uint32_t a, uint32_t len);
int  mem_free(uint32_t a, uint32_t len);
void mem_unmap(uint32_t a, uint32_t len);
uint32_t mem_find(uint32_t len, uint32_t top);

typedef void (*lifted_fn)(rt_cpu *);
lifted_fn find_lifted(uint32_t a);
lifted_fn find_resume(uint32_t a);
const char *thunk_name(uint32_t a);
const char *dyn_thunk_name(uint32_t a);
int is_replaced_lib(const char *name);

/* syscalls.c */
void vfs_init(const char *game_dir);
int  vfs_host_path(const char *guest, char *host, size_t n);
rt_cpu *thread_new_cpu(void);
int  host_call(rt_cpu *cpu, uint32_t target);
uint32_t host_thunk_addr(const char *name);    /* guest address that calls host function `name` */
/* hostlibs.c */
void host_heap_init(void);                     /* reserve the host-owned guest heap */

/* guest strings */
static inline const char *gstr(uint32_t a) { return a ? (const char *)(RT_MEM + a) : NULL; }
#endif

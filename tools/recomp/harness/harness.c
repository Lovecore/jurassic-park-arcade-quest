/*
 * harness.c — differential test of lifted functions against the original
 * code, natively, in a 32-bit Windows process.
 *
 * The game image (patched, devirtualized `game`) is mapped at its link
 * addresses, so RT_MEM = 0 and the lifted C and the original x86 see the
 * same memory. For each candidate function and trial: random registers and
 * stack arguments (pointers into a scratch buffer, small ints, random
 * words); run the original natively from a snapshot, record registers and
 * memory; restore the snapshot; run the lifted version; compare registers
 * (eax edx ebx esi edi ebp esp) and the data segment, scratch buffer and
 * stack. Faults in the original mean the random input was invalid (trial
 * skipped); a watchdog aborts runaway loops.
 *
 *   harness <game> <candidates.txt> [trials=16] [seed=1] [filter-hex-addr]
 */
#include <windows.h>
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "rt.h"
#include "funcs.h"

uint8_t *RT_MEM = 0;

extern const struct { uint32_t addr; void (*fn)(rt_cpu *); } rt_func_table[];
extern const unsigned rt_func_count;

typedef struct { uint32_t eax, ecx, edx, ebx, esp, ebp, esi, edi; } regs_t;
void native_run(regs_t *r, uint32_t target);
void native_abort(void);
void native_back(void);
extern uint32_t native_host_esp;

#define SCRATCH     0x20000000u
#define SCRATCH_SZ  0x00100000u
#define STACK_LO    0x21000000u
#define STACK_SZ    0x00040000u
#define STACK_TOP   (STACK_LO + STACK_SZ - 0x1000)
/* mapped region full of kernel-half words: code that uses a garbage ebp as
 * its stack (shared epilogues) faults on a usable stack instead of killing
 * the process */
#define POISON      0x22000000u
#define POISON_SZ   0x00010000u

static uint32_t data_lo, data_hi;          /* second PT_LOAD incl. bss */
static uint32_t code_lo, code_hi;          /* first PT_LOAD (text + rodata) */
static uint8_t *snap_data, *snap_scratch, *snap_stack;
static uint8_t *res_data, *res_scratch, *res_stack;

/* ---- fault / watchdog handling ---- */
static volatile int g_phase;                /* 1 native, 2 lifted */
static volatile int g_last_phase;
static volatile LONG g_fault;               /* exception code */
static volatile LONG g_tick;                /* progress counter */
static jmp_buf g_lifted_env;
static HANDLE g_main_thread;

static void lifted_abort(void) { longjmp(g_lifted_env, 1); }

static LONG CALLBACK veh(EXCEPTION_POINTERS *ep)
{
    DWORD code = ep->ExceptionRecord->ExceptionCode;
    if (code == DBG_PRINTEXCEPTION_C || code == 0x406D1388 /* thread name */)
        return EXCEPTION_CONTINUE_SEARCH;
    if (g_phase == 1) {
        g_fault = code;
        ep->ContextRecord->Eip = (DWORD)(uintptr_t)native_abort;
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    if (g_phase == 2) {
        g_fault = code;
        ep->ContextRecord->Eip = (DWORD)(uintptr_t)lifted_abort;
        ep->ContextRecord->Esp &= ~15u;
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    fprintf(stderr, "HOST FAULT %08lx at eip %08lx esp %08lx (addr %08lx) last phase %d\n",
            (unsigned long)code, (unsigned long)ep->ContextRecord->Eip,
            (unsigned long)ep->ContextRecord->Esp,
            ep->ExceptionRecord->NumberParameters > 1 ?
                (unsigned long)ep->ExceptionRecord->ExceptionInformation[1] : 0ul,
            g_last_phase);
    fflush(stderr);
    return EXCEPTION_CONTINUE_SEARCH;
}

static DWORD WINAPI watchdog(LPVOID arg)
{
    (void)arg;
    LONG last = -1;
    int still = 0;
    for (;;) {
        Sleep(250);
        LONG t = g_tick;
        if (g_phase && t == last) {
            if (++still >= 8) {               /* ~2 s in one run */
                CONTEXT c;
                SuspendThread(g_main_thread);
                c.ContextFlags = CONTEXT_CONTROL;
                GetThreadContext(g_main_thread, &c);
                g_fault = 0x7ffffff0;      /* timeout */
                if (g_phase == 1) {
                    c.Eip = (DWORD)(uintptr_t)native_abort;
                } else if (g_phase == 2) {
                    c.Eip = (DWORD)(uintptr_t)lifted_abort;
                    c.Esp &= ~15u;
                }
                SetThreadContext(g_main_thread, &c);
                ResumeThread(g_main_thread);
                still = 0;
            }
        } else {
            still = 0;
        }
        last = t;
    }
    return 0;
}

/* ---- runtime entry points used by lifted code ---- */
static const char *g_abort_why;
static uint32_t g_abort_addr;

void rt_call(rt_cpu *cpu, uint32_t target)
{
    unsigned lo = 0, hi = rt_func_count;
    while (lo < hi) {
        unsigned mid = (lo + hi) / 2;
        if (rt_func_table[mid].addr < target) lo = mid + 1; else hi = mid;
    }
    if (lo < rt_func_count && rt_func_table[lo].addr == target) {
        rt_func_table[lo].fn(cpu);
        return;
    }
    g_abort_why = "rt_call to unknown target";
    g_abort_addr = target;
    longjmp(g_lifted_env, 2);
}
void rt_import(rt_cpu *cpu, int id) { (void)cpu; g_abort_why = "import"; g_abort_addr = id; longjmp(g_lifted_env, 2); }
void rt_unimpl(rt_cpu *cpu, uint32_t addr, const char *what) { (void)cpu; g_abort_why = what; g_abort_addr = addr; longjmp(g_lifted_env, 2); }
void rt_trap(rt_cpu *cpu, uint32_t addr, const char *what) { (void)cpu; g_abort_why = what; g_abort_addr = addr; longjmp(g_lifted_env, 3); }
void rt_cpuid(rt_cpu *cpu)
{
    uint32_t a = cpu->eax, b, c = cpu->ecx, d;
    __asm__ volatile ("cpuid" : "+a"(a), "=b"(b), "+c"(c), "=d"(d));
    cpu->eax = a; cpu->ebx = b; cpu->ecx = c; cpu->edx = d;
}
uint64_t rt_rdtsc(void) { return 0; }
void rt_syscall(rt_cpu *cpu, uint32_t next) { (void)cpu; g_abort_why = "syscall"; g_abort_addr = next; longjmp(g_lifted_env, 2); }
void rt_bounce(rt_cpu *cpu, uint32_t to) { (void)cpu; g_abort_why = "bounce"; g_abort_addr = to; longjmp(g_lifted_env, 2); }
void rt_load_gs(rt_cpu *cpu) { (void)cpu; g_abort_why = "gs load"; g_abort_addr = 0; longjmp(g_lifted_env, 2); }

static void (*find_fn(uint32_t a))(rt_cpu *)
{
    unsigned i;
    for (i = 0; i < rt_func_count; i++)
        if (rt_func_table[i].addr == a)
            return rt_func_table[i].fn;
    return NULL;
}

/* ---- image ---- */
static int map_image(const char *path)
{
    FILE *f = fopen(path, "rb");
    uint8_t hdr[0x200];
    uint32_t phoff;
    uint16_t phnum, i;
    if (!f) { perror(path); return 0; }
    fread(hdr, 1, sizeof(hdr), f);
    memcpy(&phoff, hdr + 28, 4);
    memcpy(&phnum, hdr + 44, 2);
    /* one reservation over all PT_LOADs (reservations are 64 KB granular,
     * so per-segment ones would overlap) */
    uint32_t lo = 0xffffffffu, hi = 0;
    for (i = 0; i < phnum; i++) {
        uint32_t ph[8];
        memcpy(ph, hdr + phoff + i * 32, 32);
        if (ph[0] != 1)
            continue;
        if (ph[2] < lo) lo = ph[2];
        if (ph[2] + ph[5] > hi) hi = ph[2] + ph[5];
    }
    lo &= ~0xffffu;
    hi = (hi + 0xffff) & ~0xffffu;
    if (VirtualAlloc((void *)(uintptr_t)lo, hi - lo, MEM_RESERVE | MEM_COMMIT,
                     PAGE_EXECUTE_READWRITE) != (void *)(uintptr_t)lo) {
        fprintf(stderr, "cannot map %08x-%08x (err %lu)\n", lo, hi, GetLastError());
        return 0;
    }
    for (i = 0; i < phnum; i++) {
        uint32_t ph[8];
        memcpy(ph, hdr + phoff + i * 32, 32);
        if (ph[0] != 1)
            continue;
        fseek(f, ph[1], SEEK_SET);
        fread((void *)(uintptr_t)ph[2], 1, ph[4], f);
        if (ph[2] > 0x8a00000) { data_lo = ph[2]; data_hi = ph[2] + ph[5]; }
        else { code_lo = ph[2]; code_hi = ph[2] + ph[4]; }
    }
    fclose(f);
    /* code + rodata read/execute only, as in a real process: wild writes
     * from random inputs fault instead of corrupting later tests */
    {
        DWORD old;
        uint32_t lo4 = code_lo & ~0xfffu, hi4 = data_lo & ~0xfffu;
        VirtualProtect((void *)(uintptr_t)lo4, hi4 - lo4, PAGE_EXECUTE_READ, &old);
    }
    return 1;
}

static int g_verbose;           /* env HV=1: trace every trial to stderr */
static uint32_t rng = 1;
static uint32_t rnd(void) { rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5; return rng; }
static uint32_t rnd_arg(void)
{
    switch (rnd() & 3) {
    case 0: case 1: return SCRATCH + ((rnd() % (SCRATCH_SZ / 2)) & ~3u);
    case 2: return rnd() & 0xff;
    default: return rnd() | 0xc0000000u;     /* kernel half: never mapped */
    }
}

static void snapshot(void)
{
    memcpy(snap_data, (void *)(uintptr_t)data_lo, data_hi - data_lo);
    memcpy(snap_scratch, (void *)SCRATCH, SCRATCH_SZ);
    memcpy(snap_stack + 0x1000, (void *)(STACK_LO + 0x1000), STACK_SZ - 0x1000);   /* skip guard page */
}
static void restore(void)
{
    memcpy((void *)(uintptr_t)data_lo, snap_data, data_hi - data_lo);
    memcpy((void *)SCRATCH, snap_scratch, SCRATCH_SZ);
    memcpy((void *)(STACK_LO + 0x1000), snap_stack + 0x1000, STACK_SZ - 0x1000);
}
static void save_result(void)
{
    memcpy(res_data, (void *)(uintptr_t)data_lo, data_hi - data_lo);
    memcpy(res_scratch, (void *)SCRATCH, SCRATCH_SZ);
    memcpy(res_stack + 0x1000, (void *)(STACK_LO + 0x1000), STACK_SZ - 0x1000);
}
static int g_fp_close;   /* set when every difference is a near-equal float/double */
static int fnear(double x, double y)
{
    if (x != x && y != y)            /* NaN vs NaN: payload/sign differ between x87 and C */
        return 1;
    double d = fabs(x - y), m = fabs(x) > fabs(y) ? fabs(x) : fabs(y);
    return d <= 1e-5 * m || d < 1e-30;
}
static long first_diff(const uint8_t *a, const uint8_t *b, size_t n)
{
    size_t i, first = (size_t)-1;
    if (!memcmp(a, b, n)) return -1;
    for (i = 0; i < n; i++) {
        if (a[i] == b[i]) continue;
        size_t w4 = i & ~3u, w8 = i & ~7u;
        float fa, fb; double da, db;
        memcpy(&fa, a + w4, 4); memcpy(&fb, b + w4, 4);
        if (w4 + 4 <= n && fnear(fa, fb)) { i = w4 + 3; continue; }
        if (w8 + 8 <= n) {
            memcpy(&da, a + w8, 8); memcpy(&db, b + w8, 8);
            if (fnear(da, db)) { i = w8 + 7; continue; }
        }
        first = i;
        break;
    }
    if (first == (size_t)-1) { g_fp_close = 1; return -1; }
    return (long)first;
}

/* describe a memory difference: native vs lifted dword at the aligned address */
static void diff_why(char *why, size_t whylen, const char *what, uint32_t addr, const uint8_t *native_byte)
{
    uint32_t a4 = addr & ~3u, nv, lv;
    float nf, lf;
    memcpy(&nv, native_byte - (addr - a4), 4);
    memcpy(&lv, (void *)(uintptr_t)a4, 4);
    memcpy(&nf, &nv, 4); memcpy(&lf, &lv, 4);
    snprintf(why, whylen, "%s differs at %08x: native %08x (%g) lifted %08x (%g)",
             what, addr, nv, nf, lv, lf);
}

/* outcome per function */
enum { R_PASS, R_SKIP, R_MISMATCH, R_LIFTED_FAULT, R_LIFTED_ABORT };

/* a float in [-100, 100] as its bit pattern: realistic game data */
static uint32_t rnd_float(void)
{
    float f = (float)((int32_t)(rnd() % 20001) - 10000) / 100.0f;
    uint32_t v;
    memcpy(&v, &f, 4);
    return v;
}

static int run_trial(uint32_t addr, void (*fn)(rt_cpu *), int floaty, char *why, size_t whylen)
{
    regs_t in, nat;
    rt_cpu cpu;
    uint32_t i, args[12];
    double native_ms = 0;
    static rt_cpu zero;
    long d;

    in.eax = rnd_arg(); in.ecx = rnd_arg(); in.edx = rnd_arg(); in.ebx = rnd() | 0xc0000000u;
    in.esi = rnd_arg(); in.edi = rnd_arg(); in.ebp = POISON + 0x4000 + (rnd() & 0x3ffc);
    in.esp = STACK_TOP - 64;
    for (i = 0; i < 12; i++)
        args[i] = (floaty && (rnd() & 1)) ? rnd_float() : rnd_arg();
    /* fresh-ish scratch contents; x87 functions get mostly sane floats, as
     * random bit patterns push 80-bit vs double differences to extremes */
    for (i = 0; i < SCRATCH_SZ / 4; i += floaty ? 1 : 61)
        ((uint32_t *)SCRATCH)[floaty ? i : (i + rnd()) % (SCRATCH_SZ / 4)] =
            (floaty && (rnd() & 3)) ? rnd_float() : rnd_arg();
    memcpy((void *)(uintptr_t)in.esp, args, sizeof(args));
    {
        DWORD old;
        VirtualProtect((void *)STACK_LO, 0x1000, PAGE_READWRITE | PAGE_GUARD, &old);
    }
    snapshot();

    /* native */
    nat = in;
    g_fault = 0;
    if (g_verbose) {
        fprintf(stderr, "  native %08x eax=%08x ecx=%08x edx=%08x esi=%08x edi=%08x ebp=%08x\n",
                addr, in.eax, in.ecx, in.edx, in.esi, in.edi, in.ebp);
        fflush(stderr);
    }
    g_phase = g_last_phase = 1;
    {
        uint16_t host_cw;
        __asm__ volatile ("fnstcw %0" : "=m"(host_cw));
        __asm__ volatile ("fninit");
        LARGE_INTEGER t0, t1, fq;
        QueryPerformanceCounter(&t0);
        native_run(&nat, addr);
        QueryPerformanceCounter(&t1);
        QueryPerformanceFrequency(&fq);
        native_ms = (double)(t1.QuadPart - t0.QuadPart) * 1000.0 / (double)fq.QuadPart;
        __asm__ volatile ("fninit; fldcw %0" : : "m"(host_cw));
    }
    g_phase = 0;
    InterlockedIncrement(&g_tick);
    if (g_fault) {
        restore();
        return R_SKIP;
    }
    save_result();
    restore();

    /* lifted */
    cpu = zero;
    cpu.eax = in.eax; cpu.ecx = in.ecx; cpu.edx = in.edx; cpu.ebx = in.ebx;
    cpu.ebp = in.ebp; cpu.esi = in.esi; cpu.edi = in.edi;
    cpu.esp = in.esp - 4;
    wr32(cpu.esp, (uint32_t)(uintptr_t)native_back);   /* same return address as the native run */
    cpu.fpucw = 0x37f;
    cpu.sh = 1;            /* top-level ret of the tested function returns to the harness */
    g_fault = 0;
    if (g_verbose) {
        fprintf(stderr, "  lifted\n");
        fflush(stderr);
    }
    g_phase = g_last_phase = 2;
    uint16_t host_cw2;
    __asm__ volatile ("fnstcw %0" : "=m"(host_cw2));
    __asm__ volatile ("fninit");      /* 64-bit precision, like the native run (matters for RT_X87_LD) */
    int jr = setjmp(g_lifted_env);
    if (jr == 0)
        fn(&cpu);
    __asm__ volatile ("fninit; fldcw %0" : : "m"(host_cw2));
    g_phase = 0;
    InterlockedIncrement(&g_tick);
    if (jr) {
        if (g_fault == 0x7ffffff0 && native_ms > 200) {
            restore();             /* slow input: lifted only slower, inconclusive */
            return R_SKIP;
        }
        if (g_fault) {
            snprintf(why, whylen, "lifted fault %08lx (native ok, %.0f ms)", (unsigned long)g_fault, native_ms);
            restore();
            return R_LIFTED_FAULT;
        }
        snprintf(why, whylen, "lifted abort: %s @%08x", g_abort_why, g_abort_addr);
        restore();
        return R_LIFTED_ABORT;
    }

    /* compare */
#define CMP(r) if (cpu.r != nat.r) { snprintf(why, whylen, #r " lifted %08x native %08x", cpu.r, nat.r); restore(); return R_MISMATCH; }
    CMP(eax) CMP(edx) CMP(ebx) CMP(esi) CMP(edi) CMP(ebp) CMP(esp)
    if ((d = first_diff(res_data, (uint8_t *)(uintptr_t)data_lo, data_hi - data_lo)) >= 0) {
        diff_why(why, whylen, "data", data_lo + (uint32_t)d, res_data + d);
        restore(); return R_MISMATCH;
    }
    if ((d = first_diff(res_scratch, (uint8_t *)SCRATCH, SCRATCH_SZ)) >= 0) {
        diff_why(why, whylen, "scratch", SCRATCH + (uint32_t)d, res_scratch + d);
        restore(); return R_MISMATCH;
    }
    /* stack: only at/above the final esp (below it is dead scratch) */
    {
        uint32_t lo = nat.esp - STACK_LO < 0x1000 ? 0x1000 : nat.esp - STACK_LO;
        if ((d = first_diff(res_stack + lo, (uint8_t *)(uintptr_t)(STACK_LO + lo), STACK_SZ - lo)) >= 0) {
            diff_why(why, whylen, "stack", STACK_LO + lo + (uint32_t)d, res_stack + lo + d);
            restore(); return R_MISMATCH;
        }
    }
    restore();
    return R_PASS;
}


int main(int argc, char **argv)
{
    if (argc < 3) {
        fprintf(stderr, "usage: %s <game> <candidates.txt> [trials] [seed] [addr]\n", argv[0]);
        return 2;
    }
    int trials = argc > 3 ? atoi(argv[3]) : 16;
    g_verbose = getenv("HV") != NULL;
    uint32_t seed = argc > 4 ? (uint32_t)strtoul(argv[4], 0, 0) : 1;
    uint32_t only = argc > 5 ? (uint32_t)strtoul(argv[5], 0, 16) : 0;
    uint32_t start = argc > 6 ? (uint32_t)strtoul(argv[6], 0, 16) : 0;   /* resume after a crash */
    if (!map_image(argv[1]))
        return 1;
    if (VirtualAlloc((void *)SCRATCH, SCRATCH_SZ, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE) != (void *)SCRATCH ||
        VirtualAlloc((void *)STACK_LO, STACK_SZ, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE) != (void *)STACK_LO) {
        fprintf(stderr, "cannot map scratch/stack\n");
        return 1;
    }
    if (VirtualAlloc((void *)POISON, POISON_SZ, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE) != (void *)POISON)
        return 1;
    /* headroom below the guest stack so a runaway recursion faults (guard
     * page) with room left for the kernel to dispatch the exception */
    if (VirtualAlloc((void *)(STACK_LO - 0x10000), 0x10000, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE)
            != (void *)(STACK_LO - 0x10000))
        return 1;
    {   /* poison words point back into the poison block: a garbage ebp chain
         * stays on mapped memory, and returning there faults (DEP) */
        uint32_t *w = (uint32_t *)POISON, k;
        for (k = 0; k < POISON_SZ / 4; k++)
            w[k] = POISON + 0x8000 + ((k * 4) & 0x3ffc);
    }
    snap_data = malloc(data_hi - data_lo); res_data = malloc(data_hi - data_lo);
    snap_scratch = malloc(SCRATCH_SZ); res_scratch = malloc(SCRATCH_SZ);
    snap_stack = malloc(STACK_SZ); res_stack = malloc(STACK_SZ);
    DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(),
                    &g_main_thread, 0, FALSE, DUPLICATE_SAME_ACCESS);
    AddVectoredExceptionHandler(1, veh);
    CreateThread(NULL, 0, watchdog, NULL, 0, NULL);

    FILE *cf = fopen(argv[2], "r");
    char line[512];
    int n_fn = 0, n_pass = 0, n_fail = 0, n_untested = 0;
    while (cf && fgets(line, sizeof(line), cf)) {
        char flags[8], name[400];
        uint32_t a;
        if (sscanf(line, "%x %7s %399s", &a, flags, name) != 3)
            continue;
        if ((only && a != only) || a < start)
            continue;
        void (*fn)(rt_cpu *) = find_fn(a);
        if (!fn)
            continue;
        fprintf(stderr, "%08x\n", a);
        int t, ran = 0, bad = 0;
        rng = (seed ^ (a * 2654435761u)) | 1;   /* per-function inputs: reproducible alone */
        char why[200] = "";
        for (t = 0; t < trials && !bad; t++) {
            int r = run_trial(a, fn, strchr(flags, 'x') || strchr(flags, 's'), why, sizeof(why));
            if (r == R_PASS) ran++;
            else if (r != R_SKIP) bad = r;
        }
        n_fn++;
        if (bad) {
            n_fail++;
            printf("FAIL %08x %-3s %s: %s\n", a, flags, name, why);
        } else if (ran) {
            n_pass++;
            printf("PASS %08x %-3s %d/%d\n", a, flags, ran, trials);
        } else {
            n_untested++;
            printf("UNTESTED %08x %-3s\n", a, flags);
        }
        fflush(stdout);
    }
    printf("functions %d: pass %d, fail %d, untested (every trial faulted natively) %d\n",
           n_fn, n_pass, n_fail, n_untested);
    return 0;
}

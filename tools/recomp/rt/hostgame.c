/*
 * hostgame.c — game functions the runtime implements instead of running
 * the lifted code (listed in tools/recomp/host_funcs.txt). Ported from the
 * Linux shim (vr/shim/shim.c, io_emu.c):
 *
 *  - Sentinel HASP HL client API (statically linked into the game): an
 *    emulated dongle. Dongle memory is backed by <game>/pmuser/hasp_mem.bin;
 *    main memory [0x1e] = 1 (initialized), [0x1d] = cabinet type 4
 *    (upright; type 0 sends the boot to operator setup instead of attract).
 *    hasp_decrypt cannot be computed without the key's AES secret, but the
 *    dump ships the answers: programs_dec/ holds the plaintext of every
 *    g5/dino/data/programs_enc/ file, matched by (length, FNV-1a 64).
 *  - Raw Thrills RIO I/O board: connected, every report succeeds, so
 *    switches and ADCs read the (zeroed) report buffer. VR input will write
 *    into that buffer (layout in vr/shim/io_emu.c).
 *
 * Called with the guest stack as on i386 ([esp] = return address).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "rt.h"
#include "kernel.h"
#include "plat.h"

#define ARG(i) rd32(cpu->esp + 4 + 4 * (i))
#define RET(v) do { cpu->eax = (uint32_t)(v); cpu->esp += 4; return; } while (0)

uint32_t guest_alloc(uint32_t n);
uint32_t guest_strdup(const char *s);

/* ---- HASP ---- */
#define HASP_MEM_SIZE 4096
static uint8_t g_hasp_mem[4][HASP_MEM_SIZE];
static char g_hasp_path[1100];
static plat_mutex g_hasp_cs;

static int hasp_file_index(uint32_t fileid)
{
    switch (fileid) {
    case 0xfff4: return 0;
    case 0xfff5: return 1;
    case 0: return 2;
    default: return 3;            /* 0xfff2: main memory */
    }
}

typedef struct { uint32_t len; uint64_t hash; char *plain; } cipher_entry;
static cipher_entry *g_cipher;
static size_t g_ncipher, g_ccap;

static uint64_t fnv1a64(const uint8_t *p, size_t n)
{
    uint64_t h = 1469598103934665603ULL;
    size_t i;
    for (i = 0; i < n; i++) h = (h ^ p[i]) * 1099511628211ULL;
    return h;
}

static void index_dir(const char *enc, const char *dec)
{
    plat_dir *dir = plat_opendir(enc);
    const char *name;
    int is_dir;
    if (!dir) return;
    while ((name = plat_readdir(dir, &is_dir))) {
        char e[1200], d[1200];
        snprintf(e, sizeof(e), "%s/%s", enc, name);
        snprintf(d, sizeof(d), "%s/%s", dec, name);
        if (is_dir) {
            index_dir(e, d);
        } else {
            FILE *f = fopen(e, "rb");
            long n = 0;
            uint8_t *b = NULL;
            if (f) { fseek(f, 0, SEEK_END); n = ftell(f); fseek(f, 0, SEEK_SET); }
            if (f && n > 0 && (b = malloc((size_t)n)) && fread(b, 1, (size_t)n, f) == (size_t)n) {
                if (g_ncipher == g_ccap) {
                    g_ccap = g_ccap ? 2 * g_ccap : 2048;
                    g_cipher = realloc(g_cipher, g_ccap * sizeof(*g_cipher));
                }
                g_cipher[g_ncipher].len = (uint32_t)n;
                g_cipher[g_ncipher].hash = fnv1a64(b, (size_t)n);
                g_cipher[g_ncipher].plain = strdup(d);
                g_ncipher++;
            }
            if (f) fclose(f);
            free(b);
        }
    }
    plat_closedir(dir);
}

static void hasp_init(void)
{
    static int done;
    char enc[1100], dec[1100];
    FILE *f;
    uint8_t *main_mem;
    if (done) return;
    done = 1;
    plat_mutex_init(&g_hasp_cs);
    vfs_host_path("/pm/pmuser/hasp_mem.bin", g_hasp_path, sizeof(g_hasp_path));
    if ((f = fopen(g_hasp_path, "rb"))) {
        size_t n = fread(g_hasp_mem, 1, sizeof(g_hasp_mem), f);
        fclose(f);
        klog("hasp: loaded %zu bytes from %s\n", n, g_hasp_path);
    }
    main_mem = g_hasp_mem[hasp_file_index(0xfff2)];
    if (getenv("JPRT_CABTYPE") || main_mem[0x1d] == 0) {
        main_mem[0x1e] = 1;
        main_mem[0x1d] = (uint8_t)(getenv("JPRT_CABTYPE") ? atoi(getenv("JPRT_CABTYPE")) : 4);
        klog("hasp: cabinet type %d\n", main_mem[0x1d]);
    }
    vfs_host_path("/pm/g5/dino/data/programs_enc", enc, sizeof(enc));
    vfs_host_path("/pm/programs_dec", dec, sizeof(dec));
    index_dir(enc, dec);
    klog("hasp: %zu encrypted program files indexed\n", g_ncipher);
}

static void hasp_save(void)
{
    FILE *f = fopen(g_hasp_path, "wb");
    if (!f) return;
    fwrite(g_hasp_mem, 1, sizeof(g_hasp_mem), f);
    fclose(f);
}

static void hasp_decrypt(uint32_t buf, uint32_t len)
{
    uint64_t h;
    size_t i;
    if (!buf || !len || !g_ncipher) return;
    h = fnv1a64(RT_P(buf), len);
    for (i = 0; i < g_ncipher; i++) {
        FILE *f;
        if (g_cipher[i].len != len || g_cipher[i].hash != h) continue;
        if ((f = fopen(g_cipher[i].plain, "rb"))) {
            size_t n = fread(RT_P(buf), 1, len, f);
            fclose(f);
            if (n == len) return;
        }
        klog("hasp_decrypt: plaintext unreadable: %s\n", g_cipher[i].plain);
        return;
    }
    {
        static int misses;
        if (misses++ < 10) klog("hasp_decrypt: no plaintext for a %u-byte buffer\n", len);
    }
}

/* ---- job system worker (0x835c870) ----
 * The game's worker thread pops jobs from a queue and runs them. The
 * cabinet build polls that queue nonstop (try-lock, check, unlock: a whole
 * core, and it hammers the lock the main thread needs). This is the same
 * loop with a sleep while the queue is empty: the main thread runs any
 * jobs still queued itself when it flushes, so a sleeping worker never
 * stalls it. Queue functions: pop 0x8460e55 (8 = empty), push 0x8460d69
 * (7 = full); job: [0] state (1 running, 2 done), [0x10] fn, [0x14] arg. */
#define JOB_QUEUE 0x8e7d0c0u
#define JOB_DONE_QUEUE 0x8e7d060u
#define QUEUE_POP 0x8460e55u
#define QUEUE_PUSH 0x8460d69u
#define QUEUE_COUNT 0x2cu                     /* items queued (maintained under its lock) */
#define JOB_POOL 0x8e7d094u                   /* job records; +8: jobs submitted since the last flush */
#define JOB_POOL_ALLOC 0x8461811u
#define JOB_POOL_RESET 0x8461a6cu
#define JOB_NEXT_ID 0x8b7f53cu

uint32_t guest_call(rt_cpu *cpu, uint32_t fn, int n, const uint32_t *args);

/* host-side wake-ups (the game's version polls instead) */
static volatile uint32_t g_job_seq, g_done_seq;

static int queue_pop(rt_cpu *cpu, uint32_t q, uint32_t slot)
{
    uint32_t a[2] = { q, slot };
    wr32(slot, 0);
    return (int)guest_call(cpu, QUEUE_POP, 2, a);
}

/* run one popped job and post it to the done queue */
static void job_run(rt_cpu *cpu, uint32_t job, uint32_t slot)
{
    uint32_t r;
    wr32(job, 1);
    if (rd32(job + 0x10)) {
        uint32_t arg = rd32(job + 0x14);
        guest_call(cpu, rd32(job + 0x10), 1, &arg);
    }
    wr32(job, 2);
    do {
        uint32_t b[2] = { JOB_DONE_QUEUE, slot };
        r = guest_call(cpu, QUEUE_PUSH, 2, b);
    } while (r == 7);                         /* full: retry, as the game does */
    __atomic_add_fetch(&g_done_seq, 1, __ATOMIC_SEQ_CST);
    plat_wake32(&g_done_seq, 1);
}

/* JobWorker(arg): arg 0 = run what is queued and return; else forever */
static void job_worker(rt_cpu *cpu)
{
    uint32_t once = ARG(0) == 0, esp0 = cpu->esp;
    uint32_t slot = (esp0 - 64) & ~15u;       /* the loop's local (job pointer) */
    cpu->esp = slot - 16;                     /* guest_call frames go below it */
    for (;;) {
        uint32_t seq = __atomic_load_n(&g_job_seq, __ATOMIC_SEQ_CST), job;
        int r = queue_pop(cpu, JOB_QUEUE, slot);
        if (r == 8) {                         /* empty: sleep until a submit */
            if (once) break;
            if (!rd32(JOB_QUEUE + QUEUE_COUNT)) plat_wait32(&g_job_seq, seq, 5000000);
            continue;
        }
        if (r != 0 || !(job = rd32(slot))) continue;
        job_run(cpu, job, slot);
    }
    cpu->esp = esp0;
}

/* JobSubmit(fn, arg, tag) -> job id */
static uint32_t job_submit(rt_cpu *cpu)
{
    uint32_t fn = ARG(0), arg = ARG(1), tag = ARG(2), esp0 = cpu->esp, rec, id, node, r, k;
    if (!fn) return 0;
    id = rd32(JOB_NEXT_ID) + 1;
    wr32(JOB_NEXT_ID, id);
    rec = (esp0 - 64) & ~15u;                 /* the 24-byte record, copied into the pool */
    cpu->esp = rec - 16;
    for (k = 0; k < 24; k += 4) wr32(rec + k, 0);
    wr32(rec + 4, id);
    wr32(rec + 8, tag);
    wr32(rec + 0x10, fn);
    wr32(rec + 0x14, arg);
    {
        uint32_t a[3] = { JOB_POOL, rec, 24 };
        node = guest_call(cpu, JOB_POOL_ALLOC, 3, a);
    }
    if (node) {
        do {
            uint32_t b[2] = { JOB_QUEUE, rd32(node + 8) };
            r = guest_call(cpu, QUEUE_PUSH, 2, b);
        } while (r == 7);
        __atomic_add_fetch(&g_job_seq, 1, __ATOMIC_SEQ_CST);
        plat_wake32(&g_job_seq, 0);
    }
    cpu->esp = esp0;
    return id;
}

/* JobFlush(help): help != 0 runs queued jobs on this thread too; then waits
 * for every submitted job, drains the done queue and resets the pool */
static void job_flush(rt_cpu *cpu, int then_call)
{
    uint32_t help = ARG(0), esp0 = cpu->esp, slot = (esp0 - 64) & ~15u, job;
    cpu->esp = slot - 16;
    while (help) {
        int r = queue_pop(cpu, JOB_QUEUE, slot);
        if (r == 8) break;
        if (r != 0 || !(job = rd32(slot))) continue;
        job_run(cpu, job, slot);
    }
    for (;;) {                                /* the worker's last jobs */
        uint32_t seq = __atomic_load_n(&g_done_seq, __ATOMIC_SEQ_CST);
        if (rd32(JOB_DONE_QUEUE + QUEUE_COUNT) == rd32(JOB_POOL + 8)) break;
        plat_wait32(&g_done_seq, seq, 2000000);
    }
    while (queue_pop(cpu, JOB_DONE_QUEUE, slot) != 8) {}
    {
        uint32_t a = JOB_POOL;
        guest_call(cpu, JOB_POOL_RESET, 1, &a);
    }
    if (then_call) guest_call(cpu, then_call, 0, NULL);
    cpu->esp = esp0;
}

/* ---- RIO ---- */
#define RIO_CONNECTED_FLAG 0x8e7d61cu

void rt_host_fn(rt_cpu *cpu, uint32_t addr)
{
    hasp_init();
    switch (addr) {
    case 0x8529650:                                   /* hasp_login(feature, vendor_code, *handle) */
        klog("hasp_login(feature %u)\n", ARG(0));
        if (ARG(2)) wr32(ARG(2), 0x100 + (ARG(0) & 0xff));
        RET(0);
    case 0x85295f0: RET(0);                           /* hasp_logout */
    case 0x8529550: RET(0);                           /* hasp_encrypt: no-op */
    case 0x85294d0: hasp_decrypt(ARG(1), ARG(2)); RET(0);
    case 0x8528520: case 0x8528470: {                 /* hasp_read / hasp_write(h, fileid, off, len, buf) */
        uint32_t fileid = ARG(1), off = ARG(2), len = ARG(3), buf = ARG(4);
        uint8_t *m = g_hasp_mem[hasp_file_index(fileid)];
        if (!buf || off > HASP_MEM_SIZE || len > HASP_MEM_SIZE - off) RET(9);   /* HASP_MEM_RANGE */
        plat_mutex_lock(&g_hasp_cs);
        if (addr == 0x8528520) memcpy(RT_P(buf), m + off, len);
        else { memcpy(m + off, RT_P(buf), len); hasp_save(); }
        plat_mutex_unlock(&g_hasp_cs);
        RET(0);
    }
    case 0x85283d0: if (ARG(2)) wr32(ARG(2), HASP_MEM_SIZE); RET(0);   /* hasp_get_size */
    case 0x8528350: if (ARG(1)) wr64(ARG(1), (uint64_t)time(NULL)); RET(0);
    case 0x8528670: {                                 /* hasp_get_sessioninfo(h, fmt, char **info) */
        char b[300];
        snprintf(b, sizeof(b), "<?xml version=\"1.0\" encoding=\"UTF-8\" ?>\n<hasp_info>\n  <keyspec>\n"
                 "    <hasp>\n      <haspid>%s</haspid>\n      <typename>HASP HL</typename>\n"
                 "    </hasp>\n  </keyspec>\n</hasp_info>\n",
                 getenv("JPRT_HASPID") ? getenv("JPRT_HASPID") : "1234567890");
        if (!ARG(2)) RET(501);
        wr32(ARG(2), guest_strdup(b));
        RET(0);
    }
    case 0x85282a0: RET(0);                           /* hasp_free */
    case 0x836b128: case 0x836b1ed:                   /* RIO_Connect(Ex) */
        wr32(RIO_CONNECTED_FLAG, 1);
        klog("RIO_Connect: emulated board connected\n");
        RET(0);
    case 0x836b2a6: wr32(RIO_CONNECTED_FLAG, 0); RET(0);
    case 0x836b33a: case 0x836b4bd: RET(0);           /* RIO_SendReport / RIO_GetReport */
    case 0x835c870: job_worker(cpu); RET(0);          /* JobWorker(arg) */
    case 0x835ca20: RET(job_submit(cpu));             /* JobSubmit(fn, arg, tag) */
    case 0x835caf0: job_flush(cpu, 0); RET(0);        /* JobFlush(help) */
    case 0x835cbb0: job_flush(cpu, 0x835d3f0); RET(0); /* JobFlush + 0x835d3f0() */
    default:
        kfatal(cpu, "rt_host_fn: no host implementation for %08x", addr);
    }
}

/* ---- input maps ----
 * main() clears a developer flag (0x8db4e38) that ParseCommandLineArgs
 * never sets. When it is set, GameInputMaps installs the keyboard/mouse
 * maps (mouse x/y -> P1 aim, buttons -> triggers, keys q 4 w 5 , e . r)
 * and GameSetVariableInputMaps leaves them alone; the io_sdl driver feeds
 * them. Called from SDL_ShowCursor (WndShowCursor runs just before
 * GameInputMaps). JPRT_INPUT=rio keeps the cabinet maps. */
#define KEYBOARD_MAPS_FLAG 0x8db4e38u

void game_select_input_maps(void)
{
    const char *m = getenv("JPRT_INPUT");
    if (m && !strcmp(m, "rio")) return;
    if (!rd32(KEYBOARD_MAPS_FLAG)) klog("input: developer keyboard/mouse maps selected\n");
    wr32(KEYBOARD_MAPS_FLAG, 1);
}

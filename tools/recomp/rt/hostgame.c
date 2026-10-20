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
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "rt.h"
#include "kernel.h"

#define ARG(i) rd32(cpu->esp + 4 + 4 * (i))
#define RET(v) do { cpu->eax = (uint32_t)(v); cpu->esp += 4; return; } while (0)

uint32_t guest_alloc(uint32_t n);
uint32_t guest_strdup(const char *s);

/* ---- HASP ---- */
#define HASP_MEM_SIZE 4096
static uint8_t g_hasp_mem[4][HASP_MEM_SIZE];
static char g_hasp_path[1100];
static CRITICAL_SECTION g_hasp_cs;

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
    WIN32_FIND_DATAA fd;
    char pat[1200];
    HANDLE h;
    snprintf(pat, sizeof(pat), "%s\\*", enc);
    h = FindFirstFileA(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        char e[1200], d[1200];
        if (!strcmp(fd.cFileName, ".") || !strcmp(fd.cFileName, "..")) continue;
        snprintf(e, sizeof(e), "%s\\%s", enc, fd.cFileName);
        snprintf(d, sizeof(d), "%s\\%s", dec, fd.cFileName);
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            index_dir(e, d);
        } else if (fd.nFileSizeLow) {
            FILE *f = fopen(e, "rb");
            uint8_t *b = malloc(fd.nFileSizeLow);
            if (f && b && fread(b, 1, fd.nFileSizeLow, f) == fd.nFileSizeLow) {
                if (g_ncipher == g_ccap) {
                    g_ccap = g_ccap ? 2 * g_ccap : 2048;
                    g_cipher = realloc(g_cipher, g_ccap * sizeof(*g_cipher));
                }
                g_cipher[g_ncipher].len = fd.nFileSizeLow;
                g_cipher[g_ncipher].hash = fnv1a64(b, fd.nFileSizeLow);
                g_cipher[g_ncipher].plain = _strdup(d);
                g_ncipher++;
            }
            if (f) fclose(f);
            free(b);
        }
    } while (FindNextFileA(h, &fd));
    FindClose(h);
}

static void hasp_init(void)
{
    static int done;
    char enc[1100], dec[1100];
    FILE *f;
    uint8_t *main_mem;
    if (done) return;
    done = 1;
    InitializeCriticalSection(&g_hasp_cs);
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
        EnterCriticalSection(&g_hasp_cs);
        if (addr == 0x8528520) memcpy(RT_P(buf), m + off, len);
        else { memcpy(m + off, RT_P(buf), len); hasp_save(); }
        LeaveCriticalSection(&g_hasp_cs);
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
    default:
        kfatal(cpu, "rt_host_fn: no host implementation for %08x", addr);
    }
}

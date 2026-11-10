/*
 * hostvorbis.c — libvorbisfile (ov_*) for the recompiled process.
 *
 * SDL_mixer (lifted guest code) loads libvorbisfile.so.3 with
 * SDL_LoadObject and calls six functions through SDL_LoadFunction:
 * ov_open_callbacks, ov_info, ov_pcm_total, ov_read, ov_time_seek and
 * ov_clear. They resolve to host thunks and land here; decoding is done by
 * stb_vorbis (public domain, rt/stb_vorbis.c).
 *
 * ov_open_callbacks reads the whole stream through the guest's read
 * callback, so it works for any datasource. The guest OggVorbis_File (about
 * 720 bytes, owned by the caller) only holds our magic and slot index.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "rt.h"
#include "kernel.h"
#include "plat.h"

#define STB_VORBIS_NO_STDIO
#define STB_VORBIS_NO_PUSHDATA_API
#if defined(__clang__) || defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wall"
#pragma GCC diagnostic ignored "-Wunused-value"
#pragma GCC diagnostic ignored "-Wunused-but-set-variable"
#endif
#include "stb_vorbis.c"
#if defined(__clang__) || defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

#define ARG(i) rd32(cpu->esp + 4 + 4 * (i))
#define RET(v) do { cpu->eax = (uint32_t)(v); cpu->esp += 4; return 1; } while (0)

uint32_t guest_alloc(uint32_t n);
uint32_t guest_call(rt_cpu *cpu, uint32_t fn, int n, const uint32_t *args);

#define OV_MAGIC 0x4a50564fu                /* "OVPJ" at vf+0, slot at vf+4 */
#define NOV 64
#define SCRATCH 16384
#define OV_EREAD (-128)
#define OV_EFAULT (-129)
#define OV_ENOTVORBIS (-132)

static struct {
    int used;
    stb_vorbis *v;
    unsigned char *data;                    /* the compressed stream (stb reads it in place) */
    uint32_t vf, ds, close_fn;
    uint32_t ginfo;                         /* guest vorbis_info, 32 bytes, reused */
    uint32_t scratch;                       /* guest read buffer, reused */
    int ch;
} g_ov[NOV];
static plat_mutex g_ov_cs;

__attribute__((constructor)) static void ov_cs_init(void) { plat_mutex_init(&g_ov_cs); }

static int ov_slot(uint32_t vf)
{
    uint32_t i;
    if (!vf || rd32(vf) != OV_MAGIC) return -1;
    i = rd32(vf + 4);
    return (i < NOV && g_ov[i].used && g_ov[i].vf == vf) ? (int)i : -1;
}

static void ov_release(int i)
{
    if (g_ov[i].v) stb_vorbis_close(g_ov[i].v);
    free(g_ov[i].data);
    g_ov[i].v = NULL;
    g_ov[i].data = NULL;
    plat_mutex_lock(&g_ov_cs);
    g_ov[i].used = 0;
    plat_mutex_unlock(&g_ov_cs);
}

/* ov_open_callbacks(datasource, vf, initial, ibytes, {read, seek, close, tell}) */
static int ov_open(rt_cpu *cpu)
{
    uint32_t ds = ARG(0), vf = ARG(1), initial = ARG(2), ibytes = ARG(3);
    uint32_t read_fn = ARG(4), close_fn = ARG(6);
    unsigned char *b = NULL;
    size_t n = 0, cap = 0;
    int i, err = 0;
    if (!vf || !read_fn) return OV_EFAULT;
    plat_mutex_lock(&g_ov_cs);
    for (i = 0; i < NOV && g_ov[i].used; i++) ;
    if (i < NOV) g_ov[i].used = 1;
    plat_mutex_unlock(&g_ov_cs);
    if (i == NOV) { klog("ov_open_callbacks: too many open streams\n"); return OV_EFAULT; }
    if (!g_ov[i].scratch) g_ov[i].scratch = guest_alloc(SCRATCH);
    if (!g_ov[i].ginfo) g_ov[i].ginfo = guest_alloc(32);
    if (initial && ibytes) {                /* bytes the caller already read */
        b = malloc(cap = ibytes);
        memcpy(b, RT_P(initial), ibytes);
        n = ibytes;
    }
    for (;;) {                              /* the whole stream via read(ptr, 1, n, datasource) */
        uint32_t a[4] = { g_ov[i].scratch, 1, SCRATCH, ds }, r;
        r = guest_call(cpu, read_fn, 4, a);
        if ((int32_t)r <= 0) break;
        if (r > SCRATCH) r = SCRATCH;
        if (n + r > cap) b = realloc(b, cap = (n + r) * 2);
        memcpy(b + n, RT_P(g_ov[i].scratch), r);
        n += r;
    }
    g_ov[i].v = n ? stb_vorbis_open_memory(b, (int)n, &err, NULL) : NULL;
    if (!g_ov[i].v) {
        klog("ov_open_callbacks: not a Vorbis stream (%u bytes, stb error %d)\n", (unsigned)n, err);
        free(b);
        plat_mutex_lock(&g_ov_cs);
        g_ov[i].used = 0;
        plat_mutex_unlock(&g_ov_cs);
        return n ? OV_ENOTVORBIS : OV_EREAD;
    }
    g_ov[i].data = b;
    g_ov[i].vf = vf;
    g_ov[i].ds = ds;
    g_ov[i].close_fn = close_fn;
    g_ov[i].ch = stb_vorbis_get_info(g_ov[i].v).channels;
    memset(RT_P(vf), 0, 16);
    wr32(vf, OV_MAGIC);
    wr32(vf + 4, (uint32_t)i);
    return 0;
}

int host_vorbis(rt_cpu *cpu, const char *n)
{
    int i;
    if (!strcmp(n, "ov_open_callbacks")) RET(ov_open(cpu));
    if (!strcmp(n, "ov_info")) {
        stb_vorbis_info vi;
        uint32_t g;
        if ((i = ov_slot(ARG(0))) < 0) RET(0);
        vi = stb_vorbis_get_info(g_ov[i].v);
        g = g_ov[i].ginfo;
        /* vorbis_info: version, channels, rate, bitrate upper/nominal/lower/window, codec_setup */
        memset(RT_P(g), 0, 32);
        wr32(g + 4, (uint32_t)vi.channels);
        wr32(g + 8, vi.sample_rate);
        RET(g);
    }
    if (!strcmp(n, "ov_pcm_total")) {       /* ogg_int64_t in edx:eax */
        uint64_t t = 0;
        if ((i = ov_slot(ARG(0))) >= 0) t = stb_vorbis_stream_length_in_samples(g_ov[i].v);
        else t = (uint64_t)(int64_t)-131;   /* OV_EINVAL */
        cpu->edx = (uint32_t)(t >> 32);
        RET((uint32_t)t);
    }
    if (!strcmp(n, "ov_read")) {            /* (vf, buffer, length, bigendian, word, sgned, *bitstream) */
        uint32_t buf = ARG(1), len = ARG(2), bs = ARG(6);
        int frames, got;
        if ((i = ov_slot(ARG(0))) < 0) RET(-131);
        if (ARG(3) || ARG(4) != 2 || !ARG(5)) {
            klog("ov_read: only little-endian signed 16-bit is supported\n");
            RET(-131);
        }
        frames = (int)(len / (2u * (uint32_t)g_ov[i].ch));
        if (frames <= 0) RET(0);
        got = stb_vorbis_get_samples_short_interleaved(g_ov[i].v, g_ov[i].ch, (short *)RT_P(buf), frames * g_ov[i].ch);
        if (bs) wr32(bs, 0);
        RET((uint32_t)(got * g_ov[i].ch * 2));
    }
    if (!strcmp(n, "ov_time_seek")) {       /* (vf, double seconds) */
        double s = rdf64(cpu->esp + 8);
        if ((i = ov_slot(ARG(0))) < 0) RET(-131);
        if (s < 0) s = 0;
        RET(stb_vorbis_seek(g_ov[i].v, (unsigned int)(s * stb_vorbis_get_info(g_ov[i].v).sample_rate)) ? 0 : -137);
    }
    if (!strcmp(n, "ov_clear")) {
        uint32_t vf = ARG(0), close_fn, ds;
        if ((i = ov_slot(vf)) < 0) RET(0);
        close_fn = g_ov[i].close_fn;
        ds = g_ov[i].ds;
        ov_release(i);
        memset(RT_P(vf), 0, 8);
        if (close_fn) guest_call(cpu, close_fn, 1, &ds);
        RET(0);
    }
    return 0;
}

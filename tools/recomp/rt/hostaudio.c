/*
 * hostaudio.c — SDL 1.2 audio + RWops for the recompiled process.
 *
 * SDL_mixer is linked statically into the game (so it is lifted guest
 * code); it needs only SDL's audio device, WAV loader, format converter,
 * MixAudio and file RWops from the host.
 *
 * The device callback (SDL_mixer's mix_channels) is guest code. Rather
 * than run it on a host-created thread (no guest TLS/stack there), the
 * game's own threads pump it: SDL_GL_SwapBuffers, SDL_Delay and
 * SDL_PollEvent call audio_pump(), which refills the output device
 * (audiodev.c: waveOut / AAudio) by calling the guest callback under the
 * audio lock.
 *
 * Buffers that SDL would have malloc'ed (LoadWAV) come from the guest's
 * own libc malloc, since SDL_mixer frees them with SDL_free.
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "rt.h"
#include "kernel.h"
#include "plat.h"
#include "audiodev.h"

#define ARG(i) rd32(cpu->esp + 4 + 4 * (i))
#define RET(v) do { cpu->eax = (uint32_t)(v); cpu->esp += 4; return 1; } while (0)

uint32_t guest_alloc(uint32_t n);
uint32_t host_thunk_addr(const char *name);

#define AUDIO_U8     0x0008
#define AUDIO_S8     0x8008
#define AUDIO_U16LSB 0x0010
#define AUDIO_S16LSB 0x8010

/* ---- calling guest code from a host function ---- */
#define GUEST_RA 0xf0000ff0u            /* never executed: the callee returns to us */

static uint32_t guest_call(rt_cpu *cpu, uint32_t fn, int n, const uint32_t *args)
{
    uint32_t esp0 = cpu->esp, sp = ((esp0 - 128) & ~15u) - 4u * (uint32_t)n;
    int i;
    sp &= ~15u;                          /* i386 ABI: 16-byte aligned at the call */
    for (i = 0; i < n; i++) wr32(sp + 4u * (uint32_t)i, args[i]);
    cpu->esp = sp - 4;
    wr32(cpu->esp, GUEST_RA);
    RT_SHPUSH(cpu, GUEST_RA);
    rt_call(cpu, fn);
    RT_SHPOP(cpu);
    cpu->esp = esp0;
    return cpu->eax;
}

/* guest libc malloc/free (libc at its arcade base) */
static uint32_t g_malloc, g_free;

static uint32_t libc_sym(const char *want)
{
    char path[1024];
    FILE *f;
    uint8_t *b;
    long n;
    uint32_t r = 0, shoff, i, j;
    uint16_t shnum, shentsize;
    if (!vfs_host_path("/lib/i386-linux-gnu/libc.so.6", path, sizeof(path)) || !(f = fopen(path, "rb")))
        return 0;
    fseek(f, 0, SEEK_END); n = ftell(f); fseek(f, 0, SEEK_SET);
    b = malloc((size_t)n);
    if (fread(b, 1, (size_t)n, f) != (size_t)n) { fclose(f); free(b); return 0; }
    fclose(f);
    memcpy(&shoff, b + 0x20, 4); memcpy(&shentsize, b + 0x2e, 2); memcpy(&shnum, b + 0x30, 2);
    for (i = 0; i < shnum && !r; i++) {
        uint8_t *sh = b + shoff + i * shentsize, *lk;
        uint32_t type, off, size, link, stroff;
        memcpy(&type, sh + 4, 4);
        if (type != 11) continue;        /* SHT_DYNSYM */
        memcpy(&off, sh + 16, 4); memcpy(&size, sh + 20, 4); memcpy(&link, sh + 24, 4);
        lk = b + shoff + link * shentsize;
        memcpy(&stroff, lk + 16, 4);
        for (j = 0; j < size / 16; j++) {
            uint32_t nm, val;
            uint16_t shndx;
            memcpy(&nm, b + off + j * 16, 4); memcpy(&val, b + off + j * 16 + 4, 4);
            memcpy(&shndx, b + off + j * 16 + 14, 2);
            if (shndx && val && !strcmp((char *)b + stroff + nm, want)) { r = 0xb7141000u + val; break; }
        }
    }
    free(b);
    return r;
}

static uint32_t gmalloc(rt_cpu *cpu, uint32_t n)
{
    uint32_t a = n;
    if (!g_malloc && !(g_malloc = libc_sym("malloc"))) kfatal(cpu, "hostaudio: libc malloc not found");
    return guest_call(cpu, g_malloc, 1, &a);
}

static void gfree(rt_cpu *cpu, uint32_t p)
{
    if (!p) return;
    if (!g_free && !(g_free = libc_sym("free"))) kfatal(cpu, "hostaudio: libc free not found");
    guest_call(cpu, g_free, 1, &p);
}

/* ---- RWops (guest SDL_RWops, 32 bytes) over host files ---- */
#define RW_MAGIC 0x4a505254u            /* type field of our RWops */
#define NRW 64
static struct { FILE *f; uint32_t g; } g_rw[NRW];
static plat_mutex g_rw_cs, g_audio_cs;

__attribute__((constructor)) static void audio_cs_init(void)
{
    plat_mutex_init(&g_rw_cs);
    plat_mutex_init(&g_audio_cs);
}

static int rw_slot(uint32_t rw)
{
    uint32_t i;
    if (!rw || rd32(rw + 16) != RW_MAGIC) return -1;
    i = rd32(rw + 20);
    return (i < NRW && g_rw[i].g == rw) ? (int)i : -1;
}

static uint32_t rw_open(rt_cpu *cpu, const char *gpath, const char *mode)
{
    char host[1024], m[8];
    FILE *f;
    int i;
    uint32_t rw = 0;
    (void)cpu;
    snprintf(m, sizeof(m), "%s%s", mode, strchr(mode, 'b') ? "" : "b");
    if (!vfs_host_path(gpath, host, sizeof(host)) || !(f = fopen(host, m))) return 0;
    plat_mutex_lock(&g_rw_cs);
    for (i = 0; i < NRW && g_rw[i].f; i++) ;
    if (i < NRW) {
        if (!g_rw[i].g) g_rw[i].g = guest_alloc(32);
        rw = g_rw[i].g;
        g_rw[i].f = f;
        wr32(rw + 0, host_thunk_addr("SDL_jprt_rw_seek"));
        wr32(rw + 4, host_thunk_addr("SDL_jprt_rw_read"));
        wr32(rw + 8, host_thunk_addr("SDL_jprt_rw_write"));
        wr32(rw + 12, host_thunk_addr("SDL_jprt_rw_close"));
        wr32(rw + 16, RW_MAGIC);
        wr32(rw + 20, (uint32_t)i);
    } else
        fclose(f);
    plat_mutex_unlock(&g_rw_cs);
    if (!rw) klog("SDL_RWFromFile: too many open RWops\n");
    return rw;
}

static void rw_release(int i)
{
    plat_mutex_lock(&g_rw_cs);
    if (g_rw[i].f) fclose(g_rw[i].f);
    g_rw[i].f = NULL;
    plat_mutex_unlock(&g_rw_cs);
}

/* read n bytes from any RWops (ours directly, the guest's through its read) */
static uint32_t rw_read_any(rt_cpu *cpu, uint32_t rw, void *dst, uint32_t n)
{
    int i = rw_slot(rw);
    uint32_t got = 0;
    if (i >= 0) return (uint32_t)fread(dst, 1, n, g_rw[i].f);
    while (got < n) {
        uint32_t chunk = n - got > 4096 ? 4096 : n - got, a[4], r;
        static uint32_t scratch;
        if (!scratch) scratch = guest_alloc(4096);   /* callers hold no lock: guest RWops are rare */
        a[0] = rw; a[1] = scratch; a[2] = 1; a[3] = chunk;
        r = guest_call(cpu, rd32(rw + 4), 4, a);
        if ((int32_t)r <= 0) break;
        memcpy((uint8_t *)dst + got, RT_P(scratch), r);
        got += r;
    }
    return got;
}

static void rw_close_any(rt_cpu *cpu, uint32_t rw)
{
    int i = rw_slot(rw);
    if (i >= 0) { rw_release(i); return; }
    guest_call(cpu, rd32(rw + 12), 1, &rw);
}

/* ---- WAV ---- */
static int load_wav(rt_cpu *cpu, uint32_t src, int freesrc, uint32_t spec, uint32_t pbuf, uint32_t plen)
{
    uint8_t *b = NULL;
    uint32_t n = 0, cap = 0, r, p, fmt_ok = 0, data_off = 0, data_len = 0, g;
    uint16_t tag = 0, ch = 0, bits = 0;
    uint32_t rate = 0;
    int i = rw_slot(src);
    if (i >= 0) {                        /* whole file */
        long cur = ftell(g_rw[i].f), end;
        fseek(g_rw[i].f, 0, SEEK_END); end = ftell(g_rw[i].f); fseek(g_rw[i].f, cur, SEEK_SET);
        cap = (uint32_t)(end - cur);
        b = malloc(cap ? cap : 1);
        n = (uint32_t)fread(b, 1, cap, g_rw[i].f);
    } else {
        for (;;) {
            if (n == cap) b = realloc(b, cap = cap ? cap * 2 : 65536);
            r = rw_read_any(cpu, src, b + n, cap - n);
            if (!r) break;
            n += r;
        }
    }
    if (freesrc) rw_close_any(cpu, src);
    if (n < 12 || memcmp(b, "RIFF", 4) || memcmp(b + 8, "WAVE", 4)) { free(b); return 0; }
    for (p = 12; p + 8 <= n;) {
        uint32_t clen;
        memcpy(&clen, b + p + 4, 4);
        if (!memcmp(b + p, "fmt ", 4) && clen >= 16) {
            memcpy(&tag, b + p + 8, 2); memcpy(&ch, b + p + 10, 2);
            memcpy(&rate, b + p + 12, 4); memcpy(&bits, b + p + 22, 2);
            if (tag == 0xfffe && clen >= 26) memcpy(&tag, b + p + 32, 2);   /* extensible: subformat */
            fmt_ok = 1;
        } else if (!memcmp(b + p, "data", 4)) {
            data_off = p + 8;
            data_len = clen > n - data_off ? n - data_off : clen;
            break;
        }
        p += 8 + clen + (clen & 1);
    }
    if (!fmt_ok || !data_off || tag != 1 || (bits != 8 && bits != 16) || !ch) {
        klog("SDL_LoadWAV_RW: unsupported WAV (tag %u, %u bits, %u ch)\n", tag, bits, ch);
        free(b);
        return 0;
    }
    data_len -= data_len % (uint32_t)(ch * bits / 8);
    g = gmalloc(cpu, data_len ? data_len : 1);
    if (!g) { free(b); return 0; }
    memcpy(RT_P(g), b + data_off, data_len);
    free(b);
    /* SDL_AudioSpec: freq, format u16, channels u8, silence u8, samples u16, pad, size, callback, userdata */
    memset(RT_P(spec), 0, 24);
    wr32(spec + 0, rate);
    wr16(spec + 4, bits == 8 ? AUDIO_U8 : AUDIO_S16LSB);
    wr8(spec + 6, (uint8_t)ch);
    wr8(spec + 7, bits == 8 ? 0x80 : 0);
    wr16(spec + 8, 4096);
    wr32(pbuf, g);
    wr32(plen, data_len);
    return 1;
}

/* ---- format conversion (SDL_AudioCVT, i386 layout) ---- */
/* needed@0 src_format@4 dst_format@6 rate_incr@8 buf@16 len@20 len_cvt@24
 * len_mult@28 len_ratio@32 filters[10]@40 filter_index@80; our parameters
 * live in filters[0..3] (src_ch, src_rate, dst_ch, dst_rate). */
static int fmt_bytes(uint16_t f) { return (f & 0xff) / 8; }
static int fmt_known(uint16_t f)
{
    return f == AUDIO_U8 || f == AUDIO_S8 || f == AUDIO_U16LSB || f == AUDIO_S16LSB;
}

static float get_s(const uint8_t *p, uint16_t f)
{
    switch (f) {
    case AUDIO_U8: return ((int)p[0] - 128) / 128.0f;
    case AUDIO_S8: return (int8_t)p[0] / 128.0f;
    case AUDIO_U16LSB: return ((int)(p[0] | p[1] << 8) - 32768) / 32768.0f;
    default: return (int16_t)(p[0] | p[1] << 8) / 32768.0f;
    }
}

static void put_s(uint8_t *p, uint16_t f, float v)
{
    int s;
    if (v > 1.0f) v = 1.0f;
    if (v < -1.0f) v = -1.0f;
    switch (f) {
    case AUDIO_U8: p[0] = (uint8_t)(lrintf(v * 127.0f) + 128); break;
    case AUDIO_S8: p[0] = (uint8_t)(int8_t)lrintf(v * 127.0f); break;
    case AUDIO_U16LSB: s = (int)lrintf(v * 32767.0f) + 32768; p[0] = (uint8_t)s; p[1] = (uint8_t)(s >> 8); break;
    default: s = (int)lrintf(v * 32767.0f); p[0] = (uint8_t)s; p[1] = (uint8_t)(s >> 8); break;
    }
}

static int build_cvt(uint32_t cvt, uint16_t sf, uint32_t sc, uint32_t sr, uint16_t df, uint32_t dc, uint32_t dr)
{
    double ratio;
    int mult;
    memset(RT_P(cvt), 0, 84);
    if (!fmt_known(sf) || !fmt_known(df) || !sc || !dc || !sr || !dr) return -1;
    wr16(cvt + 4, sf); wr16(cvt + 6, df);
    wr32(cvt + 40, sc); wr32(cvt + 44, sr); wr32(cvt + 48, dc); wr32(cvt + 52, dr);
    ratio = (double)(fmt_bytes(df) * dc) / (fmt_bytes(sf) * sc) * (double)dr / sr;
    mult = (int)ceil(ratio) + 1;
    if (mult < 1) mult = 1;
    wr32(cvt + 28, (uint32_t)mult);
    wrf64(cvt + 32, ratio);
    wrf64(cvt + 8, (double)dr / sr);
    if (sf == df && sc == dc && sr == dr) { wr32(cvt + 28, 1); wrf64(cvt + 32, 1.0); return 0; }
    wr32(cvt + 0, 1);
    return 1;
}

static int convert(uint32_t cvt)
{
    uint16_t sf = rd16(cvt + 4), df = rd16(cvt + 6);
    uint32_t sc = rd32(cvt + 40), sr = rd32(cvt + 44), dc = rd32(cvt + 48), dr = rd32(cvt + 52);
    uint32_t buf = rd32(cvt + 16), len = rd32(cvt + 20), cap = len * rd32(cvt + 28);
    uint32_t sfb = (uint32_t)fmt_bytes(sf) * sc, dfb = (uint32_t)fmt_bytes(df) * dc, frames, outf, i, c;
    float *in;
    uint8_t *out;
    if (!rd32(cvt + 0) || !sc) { wr32(cvt + 24, len); return 0; }
    frames = len / sfb;
    outf = (uint32_t)((uint64_t)frames * dr / sr);
    if ((uint64_t)outf * dfb > cap) outf = cap / dfb;
    in = malloc(((size_t)frames + 1) * dc * sizeof(float));
    for (i = 0; i < frames; i++) {
        const uint8_t *s = RT_P(buf + i * sfb);
        for (c = 0; c < dc; c++) {
            float v;
            if (dc >= sc) v = get_s(s + (c % sc) * fmt_bytes(sf), sf);
            else {                       /* downmix: average the source channels landing here */
                uint32_t k, m = 0;
                v = 0;
                for (k = c; k < sc; k += dc) { v += get_s(s + k * fmt_bytes(sf), sf); m++; }
                v /= (float)m;
            }
            in[(size_t)i * dc + c] = v;
        }
    }
    for (c = 0; c < dc; c++) in[(size_t)frames * dc + c] = frames ? in[(size_t)(frames - 1) * dc + c] : 0;
    out = malloc((size_t)outf * dfb + 1);
    for (i = 0; i < outf; i++) {
        double pos = (double)i * sr / dr;
        uint32_t i0 = (uint32_t)pos;
        float t = (float)(pos - i0);
        if (i0 >= frames) i0 = frames ? frames - 1 : 0, t = 0;
        for (c = 0; c < dc; c++) {
            float v = in[(size_t)i0 * dc + c] * (1 - t) + in[(size_t)(i0 + 1) * dc + c] * t;
            put_s(out + (size_t)i * dfb + c * fmt_bytes(df), df, v);
        }
    }
    memcpy(RT_P(buf), out, (size_t)outf * dfb);
    wr32(cvt + 24, outf * dfb);
    free(in);
    free(out);
    return 0;
}

/* ---- device (audiodev.c), pumped by the game's threads ---- */
static int g_nbuf = 6, g_open, g_paused = 1;
static uint32_t g_cb, g_ud, g_stream, g_chunk;  /* guest callback, userdata, stream buffer, bytes */
static uint16_t g_fmt;
static uint8_t g_silence;
static int32_t g_pumping;
static uint8_t *g_out;
static FILE *g_dump;              /* JPRT_AUDIO_DUMP=file: raw PCM of everything played */

static int open_audio(rt_cpu *cpu, uint32_t want, uint32_t have)
{
    uint32_t freq = rd32(want), frames, block;
    uint16_t fmt = rd16(want + 4);
    uint8_t ch = rd8(want + 6);
    (void)cpu;
    if (g_open) return -1;
    if (getenv("JPRT_AUDIO") && !atoi(getenv("JPRT_AUDIO"))) return -1;
    if (getenv("JPRT_AUDIO_BUFS")) g_nbuf = atoi(getenv("JPRT_AUDIO_BUFS"));
    if (g_nbuf < 2) g_nbuf = 2;
    if (g_nbuf > 16) g_nbuf = 16;
    if (fmt != AUDIO_U8) fmt = AUDIO_S16LSB;
    if (ch < 1) ch = 1;
    if (ch > 2) ch = 2;
    if (!freq) freq = 44100;
    frames = 512;                        /* our pump granularity, independent of the spec's samples */
    block = (uint32_t)ch * (fmt == AUDIO_U8 ? 1 : 2);
    g_chunk = frames * block;
    if (adev_open(freq, ch, fmt == AUDIO_U8 ? 8 : 16, g_chunk, g_nbuf) < 0) {
        klog("SDL_OpenAudio: no output device\n");
        return -1;
    }
    g_fmt = fmt;
    g_silence = fmt == AUDIO_U8 ? 0x80 : 0;
    g_cb = rd32(want + 16);
    g_ud = rd32(want + 20);
    if (!g_stream) g_stream = guest_alloc(64 * 1024);
    g_out = malloc(g_chunk);
    if (!have) have = want;              /* SDL 1.2: NULL obtained = desired is guaranteed */
    if (have != want) memcpy(RT_P(have), RT_P(want), 24);
    wr32(have + 0, freq);
    wr16(have + 4, fmt);
    wr8(have + 6, ch);
    wr8(have + 7, g_silence);
    wr32(have + 12, (uint32_t)rd16(have + 8) * block);
    if (getenv("JPRT_AUDIO_DUMP")) g_dump = fopen(getenv("JPRT_AUDIO_DUMP"), "wb");
    g_paused = 1;
    g_open = 1;
    klog("SDL_OpenAudio: %u Hz, %u ch, %s, %d x %u-byte buffers (~%u ms)\n", freq, ch,
         fmt == AUDIO_U8 ? "u8" : "s16", g_nbuf, g_chunk, (unsigned)(1000ull * g_nbuf * frames / freq));
    return 0;
}

void audio_pump(rt_cpu *cpu)
{
    int n;
    if (!g_open || !plat_atomic_cas(&g_pumping, 0, 1)) return;
    for (n = adev_free_chunks(); n > 0; n--) {
        plat_mutex_lock(&g_audio_cs);
        memset(RT_P(g_stream), g_silence, g_chunk);
        if (!g_paused && g_cb) {
            uint32_t a[3] = { g_ud, g_stream, g_chunk };
            guest_call(cpu, g_cb, 3, a);
        }
        memcpy(g_out, RT_P(g_stream), g_chunk);
        plat_mutex_unlock(&g_audio_cs);
        if (g_dump) fwrite(g_out, 1, g_chunk, g_dump);
        if (g_fmt == AUDIO_S16LSB) {     /* level log: proves the mixer produces sound */
            static unsigned nchunks, peak;
            uint32_t k;
            for (k = 0; k + 1 < g_chunk; k += 2) {
                int v = abs((int16_t)(g_out[k] | g_out[k + 1] << 8));
                if ((unsigned)v > peak) peak = (unsigned)v;
            }
            if (++nchunks % 430 == 0) { klog("audio: %u chunks, peak %u\n", nchunks, peak); peak = 0; }
        }
        adev_submit(g_out);
    }
    plat_atomic_xchg(&g_pumping, 0);
}

static void close_audio(void)
{
    if (!g_open) return;
    g_open = 0;
    adev_close();
    free(g_out);
    g_out = NULL;
}

static void mix_audio(uint32_t dst, uint32_t src, uint32_t len, int vol)
{
    uint32_t i;
    if (vol <= 0) return;
    if (vol > 128) vol = 128;
    if (g_fmt == AUDIO_U8) {
        for (i = 0; i < len; i++) {
            int s = ((int)rd8(src + i) - 128) * vol / 128 + ((int)rd8(dst + i) - 128);
            if (s > 127) s = 127;
            if (s < -128) s = -128;
            wr8(dst + i, (uint8_t)(s + 128));
        }
        return;
    }
    for (i = 0; i + 1 < len; i += 2) {
        int s = (int16_t)rd16(src + i) * vol / 128 + (int16_t)rd16(dst + i);
        if (s > 32767) s = 32767;
        if (s < -32768) s = -32768;
        wr16(dst + i, (uint16_t)s);
    }
}

/* ---- entry from hostlibs.c sdl() ---- */
int sdl_audio(rt_cpu *cpu, const char *n)
{
    if (!strcmp(n, "SDL_OpenAudio")) RET(open_audio(cpu, ARG(0), ARG(1)));
    if (!strcmp(n, "SDL_PauseAudio")) { g_paused = ARG(0) != 0; audio_pump(cpu); RET(0); }
    if (!strcmp(n, "SDL_CloseAudio")) { close_audio(); RET(0); }
    if (!strcmp(n, "SDL_LockAudio")) { plat_mutex_lock(&g_audio_cs); RET(0); }
    if (!strcmp(n, "SDL_UnlockAudio")) { plat_mutex_unlock(&g_audio_cs); RET(0); }
    if (!strcmp(n, "SDL_GetAudioStatus")) RET(!g_open ? 0 : g_paused ? 2 : 1);
    if (!strcmp(n, "SDL_MixAudio")) { mix_audio(ARG(0), ARG(1), ARG(2), (int)ARG(3)); RET(0); }
    if (!strcmp(n, "SDL_BuildAudioCVT"))
        RET(build_cvt(ARG(0), (uint16_t)ARG(1), ARG(2) & 0xff, ARG(3), (uint16_t)ARG(4), ARG(5) & 0xff, ARG(6)));
    if (!strcmp(n, "SDL_ConvertAudio")) RET(convert(ARG(0)));
    if (!strcmp(n, "SDL_LoadWAV_RW")) {
        uint32_t spec = ARG(2);
        RET(load_wav(cpu, ARG(0), (int)ARG(1), spec, ARG(3), ARG(4)) ? spec : 0);
    }
    if (!strcmp(n, "SDL_FreeWAV")) { gfree(cpu, ARG(0)); RET(0); }
    if (!strcmp(n, "SDL_RWFromFile")) {
        const char *f = gstr(ARG(0)), *m = gstr(ARG(1));
        uint32_t rw = (f && m) ? rw_open(cpu, f, m) : 0;
        if (!rw) klog("SDL_RWFromFile(%s): cannot open\n", f ? f : "(null)");
        RET(rw);
    }
    if (!strcmp(n, "SDL_FreeRW")) {
        int i = rw_slot(ARG(0));
        if (i >= 0) rw_release(i);
        RET(0);
    }
    if (!strcmp(n, "SDL_ReadLE16") || !strcmp(n, "SDL_ReadBE16")) {
        uint8_t v[2] = { 0, 0 };
        rw_read_any(cpu, ARG(0), v, 2);
        RET(n[8] == 'L' ? (uint32_t)(v[0] | v[1] << 8) : (uint32_t)(v[1] | v[0] << 8));
    }
    if (!strcmp(n, "SDL_ReadLE32") || !strcmp(n, "SDL_ReadBE32")) {
        uint8_t v[4] = { 0, 0, 0, 0 };
        rw_read_any(cpu, ARG(0), v, 4);
        RET(n[8] == 'L' ? (uint32_t)(v[0] | v[1] << 8 | v[2] << 16 | (uint32_t)v[3] << 24)
                        : (uint32_t)(v[3] | v[2] << 8 | v[1] << 16 | (uint32_t)v[0] << 24));
    }
    /* our RWops' function pointers */
    if (!strcmp(n, "SDL_jprt_rw_seek")) {
        int i = rw_slot(ARG(0));
        int32_t off = (int32_t)ARG(1);
        int wh = (int)ARG(2);
        if (i < 0 || fseek(g_rw[i].f, off, wh == 1 ? SEEK_CUR : wh == 2 ? SEEK_END : SEEK_SET)) RET(-1);
        RET((uint32_t)ftell(g_rw[i].f));
    }
    if (!strcmp(n, "SDL_jprt_rw_read")) {
        int i = rw_slot(ARG(0));
        uint32_t size = ARG(2), num = ARG(3);
        if (i < 0) RET(-1);
        if (!size || !num) RET(0);
        RET((uint32_t)fread(RT_P(ARG(1)), size, num, g_rw[i].f));
    }
    if (!strcmp(n, "SDL_jprt_rw_write")) {
        int i = rw_slot(ARG(0));
        if (i < 0) RET(-1);
        RET((uint32_t)fwrite(RT_P(ARG(1)), ARG(2), ARG(3), g_rw[i].f));
    }
    if (!strcmp(n, "SDL_jprt_rw_close")) {
        int i = rw_slot(ARG(0));
        if (i >= 0) rw_release(i);
        RET(0);
    }
    return 0;
}

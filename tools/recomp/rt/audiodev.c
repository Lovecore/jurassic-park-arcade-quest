/*
 * audiodev.c — the PCM output device under hostaudio.c's SDL audio:
 * winmm waveOut on Windows, AAudio on Android (the Quest). The game's
 * threads pump it: hostaudio asks how many chunks fit, renders them with
 * the guest SDL_mixer callback and submits them.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "audiodev.h"
#include "rt.h"
#include "kernel.h"

#if defined(_WIN32)
#include <windows.h>
#include <mmsystem.h>

#define NHDR 16
static HWAVEOUT g_wo;
static WAVEHDR g_hdr[NHDR];
static int g_nbuf;
static uint32_t g_chunk;

int adev_open(uint32_t freq, int channels, int bits, uint32_t chunk_bytes, int nbuf)
{
    WAVEFORMATEX wf;
    int i;
    memset(&wf, 0, sizeof(wf));
    wf.wFormatTag = WAVE_FORMAT_PCM;
    wf.nChannels = (WORD)channels;
    wf.nSamplesPerSec = freq;
    wf.wBitsPerSample = (WORD)bits;
    wf.nBlockAlign = (WORD)(channels * bits / 8);
    wf.nAvgBytesPerSec = freq * wf.nBlockAlign;
    if (waveOutOpen(&g_wo, WAVE_MAPPER, &wf, 0, 0, CALLBACK_NULL) != MMSYSERR_NOERROR) return -1;
    g_nbuf = nbuf > NHDR ? NHDR : nbuf;
    g_chunk = chunk_bytes;
    for (i = 0; i < g_nbuf; i++) {
        memset(&g_hdr[i], 0, sizeof(g_hdr[i]));
        g_hdr[i].lpData = malloc(chunk_bytes);
        g_hdr[i].dwBufferLength = chunk_bytes;
        g_hdr[i].dwFlags = WHDR_DONE;           /* free */
    }
    return 0;
}

int adev_free_chunks(void)
{
    int i, n = 0;
    for (i = 0; i < g_nbuf; i++) n += (g_hdr[i].dwFlags & WHDR_DONE) != 0;
    return n;
}

void adev_submit(const void *data)
{
    int i;
    for (i = 0; i < g_nbuf; i++) {
        WAVEHDR *h = &g_hdr[i];
        if (!(h->dwFlags & WHDR_DONE)) continue;
        if (h->dwFlags & WHDR_PREPARED) waveOutUnprepareHeader(g_wo, h, sizeof(*h));
        memcpy(h->lpData, data, g_chunk);
        h->dwFlags = 0;
        waveOutPrepareHeader(g_wo, h, sizeof(*h));
        waveOutWrite(g_wo, h, sizeof(*h));
        return;
    }
}

void adev_restart(void) {}

void adev_close(void)
{
    int i;
    waveOutReset(g_wo);
    for (i = 0; i < g_nbuf; i++) {
        if (g_hdr[i].dwFlags & WHDR_PREPARED) waveOutUnprepareHeader(g_wo, &g_hdr[i], sizeof(g_hdr[i]));
        free(g_hdr[i].lpData);
        g_hdr[i].lpData = NULL;
    }
    waveOutClose(g_wo);
}

#elif defined(__ANDROID__)
#include <aaudio/AAudio.h>
#include <stdatomic.h>

/* The game mixes on its own threads (audio_pump) whenever they pass through
 * SDL; AAudio's data callback plays from a ring between the two at the
 * device's rate. The fill level is known exactly from the ring indices, so
 * the game is paced without blocking its render thread. */
static AAudioStream *g_st;
static uint32_t g_chunk, g_frame_bytes;
static int32_t g_target_frames;               /* ring fill the game keeps topped up */
#define RING_FRAMES 32768                     /* power of two */
static int16_t *g_ring;                       /* interleaved, g_frame_bytes per frame */
static int g_ch;
static _Atomic uint64_t g_wr, g_rd;           /* frames written / played */
static _Atomic uint32_t g_underruns;

static aaudio_data_callback_result_t data_cb(AAudioStream *st, void *user, void *audio, int32_t frames)
{
    uint64_t rd = atomic_load_explicit(&g_rd, memory_order_relaxed);
    uint64_t wr = atomic_load_explicit(&g_wr, memory_order_acquire);
    int32_t have = (int32_t)(wr - rd), n = have < frames ? have : frames, i;
    int16_t *out = audio;
    (void)st; (void)user;
    for (i = 0; i < n; i++)
        memcpy(out + (size_t)i * g_ch, g_ring + (size_t)((rd + (uint64_t)i) & (RING_FRAMES - 1)) * g_ch, g_frame_bytes);
    if (n < frames) {
        memset(out + (size_t)n * g_ch, 0, (size_t)(frames - n) * g_frame_bytes);
        if (wr) atomic_fetch_add_explicit(&g_underruns, 1, memory_order_relaxed);
    }
    atomic_store_explicit(&g_rd, rd + (uint64_t)n, memory_order_release);
    return AAUDIO_CALLBACK_RESULT_CONTINUE;
}

static uint32_t g_freq;
static _Atomic int g_restart;                 /* reopen the stream on the next pump */

static void error_cb(AAudioStream *st, void *user, aaudio_result_t err)
{
    (void)st; (void)user;
    klog("audio: stream error %d: reopening\n", (int)err);
    atomic_store(&g_restart, 1);              /* not from this callback (AAudio rule) */
}

static int open_stream(void)
{
    AAudioStreamBuilder *b;
    if (AAudio_createStreamBuilder(&b) != AAUDIO_OK) return -1;
    AAudioStreamBuilder_setSampleRate(b, (int32_t)g_freq);
    AAudioStreamBuilder_setChannelCount(b, g_ch);
    AAudioStreamBuilder_setFormat(b, AAUDIO_FORMAT_PCM_I16);
    AAudioStreamBuilder_setPerformanceMode(b, AAUDIO_PERFORMANCE_MODE_LOW_LATENCY);
    AAudioStreamBuilder_setSharingMode(b, AAUDIO_SHARING_MODE_SHARED);
    AAudioStreamBuilder_setUsage(b, AAUDIO_USAGE_GAME);
    AAudioStreamBuilder_setContentType(b, AAUDIO_CONTENT_TYPE_MUSIC);
    AAudioStreamBuilder_setDataCallback(b, data_cb, NULL);
    AAudioStreamBuilder_setErrorCallback(b, error_cb, NULL);
    if (AAudioStreamBuilder_openStream(b, &g_st) != AAUDIO_OK) { AAudioStreamBuilder_delete(b); g_st = NULL; return -1; }
    AAudioStreamBuilder_delete(b);
    klog("audio: AAudio %d Hz callback, burst %d, game keeps %d frames queued\n", AAudioStream_getSampleRate(g_st),
         AAudioStream_getFramesPerBurst(g_st), g_target_frames);
    AAudioStream_requestStart(g_st);
    return 0;
}

/* Horizon OS mutes an app's streams while it is not the focused app, and a
 * stream opened during that time stays muted after focus returns: the
 * video side asks for a fresh stream whenever the session gains focus */
void adev_restart(void) { atomic_store(&g_restart, 1); }

int adev_open(uint32_t freq, int channels, int bits, uint32_t chunk_bytes, int nbuf)
{
    int32_t chunk_frames;
    if (bits != 16) return -1;
    g_freq = freq;
    g_chunk = chunk_bytes;
    g_ch = channels;
    g_frame_bytes = (uint32_t)(channels * bits / 8);
    chunk_frames = (int32_t)(chunk_bytes / g_frame_bytes);
    /* the game's own queue depth, but at least ~120 ms: its threads only
     * reach SDL once per frame and hitch on loads */
    g_target_frames = chunk_frames * nbuf;
    if (g_target_frames < (int32_t)freq / 8) g_target_frames = (int32_t)freq / 8;
    if (getenv("JPRT_AUDIO_MS")) g_target_frames = (int32_t)((int64_t)freq * atoi(getenv("JPRT_AUDIO_MS")) / 1000);
    if (g_target_frames > RING_FRAMES - chunk_frames) g_target_frames = RING_FRAMES - chunk_frames;
    g_ring = calloc(RING_FRAMES, g_frame_bytes);
    if (!g_ring) return -1;
    atomic_store(&g_restart, 0);
    return open_stream();
}

int adev_free_chunks(void)
{
    static uint64_t last_log;
    if (atomic_exchange(&g_restart, 0)) {    /* on a game thread (the pump), never in a callback */
        if (g_st) { AAudioStream_requestStop(g_st); AAudioStream_close(g_st); g_st = NULL; }
        open_stream();
    }
    int64_t queued = (int64_t)(atomic_load(&g_wr) - atomic_load(&g_rd));
    int64_t room = (int64_t)g_target_frames - queued;
    uint64_t rd = atomic_load(&g_rd);
    if (rd - last_log > 44100ull * 30) {          /* every ~30 s of audio */
        last_log = rd;
        klog("audio: %u device underruns so far\n", atomic_load(&g_underruns));
    }
    return room <= 0 ? 0 : (int)(room / (int64_t)(g_chunk / g_frame_bytes));
}

void adev_submit(const void *data)
{
    uint64_t wr = atomic_load_explicit(&g_wr, memory_order_relaxed);
    uint32_t frames = g_chunk / g_frame_bytes, i;
    const int16_t *in = data;
    for (i = 0; i < frames; i++)
        memcpy(g_ring + (size_t)((wr + i) & (RING_FRAMES - 1)) * g_ch, in + (size_t)i * g_ch, g_frame_bytes);
    atomic_store_explicit(&g_wr, wr + frames, memory_order_release);
}

void adev_close(void)
{
    AAudioStream_requestStop(g_st);
    AAudioStream_close(g_st);
    g_st = NULL;
}

#else   /* other hosts: no device (audio disabled) */
int adev_open(uint32_t freq, int channels, int bits, uint32_t chunk_bytes, int nbuf)
{
    (void)freq; (void)channels; (void)bits; (void)chunk_bytes; (void)nbuf;
    return -1;
}
int adev_free_chunks(void) { return 0; }
void adev_submit(const void *data) { (void)data; }
void adev_close(void) {}
void adev_restart(void) {}
#endif

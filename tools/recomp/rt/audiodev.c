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

static AAudioStream *g_st;
static uint32_t g_chunk, g_frame_bytes;
static int32_t g_target_frames;

int adev_open(uint32_t freq, int channels, int bits, uint32_t chunk_bytes, int nbuf)
{
    AAudioStreamBuilder *b;
    if (bits != 16) return -1;
    if (AAudio_createStreamBuilder(&b) != AAUDIO_OK) return -1;
    AAudioStreamBuilder_setSampleRate(b, (int32_t)freq);
    AAudioStreamBuilder_setChannelCount(b, channels);
    AAudioStreamBuilder_setFormat(b, AAUDIO_FORMAT_PCM_I16);
    AAudioStreamBuilder_setPerformanceMode(b, AAUDIO_PERFORMANCE_MODE_LOW_LATENCY);
    AAudioStreamBuilder_setSharingMode(b, AAUDIO_SHARING_MODE_SHARED);
    if (AAudioStreamBuilder_openStream(b, &g_st) != AAUDIO_OK) { AAudioStreamBuilder_delete(b); return -1; }
    AAudioStreamBuilder_delete(b);
    g_chunk = chunk_bytes;
    g_frame_bytes = (uint32_t)(channels * bits / 8);
    g_target_frames = (int32_t)(chunk_bytes / g_frame_bytes) * nbuf;
    AAudioStream_setBufferSizeInFrames(g_st, g_target_frames);
    AAudioStream_requestStart(g_st);
    return 0;
}

int adev_free_chunks(void)
{
    int64_t queued = AAudioStream_getFramesWritten(g_st) - AAudioStream_getFramesRead(g_st);
    int64_t room = (int64_t)g_target_frames - queued;
    return room <= 0 ? 0 : (int)(room / (int64_t)(g_chunk / g_frame_bytes));
}

void adev_submit(const void *data)
{
    AAudioStream_write(g_st, data, (int32_t)(g_chunk / g_frame_bytes), 0);
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
#endif

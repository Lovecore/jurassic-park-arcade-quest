/* audiodev.h — PCM output device (audiodev.c) */
#ifndef AUDIODEV_H
#define AUDIODEV_H
#include <stdint.h>

/* open a little-endian PCM stream that holds up to nbuf chunks of chunk_bytes */
int adev_open(uint32_t freq, int channels, int bits, uint32_t chunk_bytes, int nbuf);
int adev_free_chunks(void);            /* chunks that can be submitted now */
void adev_submit(const void *data);    /* one chunk (chunk_bytes) */
void adev_close(void);
void adev_restart(void);              /* reopen the device stream (Android: after a focus change) */

#endif

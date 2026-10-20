/*
 * texdecode.c — software decoders for the block-compressed formats the
 * game ships (S3TC DXT1/DXT5, LATC1/LATC2), for GLES devices without
 * those extensions (the Quest's Adreno has no BCn). Output is tightly
 * packed: RGBA8 for DXT, L8 for LATC1, LA8 for LATC2.
 */
#include <stdint.h>
#include <string.h>
#include "texdecode.h"

static void rgb565(uint16_t c, uint8_t *o)
{
    o[0] = (uint8_t)(((c >> 11) & 31) * 255 / 31);
    o[1] = (uint8_t)(((c >> 5) & 63) * 255 / 63);
    o[2] = (uint8_t)((c & 31) * 255 / 31);
    o[3] = 255;
}

/* one 4x4 colour block (BC1 layout); `dxt1` enables the 3-colour + black mode */
static void color_block(const uint8_t *b, uint8_t out[16][4], int dxt1)
{
    uint8_t pal[4][4];
    uint16_t c0 = (uint16_t)(b[0] | b[1] << 8), c1 = (uint16_t)(b[2] | b[3] << 8);
    uint32_t bits = (uint32_t)b[4] | (uint32_t)b[5] << 8 | (uint32_t)b[6] << 16 | (uint32_t)b[7] << 24;
    int i;
    rgb565(c0, pal[0]);
    rgb565(c1, pal[1]);
    if (c0 > c1 || !dxt1) {
        for (i = 0; i < 3; i++) {
            pal[2][i] = (uint8_t)((2 * pal[0][i] + pal[1][i]) / 3);
            pal[3][i] = (uint8_t)((pal[0][i] + 2 * pal[1][i]) / 3);
        }
        pal[2][3] = pal[3][3] = 255;
    } else {
        for (i = 0; i < 3; i++) pal[2][i] = (uint8_t)((pal[0][i] + pal[1][i]) / 2);
        pal[2][3] = 255;
        pal[3][0] = pal[3][1] = pal[3][2] = 0;
        pal[3][3] = 0;
    }
    for (i = 0; i < 16; i++) memcpy(out[i], pal[(bits >> (2 * i)) & 3], 4);
}

/* one 4x4 single-channel block (BC4 / DXT5 alpha layout) */
static void channel_block(const uint8_t *b, uint8_t out[16])
{
    uint8_t v[8];
    uint64_t bits = 0;
    int i;
    v[0] = b[0];
    v[1] = b[1];
    if (v[0] > v[1])
        for (i = 1; i < 7; i++) v[i + 1] = (uint8_t)(((7 - i) * v[0] + i * v[1]) / 7);
    else {
        for (i = 1; i < 5; i++) v[i + 1] = (uint8_t)(((5 - i) * v[0] + i * v[1]) / 5);
        v[6] = 0;
        v[7] = 255;
    }
    for (i = 0; i < 6; i++) bits |= (uint64_t)b[2 + i] << (8 * i);
    for (i = 0; i < 16; i++) out[i] = v[(bits >> (3 * i)) & 7];
}

int tex_block_bytes(uint32_t fmt)
{
    switch (fmt) {
    case TEX_DXT1_RGB: case TEX_DXT1_RGBA: case TEX_LATC1: return 8;
    case TEX_DXT3: case TEX_DXT5: case TEX_LATC2: return 16;
    }
    return 0;
}

int tex_decoded_bpp(uint32_t fmt)
{
    switch (fmt) {
    case TEX_LATC1: return 1;
    case TEX_LATC2: return 2;
    case TEX_DXT1_RGB: case TEX_DXT1_RGBA: case TEX_DXT3: case TEX_DXT5: return 4;
    }
    return 0;
}

int tex_decode(uint32_t fmt, const uint8_t *src, uint32_t size, int w, int h, uint8_t *dst)
{
    int bx, by, bw = (w + 3) / 4, bh = (h + 3) / 4, bb = tex_block_bytes(fmt), bpp = tex_decoded_bpp(fmt);
    if (!bb || (uint64_t)bw * bh * bb > size) return -1;
    for (by = 0; by < bh; by++)
        for (bx = 0; bx < bw; bx++) {
            const uint8_t *b = src + ((size_t)by * bw + bx) * bb;
            uint8_t px[16][4], a[16], c2[16];
            int i;
            switch (fmt) {
            case TEX_DXT1_RGB: case TEX_DXT1_RGBA:
                color_block(b, px, 1);
                if (fmt == TEX_DXT1_RGB) for (i = 0; i < 16; i++) px[i][3] = 255;
                break;
            case TEX_DXT3:
                color_block(b + 8, px, 0);
                for (i = 0; i < 16; i++) px[i][3] = (uint8_t)(((b[i / 2] >> (4 * (i & 1))) & 15) * 17);
                break;
            case TEX_DXT5:
                channel_block(b, a);
                color_block(b + 8, px, 0);
                for (i = 0; i < 16; i++) px[i][3] = a[i];
                break;
            case TEX_LATC1:
                channel_block(b, a);
                for (i = 0; i < 16; i++) px[i][0] = a[i];
                break;
            case TEX_LATC2:
                channel_block(b, a);
                channel_block(b + 8, c2);
                for (i = 0; i < 16; i++) { px[i][0] = a[i]; px[i][1] = c2[i]; }
                break;
            }
            for (i = 0; i < 16; i++) {
                int x = bx * 4 + (i & 3), y = by * 4 + (i >> 2);
                if (x < w && y < h) memcpy(dst + ((size_t)y * w + x) * bpp, px[i], (size_t)bpp);
            }
        }
    return 0;
}

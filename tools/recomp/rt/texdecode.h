/* texdecode.h — S3TC / LATC software decoders (texdecode.c) */
#ifndef TEXDECODE_H
#define TEXDECODE_H
#include <stdint.h>

#define TEX_DXT1_RGB  0x83F0u
#define TEX_DXT1_RGBA 0x83F1u
#define TEX_DXT3      0x83F2u
#define TEX_DXT5      0x83F3u
#define TEX_LATC1     0x8C70u   /* GL_COMPRESSED_LUMINANCE_LATC1_EXT */
#define TEX_LATC2     0x8C72u   /* GL_COMPRESSED_LUMINANCE_ALPHA_LATC2_EXT */

int tex_block_bytes(uint32_t fmt);           /* 0: not a supported compressed format */
int tex_decoded_bpp(uint32_t fmt);           /* 4 (RGBA8), 2 (LA8) or 1 (L8) */
/* decode w x h texels into dst (tightly packed); -1 if src is too short */
int tex_decode(uint32_t fmt, const uint8_t *src, uint32_t size, int w, int h, uint8_t *dst);

#endif

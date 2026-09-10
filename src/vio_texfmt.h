/*
 * php-vio - Texture data formats (GAP-PHASE5 Block 9)
 *
 * Shared size arithmetic for texture uploads: RGBA8 / R8 and the block-
 * compressed BC1 / BC3 / BC4 / BC5 / BC7 formats (4x4 blocks of 8 or 16
 * bytes). Data handed to vio_texture(['format', 'layers', 'mip_levels']) is
 * level-major with the layers consecutive inside a level, every image tightly
 * packed - the KTX2 level layout, so a KTX2 payload maps 1:1.
 */

#ifndef VIO_TEXFMT_H
#define VIO_TEXFMT_H

#include <stddef.h>
#include "../include/vio_types.h"

static inline int vio_texfmt_is_compressed(int f)
{
    return f >= VIO_FORMAT_BC1 && f <= VIO_FORMAT_BC7;
}

/* Bytes per 4x4 block, 0 for uncompressed formats. */
static inline int vio_texfmt_block_bytes(int f)
{
    switch (f) {
        case VIO_FORMAT_BC1: case VIO_FORMAT_BC4: return 8;
        case VIO_FORMAT_BC3: case VIO_FORMAT_BC5: case VIO_FORMAT_BC7: return 16;
        default: return 0;
    }
}

/* Colour channels the format carries (what a shader reads back as non-zero). */
static inline int vio_texfmt_channels(int f)
{
    switch (f) {
        case VIO_FORMAT_R8: case VIO_FORMAT_BC4: return 1;
        case VIO_FORMAT_BC5: return 2;
        default: return 4;
    }
}

/* Bytes per row of texels (uncompressed) or per row of blocks (compressed). */
static inline size_t vio_texfmt_row_pitch(int f, int w)
{
    int bb = vio_texfmt_block_bytes(f);
    if (bb) return (size_t)((w + 3) / 4) * (size_t)bb;
    return (size_t)w * (f == VIO_FORMAT_R8 ? 1u : 4u);
}

/* Rows in one image: texel rows, or block rows for compressed formats. */
static inline size_t vio_texfmt_rows(int f, int h)
{
    return vio_texfmt_block_bytes(f) ? (size_t)((h + 3) / 4) : (size_t)h;
}

static inline size_t vio_texfmt_image_size(int f, int w, int h)
{
    return vio_texfmt_row_pitch(f, w) * vio_texfmt_rows(f, h);
}

/* Total bytes of a level-major payload: every level holds `layers` images. */
static inline size_t vio_texfmt_data_size(int f, int w, int h, int layers, int levels)
{
    size_t total = 0;
    if (layers < 1) layers = 1;
    if (levels < 1) levels = 1;
    for (int l = 0; l < levels; l++) {
        total += vio_texfmt_image_size(f, w, h) * (size_t)layers;
        w = w > 1 ? w / 2 : 1;
        h = h > 1 ? h / 2 : 1;
    }
    return total;
}

/* Full mip chain length for a w x h image. */
static inline int vio_texfmt_full_mip_count(int w, int h)
{
    int n = 1, m = w > h ? w : h;
    while (m > 1) { m /= 2; n++; }
    return n;
}

#endif /* VIO_TEXFMT_H */

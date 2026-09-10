/*
 * php-vio - KTX2 container reader (GAP-PHASE5 Block 9), see vio_ktx2.h
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "vio_ktx2.h"
#include "vio_texfmt.h"

static const uint8_t vio_ktx2_identifier[12] = {
    0xAB, 0x4B, 0x54, 0x58, 0x20, 0x32, 0x30, 0xBB, 0x0D, 0x0A, 0x1A, 0x0A
};

static uint32_t rd32(const uint8_t *p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }
static uint64_t rd64(const uint8_t *p) { return (uint64_t)rd32(p) | ((uint64_t)rd32(p + 4) << 32); }

/* VkFormat -> vio texture format; -1 when unsupported. sRGB twins map to UNORM. */
static int vio_ktx2_format(uint32_t vk)
{
    switch (vk) {
        case 9:                       return VIO_FORMAT_R8;      /* VK_FORMAT_R8_UNORM */
        case 37: case 43:             return VIO_FORMAT_RGBA8;   /* R8G8B8A8_UNORM / _SRGB */
        case 131: case 132:
        case 133: case 134:           return VIO_FORMAT_BC1;     /* BC1_RGB(A)_UNORM / _SRGB */
        case 137: case 138:           return VIO_FORMAT_BC3;
        case 139:                     return VIO_FORMAT_BC4;     /* BC4_UNORM */
        case 141:                     return VIO_FORMAT_BC5;     /* BC5_UNORM */
        case 145: case 146:           return VIO_FORMAT_BC7;
        default:                      return -1;
    }
}

static int fail(char *err, size_t err_len, const char *msg)
{
    if (err && err_len) snprintf(err, err_len, "%s", msg);
    return -1;
}

int vio_ktx2_parse(const uint8_t *b, size_t len, vio_ktx2_info *out, char *err, size_t err_len)
{
    if (!b || !out) return fail(err, err_len, "no data");
    memset(out, 0, sizeof(*out));
    if (len < 80) return fail(err, err_len, "file shorter than the KTX2 header");
    if (memcmp(b, vio_ktx2_identifier, 12) != 0) return fail(err, err_len, "not a KTX2 file (identifier mismatch)");

    uint32_t vk_format    = rd32(b + 12);
    /* typeSize at +16 is only needed for endianness conversion of >8-bit texels; the
     * formats accepted here are byte streams or blocks, so it is not consulted. */
    uint32_t width        = rd32(b + 20);
    uint32_t height       = rd32(b + 24);
    uint32_t depth        = rd32(b + 28);
    uint32_t layer_count  = rd32(b + 32);
    uint32_t face_count   = rd32(b + 36);
    uint32_t level_count  = rd32(b + 40);
    uint32_t supercomp    = rd32(b + 44);

    if (supercomp != 0) return fail(err, err_len, "supercompressed KTX2 (Basis / zstd) is not supported - decode offline");
    if (face_count != 1) return fail(err, err_len, "cubemap KTX2 files are not supported here (use vio_cubemap)");
    if (depth > 1) return fail(err, err_len, "3D KTX2 textures are not supported here (use vio_texture_3d)");
    if (width == 0 || height == 0 || width > 16384 || height > 16384) return fail(err, err_len, "invalid KTX2 dimensions");
    int fmt = vio_ktx2_format(vk_format);
    if (fmt < 0) {
        if (err && err_len) snprintf(err, err_len, "unsupported vkFormat %u (supported: R8, RGBA8, BC1/BC3/BC4/BC5/BC7)", vk_format);
        return -1;
    }
    if (layer_count > 2048) return fail(err, err_len, "too many array layers");
    if (level_count > VIO_KTX2_MAX_LEVELS) return fail(err, err_len, "more than 16 mip levels");
    int layers = layer_count == 0 ? 1 : (int)layer_count;
    int levels = level_count == 0 ? 1 : (int)level_count;
    if (levels > vio_texfmt_full_mip_count((int)width, (int)height)) return fail(err, err_len, "level count exceeds the mip chain of the base size");

    /* Level index follows the 80-byte header + index section. */
    size_t need_index = 80 + (size_t)levels * 24;
    if (len < need_index) return fail(err, err_len, "truncated KTX2 level index");
    int lw = (int)width, lh = (int)height;
    for (int l = 0; l < levels; l++) {
        const uint8_t *e = b + 80 + (size_t)l * 24;
        uint64_t off = rd64(e), length = rd64(e + 8);
        uint64_t want = (uint64_t)vio_texfmt_image_size(fmt, lw, lh) * (uint64_t)layers;
        if (off > len || length > len - off) return fail(err, err_len, "KTX2 level data outside the file");
        if (length < want) return fail(err, err_len, "KTX2 level shorter than its image size");
        out->level_offset[l] = off;
        out->level_length[l] = want;   /* what we copy; trailing padding is ignored */
        lw = lw > 1 ? lw / 2 : 1;
        lh = lh > 1 ? lh / 2 : 1;
    }

    out->vk_format  = vk_format;
    out->vio_format = fmt;
    out->width  = (int)width;
    out->height = (int)height;
    out->layers = layers;
    out->levels = levels;
    return 0;
}

int vio_ktx2_repack(const vio_ktx2_info *info, const uint8_t *b, size_t len, int mip_offset,
                    uint8_t **out, size_t *out_len, int *out_w, int *out_h, int *out_levels)
{
    if (!info || !b || !out || !out_len) return -1;
    if (mip_offset < 0) mip_offset = 0;
    if (mip_offset > info->levels - 1) mip_offset = info->levels - 1;
    int w = info->width, h = info->height;
    for (int l = 0; l < mip_offset; l++) { w = w > 1 ? w / 2 : 1; h = h > 1 ? h / 2 : 1; }
    int levels = info->levels - mip_offset;

    size_t total = 0;
    for (int l = mip_offset; l < info->levels; l++) total += (size_t)info->level_length[l];
    uint8_t *buf = (uint8_t *)malloc(total ? total : 1);
    if (!buf) return -1;
    size_t pos = 0;
    for (int l = mip_offset; l < info->levels; l++) {
        size_t n = (size_t)info->level_length[l];
        if (info->level_offset[l] + n > len) { free(buf); return -1; }
        memcpy(buf + pos, b + info->level_offset[l], n);
        pos += n;
    }
    *out = buf;
    *out_len = total;
    if (out_w) *out_w = w;
    if (out_h) *out_h = h;
    if (out_levels) *out_levels = levels;
    return 0;
}

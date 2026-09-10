/*
 * php-vio - KTX2 container reader (GAP-PHASE5 Block 9)
 *
 * Reads the subset a GPU texture loader needs: 2D textures and 2D arrays in
 * R8 / RGBA8 / BC1 / BC3 / BC4 / BC5 / BC7 without supercompression (Basis /
 * zstd payloads are rejected - decode them offline). sRGB variants load as
 * their UNORM twin (the sampler does not linearise). The level index is
 * validated against the payload so a truncated file never reads out of bounds.
 */

#ifndef VIO_KTX2_H
#define VIO_KTX2_H

#include <stddef.h>
#include <stdint.h>

#define VIO_KTX2_MAX_LEVELS 16

typedef struct _vio_ktx2_info {
    uint32_t vk_format;                       /* as stored in the header */
    int      vio_format;                      /* VIO_FORMAT_RGBA8 / R8 / BC* */
    int      width, height;
    int      layers;                          /* 1 for a plain 2D texture */
    int      levels;                          /* >= 1 */
    uint64_t level_offset[VIO_KTX2_MAX_LEVELS];
    uint64_t level_length[VIO_KTX2_MAX_LEVELS];
} vio_ktx2_info;

/* Parse header + level index. 0 on success; otherwise -1 with a message in err. */
int vio_ktx2_parse(const uint8_t *bytes, size_t len, vio_ktx2_info *out, char *err, size_t err_len);

/* Repack the levels (skipping the `mip_offset` largest ones, clamped so at
 * least one remains) into the level-major layout vio_texture_desc expects.
 * *out is malloc()ed; the caller free()s it. Fills the surviving base size and
 * level count. 0 on success. */
int vio_ktx2_repack(const vio_ktx2_info *info, const uint8_t *bytes, size_t len, int mip_offset,
                    uint8_t **out, size_t *out_len, int *out_w, int *out_h, int *out_levels);

#endif /* VIO_KTX2_H */

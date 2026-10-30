#ifndef VIO_OPACITY_MICROMAP_H
#define VIO_OPACITY_MICROMAP_H

#include <stddef.h>
#include <stdint.h>

/* Opacity micromap baking: classifies the 4^level micro-triangles of each
 * triangle from an alpha texture. Plain C, no GPU, no Zend. */

#define VIO_OMM_STATE_TRANSPARENT          0
#define VIO_OMM_STATE_OPAQUE               1
#define VIO_OMM_STATE_UNKNOWN_TRANSPARENT  2
#define VIO_OMM_STATE_UNKNOWN_OPAQUE       3

typedef struct {
    const float          *uvs;          /* u, v per vertex */
    const uint32_t       *indices;      /* tri_count * 3, already range-checked */
    int                   tri_count;
    const unsigned char  *pixels;
    int                   width, height;
    int                   stride;       /* bytes per texel */
    int                   channel;      /* byte offset of alpha within a texel */
    int                   level;        /* 0..12 */
    int                   format;       /* 2 or 4 */
    float                 threshold;    /* 0..1 */
    int                   clamp;        /* 0 = repeat */
} vio_omm_bake_desc;

/* Writes tri_count * 4^level state bytes in micro-triangle order. counts[] is
 * indexed by state. */
void vio_omm_bake(const vio_omm_bake_desc *d, unsigned char *states, uint64_t counts[4]);

#endif

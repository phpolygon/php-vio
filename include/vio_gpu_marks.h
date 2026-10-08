#ifndef VIO_GPU_MARKS_H
#define VIO_GPU_MARKS_H

#include <stdint.h>
#include <string.h>

/* Named GPU timestamps (vio_gpu_timestamp / vio_gpu_timings, OPEN-ITEMS-PLAN A19).
 * A backend keeps one vio_gpu_mark_names per frame slot next to the begin / end
 * stamps of vio_gpu_frame_time: timestamp 0 = frame begin, 1 = frame end,
 * 2 + i = mark i. When the slot's frame has retired, vio_gpu_mark_resolve turns
 * the stamps into section lengths. */
#define VIO_GPU_MARKS_MAX      32
#define VIO_GPU_MARK_NAME_MAX  48
#define VIO_GPU_TS_PER_FRAME   (2 + VIO_GPU_MARKS_MAX)

typedef struct _vio_gpu_mark_names {
    int  count;
    char name[VIO_GPU_MARKS_MAX][VIO_GPU_MARK_NAME_MAX];
} vio_gpu_mark_names;

typedef struct _vio_gpu_mark_result {
    int    count;
    char   name[VIO_GPU_MARKS_MAX][VIO_GPU_MARK_NAME_MAX];
    double ms[VIO_GPU_MARKS_MAX];   /* previous mark (or frame begin) to this mark */
} vio_gpu_mark_result;

/* Next mark index of the slot, or -1 when the frame already has the maximum. */
static inline int vio_gpu_mark_push(vio_gpu_mark_names *n, const char *name)
{
    if (n->count >= VIO_GPU_MARKS_MAX) return -1;
    int i = n->count++;
    strncpy(n->name[i], name, VIO_GPU_MARK_NAME_MAX - 1);
    n->name[i][VIO_GPU_MARK_NAME_MAX - 1] = '\0';
    return i;
}

/* ticks[0] = frame begin, ticks[2 + i] = mark i, in units of ms_per_tick. */
static inline void vio_gpu_mark_resolve(vio_gpu_mark_result *out, const vio_gpu_mark_names *n,
                                        const uint64_t *ticks, double ms_per_tick)
{
    uint64_t prev = ticks[0];
    out->count = n->count;
    for (int i = 0; i < n->count; i++) {
        uint64_t t = ticks[2 + i];
        memcpy(out->name[i], n->name[i], VIO_GPU_MARK_NAME_MAX);
        out->ms[i] = t > prev ? (double)(t - prev) * ms_per_tick : 0.0;
        if (t > prev) prev = t;
    }
}

#endif /* VIO_GPU_MARKS_H */

#include "vio_opacity_micromap.h"

#include <math.h>

static int omm_wrap(int i, int n, int clamp)
{
    if (clamp) return i < 0 ? 0 : (i >= n ? n - 1 : i);
    i %= n;
    return i < 0 ? i + n : i;
}

static float omm_texel(const vio_omm_bake_desc *d, int x, int y)
{
    x = omm_wrap(x, d->width, d->clamp);
    y = omm_wrap(y, d->height, d->clamp);
    return (float)d->pixels[((size_t)y * (size_t)d->width + (size_t)x) * (size_t)d->stride + (size_t)d->channel] / 255.0f;
}

/* Position in texels, bilinear like the sampler an any-hit shader would use. */
static float omm_sample(const vio_omm_bake_desc *d, float x, float y)
{
    float fx = x - 0.5f, fy = y - 0.5f;
    float bx = floorf(fx), by = floorf(fy);
    float tx = fx - bx, ty = fy - by;
    int ix = (int)bx, iy = (int)by;
    float top = omm_texel(d, ix, iy) * (1.0f - tx) + omm_texel(d, ix + 1, iy) * tx;
    float bot = omm_texel(d, ix, iy + 1) * (1.0f - tx) + omm_texel(d, ix + 1, iy + 1) * tx;
    return top * (1.0f - ty) + bot * ty;
}

static float omm_min3(float a, float b, float c) { float m = a < b ? a : b; return m < c ? m : c; }
static float omm_max3(float a, float b, float c) { float m = a > b ? a : b; return m > c ? m : c; }

/* Corners a, b, c in texels. Known-state test: every texel a bilinear lookup
 * inside the micro-triangle can reach, so a known state is never wrong. */
static int omm_classify(const vio_omm_bake_desc *d, const float a[2], const float b[2], const float c[2])
{
    int x0 = (int)floorf(omm_min3(a[0], b[0], c[0]) - 0.5f), x1 = (int)floorf(omm_max3(a[0], b[0], c[0]) - 0.5f) + 1;
    int y0 = (int)floorf(omm_min3(a[1], b[1], c[1]) - 0.5f), y1 = (int)floorf(omm_max3(a[1], b[1], c[1]) - 0.5f) + 1;
    if (x1 - x0 >= d->width) x1 = x0 + d->width - 1;
    if (y1 - y0 >= d->height) y1 = y0 + d->height - 1;
    int above = 0, below = 0;
    for (int y = y0; y <= y1 && !(above && below); y++) {
        for (int x = x0; x <= x1; x++) {
            if (omm_texel(d, x, y) >= d->threshold) above = 1; else below = 1;
            if (above && below) break;
        }
    }
    if (d->format == 4 && !below) return VIO_OMM_STATE_OPAQUE;
    if (d->format == 4 && !above) return VIO_OMM_STATE_TRANSPARENT;

    /* Mixed: the covered share decides. About one sample per texel of edge length. */
    float edge = omm_max3(hypotf(a[0] - b[0], a[1] - b[1]), hypotf(b[0] - c[0], b[1] - c[1]), hypotf(c[0] - a[0], c[1] - a[1]));
    int k = (int)ceilf(edge);
    if (k < 2) k = 2;
    if (k > 16) k = 16;
    int hits = 0, total = 0;
    for (int i = 0; i < k; i++) {
        for (int j = 0; j < k - i; j++) {
            float s = ((float)i + 1.0f / 3.0f) / (float)k, t = ((float)j + 1.0f / 3.0f) / (float)k, r = 1.0f - s - t;
            if (omm_sample(d, a[0] * r + b[0] * s + c[0] * t, a[1] * r + b[1] * s + c[1] * t) >= d->threshold) hits++;
            total++;
        }
    }
    int opaque = hits * 2 >= total;
    if (d->format == 4) return opaque ? VIO_OMM_STATE_UNKNOWN_OPAQUE : VIO_OMM_STATE_UNKNOWN_TRANSPARENT;
    return opaque ? VIO_OMM_STATE_OPAQUE : VIO_OMM_STATE_TRANSPARENT;
}

/* Hit barycentrics (u, v) weigh vertices 1 and 2, w = 1 - u - v vertex 0. */
static void omm_corner(const float p[3][2], const float bary[2], float out[2])
{
    float w = 1.0f - bary[0] - bary[1];
    out[0] = p[0][0] * w + p[1][0] * bary[0] + p[2][0] * bary[1];
    out[1] = p[0][1] * w + p[1][1] * bary[0] + p[2][1] * bary[1];
}

typedef struct {
    const vio_omm_bake_desc *d;
    float                    p[3][2];    /* the triangle, in texels */
    unsigned char           *out;
    uint64_t                *counts;
} omm_walk;

/* The micro-triangle order, shared by D3D12 and Vulkan and measured on hardware
 * (test 252): each triangle (a, b, c) splits into four children that take
 * consecutive index ranges - corner a, the inverted centre, corner b, corner c. */
static void omm_descend(omm_walk *m, const float a[2], const float b[2], const float c[2], int level, size_t index)
{
    if (level == 0) {
        float pa[2], pb[2], pc[2];
        omm_corner(m->p, a, pa);
        omm_corner(m->p, b, pb);
        omm_corner(m->p, c, pc);
        int s = omm_classify(m->d, pa, pb, pc);
        m->out[index] = (unsigned char)s;
        m->counts[s]++;
        return;
    }
    float ab[2] = { (a[0] + b[0]) * 0.5f, (a[1] + b[1]) * 0.5f };
    float bc[2] = { (b[0] + c[0]) * 0.5f, (b[1] + c[1]) * 0.5f };
    float ca[2] = { (c[0] + a[0]) * 0.5f, (c[1] + a[1]) * 0.5f };
    size_t step = (size_t)1 << (2 * (level - 1));
    omm_descend(m, a, ab, ca, level - 1, index);
    omm_descend(m, ca, bc, ab, level - 1, index + step);
    omm_descend(m, ab, b, bc, level - 1, index + 2 * step);
    omm_descend(m, bc, ca, c, level - 1, index + 3 * step);
}

void vio_omm_bake(const vio_omm_bake_desc *d, unsigned char *states, uint64_t counts[4])
{
    static const float origin[2] = { 0.0f, 0.0f }, unit_u[2] = { 1.0f, 0.0f }, unit_v[2] = { 0.0f, 1.0f };
    const size_t per = (size_t)1 << (2 * d->level);
    counts[0] = counts[1] = counts[2] = counts[3] = 0;
    for (int t = 0; t < d->tri_count; t++) {
        omm_walk m;
        m.d = d;
        m.out = states + (size_t)t * per;
        m.counts = counts;
        for (int i = 0; i < 3; i++) {
            uint32_t v = d->indices[(size_t)t * 3 + (size_t)i];
            m.p[i][0] = d->uvs[(size_t)v * 2] * (float)d->width;
            m.p[i][1] = d->uvs[(size_t)v * 2 + 1] * (float)d->height;
        }
        omm_descend(&m, origin, unit_u, unit_v, d->level, 0);
    }
}

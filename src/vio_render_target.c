/*
 * php-vio - Render target (offscreen FBO) Zend object
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "vio_render_target.h"
#include <math.h>
#include <string.h>
#include "../include/vio_backend.h"

zend_class_entry *vio_render_target_ce = NULL;
static zend_object_handlers vio_render_target_handlers;

static zend_object *vio_render_target_create_object(zend_class_entry *ce)
{
    vio_render_target_object *rt = zend_object_alloc(sizeof(vio_render_target_object), ce);

    rt->fbo           = 0;
    rt->color_texture = 0;
    rt->depth_texture = 0;
    rt->d3d11_rtv       = NULL;
    rt->d3d11_dsv       = NULL;
    rt->d3d11_color_tex = NULL;
    rt->d3d11_depth_tex = NULL;
    rt->d3d11_depth_srv = NULL;
    rt->d3d11_color_srv = NULL;
    rt->d3d11_color_backend_texture = NULL;
    rt->d3d11_depth_backend_texture = NULL;
    rt->d3d12_color_resource = NULL;
    rt->d3d12_depth_resource = NULL;
    rt->d3d12_rtv_heap       = NULL;
    rt->d3d12_dsv_heap       = NULL;
    rt->d3d12_color_backend_texture = NULL;
    rt->d3d12_depth_backend_texture = NULL;
    rt->metal_color_texture = NULL;
    rt->metal_depth_texture = NULL;
    rt->metal_color_backend_texture = NULL;
    rt->metal_depth_backend_texture = NULL;
    rt->metal_msaa_color_texture = NULL;
    rt->metal_msaa_depth_texture = NULL;
    rt->vulkan_color_image  = NULL;
    rt->vulkan_color_alloc  = NULL;
    rt->vulkan_color_view   = NULL;
    rt->vulkan_depth_image  = NULL;
    rt->vulkan_depth_alloc  = NULL;
    rt->vulkan_depth_view   = NULL;
    rt->vulkan_render_pass  = NULL;
    rt->vulkan_framebuffer  = NULL;
    rt->vulkan_sampler      = NULL;
    rt->vulkan_color_backend_texture = NULL;
    rt->width         = 0;
    rt->height        = 0;
    rt->depth_only    = 0;
    rt->samples       = 1;
    rt->is_cube       = 0;
    rt->mip_levels    = 1;
    rt->bound_face    = -1;
    rt->bound_level   = 0;
    rt->valid         = 0;
    rt->backend_type  = VIO_RT_BACKEND_NONE;
    rt->backend       = NULL;

    zend_object_std_init(&rt->std, ce);
    object_properties_init(&rt->std, ce);
    rt->std.handlers = &vio_render_target_handlers;

    return &rt->std;
}

int vio_rt_format_bpp(int format)
{
    switch (format) {
        case VIO_FORMAT_RGBA16F:    return 8;
        case VIO_FORMAT_RGBA32F:    return 16;
        case VIO_FORMAT_R16F:       return 2;
        case VIO_FORMAT_R8:         return 1;
        case VIO_FORMAT_RGBA8:
        case VIO_FORMAT_R11G11B10F:
        case VIO_FORMAT_RG16F:
        case VIO_FORMAT_R32F:
        default:                    return 4;
    }
}

static float vio_rt_half_to_float(uint16_t h)
{
    uint32_t sign = (uint32_t)(h & 0x8000) << 16;
    uint32_t exp  = (h >> 10) & 0x1F;
    uint32_t mant = h & 0x3FF;
    uint32_t bits;
    if (exp == 0) {
        if (mant == 0) { bits = sign; }
        else {
            exp = 127 - 15 + 1;
            while (!(mant & 0x400)) { mant <<= 1; exp--; }
            mant &= 0x3FF;
            bits = sign | (exp << 23) | (mant << 13);
        }
    } else if (exp == 31) {
        bits = sign | 0x7F800000u | (mant << 13);
    } else {
        bits = sign | ((exp + 127 - 15) << 23) | (mant << 13);
    }
    float f; memcpy(&f, &bits, sizeof(f)); return f;
}

/* 11-bit (5e6m) / 10-bit (5e5m) unsigned floats of R11G11B10F. */
static float vio_rt_ufloat_to_float(uint32_t v, int mant_bits)
{
    uint32_t exp  = (v >> mant_bits) & 0x1F;
    uint32_t mant = v & ((1u << mant_bits) - 1u);
    if (exp == 0) {
        return mant ? (float)mant / (float)(1u << mant_bits) * (1.0f / 16384.0f) : 0.0f;
    }
    if (exp == 31) return mant ? 0.0f : 1e30f;
    return (1.0f + (float)mant / (float)(1u << mant_bits)) * ldexpf(1.0f, (int)exp - 15);
}

static unsigned char vio_rt_unit_to_byte(float v)
{
    if (!(v > 0.0f)) return 0;          /* also catches NaN */
    if (v >= 1.0f) return 255;
    return (unsigned char)(v * 255.0f + 0.5f);
}

void vio_rt_convert_to_rgba8(int format, int bgra, const void *src, size_t src_pitch,
                             int w, int h, unsigned char *out)
{
    for (int y = 0; y < h; y++) {
        const unsigned char *row = (const unsigned char *)src + (size_t)y * src_pitch;
        unsigned char *dst = out + (size_t)y * w * 4;
        for (int x = 0; x < w; x++, dst += 4) {
            float r = 0.0f, g = 0.0f, b = 0.0f, a = 1.0f;
            switch (format) {
                case VIO_FORMAT_RGBA8: {
                    const unsigned char *p = row + x * 4;
                    if (bgra) { dst[0] = p[2]; dst[1] = p[1]; dst[2] = p[0]; dst[3] = p[3]; }
                    else      { dst[0] = p[0]; dst[1] = p[1]; dst[2] = p[2]; dst[3] = p[3]; }
                    continue;
                }
                case VIO_FORMAT_R8:
                    dst[0] = row[x]; dst[1] = 0; dst[2] = 0; dst[3] = 255;
                    continue;
                case VIO_FORMAT_RGBA16F: {
                    const uint16_t *p = (const uint16_t *)(row + x * 8);
                    r = vio_rt_half_to_float(p[0]); g = vio_rt_half_to_float(p[1]);
                    b = vio_rt_half_to_float(p[2]); a = vio_rt_half_to_float(p[3]);
                    break;
                }
                case VIO_FORMAT_RGBA32F: {
                    const float *p = (const float *)(row + x * 16);
                    r = p[0]; g = p[1]; b = p[2]; a = p[3];
                    break;
                }
                case VIO_FORMAT_R11G11B10F: {
                    uint32_t v; memcpy(&v, row + x * 4, 4);
                    r = vio_rt_ufloat_to_float(v & 0x7FFu, 6);
                    g = vio_rt_ufloat_to_float((v >> 11) & 0x7FFu, 6);
                    b = vio_rt_ufloat_to_float((v >> 22) & 0x3FFu, 5);
                    break;
                }
                case VIO_FORMAT_RG16F: {
                    const uint16_t *p = (const uint16_t *)(row + x * 4);
                    r = vio_rt_half_to_float(p[0]); g = vio_rt_half_to_float(p[1]);
                    break;
                }
                case VIO_FORMAT_R16F: {
                    const uint16_t *p = (const uint16_t *)(row + x * 2);
                    r = vio_rt_half_to_float(p[0]);
                    break;
                }
                case VIO_FORMAT_R32F: {
                    float f; memcpy(&f, row + x * 4, 4); r = f;
                    break;
                }
                default:
                    break;
            }
            dst[0] = vio_rt_unit_to_byte(r); dst[1] = vio_rt_unit_to_byte(g);
            dst[2] = vio_rt_unit_to_byte(b); dst[3] = vio_rt_unit_to_byte(a);
        }
    }
}

static void vio_render_target_free_object(zend_object *obj)
{
    vio_render_target_object *rt = vio_render_target_from_obj(obj);

    if (rt->backend && rt->backend->destroy_render_target) {
        rt->backend->destroy_render_target(rt);
    }

    zend_object_std_dtor(&rt->std);
}

void vio_render_target_register(void)
{
    zend_class_entry ce;

    INIT_CLASS_ENTRY(ce, "VioRenderTarget", NULL);
    vio_render_target_ce = zend_register_internal_class(&ce);
    vio_render_target_ce->ce_flags |= ZEND_ACC_FINAL | ZEND_ACC_NO_DYNAMIC_PROPERTIES | ZEND_ACC_NOT_SERIALIZABLE;
    vio_render_target_ce->create_object = vio_render_target_create_object;

    memcpy(&vio_render_target_handlers, &std_object_handlers, sizeof(zend_object_handlers));
    vio_render_target_handlers.offset   = XtOffsetOf(vio_render_target_object, std);
    vio_render_target_handlers.free_obj = vio_render_target_free_object;
    vio_render_target_handlers.clone_obj = NULL;
}

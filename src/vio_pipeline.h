/*
 * php-vio - Pipeline management
 */

#ifndef VIO_PIPELINE_H
#define VIO_PIPELINE_H

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "php.h"
#include "../include/vio_types.h"

struct _vio_backend;

typedef struct _vio_pipeline_object {
    unsigned int   shader_program;   /* GL program (copied, not owned) */
    void          *backend_pipeline; /* Backend-specific pipeline (D3D11/D3D12/Vulkan) */
    const struct _vio_backend *backend; /* owner of backend_pipeline — destroy_pipeline on free */
    void          *backend_shader;   /* Backend shader ref (for pipeline creation) */
    void          *shader_ref;       /* vio_shader_object* (for uniform cbuffer) */
    zend_object   *shader_obj;       /* strong ref to the VioShader zend_object so
                                        shader_ref / backend_shader can't dangle if
                                        the PHP VioShader goes out of scope while the
                                        pipeline is still alive (the pipeline owns it) */
    vio_topology   topology;
    vio_cull_mode  cull_mode;
    int            depth_test;
    vio_depth_func depth_func;
    vio_blend_mode blend;
    int            depth_write;      /* depth writes on (default 1) — independent of depth_test */
    int            color_mask;       /* VIO_COLOR_* bits (default VIO_COLOR_RGBA) */
    float          depth_bias;
    float          slope_scaled_depth_bias;
    int            hdr_output;        /* 1 => PSO output (RTV) format is FP16
                                         (R16G16B16A16_FLOAT) to match an hdr=true
                                         render target; 0 (default) => R8G8B8A8_UNORM.
                                         D3D12-only this round; other backends ignore. */
    int            color_count;       /* 'attachments' => [...]: MRT output formats the
                                         D3D12 PSO is built with (0 => single target from
                                         hdr_output). Other backends derive them from the
                                         bound render target. */
    int            color_formats[4];  /* vio_pixel_format, VIO_MAX_COLOR_ATTACHMENTS */
    int            valid;
    zend_object    std;
} vio_pipeline_object;

extern zend_class_entry *vio_pipeline_ce;

void vio_pipeline_register(void);

static inline vio_pipeline_object *vio_pipeline_from_obj(zend_object *obj) {
    return (vio_pipeline_object *)((char *)obj - XtOffsetOf(vio_pipeline_object, std));
}

#define Z_VIO_PIPELINE_P(zv) vio_pipeline_from_obj(Z_OBJ_P(zv))

#endif /* VIO_PIPELINE_H */

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

typedef struct _vio_pipeline_object {
    unsigned int   shader_program;   /* GL program (copied, not owned) */
    void          *backend_pipeline; /* Backend-specific pipeline (D3D11/D3D12/Vulkan) */
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
    float          depth_bias;
    float          slope_scaled_depth_bias;
    int            hdr_output;        /* 1 => PSO output (RTV) format is FP16
                                         (R16G16B16A16_FLOAT) to match an hdr=true
                                         render target; 0 (default) => R8G8B8A8_UNORM.
                                         D3D12-only this round; other backends ignore. */
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

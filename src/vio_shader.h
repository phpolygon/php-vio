/*
 * php-vio - Shader management
 */

#ifndef VIO_SHADER_H
#define VIO_SHADER_H

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "php.h"
#include "../include/vio_types.h"

#define VIO_MAX_UNIFORMS 256
#define VIO_CBUFFER_SIZE 4096
#define VIO_MAX_SAMPLERS 16

typedef struct _vio_uniform_entry {
    char    name[64];
    int     offset;    /* byte offset in cbuffer */
    int     size;      /* size in bytes */
} vio_uniform_entry;

typedef struct _vio_shader_object {
    unsigned int      program;       /* GL program ID (0 if not OpenGL) */
    vio_shader_format format;
    uint32_t         *vert_spirv;    /* SPIR-V binary for vertex shader */
    size_t            vert_spirv_size;
    uint32_t         *frag_spirv;    /* SPIR-V binary for fragment shader */
    size_t            frag_spirv_size;
    void             *backend_shader; /* Backend-specific compiled shader (D3D11/D3D12/Vulkan) */
    /* Uniform buffer for D3D constant buffer mapping — vertex stage */
    unsigned char     cbuffer_data[VIO_CBUFFER_SIZE];
    vio_uniform_entry uniforms[VIO_MAX_UNIFORMS];
    int               uniform_count;
    int               cbuffer_total_size;
    void             *cbuffer_backend;  /* Backend constant buffer (vertex) */
    int               cbuffer_dirty;    /* 1 if data changed since last upload */
    /* Fragment stage constant buffer */
    unsigned char     frag_cbuffer_data[VIO_CBUFFER_SIZE];
    vio_uniform_entry frag_uniforms[VIO_MAX_UNIFORMS];
    int               frag_uniform_count;
    int               frag_cbuffer_total_size;
    void             *frag_cbuffer_backend;
    int               frag_cbuffer_dirty;
    /* Sampler binding map: sampler_names[i] -> hlsl register i */
    char              sampler_names[VIO_MAX_SAMPLERS][64];
    int               sampler_count;
    /* Runtime GL-slot to HLSL-binding remap: gl_to_hlsl[gl_slot] = hlsl_binding (-1 = unmapped) */
    int               gl_to_hlsl_sampler[16];
    int               valid;
    zend_object       std;
} vio_shader_object;

extern zend_class_entry *vio_shader_ce;

void vio_shader_register(void);

static inline vio_shader_object *vio_shader_from_obj(zend_object *obj) {
    return (vio_shader_object *)((char *)obj - XtOffsetOf(vio_shader_object, std));
}

#define Z_VIO_SHADER_P(zv) vio_shader_from_obj(Z_OBJ_P(zv))

#endif /* VIO_SHADER_H */

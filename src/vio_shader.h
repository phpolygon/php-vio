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

struct _vio_backend;

/* Number of optional stages beyond vertex + fragment: geometry, tessellation
 * control, tessellation evaluation. Extra-stage arrays are indexed by
 * VIO_EXTRA_STAGE_INDEX(stage) == stage - VIO_STAGE_GEOMETRY. */
#define VIO_EXTRA_STAGE_COUNT 3
#define VIO_EXTRA_STAGE_INDEX(stage) ((stage) - VIO_STAGE_GEOMETRY)

/* Constant block of one extra stage (typed-register backends: D3D11/D3D12).
 * Same layout/lifecycle as the vertex cbuffer fields below; allocated only
 * when the stage exists so the common VS+FS shader object stays small. */
typedef struct _vio_shader_stage_cb {
    unsigned char     data[VIO_CBUFFER_SIZE];
    vio_uniform_entry uniforms[VIO_MAX_UNIFORMS];
    int               uniform_count;
    int               total_size;
    void             *backend;   /* backend constant buffer (create_buffer) */
    int               dirty;
} vio_shader_stage_cb;

typedef struct _vio_shader_object {
    unsigned int      program;       /* GL program ID (0 if not OpenGL) */
    vio_shader_format format;
    uint32_t         *vert_spirv;    /* SPIR-V binary for vertex shader */
    size_t            vert_spirv_size;
    uint32_t         *frag_spirv;    /* SPIR-V binary for fragment shader */
    size_t            frag_spirv_size;
    /* Optional geometry / tess-control / tess-eval stages (NULL = absent). */
    uint32_t         *stage_spirv[VIO_EXTRA_STAGE_COUNT];
    size_t            stage_spirv_size[VIO_EXTRA_STAGE_COUNT];
    vio_shader_stage_cb *stage_cb[VIO_EXTRA_STAGE_COUNT];
    int               has_geometry;  /* 1 => geometry stage present */
    int               has_tessellation; /* 1 => tess control + eval present */
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
    int               sampler_is_depth[VIO_MAX_SAMPLERS]; /* 1 if sampler2DShadow */
    int               sampler_hlsl_reg[VIO_MAX_SAMPLERS];  /* actual HLSL t-register assigned by the
                                                            * cross-compiler (regular: 0,1,2..; depth: 4,5,6..)
                                                            * — mirrors vio_shader_reflect.c emit logic */
    int               sampler_count;
    /* Runtime GL-slot to HLSL-binding remap: gl_to_hlsl[gl_slot] = hlsl_binding (-1 = unmapped) */
    int               gl_to_hlsl_sampler[16];
    /* Lazily-built name -> (stage,offset,size) map for O(1) uniform lookup on the
     * hot draw path (replaces a linear strcmp scan run for every uniform set).
     * NULL until first use; freed in vio_shader_free_object. */
    HashTable        *uniform_lookup;
    int               valid;
    const struct _vio_backend *backend;
    unsigned int gl_generation;   /* OpenGL: context generation that owns the GL names (vio_opengl.c) */
    zend_object       std;
} vio_shader_object;

extern zend_class_entry *vio_shader_ce;

void vio_shader_register(void);

static inline vio_shader_object *vio_shader_from_obj(zend_object *obj) {
    return (vio_shader_object *)((char *)obj - XtOffsetOf(vio_shader_object, std));
}

#define Z_VIO_SHADER_P(zv) vio_shader_from_obj(Z_OBJ_P(zv))

#endif /* VIO_SHADER_H */

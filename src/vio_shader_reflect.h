/*
 * php-vio - Shader reflection and cross-compilation (via SPIRV-Cross)
 */

#ifndef VIO_SHADER_REFLECT_H
#define VIO_SHADER_REFLECT_H

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <stddef.h>
#include <stdint.h>

/* Transpile SPIR-V to GLSL (target version e.g. 410).
 * Returns malloc'd string (caller frees). NULL on failure. */
char *vio_spirv_to_glsl(const uint32_t *spirv, size_t spirv_size, int version, char **error_msg);

/* Transpile a COMPUTE SPIR-V module to GLSL (target version >= 430, clamped).
 * Unlike vio_spirv_to_glsl(), keeps UBOs as std140 blocks and SSBOs as std430
 * blocks with their explicit `binding =` qualifiers (required by the OpenGL
 * compute primitive). Returns malloc'd string (caller frees). NULL on failure. */
char *vio_spirv_to_glsl_compute(const uint32_t *spirv, size_t spirv_size, int version, char **error_msg);

/* Transpile SPIR-V to MSL.
 * Returns malloc'd string (caller frees). NULL on failure. */
char *vio_spirv_to_msl(const uint32_t *spirv, size_t spirv_size, char **error_msg);

/* Transpile SPIR-V to HLSL (target shader_model e.g. 50=SM5.0, 51=SM5.1).
 * Returns malloc'd string (caller frees). NULL on failure. */
char *vio_spirv_to_hlsl(const uint32_t *spirv, size_t spirv_size, int shader_model, char **error_msg);

/* Same, with explicit control over the GL -> D3D clip-space depth fixup
 * (z' = (z + w) / 2 on gl_Position writes). SPIRV-Cross applies it to EVERY
 * vertex-like stage, so a VS + GS (or VS + TES) pair would convert twice;
 * callers pass fixup_depth = 1 only for the LAST stage that writes
 * gl_Position and 0 for the ones before it. */
char *vio_spirv_to_hlsl_ex(const uint32_t *spirv, size_t spirv_size, int shader_model,
                           int fixup_depth, char **error_msg);

/* Can the linked SPIRV-Cross emit HLSL for this graphics stage
 * (vio_shader_stage)? Geometry (2025-05) and hull / domain support arrived in
 * SPIRV-Cross long after vertex / fragment, and the Vulkan-SDK builds the
 * Windows CI and users link against lag behind - so the D3D backends only
 * report VIO_FEATURE_GEOMETRY / TESSELLATION when a canonical minimal stage
 * actually transpiles. Probed once per process (glslang + SPIRV-Cross round
 * trip of a few lines of GLSL) and cached. Returns 1 / 0. */
int vio_hlsl_stage_supported(int stage);

/* The transpiled HLSL of the canonical probe stage (malloc'd, caller frees;
 * NULL when glslang / SPIRV-Cross cannot produce it). The D3D backends feed it
 * through FXC as the second half of the probe: SPIRV-Cross may emit HLSL that
 * the compiler then rejects (e.g. an undeclared `gl_in` in a geometry
 * shader), and only a stage that survives both steps is reported. */
char *vio_hlsl_probe_hlsl(int stage, int shader_model);

/* Reflection info for a single resource */
typedef struct _vio_reflect_resource {
    const char *name;
    unsigned int id;
    unsigned int set;
    unsigned int binding;
    unsigned int location;
    unsigned int vecsize;   /* component count: 1=float, 2=vec2, 3=vec3, 4=vec4/mat4 */
    unsigned int columns;   /* matrix columns: 1 for scalars/vectors, 4 for mat4 (a
                               mat4 vertex input occupies 4 consecutive locations) */
    unsigned int is_depth;  /* 1 if sampled image has Depth=1 (sampler2DShadow) */
} vio_reflect_resource;

/* Reflection result */
typedef struct _vio_reflect_result {
    vio_reflect_resource *inputs;
    int                   input_count;
    vio_reflect_resource *uniforms;
    int                   uniform_count;
    vio_reflect_resource *textures;
    int                   texture_count;
    vio_reflect_resource *ubos;
    int                   ubo_count;
    vio_reflect_resource *storage_buffers;  /* SSBO / (RW)StructuredBuffer — compute */
    int                   storage_buffer_count;
    vio_reflect_resource *storage_images;   /* image2D/image3D — compute (deferred use) */
    int                   storage_image_count;
} vio_reflect_result;

/* Reflect SPIR-V module. Returns 0 on success.
 * result must be freed with vio_reflect_free(). */
int  vio_spirv_reflect(const uint32_t *spirv, size_t spirv_size,
                        vio_reflect_result *result, char **error_msg);

void vio_reflect_free(vio_reflect_result *result);

/* Forward declaration for vio_uniform_entry (defined in vio_shader.h) */
struct _vio_uniform_entry;
typedef struct _vio_uniform_entry vio_uniform_entry;

/* Extract uniform member offsets from a SPIR-V module's UBO/push constant block.
 * Fills entries[] with name/offset/size. Returns number of entries found.
 * total_size is set to the aligned total size of the constant buffer. */
int vio_spirv_get_uniform_offsets(const uint32_t *spirv, size_t spirv_size,
                                   vio_uniform_entry *entries, int max_entries,
                                   int *total_size);

#endif /* VIO_SHADER_REFLECT_H */

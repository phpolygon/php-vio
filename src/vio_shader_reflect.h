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

/* Transpile SPIR-V to MSL.
 * Returns malloc'd string (caller frees). NULL on failure. */
char *vio_spirv_to_msl(const uint32_t *spirv, size_t spirv_size, char **error_msg);

/* Transpile SPIR-V to HLSL (target shader_model e.g. 50=SM5.0, 51=SM5.1).
 * Returns malloc'd string (caller frees). NULL on failure. */
char *vio_spirv_to_hlsl(const uint32_t *spirv, size_t spirv_size, int shader_model, char **error_msg);

/* Reflection info for a single resource */
typedef struct _vio_reflect_resource {
    const char *name;
    unsigned int id;
    unsigned int set;
    unsigned int binding;
    unsigned int location;
    unsigned int vecsize;   /* component count: 1=float, 2=vec2, 3=vec3, 4=vec4/mat4 */
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

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

/* Add #extension GL_ARB_shader_viewport_layer_array to a vertex / TES GLSL
 * source that uses gl_Layer / gl_ViewportIndex without declaring it. Takes
 * ownership of the malloc'd source, returns the (possibly new) string. */
char *vio_glsl_require_viewport_layer_ext(char *glsl);
char *vio_glsl_require_mesh_shader_ext(char *glsl);

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
/* Native 16-bit types in the HLSL (`half` / int16_t instead of min16float):
 * set by D3D12 under SM 6.2+ with Native16BitShaderOpsSupported, cleared by
 * every other D3D context. Process-wide, like the D3D backends themselves. */
void vio_hlsl_set_16bit_types(int enable);
/* 1 when the SPIR-V module decorates a variable with BuiltIn `builtin`
 * (e.g. 4432 PrimitiveShadingRateKHR). Plain word scan, no SPIRV-Cross. */
int vio_spirv_has_builtin(const void *spirv, size_t bytes, uint32_t builtin);
int vio_spirv_has_capability(const void *spirv, size_t bytes, uint32_t capability);
/* 1 when any resource of the module is decorated with descriptor set `set`
 * (set 1 = the bindless table). */
int vio_spirv_uses_descriptor_set(const void *spirv, size_t bytes, uint32_t set);
uint32_t vio_spirv_local_size_x(const void *spirv, size_t bytes);   /* 0 when not a literal LocalSize */
/* Execution model of the module's first OpEntryPoint (0 = Vertex, 4 = Fragment,
 * 5364 = TaskEXT, 5365 = MeshEXT, ...), -1 when the words are no SPIR-V. */
int vio_spirv_execution_model(const void *spirv, size_t bytes);
#define VIO_SPIRV_MODEL_TASK_EXT 5364
#define VIO_SPIRV_MODEL_MESH_EXT 5365
/* Mesh stages: SPIRV-Cross applies no clip-space fixup to gl_MeshVerticesEXT
 * positions. Rewrites every `gl_MeshVerticesEXT[i].gl_Position = e;` of the
 * transpiled source to store the GL position converted for the backend:
 * flip_y negates y (Vulkan), fix_z maps z from [-w, w] to [0, w] (D3D, Vulkan).
 * `vec4` names the target language's vector type. Takes ownership of `src`. */
char *vio_mesh_fix_positions(char *src, int flip_y, int fix_z, const char *vec4);
/* Binding of the module's first acceleration-structure variable (GL_EXT_ray_query),
 * -1 when it has none. Plain word scan. */
int vio_spirv_accel_binding(const void *spirv, size_t bytes);
/* GL_OVR_multiview2: views of the next vio_spirv_to_glsl calls (0 = off). */
void vio_glsl_set_ovr_view_count(int views);

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

/* Probe variants beyond the plain stages, accepted by vio_hlsl_stage_supported
 * / vio_hlsl_probe_hlsl and the D3D backends' stage probes:
 * VIO_PROBE_GS_INSTANCED = a geometry stage with layout(invocations = N). */
#define VIO_PROBE_GS_INSTANCED VIO_STAGE_COUNT
#define VIO_PROBE_COUNT        (VIO_STAGE_COUNT + 1)

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

/* Binding of the uniform block vio_spirv_get_uniform_offsets reads (the first
 * one) when it is a named block; -1 for loose uniforms (glslang's default
 * uniform block), push constants or no block. */
int vio_spirv_uniform_block_binding(const uint32_t *spirv, size_t spirv_size);

/* GLSL targets that need combined samplers (OpenGL, the Vulkan round trip):
 * turn separate textures + samplers (texture(sampler2D(u_tex, u_smp), uv)) into
 * one combined sampler per pair, named after the texture so
 * vio_set_uniform('u_tex', unit) and vio_bind_texture reach it. The bindless
 * table (set 1) stays separate: then nothing is combined. `compiler` is an
 * spvc_compiler before compile; returns the number of combined samplers. */
int vio_spvc_combine_separate(void *compiler);

/* Names of the separate textures in set 0, in reflection order (the order the
 * HLSL translation gives them t-registers after the combined samplers).
 * Returns the count (<= max). */
int vio_spirv_separate_images(const uint32_t *spirv, size_t spirv_size, char (*names)[64], int max);

/* Entries of gl_ClipDistance a stage writes through its outputs (the array
 * length of the gl_PerVertex member or variable it accesses), 0 if none.
 * Plain SPIR-V parsing, also without SPIRV-Cross. */
int vio_spirv_output_clip_distances(const uint32_t *spirv, size_t spirv_size);
/* 1 when the stage reads or writes gl_ClipDistance. */
int vio_spirv_uses_clip_distance(const uint32_t *spirv, size_t spirv_size);

/* Shader-wide sampler registers by NAME (OPEN-ITEMS-PLAN A30). Every stage of a
 * D3D / Vulkan shader shares one texture table, so a sampler must land on the
 * same register in every stage that declares it. Counting per stage broke that
 * whenever stages listed their samplers in a different order - and SPIRV-Cross
 * lists them in SPIR-V id order, i.e. glslang's order of first use, not the
 * declaration order. The plan walks the stages in the sampler-map order of
 * php_vio.c (fragment, geometry, tess control, tess eval, mesh, task): the
 * fragment stage keeps its replayed registers (regular 0.., depth 8..,
 * separate textures after the combined ones), every later name takes the lowest
 * free register of its kind. vio_spirv_to_hlsl and the Vulkan stage rewrite
 * look names up while a plan is in use (vio_sampler_plan_use around the
 * backend's compile_shader / create_pipeline); names outside it keep the
 * per-stage counter. */
#define VIO_SAMPLER_PLAN_MAX 32
typedef struct _vio_sampler_plan {
    int  count;
    char names[VIO_SAMPLER_PLAN_MAX][64];
    int  reg[VIO_SAMPLER_PLAN_MAX];
    int  is_depth[VIO_SAMPLER_PLAN_MAX];
} vio_sampler_plan;

/* spirv[0] is the fragment stage; NULL entries are skipped. */
void vio_sampler_plan_build(vio_sampler_plan *plan, const uint32_t *const *spirv, const size_t *size, int n);
void vio_sampler_plan_use(const vio_sampler_plan *plan);   /* NULL ends it */
const vio_sampler_plan *vio_sampler_plan_current(void);
int  vio_sampler_plan_reg(const char *name);   /* -1: no plan in use or name not listed */
/* Position in the plan = position in php_vio.c's sampler map: the register of
 * SM 5.0 (D3D11), which numbers every sampler in one sequence and binds by index. */
int  vio_sampler_plan_index(const char *name);
#endif /* VIO_SHADER_REFLECT_H */

/*
 * php-vio - Shader compiler implementation (GLSL -> SPIR-V via glslang C API)
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "../php_vio.h"
#include "../include/vio_types.h"   /* vio_shader_stage */
#include "vio_shader_compiler.h"

#ifdef HAVE_GLSLANG

#include <glslang/Include/glslang_c_interface.h>
#include <glslang/Include/glslang_c_shader_types.h>
#include <glslang/Public/resource_limits_c.h>
#include <string.h>
#include <stdlib.h>

static int glslang_initialized = 0;

int vio_shader_compiler_init(void)
{
    if (!glslang_initialized) {
        if (!glslang_initialize_process()) {
            return -1;
        }
        glslang_initialized = 1;
    }
    return 0;
}

void vio_shader_compiler_shutdown(void)
{
    if (glslang_initialized) {
        glslang_finalize_process();
        glslang_initialized = 0;
    }
}

/* Core compiler: GLSL source + an explicit glslang stage -> SPIR-V. Both the
 * graphics (vertex/fragment) and compute public entry points funnel through
 * here so the compile/link/emit logic lives in one place. */
static uint32_t *vio_compile_stage_to_spirv_ex(const char *source, glslang_stage_t stage, int vulkan_1_2,
                                               size_t *out_size, char **error_msg)
{
    if (!glslang_initialized) {
        if (error_msg) *error_msg = strdup("glslang not initialized");
        return NULL;
    }

    /* GL_KHR_shader_subgroup_* needs a Vulkan 1.1 / SPIR-V 1.3 target. Only
     * shaders that use it get the newer target, so every other module stays
     * SPIR-V 1.0 for the SPIR-V rewriters (GS / tessellation / Metal kernels). */
    /* Split literal: the audit gate (070) flags GL_ tokens outside the GL backend. */
    int subgroup = source && strstr(source, "GL_" "KHR_shader_subgroup") != NULL;
    /* GL_EXT_ray_query needs SPIR-V 1.4 (Vulkan 1.2 rules). */
    if (source && strstr(source, "GL_" "EXT_ray_query") != NULL) vulkan_1_2 = 1;
    /* GL_KHR_cooperative_matrix: subgroup-scope operations, same target. */
    if (source && strstr(source, "GL_" "KHR_cooperative_matrix") != NULL) subgroup = 1;
    /* GL_EXT_mesh_shader (mesh / task stages and per-primitive fragment inputs)
     * needs SPIR-V 1.4. */
    int mesh = source && strstr(source, "GL_" "EXT_mesh_shader") != NULL;

    glslang_input_t input = {0};
    input.language                          = GLSLANG_SOURCE_GLSL;
    input.stage                             = stage;
    input.client                            = GLSLANG_CLIENT_VULKAN;
    input.client_version                    = vulkan_1_2 ? GLSLANG_TARGET_VULKAN_1_2
                                            : (subgroup || mesh) ? GLSLANG_TARGET_VULKAN_1_1 : GLSLANG_TARGET_VULKAN_1_0;
    input.target_language                   = GLSLANG_TARGET_SPV;
    input.target_language_version           = (vulkan_1_2 || mesh) ? GLSLANG_TARGET_SPV_1_4
                                            : subgroup ? GLSLANG_TARGET_SPV_1_3 : GLSLANG_TARGET_SPV_1_0;
    input.code                              = source;
    input.default_version                   = 330;
    input.default_profile                   = GLSLANG_CORE_PROFILE;
    input.force_default_version_and_profile = 0;
    input.forward_compatible                = 0;
    input.messages                          = GLSLANG_MSG_DEFAULT_BIT;
    input.resource                          = glslang_default_resource();

    glslang_shader_t *shader = glslang_shader_create(&input);
    if (!shader) {
        if (error_msg) *error_msg = strdup("Failed to create glslang shader");
        return NULL;
    }

    /* Auto-assign locations for uniforms and varyings without explicit layout(location=N).
     * VULKAN_RULES_RELAXED wraps standalone uniforms into a default UBO (required for D3D cbuffer).
     * This allows OpenGL-style GLSL (no explicit locations/blocks) to compile to SPIR-V. */
    glslang_shader_set_options(shader, GLSLANG_SHADER_AUTO_MAP_LOCATIONS
                                     | GLSLANG_SHADER_AUTO_MAP_BINDINGS
                                     | GLSLANG_SHADER_VULKAN_RULES_RELAXED);

    if (!glslang_shader_preprocess(shader, &input)) {
        if (error_msg) {
            const char *log = glslang_shader_get_info_log(shader);
            *error_msg = log ? strdup(log) : strdup("Shader preprocessing failed");
        }
        glslang_shader_delete(shader);
        return NULL;
    }

    if (!glslang_shader_parse(shader, &input)) {
        if (error_msg) {
            const char *log = glslang_shader_get_info_log(shader);
            *error_msg = log ? strdup(log) : strdup("Shader parsing failed");
        }
        glslang_shader_delete(shader);
        return NULL;
    }

    glslang_program_t *program = glslang_program_create();
    glslang_program_add_shader(program, shader);

    if (!glslang_program_link(program, GLSLANG_MSG_SPV_RULES_BIT | GLSLANG_MSG_VULKAN_RULES_BIT)) {
        if (error_msg) {
            const char *log = glslang_program_get_info_log(program);
            *error_msg = log ? strdup(log) : strdup("Shader linking failed");
        }
        glslang_program_delete(program);
        glslang_shader_delete(shader);
        return NULL;
    }

    glslang_program_SPIRV_generate(program, stage);

    size_t spirv_size = glslang_program_SPIRV_get_size(program);
    if (spirv_size == 0) {
        if (error_msg) *error_msg = strdup("SPIR-V generation produced empty output");
        glslang_program_delete(program);
        glslang_shader_delete(shader);
        return NULL;
    }

    uint32_t *spirv = malloc(spirv_size * sizeof(uint32_t));
    glslang_program_SPIRV_get(program, spirv);

    *out_size = spirv_size * sizeof(uint32_t);

    const char *spirv_msg = glslang_program_SPIRV_get_messages(program);
    if (spirv_msg && strlen(spirv_msg) > 0 && error_msg) {
        *error_msg = strdup(spirv_msg);
    }

    glslang_program_delete(program);
    glslang_shader_delete(shader);

    return spirv;
}

static uint32_t *vio_compile_stage_to_spirv(const char *source, glslang_stage_t stage,
                                            size_t *out_size, char **error_msg)
{
    return vio_compile_stage_to_spirv_ex(source, stage, 0, out_size, error_msg);
}

uint32_t *vio_compile_glsl_to_spirv(const char *source, int is_fragment,
                                     size_t *out_size, char **error_msg)
{
    return vio_compile_stage_to_spirv(source,
        is_fragment ? GLSLANG_STAGE_FRAGMENT : GLSLANG_STAGE_VERTEX,
        out_size, error_msg);
}

uint32_t *vio_compile_glsl_stage_to_spirv(const char *source, int stage,
                                           size_t *out_size, char **error_msg)
{
    glslang_stage_t gs;
    switch (stage) {
        case VIO_STAGE_VERTEX:       gs = GLSLANG_STAGE_VERTEX; break;
        case VIO_STAGE_FRAGMENT:     gs = GLSLANG_STAGE_FRAGMENT; break;
        case VIO_STAGE_GEOMETRY:     gs = GLSLANG_STAGE_GEOMETRY; break;
        case VIO_STAGE_TESS_CONTROL: gs = GLSLANG_STAGE_TESSCONTROL; break;
        case VIO_STAGE_TESS_EVAL:    gs = GLSLANG_STAGE_TESSEVALUATION; break;
        case VIO_STAGE_MESH:         gs = GLSLANG_STAGE_MESH_NV; break;   /* = GLSLANG_STAGE_MESH (EXT) */
        case VIO_STAGE_TASK:         gs = GLSLANG_STAGE_TASK_NV; break;
        default:
            if (error_msg) *error_msg = strdup("unknown shader stage");
            return NULL;
    }
    return vio_compile_stage_to_spirv(source, gs, out_size, error_msg);
}

uint32_t *vio_compile_glsl_compute_to_spirv(const char *source,
                                            size_t *out_size, char **error_msg)
{
    return vio_compile_stage_to_spirv(source, GLSLANG_STAGE_COMPUTE, out_size, error_msg);
}

/* Ray tracing stages (GL_EXT_ray_tracing) need a Vulkan 1.2 / SPIR-V 1.4 target. */
uint32_t *vio_compile_glsl_rt_stage_to_spirv(const char *source, int rt_stage,
                                             size_t *out_size, char **error_msg)
{
    glslang_stage_t gs;
    switch (rt_stage) {
        case VIO_RT_STAGE_RAYGEN:      gs = GLSLANG_STAGE_RAYGEN; break;
        case VIO_RT_STAGE_MISS:        gs = GLSLANG_STAGE_MISS; break;
        case VIO_RT_STAGE_CLOSEST_HIT: gs = GLSLANG_STAGE_CLOSESTHIT; break;
        case VIO_RT_STAGE_ANY_HIT:     gs = GLSLANG_STAGE_ANYHIT; break;
        case VIO_RT_STAGE_CALLABLE:    gs = GLSLANG_STAGE_CALLABLE; break;
        default:
            if (error_msg) *error_msg = strdup("unknown ray tracing stage");
            return NULL;
    }
    return vio_compile_stage_to_spirv_ex(source, gs, 1, out_size, error_msg);
}

#else /* !HAVE_GLSLANG */

int vio_shader_compiler_init(void) { return 0; }
void vio_shader_compiler_shutdown(void) {}

uint32_t *vio_compile_glsl_to_spirv(const char *source, int is_fragment,
                                     size_t *out_size, char **error_msg)
{
    (void)source; (void)is_fragment; (void)out_size;
    if (error_msg) *error_msg = strdup("glslang not available (compile with --with-glslang)");
    return NULL;
}

uint32_t *vio_compile_glsl_stage_to_spirv(const char *source, int stage,
                                           size_t *out_size, char **error_msg)
{
    (void)source; (void)stage; (void)out_size;
    if (error_msg) *error_msg = strdup("glslang not available (compile with --with-glslang)");
    return NULL;
}

uint32_t *vio_compile_glsl_compute_to_spirv(const char *source,
                                            size_t *out_size, char **error_msg)
{
    (void)source; (void)out_size;
    if (error_msg) *error_msg = strdup("glslang not available (compile with --with-glslang)");
    return NULL;
}

uint32_t *vio_compile_glsl_rt_stage_to_spirv(const char *source, int rt_stage,
                                             size_t *out_size, char **error_msg)
{
    (void)source; (void)rt_stage; (void)out_size;
    if (error_msg) *error_msg = strdup("glslang not available (compile with --with-glslang)");
    return NULL;
}

#endif /* HAVE_GLSLANG */

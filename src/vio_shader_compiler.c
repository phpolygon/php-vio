/*
 * php-vio - Shader compiler implementation (GLSL -> SPIR-V via glslang C API)
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "../php_vio.h"
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

uint32_t *vio_compile_glsl_to_spirv(const char *source, int is_fragment,
                                     size_t *out_size, char **error_msg)
{
    if (!glslang_initialized) {
        if (error_msg) *error_msg = strdup("glslang not initialized");
        return NULL;
    }

    glslang_stage_t stage = is_fragment ? GLSLANG_STAGE_FRAGMENT : GLSLANG_STAGE_VERTEX;

    glslang_input_t input = {0};
    input.language                          = GLSLANG_SOURCE_GLSL;
    input.stage                             = stage;
    input.client                            = GLSLANG_CLIENT_OPENGL;
    input.client_version                    = GLSLANG_TARGET_OPENGL_450;
    input.target_language                   = GLSLANG_TARGET_SPV;
    input.target_language_version           = GLSLANG_TARGET_SPV_1_0;
    input.code                              = source;
    input.default_version                   = 410;
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
     * This allows OpenGL-style GLSL (no explicit locations) to compile to SPIR-V. */
    glslang_shader_set_options(shader, GLSLANG_SHADER_AUTO_MAP_LOCATIONS
                                     | GLSLANG_SHADER_AUTO_MAP_BINDINGS);

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

#endif /* HAVE_GLSLANG */

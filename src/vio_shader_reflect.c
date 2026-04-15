/*
 * php-vio - Shader reflection and cross-compilation via SPIRV-Cross C API
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "../php_vio.h"
#include "vio_shader_reflect.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

#ifdef HAVE_SPIRV_CROSS

#include <spirv_cross/spirv_cross_c.h>

/* ── Transpilation ───────────────────────────────────────────────── */

char *vio_spirv_to_glsl(const uint32_t *spirv, size_t spirv_size, int version, char **error_msg)
{
    spvc_context ctx = NULL;
    spvc_parsed_ir ir = NULL;
    spvc_compiler compiler = NULL;
    spvc_compiler_options options = NULL;
    const char *result = NULL;
    char *output = NULL;

    if (spvc_context_create(&ctx) != SPVC_SUCCESS) {
        if (error_msg) *error_msg = strdup("Failed to create SPIRV-Cross context");
        return NULL;
    }

    size_t word_count = spirv_size / sizeof(uint32_t);

    if (spvc_context_parse_spirv(ctx, spirv, word_count, &ir) != SPVC_SUCCESS) {
        if (error_msg) *error_msg = strdup(spvc_context_get_last_error_string(ctx));
        spvc_context_destroy(ctx);
        return NULL;
    }

    if (spvc_context_create_compiler(ctx, SPVC_BACKEND_GLSL, ir, SPVC_CAPTURE_MODE_TAKE_OWNERSHIP, &compiler) != SPVC_SUCCESS) {
        if (error_msg) *error_msg = strdup(spvc_context_get_last_error_string(ctx));
        spvc_context_destroy(ctx);
        return NULL;
    }

    spvc_compiler_create_compiler_options(compiler, &options);
    spvc_compiler_options_set_uint(options, SPVC_COMPILER_OPTION_GLSL_VERSION, version);
    spvc_compiler_options_set_bool(options, SPVC_COMPILER_OPTION_GLSL_ES, SPVC_FALSE);
    /* Flatten UBOs to plain uniforms for GLSL <=420 (macOS only supports 410) */
    spvc_compiler_options_set_bool(options, SPVC_COMPILER_OPTION_GLSL_EMIT_UNIFORM_BUFFER_AS_PLAIN_UNIFORMS, SPVC_TRUE);
    spvc_compiler_install_compiler_options(compiler, options);

    if (spvc_compiler_compile(compiler, &result) != SPVC_SUCCESS) {
        if (error_msg) *error_msg = strdup(spvc_context_get_last_error_string(ctx));
        spvc_context_destroy(ctx);
        return NULL;
    }

    fprintf(stderr, "[vio] SPIRV-Cross output (first 500 chars):\n%.500s\n---\n", result);
    output = strdup(result);
    spvc_context_destroy(ctx);
    return output;
}

char *vio_spirv_to_msl(const uint32_t *spirv, size_t spirv_size, char **error_msg)
{
    spvc_context ctx = NULL;
    spvc_parsed_ir ir = NULL;
    spvc_compiler compiler = NULL;
    const char *result = NULL;
    char *output = NULL;

    if (spvc_context_create(&ctx) != SPVC_SUCCESS) {
        if (error_msg) *error_msg = strdup("Failed to create SPIRV-Cross context");
        return NULL;
    }

    size_t word_count = spirv_size / sizeof(uint32_t);

    if (spvc_context_parse_spirv(ctx, spirv, word_count, &ir) != SPVC_SUCCESS) {
        if (error_msg) *error_msg = strdup(spvc_context_get_last_error_string(ctx));
        spvc_context_destroy(ctx);
        return NULL;
    }

    if (spvc_context_create_compiler(ctx, SPVC_BACKEND_MSL, ir, SPVC_CAPTURE_MODE_TAKE_OWNERSHIP, &compiler) != SPVC_SUCCESS) {
        if (error_msg) *error_msg = strdup(spvc_context_get_last_error_string(ctx));
        spvc_context_destroy(ctx);
        return NULL;
    }

    if (spvc_compiler_compile(compiler, &result) != SPVC_SUCCESS) {
        if (error_msg) *error_msg = strdup(spvc_context_get_last_error_string(ctx));
        spvc_context_destroy(ctx);
        return NULL;
    }

    output = strdup(result);
    spvc_context_destroy(ctx);
    return output;
}

char *vio_spirv_to_hlsl(const uint32_t *spirv, size_t spirv_size, int shader_model, char **error_msg)
{
    spvc_context ctx = NULL;
    spvc_parsed_ir ir = NULL;
    spvc_compiler compiler = NULL;
    spvc_compiler_options options = NULL;
    const char *result = NULL;
    char *output = NULL;

    if (spvc_context_create(&ctx) != SPVC_SUCCESS) {
        if (error_msg) *error_msg = strdup("Failed to create SPIRV-Cross context");
        return NULL;
    }

    size_t word_count = spirv_size / sizeof(uint32_t);

    if (spvc_context_parse_spirv(ctx, spirv, word_count, &ir) != SPVC_SUCCESS) {
        if (error_msg) *error_msg = strdup(spvc_context_get_last_error_string(ctx));
        spvc_context_destroy(ctx);
        return NULL;
    }

    if (spvc_context_create_compiler(ctx, SPVC_BACKEND_HLSL, ir, SPVC_CAPTURE_MODE_TAKE_OWNERSHIP, &compiler) != SPVC_SUCCESS) {
        if (error_msg) *error_msg = strdup(spvc_context_get_last_error_string(ctx));
        spvc_context_destroy(ctx);
        return NULL;
    }

    spvc_compiler_create_compiler_options(compiler, &options);
    spvc_compiler_options_set_uint(options, SPVC_COMPILER_OPTION_HLSL_SHADER_MODEL, shader_model);
    spvc_compiler_options_set_bool(options, SPVC_COMPILER_OPTION_HLSL_POINT_SIZE_COMPAT, SPVC_TRUE);
    spvc_compiler_options_set_bool(options, SPVC_COMPILER_OPTION_HLSL_POINT_COORD_COMPAT, SPVC_TRUE);
    spvc_compiler_install_compiler_options(compiler, options);

    /* Remap combined image-samplers to avoid overlapping register semantics.
     * GLSL uses combined sampler2D; HLSL needs separate texture (t) and sampler (s)
     * registers. Shift sampler bindings to avoid collision with texture bindings. */
    spvc_resources resources = NULL;
    spvc_compiler_create_shader_resources(compiler, &resources);
    if (resources) {
        const spvc_reflected_resource *sampled_images;
        size_t sampled_count;
        spvc_resources_get_resource_list_for_type(resources, SPVC_RESOURCE_TYPE_SAMPLED_IMAGE,
                                                   &sampled_images, &sampled_count);
        /* Assign unique texture (t) and sampler (s) registers per combined image-sampler */
        for (size_t i = 0; i < sampled_count; i++) {
            spvc_compiler_set_decoration(compiler, sampled_images[i].id,
                                          SpvDecorationBinding, (unsigned int)i);
        }

        /* Also handle separate images and samplers */
        const spvc_reflected_resource *sep_images;
        size_t sep_image_count;
        spvc_resources_get_resource_list_for_type(resources, SPVC_RESOURCE_TYPE_SEPARATE_IMAGE,
                                                   &sep_images, &sep_image_count);
        for (size_t i = 0; i < sep_image_count; i++) {
            spvc_compiler_set_decoration(compiler, sep_images[i].id,
                                          SpvDecorationBinding, (unsigned int)(sampled_count + i));
        }

        const spvc_reflected_resource *sep_samplers;
        size_t sep_sampler_count;
        spvc_resources_get_resource_list_for_type(resources, SPVC_RESOURCE_TYPE_SEPARATE_SAMPLERS,
                                                   &sep_samplers, &sep_sampler_count);
        for (size_t i = 0; i < sep_sampler_count; i++) {
            spvc_compiler_set_decoration(compiler, sep_samplers[i].id,
                                          SpvDecorationBinding, (unsigned int)(sampled_count + sep_image_count + i));
        }
    }

    if (spvc_compiler_compile(compiler, &result) != SPVC_SUCCESS) {
        if (error_msg) *error_msg = strdup(spvc_context_get_last_error_string(ctx));
        spvc_context_destroy(ctx);
        return NULL;
    }

    output = strdup(result);
    spvc_context_destroy(ctx);
    return output;
}

/* ── Reflection ──────────────────────────────────────────────────── */

static void copy_resources(spvc_compiler compiler, spvc_resources resources,
                           spvc_resource_type type,
                           vio_reflect_resource **out, int *out_count)
{
    const spvc_reflected_resource *list;
    size_t count;
    spvc_resources_get_resource_list_for_type(resources, type, &list, &count);

    if (count == 0) {
        *out = NULL;
        *out_count = 0;
        return;
    }

    *out = calloc(count, sizeof(vio_reflect_resource));
    *out_count = (int)count;

    for (size_t i = 0; i < count; i++) {
        (*out)[i].name     = strdup(spvc_compiler_get_name(compiler, list[i].id));
        (*out)[i].id       = list[i].id;
        (*out)[i].set      = spvc_compiler_get_decoration(compiler, list[i].id, SpvDecorationDescriptorSet);
        (*out)[i].binding  = spvc_compiler_get_decoration(compiler, list[i].id, SpvDecorationBinding);
        (*out)[i].location = spvc_compiler_get_decoration(compiler, list[i].id, SpvDecorationLocation);
    }
}

int vio_spirv_reflect(const uint32_t *spirv, size_t spirv_size,
                       vio_reflect_result *result, char **error_msg)
{
    memset(result, 0, sizeof(vio_reflect_result));

    spvc_context ctx = NULL;
    spvc_parsed_ir ir = NULL;
    spvc_compiler compiler = NULL;
    spvc_resources resources = NULL;

    if (spvc_context_create(&ctx) != SPVC_SUCCESS) {
        if (error_msg) *error_msg = strdup("Failed to create SPIRV-Cross context");
        return -1;
    }

    size_t word_count = spirv_size / sizeof(uint32_t);

    if (spvc_context_parse_spirv(ctx, spirv, word_count, &ir) != SPVC_SUCCESS) {
        if (error_msg) *error_msg = strdup(spvc_context_get_last_error_string(ctx));
        spvc_context_destroy(ctx);
        return -1;
    }

    /* Use GLSL backend for reflection - it supports all resource types */
    if (spvc_context_create_compiler(ctx, SPVC_BACKEND_GLSL, ir, SPVC_CAPTURE_MODE_TAKE_OWNERSHIP, &compiler) != SPVC_SUCCESS) {
        if (error_msg) *error_msg = strdup(spvc_context_get_last_error_string(ctx));
        spvc_context_destroy(ctx);
        return -1;
    }

    if (spvc_compiler_create_shader_resources(compiler, &resources) != SPVC_SUCCESS) {
        if (error_msg) *error_msg = strdup(spvc_context_get_last_error_string(ctx));
        spvc_context_destroy(ctx);
        return -1;
    }

    copy_resources(compiler, resources, SPVC_RESOURCE_TYPE_STAGE_INPUT, &result->inputs, &result->input_count);
    copy_resources(compiler, resources, SPVC_RESOURCE_TYPE_UNIFORM_BUFFER, &result->ubos, &result->ubo_count);
    copy_resources(compiler, resources, SPVC_RESOURCE_TYPE_SAMPLED_IMAGE, &result->textures, &result->texture_count);
    copy_resources(compiler, resources, SPVC_RESOURCE_TYPE_PUSH_CONSTANT, &result->uniforms, &result->uniform_count);

    spvc_context_destroy(ctx);
    return 0;
}

void vio_reflect_free(vio_reflect_result *result)
{
    for (int i = 0; i < result->input_count; i++) free((void *)result->inputs[i].name);
    for (int i = 0; i < result->uniform_count; i++) free((void *)result->uniforms[i].name);
    for (int i = 0; i < result->texture_count; i++) free((void *)result->textures[i].name);
    for (int i = 0; i < result->ubo_count; i++) free((void *)result->ubos[i].name);
    free(result->inputs);
    free(result->uniforms);
    free(result->textures);
    free(result->ubos);
    memset(result, 0, sizeof(vio_reflect_result));
}

#else /* !HAVE_SPIRV_CROSS */

char *vio_spirv_to_glsl(const uint32_t *spirv, size_t spirv_size, int version, char **error_msg)
{
    (void)spirv; (void)spirv_size; (void)version;
    if (error_msg) *error_msg = strdup("spirv-cross not available (compile with --with-spirv-cross)");
    return NULL;
}

char *vio_spirv_to_msl(const uint32_t *spirv, size_t spirv_size, char **error_msg)
{
    (void)spirv; (void)spirv_size;
    if (error_msg) *error_msg = strdup("spirv-cross not available (compile with --with-spirv-cross)");
    return NULL;
}

char *vio_spirv_to_hlsl(const uint32_t *spirv, size_t spirv_size, int shader_model, char **error_msg)
{
    (void)spirv; (void)spirv_size; (void)shader_model;
    if (error_msg) *error_msg = strdup("spirv-cross not available (compile with --with-spirv-cross)");
    return NULL;
}

int vio_spirv_reflect(const uint32_t *spirv, size_t spirv_size,
                       vio_reflect_result *result, char **error_msg)
{
    (void)spirv; (void)spirv_size;
    memset(result, 0, sizeof(vio_reflect_result));
    if (error_msg) *error_msg = strdup("spirv-cross not available (compile with --with-spirv-cross)");
    return -1;
}

void vio_reflect_free(vio_reflect_result *result) {
    memset(result, 0, sizeof(vio_reflect_result));
}

#endif /* HAVE_SPIRV_CROSS */

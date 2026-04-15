/*
 * php-vio - Shader reflection and cross-compilation via SPIRV-Cross C API
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "../php_vio.h"
#include "vio_shader.h"
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

        /* Extract vector size from type (1=float, 2=vec2, 3=vec3, 4=vec4) */
        spvc_type type_handle = spvc_compiler_get_type_handle(compiler, list[i].type_id);
        (*out)[i].vecsize = type_handle ? spvc_type_get_vector_size(type_handle) : 3;
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

int vio_spirv_get_uniform_offsets(const uint32_t *spirv, size_t spirv_size,
                                   vio_uniform_entry *entries, int max_entries,
                                   int *total_size)
{
    spvc_context ctx = NULL;
    spvc_parsed_ir ir = NULL;
    spvc_compiler compiler = NULL;
    spvc_resources resources = NULL;
    int count = 0;

    *total_size = 0;

    if (spvc_context_create(&ctx) != SPVC_SUCCESS) return 0;

    size_t word_count = spirv_size / sizeof(uint32_t);
    if (spvc_context_parse_spirv(ctx, spirv, word_count, &ir) != SPVC_SUCCESS) {
        spvc_context_destroy(ctx);
        return 0;
    }

    if (spvc_context_create_compiler(ctx, SPVC_BACKEND_HLSL, ir,
            SPVC_CAPTURE_MODE_TAKE_OWNERSHIP, &compiler) != SPVC_SUCCESS) {
        spvc_context_destroy(ctx);
        return 0;
    }

    if (spvc_compiler_create_shader_resources(compiler, &resources) != SPVC_SUCCESS) {
        spvc_context_destroy(ctx);
        return 0;
    }

    /* Get uniform buffers (cbuffer _Global in HLSL) */
    const spvc_reflected_resource *ubos;
    size_t ubo_count;
    spvc_resources_get_resource_list_for_type(resources, SPVC_RESOURCE_TYPE_UNIFORM_BUFFER,
                                              &ubos, &ubo_count);

    /* Also check push constants (used by OpenGL-style uniforms) */
    const spvc_reflected_resource *push_constants;
    size_t push_count;
    spvc_resources_get_resource_list_for_type(resources, SPVC_RESOURCE_TYPE_PUSH_CONSTANT,
                                              &push_constants, &push_count);

    /* Process the first UBO or push constant block */
    spvc_type_id type_id = 0;
    if (ubo_count > 0) {
        type_id = ubos[0].base_type_id;
    } else if (push_count > 0) {
        type_id = push_constants[0].base_type_id;
    }

    if (type_id) {
        spvc_type type = spvc_compiler_get_type_handle(compiler, type_id);
        unsigned int member_count = spvc_type_get_num_member_types(type);

        for (unsigned int i = 0; i < member_count && count < max_entries; i++) {
            const char *name = spvc_compiler_get_member_name(compiler, type_id, i);
            unsigned int base_offset = 0;
            size_t member_size = 0;
            spvc_compiler_type_struct_member_offset(compiler, type, i, &base_offset);
            spvc_compiler_get_declared_struct_member_size(compiler, type, i, &member_size);

            if (!name || !name[0]) continue;

            /* Check if this member is a struct array (e.g. DirLight u_dir_lights[16]) */
            spvc_type_id member_type_id = spvc_type_get_member_type(type, i);
            spvc_type member_type = spvc_compiler_get_type_handle(compiler, member_type_id);
            unsigned int num_array_dims = spvc_type_get_num_array_dimensions(member_type);

            /* For array-of-struct, get the element (struct) type via parent_type */
            spvc_type_id elem_type_id = 0;
            spvc_type elem_type = NULL;
            unsigned int num_struct_members = 0;

            if (num_array_dims > 0) {
                elem_type_id = spvc_type_get_base_type_id(member_type);
                if (elem_type_id) {
                    elem_type = spvc_compiler_get_type_handle(compiler, elem_type_id);
                    num_struct_members = spvc_type_get_num_member_types(elem_type);
                }
            }

            if (num_array_dims > 0 && num_struct_members > 0 && elem_type) {
                /* Struct array: flatten to name[idx].field entries */
                unsigned int array_size = spvc_type_get_array_dimension(member_type, 0);
                unsigned int array_stride = 0;
                spvc_compiler_type_struct_member_array_stride(compiler, type, i, &array_stride);

                if (array_stride == 0 && array_size > 1) {
                    array_stride = (unsigned int)(member_size / array_size);
                }

                for (unsigned int ai = 0; ai < array_size && count < max_entries; ai++) {
                    for (unsigned int si = 0; si < num_struct_members && count < max_entries; si++) {
                        const char *field = spvc_compiler_get_member_name(compiler, elem_type_id, si);
                        unsigned int field_offset = 0;
                        size_t field_size = 0;
                        spvc_compiler_type_struct_member_offset(compiler, elem_type, si, &field_offset);
                        spvc_compiler_get_declared_struct_member_size(compiler, elem_type, si, &field_size);

                        if (field && field[0]) {
                            snprintf(entries[count].name, sizeof(entries[count].name),
                                     "%s[%u].%s", name, ai, field);
                            entries[count].offset = (int)(base_offset + ai * array_stride + field_offset);
                            entries[count].size = (int)field_size;
                            int end = entries[count].offset + (int)field_size;
                            if (end > *total_size) *total_size = end;
                            count++;
                        }
                    }
                }
            } else {
                /* Simple scalar/vector/matrix member */
                strncpy(entries[count].name, name, sizeof(entries[count].name) - 1);
                entries[count].name[sizeof(entries[count].name) - 1] = '\0';
                entries[count].offset = (int)base_offset;
                entries[count].size = (int)member_size;
                int end = (int)(base_offset + member_size);
                if (end > *total_size) *total_size = end;
                count++;
            }
        }
    }

    /* Align total size to 16 bytes (D3D11 CB requirement) */
    *total_size = (*total_size + 15) & ~15;

    spvc_context_destroy(ctx);
    return count;
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

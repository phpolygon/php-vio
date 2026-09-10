/*
 * php-vio - Vulkan 3D pipeline: shaders, descriptor layouts, pipeline variants
 * (GAP-PHASE5 Block 10). Conventions: see vio_vulkan_3d.h.
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "php.h"

#ifdef HAVE_VULKAN

#include <vulkan/vulkan.h>
#include "vio_vulkan.h"
#include "vio_vulkan_3d.h"
#include "../../vio_shader.h"
#include "../../vio_shader_cache.h"
#include "../../vio_shader_compiler.h"
#include "../../../include/vio_types.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

#if defined(HAVE_SPIRV_CROSS) && defined(HAVE_GLSLANG)
#include <spirv_cross/spirv_cross_c.h>
#define VK3D_HAVE_TRANSPILE 1
#endif

vio_vk3d_shader   *vk3d_live_shaders   = NULL;
vio_vk3d_pipeline *vk3d_live_pipelines = NULL;

int vio_vk3d_available(void)
{
#ifdef VK3D_HAVE_TRANSPILE
    return 1;
#else
    return 0;
#endif
}

/* ── Shader round trip ─────────────────────────────────────────────── */

#ifdef VK3D_HAVE_TRANSPILE

static void vk3d_add_binding(vio_vk3d_shader *sh, uint32_t binding, VkDescriptorType type,
                             VkShaderStageFlags stage, VkImageViewType dim, int is_depth, uint32_t size)
{
    for (int i = 0; i < sh->binding_count; i++) {
        vk3d_binding *b = &sh->bindings[i];
        if (b->binding == binding) {
            b->stages |= stage;
            if (size > b->size) b->size = size;
            return;
        }
    }
    if (sh->binding_count >= VK3D_MAX_BINDINGS) return;
    vk3d_binding *b = &sh->bindings[sh->binding_count++];
    b->binding  = binding;
    b->type     = type;
    b->stages   = stage;
    b->dim      = dim;
    b->is_depth = is_depth;
    b->size     = size;
}

static VkImageViewType vk3d_sampler_dim(spvc_compiler c, spvc_type_id type_id, int *is_depth)
{
    *is_depth = 0;
    spvc_type t = spvc_compiler_get_type_handle(c, type_id);
    if (!t) return VK_IMAGE_VIEW_TYPE_2D;
    spvc_type img = spvc_compiler_get_type_handle(c, spvc_type_get_base_type_id(t));
    if (spvc_type_get_image_is_depth(t) || (img && spvc_type_get_image_is_depth(img))) *is_depth = 1;
    SpvDim dim = spvc_type_get_image_dimension(t);
    int arrayed = spvc_type_get_image_arrayed(t) ? 1 : 0;
    switch (dim) {
        case SpvDim3D:   return VK_IMAGE_VIEW_TYPE_3D;
        case SpvDimCube: return arrayed ? VK_IMAGE_VIEW_TYPE_CUBE_ARRAY : VK_IMAGE_VIEW_TYPE_CUBE;
        default:         return arrayed ? VK_IMAGE_VIEW_TYPE_2D_ARRAY : VK_IMAGE_VIEW_TYPE_2D;
    }
}

/* Vertex outputs by name -> location, so fragment inputs of the same name match. */
typedef struct _vk3d_varyings {
    char names[32][64];
    int  locs[32];
    int  count;
    int  next;   /* first location past every assigned one */
} vk3d_varyings;

/* Stage inputs / outputs without a Location decoration get one: explicit ones are
 * kept; vertex outputs are numbered in declaration order and recorded, fragment
 * inputs take the location of the vertex output with the same name. */
static void vk3d_assign_locations(spvc_compiler c, spvc_resources res, spvc_resource_type type,
                                  vk3d_varyings *vt, int record, int match)
{
    const spvc_reflected_resource *list;
    size_t n;
    spvc_resources_get_resource_list_for_type(res, type, &list, &n);
    int next = 0;
    for (size_t i = 0; i < n; i++) {
        if (spvc_compiler_has_decoration(c, list[i].id, SpvDecorationLocation)) {
            int loc = (int)spvc_compiler_get_decoration(c, list[i].id, SpvDecorationLocation);
            spvc_type t = spvc_compiler_get_type_handle(c, list[i].type_id);
            int span = t ? (int)spvc_type_get_columns(t) : 1;
            if (loc + (span > 0 ? span : 1) > next) next = loc + (span > 0 ? span : 1);
        }
    }
    if (match && vt && vt->next > next) next = vt->next;
    for (size_t i = 0; i < n; i++) {
        const char *name = list[i].name ? list[i].name : "";
        int loc = -1;
        if (spvc_compiler_has_decoration(c, list[i].id, SpvDecorationLocation)) {
            loc = (int)spvc_compiler_get_decoration(c, list[i].id, SpvDecorationLocation);
        } else {
            if (match && vt) {
                for (int k = 0; k < vt->count; k++) {
                    if (strcmp(vt->names[k], name) == 0) { loc = vt->locs[k]; break; }
                }
            }
            if (loc < 0) {
                loc = next;
                spvc_type t = spvc_compiler_get_type_handle(c, list[i].type_id);
                int span = t ? (int)spvc_type_get_columns(t) : 1;
                next += span > 0 ? span : 1;
            }
            spvc_compiler_set_decoration(c, list[i].id, SpvDecorationLocation, (unsigned)loc);
        }
        if (record && vt && vt->count < 32) {
            snprintf(vt->names[vt->count], sizeof(vt->names[0]), "%s", name);
            vt->locs[vt->count] = loc;
            vt->count++;
            if (loc + 1 > vt->next) vt->next = loc + 1;
        }
    }
}

/* Assign set 0 / the Block-10 binding scheme to every resource of one stage and
 * record the bindings on the shader. */
static int vk3d_remap_stage(spvc_compiler c, int is_fragment, vio_vk3d_shader *sh, vk3d_varyings *vt)
{
    spvc_resources res = NULL;
    if (spvc_compiler_create_shader_resources(c, &res) != SPVC_SUCCESS || !res) return -1;
    if (is_fragment) {
        vk3d_assign_locations(c, res, SPVC_RESOURCE_TYPE_STAGE_INPUT, vt, 0, 1);
        vk3d_assign_locations(c, res, SPVC_RESOURCE_TYPE_STAGE_OUTPUT, NULL, 0, 0);
    } else {
        vk3d_assign_locations(c, res, SPVC_RESOURCE_TYPE_STAGE_INPUT, NULL, 0, 0);
        vk3d_assign_locations(c, res, SPVC_RESOURCE_TYPE_STAGE_OUTPUT, vt, 1, 0);
    }
    VkShaderStageFlags stage = is_fragment ? VK_SHADER_STAGE_FRAGMENT_BIT : VK_SHADER_STAGE_VERTEX_BIT;
    const spvc_reflected_resource *list;
    size_t n;

    spvc_resources_get_resource_list_for_type(res, SPVC_RESOURCE_TYPE_UNIFORM_BUFFER, &list, &n);
    for (size_t i = 0; i < n; i++) {
        if (i > VK3D_MAX_EXTRA_UBO) break;
        uint32_t binding = i == 0 ? (is_fragment ? VK3D_B_FS_UBO : VK3D_B_VS_UBO)
                                  : (uint32_t)(VK3D_B_EXTRA_UBO0 + (i - 1));
        size_t size = 0;
        spvc_compiler_get_declared_struct_size(c, spvc_compiler_get_type_handle(c, list[i].base_type_id), &size);
        spvc_compiler_set_decoration(c, list[i].id, SpvDecorationDescriptorSet, 0);
        spvc_compiler_set_decoration(c, list[i].id, SpvDecorationBinding, binding);
        vk3d_add_binding(sh, binding, i == 0 ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC : VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
                         stage, VK_IMAGE_VIEW_TYPE_2D, 0, (uint32_t)size);
    }

    /* Samplers: regular 0.., shadow 8.. (the D3D12 register replay in php_vio.c). */
    spvc_resources_get_resource_list_for_type(res, SPVC_RESOURCE_TYPE_SAMPLED_IMAGE, &list, &n);
    int regular = 0, shadow = 8;
    for (size_t i = 0; i < n; i++) {
        int is_depth = 0;
        VkImageViewType dim = vk3d_sampler_dim(c, list[i].type_id, &is_depth);
        int reg = is_depth ? shadow++ : regular++;
        if (reg >= VK3D_MAX_SAMPLERS) continue;
        uint32_t binding = (uint32_t)(VK3D_B_SAMPLER0 + reg);
        spvc_compiler_set_decoration(c, list[i].id, SpvDecorationDescriptorSet, 0);
        spvc_compiler_set_decoration(c, list[i].id, SpvDecorationBinding, binding);
        vk3d_add_binding(sh, binding, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, stage, dim, is_depth, 0);
        if (is_fragment && i < VK3D_MAX_SAMPLERS) {
            sh->fs_sampler_binding[i] = (int)binding;
            sh->fs_sampler_depth[i] = is_depth;
            if ((int)i + 1 > sh->fs_sampler_count) sh->fs_sampler_count = (int)i + 1;
        }
    }

    spvc_resources_get_resource_list_for_type(res, SPVC_RESOURCE_TYPE_STORAGE_BUFFER, &list, &n);
    for (size_t i = 0; i < n; i++) {
        unsigned orig = spvc_compiler_get_decoration(c, list[i].id, SpvDecorationBinding);
        if (orig >= VK3D_MAX_STORAGE) continue;
        uint32_t binding = (uint32_t)(VK3D_B_STORAGE0 + orig);
        spvc_compiler_set_decoration(c, list[i].id, SpvDecorationDescriptorSet, 0);
        spvc_compiler_set_decoration(c, list[i].id, SpvDecorationBinding, binding);
        vk3d_add_binding(sh, binding, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, stage, VK_IMAGE_VIEW_TYPE_2D, 0, 0);
    }

    spvc_resources_get_resource_list_for_type(res, SPVC_RESOURCE_TYPE_SEPARATE_IMAGE, &list, &n);
    if (n > 0) {
        php_error_docref(NULL, E_NOTICE, "Vulkan: separate texture/sampler objects are not supported by the 3D pipeline; use combined samplers");
    }
    return 0;
}

/* Vertex stage: GL clip space -> Vulkan (NDC +Y = top row like D3D, depth [0,1]).
 * SPIRV-Cross emits the entry point last, so every return inside main and its
 * closing brace get the fixup. */
static char *vk3d_vs_fixup(const char *glsl)
{
    static const char fix[] = "gl_Position.y = -gl_Position.y; gl_Position.z = (gl_Position.z + gl_Position.w) * 0.5; ";
    const char *m = strstr(glsl, "void main()");
    const char *end = strrchr(glsl, '}');
    if (!m || !end || end < m) return strdup(glsl);
    size_t fixlen = sizeof(fix) - 1, returns = 0, len = strlen(glsl);
    for (const char *p = m; (p = strstr(p, "return;")) != NULL && p < end; p += 7) returns++;
    char *out = (char *)malloc(len + (returns + 1) * (fixlen + 4) + 1);
    if (!out) return NULL;
    char *w = out;
    memcpy(w, glsl, (size_t)(m - glsl)); w += m - glsl;
    const char *p = m;
    for (;;) {
        const char *r = strstr(p, "return;");
        if (!r || r >= end) break;
        memcpy(w, p, (size_t)(r - p)); w += r - p;
        *w++ = '{'; *w++ = ' ';
        memcpy(w, fix, fixlen); w += fixlen;
        memcpy(w, "return; }", 9); w += 9;
        p = r + 7;
    }
    memcpy(w, p, (size_t)(end - p)); w += end - p;
    memcpy(w, fix, fixlen); w += fixlen;
    size_t tail = strlen(end);
    memcpy(w, end, tail); w += tail;
    *w = '\0';
    return out;
}

static uint32_t *vk3d_stage(const uint32_t *spirv, size_t spirv_bytes, int is_fragment,
                            vio_vk3d_shader *sh, vk3d_varyings *vt, const uint32_t *vs_spirv, size_t vs_bytes,
                            size_t *out_bytes)
{
    spvc_context ctx = NULL;
    spvc_parsed_ir ir = NULL;
    spvc_compiler c = NULL;
    const char *stage_name = is_fragment ? "fragment" : "vertex";

    if (spvc_context_create(&ctx) != SPVC_SUCCESS) return NULL;
    if (spvc_context_parse_spirv(ctx, spirv, spirv_bytes / sizeof(uint32_t), &ir) != SPVC_SUCCESS ||
        spvc_context_create_compiler(ctx, SPVC_BACKEND_GLSL, ir, SPVC_CAPTURE_MODE_TAKE_OWNERSHIP, &c) != SPVC_SUCCESS ||
        vk3d_remap_stage(c, is_fragment, sh, vt) != 0) {
        php_error_docref(NULL, E_WARNING, "Vulkan: %s SPIR-V reflection failed: %s", stage_name,
                         spvc_context_get_last_error_string(ctx));
        spvc_context_destroy(ctx);
        return NULL;
    }

    uint64_t key = vio_shader_cache_hash(is_fragment ? "vk3d-fs-2" : "vk3d-vs-2", spirv, spirv_bytes);
    /* Fragment input locations follow the vertex outputs' names. */
    if (is_fragment && vs_spirv) key = vio_shader_cache_hash_more(key, vs_spirv, vs_bytes);
    if (vio_shader_cache_dir()) {
        size_t len = 0;
        void *data = vio_shader_cache_load(key, "vkspv", &len);
        if (data && len >= 20 && (len % 4) == 0 && *(const uint32_t *)data == 0x07230203) {
            spvc_context_destroy(ctx);
            *out_bytes = len;
            return (uint32_t *)data;
        }
        free(data);
    }

    spvc_compiler_options opts = NULL;
    spvc_compiler_create_compiler_options(c, &opts);
    spvc_compiler_options_set_uint(opts, SPVC_COMPILER_OPTION_GLSL_VERSION, 450);
    spvc_compiler_options_set_bool(opts, SPVC_COMPILER_OPTION_GLSL_ES, SPVC_FALSE);
    spvc_compiler_options_set_bool(opts, SPVC_COMPILER_OPTION_GLSL_VULKAN_SEMANTICS, SPVC_TRUE);
    spvc_compiler_install_compiler_options(c, opts);

    const char *glsl = NULL;
    if (spvc_compiler_compile(c, &glsl) != SPVC_SUCCESS || !glsl) {
        php_error_docref(NULL, E_WARNING, "Vulkan: %s SPIR-V -> GLSL failed: %s", stage_name,
                         spvc_context_get_last_error_string(ctx));
        spvc_context_destroy(ctx);
        return NULL;
    }
    char *src = is_fragment ? strdup(glsl) : vk3d_vs_fixup(glsl);
    spvc_context_destroy(ctx);
    if (!src) return NULL;
    if (getenv("VIO_DUMP_VK_GLSL")) {
        fprintf(stderr, "==== Vulkan %s GLSL ====\n%s\n==== end ====\n", stage_name, src);
        fflush(stderr);
    }

    char *err = NULL;
    size_t bytes = 0;
    uint32_t *out = vio_compile_glsl_to_spirv(src, is_fragment, &bytes, &err);
    if (!out) {
        php_error_docref(NULL, E_WARNING, "Vulkan: %s GLSL -> SPIR-V failed: %s", stage_name, err ? err : "unknown");
        free(err);
        free(src);
        return NULL;
    }
    free(err);
    free(src);
    if (vio_shader_cache_dir()) vio_shader_cache_store(key, "vkspv", out, bytes);
    *out_bytes = bytes;
    return out;
}

/* Does a fragment stage matter without colour attachments? True when it can discard
 * (OpKill / OpTerminateInvocation / OpDemoteToHelperInvocation) or writes gl_FragDepth. */
static int vk3d_fs_needed_without_color(const uint32_t *code, size_t bytes)
{
    size_t words = bytes / 4, i = 5;
    while (i < words) {
        uint32_t op = code[i] & 0xFFFF, count = code[i] >> 16;
        if (count == 0) break;
        if (op == 252 || op == 4416 || op == 5380) return 1;
        if (op == 71 && count >= 4 && code[i + 2] == 11 && code[i + 3] == 22) return 1;   /* OpDecorate BuiltIn FragDepth */
        i += count;
    }
    return 0;
}

static VkShaderModule vk3d_module(const uint32_t *code, size_t bytes)
{
    VkShaderModuleCreateInfo ci = {0};
    ci.sType    = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    ci.codeSize = bytes;
    ci.pCode    = code;
    VkShaderModule m = VK_NULL_HANDLE;
    if (vkCreateShaderModule(vio_vk.device, &ci, NULL, &m) != VK_SUCCESS) return VK_NULL_HANDLE;
    return m;
}

#endif /* VK3D_HAVE_TRANSPILE */

static void vk3d_shader_unlink(vio_vk3d_shader *sh)
{
    if (sh->prev) sh->prev->next = sh->next;
    else if (vk3d_live_shaders == sh) vk3d_live_shaders = sh->next;
    if (sh->next) sh->next->prev = sh->prev;
    sh->next = sh->prev = NULL;
}

void vk3d_shader_release_gpu(vio_vk3d_shader *sh)
{
    if (!sh) return;
    vk3d_shader_unlink(sh);
    if (vio_vk.device) {
        if (sh->layout)     vio_vk_defer_destroy(VIO_VK_GRAVE_PIPELINE_LAYOUT, (uint64_t)sh->layout, NULL);
        if (sh->set_layout) vio_vk_defer_destroy(VIO_VK_GRAVE_SET_LAYOUT, (uint64_t)sh->set_layout, NULL);
        if (sh->vs)         vio_vk_defer_destroy(VIO_VK_GRAVE_SHADER_MODULE, (uint64_t)sh->vs, NULL);
        if (sh->fs)         vio_vk_defer_destroy(VIO_VK_GRAVE_SHADER_MODULE, (uint64_t)sh->fs, NULL);
    }
    sh->layout = VK_NULL_HANDLE;
    sh->set_layout = VK_NULL_HANDLE;
    sh->vs = sh->fs = VK_NULL_HANDLE;
    sh->dead = 1;
}

void *vio_vk3d_compile_shader(vio_shader_desc *desc)
{
#ifndef VK3D_HAVE_TRANSPILE
    (void)desc;
    return NULL;
#else
    if (!desc || !vio_vk.device || !desc->vertex_data || !desc->fragment_data) return NULL;
    if (desc->vertex_size < 20 || desc->fragment_size < 20 ||
        *(const uint32_t *)desc->vertex_data != 0x07230203 || *(const uint32_t *)desc->fragment_data != 0x07230203) {
        php_error_docref(NULL, E_WARNING, "Vulkan: 3D shaders must reach the backend as SPIR-V");
        return NULL;
    }
    vio_vk3d_shader *sh = (vio_vk3d_shader *)calloc(1, sizeof(vio_vk3d_shader));
    if (!sh) return NULL;

    size_t vb = 0, fb = 0;
    vk3d_varyings vt;
    memset(&vt, 0, sizeof(vt));
    uint32_t *vs = vk3d_stage((const uint32_t *)desc->vertex_data, desc->vertex_size, 0, sh, &vt, NULL, 0, &vb);
    uint32_t *fs = vs ? vk3d_stage((const uint32_t *)desc->fragment_data, desc->fragment_size, 1, sh, &vt,
                                   (const uint32_t *)desc->vertex_data, desc->vertex_size, &fb) : NULL;
    if (!vs || !fs) { free(vs); free(fs); free(sh); return NULL; }
    sh->vs = vk3d_module(vs, vb);
    sh->fs = vk3d_module(fs, fb);
    sh->fs_needed_without_color = vk3d_fs_needed_without_color(fs, fb);
    free(vs);
    free(fs);

    /* Bindings ascending: dynamic offsets are consumed in binding order. */
    for (int i = 1; i < sh->binding_count; i++) {
        vk3d_binding tmp = sh->bindings[i];
        int j = i - 1;
        while (j >= 0 && sh->bindings[j].binding > tmp.binding) { sh->bindings[j + 1] = sh->bindings[j]; j--; }
        sh->bindings[j + 1] = tmp;
    }
    VkDescriptorSetLayoutBinding lb[VK3D_MAX_BINDINGS];
    memset(lb, 0, sizeof(lb));
    for (int i = 0; i < sh->binding_count; i++) {
        lb[i].binding         = sh->bindings[i].binding;
        lb[i].descriptorType  = sh->bindings[i].type;
        lb[i].descriptorCount = 1;
        lb[i].stageFlags      = sh->bindings[i].stages;
    }
    VkDescriptorSetLayoutCreateInfo dsl = {0};
    dsl.sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    dsl.bindingCount = (uint32_t)sh->binding_count;
    dsl.pBindings    = lb;
    int ok = sh->vs && sh->fs &&
             vkCreateDescriptorSetLayout(vio_vk.device, &dsl, NULL, &sh->set_layout) == VK_SUCCESS;
    if (ok) {
        VkPipelineLayoutCreateInfo pl = {0};
        pl.sType          = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        pl.setLayoutCount = 1;
        pl.pSetLayouts    = &sh->set_layout;
        ok = vkCreatePipelineLayout(vio_vk.device, &pl, NULL, &sh->layout) == VK_SUCCESS;
    }
    if (!ok) {
        php_error_docref(NULL, E_WARNING, "Vulkan: shader module / descriptor layout creation failed");
        vk3d_shader_release_gpu(sh);
        free(sh);
        return NULL;
    }
    sh->next = vk3d_live_shaders;
    if (sh->next) sh->next->prev = sh;
    vk3d_live_shaders = sh;
    return sh;
#endif
}

void vio_vk3d_destroy_shader_obj(void *shader_obj)
{
    vio_shader_object *so = (vio_shader_object *)shader_obj;
    if (!so || !so->backend_shader) return;
    vio_vk3d_shader *sh = (vio_vk3d_shader *)so->backend_shader;
    vk3d_shader_release_gpu(sh);
    free(sh);
    so->backend_shader = NULL;
}

/* ── Pipelines ─────────────────────────────────────────────────────── */

static void vk3d_pipeline_unlink(vio_vk3d_pipeline *p)
{
    if (p->prev) p->prev->next = p->next;
    else if (vk3d_live_pipelines == p) vk3d_live_pipelines = p->next;
    if (p->next) p->next->prev = p->prev;
    p->next = p->prev = NULL;
}

void *vio_vk3d_create_pipeline(vio_pipeline_desc *desc)
{
    if (!desc || !desc->shader || !vio_vk.device) return NULL;
    vio_vk3d_pipeline *p = (vio_vk3d_pipeline *)calloc(1, sizeof(vio_vk3d_pipeline));
    if (!p) return NULL;
    p->shader = (vio_vk3d_shader *)desc->shader;
    p->desc = *desc;
    p->attrib_count = desc->vertex_attrib_count > 16 ? 16 : desc->vertex_attrib_count;
    if (desc->vertex_layout && p->attrib_count > 0) {
        memcpy(p->attribs, desc->vertex_layout, sizeof(vio_vertex_attrib) * (size_t)p->attrib_count);
    }
    p->desc.vertex_layout = p->attribs;
    for (int i = 0; i < p->attrib_count; i++) {
        int loc = p->attribs[i].location;
        if (loc >= 3 && loc <= 6) p->has_instance_attrs = 1;
        else p->vertex_stride += vk3d_format_size(p->attribs[i].format);
    }
    p->next = vk3d_live_pipelines;
    if (p->next) p->next->prev = p;
    vk3d_live_pipelines = p;
    return p;
}

void vk3d_pipeline_release_gpu(vio_vk3d_pipeline *p)
{
    if (!p) return;
    vk3d_pipeline_unlink(p);
    for (int i = 0; i < p->variant_count; i++) {
        if (p->variants[i].pipeline && vio_vk.device) {
            vio_vk_defer_destroy(VIO_VK_GRAVE_PIPELINE, (uint64_t)p->variants[i].pipeline, NULL);
        }
        p->variants[i].pipeline = VK_NULL_HANDLE;
    }
    p->variant_count = 0;
    p->dead = 1;
}

void vio_vk3d_destroy_pipeline(void *pipeline)
{
    vio_vk3d_pipeline *p = (vio_vk3d_pipeline *)pipeline;
    if (!p) return;
    vk3d_forget_pipeline(p);
    vk3d_pipeline_release_gpu(p);
    free(p);
}

static VkCompareOp vk3d_compare(int f)
{
    switch (f) {
        case VIO_CMP_NEVER:    return VK_COMPARE_OP_NEVER;
        case VIO_CMP_LESS:     return VK_COMPARE_OP_LESS;
        case VIO_CMP_EQUAL:    return VK_COMPARE_OP_EQUAL;
        case VIO_CMP_LEQUAL:   return VK_COMPARE_OP_LESS_OR_EQUAL;
        case VIO_CMP_GREATER:  return VK_COMPARE_OP_GREATER;
        case VIO_CMP_NOTEQUAL: return VK_COMPARE_OP_NOT_EQUAL;
        case VIO_CMP_GEQUAL:   return VK_COMPARE_OP_GREATER_OR_EQUAL;
        default:               return VK_COMPARE_OP_ALWAYS;
    }
}

static VkStencilOp vk3d_stencil_op(int op)
{
    switch (op) {
        case VIO_STENCIL_ZERO:      return VK_STENCIL_OP_ZERO;
        case VIO_STENCIL_REPLACE:   return VK_STENCIL_OP_REPLACE;
        case VIO_STENCIL_INCR:      return VK_STENCIL_OP_INCREMENT_AND_CLAMP;
        case VIO_STENCIL_DECR:      return VK_STENCIL_OP_DECREMENT_AND_CLAMP;
        case VIO_STENCIL_INVERT:    return VK_STENCIL_OP_INVERT;
        case VIO_STENCIL_INCR_WRAP: return VK_STENCIL_OP_INCREMENT_AND_WRAP;
        case VIO_STENCIL_DECR_WRAP: return VK_STENCIL_OP_DECREMENT_AND_WRAP;
        default:                    return VK_STENCIL_OP_KEEP;
    }
}

/* Same factor table as d3d12_fill_rt_blend; cm is taken literally (0 = no writes). */
static void vk3d_blend(VkPipelineColorBlendAttachmentState *b, int blend, int cm)
{
    memset(b, 0, sizeof(*b));
    if (cm & VIO_COLOR_R) b->colorWriteMask |= VK_COLOR_COMPONENT_R_BIT;
    if (cm & VIO_COLOR_G) b->colorWriteMask |= VK_COLOR_COMPONENT_G_BIT;
    if (cm & VIO_COLOR_B) b->colorWriteMask |= VK_COLOR_COMPONENT_B_BIT;
    if (cm & VIO_COLOR_A) b->colorWriteMask |= VK_COLOR_COMPONENT_A_BIT;
    b->colorBlendOp = b->alphaBlendOp = VK_BLEND_OP_ADD;
    b->srcColorBlendFactor = b->srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    b->dstColorBlendFactor = b->dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
    switch (blend) {
        case VIO_BLEND_ALPHA:
            b->blendEnable = VK_TRUE;
            b->srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA; b->dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
            b->srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;       b->dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA; break;
        case VIO_BLEND_ADDITIVE:
            b->blendEnable = VK_TRUE;
            b->srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA; b->dstColorBlendFactor = VK_BLEND_FACTOR_ONE;
            b->srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;       b->dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE; break;
        case VIO_BLEND_PREMULTIPLIED:
            b->blendEnable = VK_TRUE;
            b->srcColorBlendFactor = VK_BLEND_FACTOR_ONE; b->dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
            b->srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE; b->dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA; break;
        case VIO_BLEND_MULTIPLY:
            b->blendEnable = VK_TRUE;
            b->srcColorBlendFactor = VK_BLEND_FACTOR_DST_COLOR; b->dstColorBlendFactor = VK_BLEND_FACTOR_ZERO;
            b->srcAlphaBlendFactor = VK_BLEND_FACTOR_DST_ALPHA; b->dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO; break;
        case VIO_BLEND_SCREEN:
            b->blendEnable = VK_TRUE;
            b->srcColorBlendFactor = VK_BLEND_FACTOR_ONE; b->dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR;
            b->srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE; b->dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA; break;
        case VIO_BLEND_MIN:
            b->blendEnable = VK_TRUE;
            b->colorBlendOp = b->alphaBlendOp = VK_BLEND_OP_MIN;
            b->dstColorBlendFactor = b->dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE; break;
        case VIO_BLEND_MAX:
            b->blendEnable = VK_TRUE;
            b->colorBlendOp = b->alphaBlendOp = VK_BLEND_OP_MAX;
            b->dstColorBlendFactor = b->dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE; break;
        default: break;
    }
}

static VkPrimitiveTopology vk3d_topology(vio_topology t)
{
    switch (t) {
        case VIO_TRIANGLE_STRIP: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
        case VIO_TRIANGLE_FAN:   return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN;
        case VIO_LINES:          return VK_PRIMITIVE_TOPOLOGY_LINE_LIST;
        case VIO_LINE_STRIP:     return VK_PRIMITIVE_TOPOLOGY_LINE_STRIP;
        case VIO_POINTS:         return VK_PRIMITIVE_TOPOLOGY_POINT_LIST;
        default:                 return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    }
}

static uint64_t vk3d_mix(uint64_t h, uint64_t v)
{
    h ^= v + 0x9E3779B97F4A7C15ULL + (h << 6) + (h >> 2);
    return h;
}

/* The pipeline for the render pass currently open (attachment signature) and the
 * mesh vertex stride, created on first use. */
VkPipeline vk3d_pipeline_variant(vio_vk3d_pipeline *p, uint32_t stride)
{
    if (!p || p->dead || !p->shader || p->shader->dead || !vio_vk.cur_render_pass) return VK_NULL_HANDLE;
    if (stride == 0) stride = p->vertex_stride;
    int cc = vio_vk.cur_color_count > 4 ? 4 : vio_vk.cur_color_count;
    uint64_t key = 1469598103934665603ULL;
    key = vk3d_mix(key, (uint64_t)cc);
    for (int i = 0; i < cc; i++) key = vk3d_mix(key, (uint64_t)vio_vk.cur_color_formats[i]);
    key = vk3d_mix(key, (uint64_t)vio_vk.cur_samples);
    key = vk3d_mix(key, (uint64_t)vio_vk.cur_has_depth);
    key = vk3d_mix(key, (uint64_t)stride);
    for (int i = 0; i < p->variant_count; i++) {
        if (p->variants[i].key == key) return p->variants[i].pipeline;
    }

    const vio_pipeline_desc *d = &p->desc;
    VkPipelineShaderStageCreateInfo stages[2];
    memset(stages, 0, sizeof(stages));
    stages[0].sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage  = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = p->shader->vs;
    stages[0].pName  = "main";
    stages[1].sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage  = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = p->shader->fs;
    stages[1].pName  = "main";

    VkVertexInputBindingDescription vb[2];
    VkVertexInputAttributeDescription va[16];
    memset(vb, 0, sizeof(vb));
    memset(va, 0, sizeof(va));
    uint32_t off = 0;
    for (int i = 0; i < p->attrib_count; i++) {
        int loc = p->attribs[i].location;
        va[i].location = (uint32_t)loc;
        va[i].format   = vk3d_format_from_vio(p->attribs[i].format);
        if (loc >= 3 && loc <= 6) {
            va[i].binding = 1;
            va[i].offset  = (uint32_t)(loc - 3) * 16u;
        } else {
            va[i].binding = 0;
            va[i].offset  = off;
            off += vk3d_format_size(p->attribs[i].format);
        }
    }
    vb[0].binding = 0; vb[0].stride = stride; vb[0].inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
    vb[1].binding = 1; vb[1].stride = 64;     vb[1].inputRate = VK_VERTEX_INPUT_RATE_INSTANCE;
    VkPipelineVertexInputStateCreateInfo vi = {0};
    vi.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vi.vertexBindingDescriptionCount   = p->has_instance_attrs ? 2 : 1;
    vi.pVertexBindingDescriptions      = vb;
    vi.vertexAttributeDescriptionCount = (uint32_t)p->attrib_count;
    vi.pVertexAttributeDescriptions    = va;

    VkPipelineInputAssemblyStateCreateInfo ia = {0};
    ia.sType    = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    ia.topology = vk3d_topology(d->topology);

    VkPipelineViewportStateCreateInfo vp = {0};
    vp.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    vp.viewportCount = 1;
    vp.scissorCount  = 1;

    VkPipelineRasterizationStateCreateInfo rs = {0};
    rs.sType       = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.cullMode    = d->cull_mode == VIO_CULL_BACK ? VK_CULL_MODE_BACK_BIT
                   : (d->cull_mode == VIO_CULL_FRONT ? VK_CULL_MODE_FRONT_BIT : VK_CULL_MODE_NONE);
    rs.frontFace   = VK_FRONT_FACE_COUNTER_CLOCKWISE;   /* the image is D3D-oriented, so visual CCW stays front */
    rs.lineWidth   = 1.0f;
    if (d->depth_bias != 0.0f || d->slope_scaled_depth_bias != 0.0f) {
        rs.depthBiasEnable         = VK_TRUE;
        rs.depthBiasConstantFactor = d->depth_bias;
        rs.depthBiasSlopeFactor    = d->slope_scaled_depth_bias;
    }

    VkPipelineMultisampleStateCreateInfo ms = {0};
    ms.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    ms.rasterizationSamples = (VkSampleCountFlagBits)(vio_vk.cur_samples > 1 ? vio_vk.cur_samples : 1);

    VkPipelineDepthStencilStateCreateInfo ds = {0};
    ds.sType            = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    ds.depthTestEnable  = d->depth_test ? VK_TRUE : VK_FALSE;
    ds.depthWriteEnable = (d->depth_test && d->depth_write) ? VK_TRUE : VK_FALSE;
    ds.depthCompareOp   = d->depth_func == VIO_DEPTH_LEQUAL ? VK_COMPARE_OP_LESS_OR_EQUAL : VK_COMPARE_OP_LESS;
    if (d->stencil_enable && vio_vk.depth_has_stencil) {
        ds.stencilTestEnable = VK_TRUE;
        ds.front.failOp      = vk3d_stencil_op(d->stencil_fail_op);
        ds.front.passOp      = vk3d_stencil_op(d->stencil_pass_op);
        ds.front.depthFailOp = vk3d_stencil_op(d->stencil_depth_fail_op);
        ds.front.compareOp   = vk3d_compare(d->stencil_func);
        ds.front.compareMask = (uint32_t)d->stencil_read_mask;
        ds.front.writeMask   = (uint32_t)d->stencil_write_mask;
        ds.front.reference   = (uint32_t)d->stencil_ref;
        ds.back = ds.front;
    }

    VkPipelineColorBlendAttachmentState att[4];
    for (int i = 0; i < cc; i++) {
        if (d->per_attachment) vk3d_blend(&att[i], d->attachment_blend[i], d->attachment_mask[i]);
        else vk3d_blend(&att[i], (int)d->blend, d->color_mask ? d->color_mask : VIO_COLOR_RGBA);
        if (i > 0 && !vio_vk.independent_blend) att[i] = att[0];
    }
    VkPipelineColorBlendStateCreateInfo cb = {0};
    cb.sType           = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    cb.attachmentCount = (uint32_t)cc;
    cb.pAttachments    = cc > 0 ? att : NULL;

    VkDynamicState dyn_states[2] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
    VkPipelineDynamicStateCreateInfo dyn = {0};
    dyn.sType             = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dyn.dynamicStateCount = 2;
    dyn.pDynamicStates    = dyn_states;

    VkGraphicsPipelineCreateInfo gi = {0};
    gi.sType               = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    /* Depth-only passes skip a fragment stage that could only write colour. */
    gi.stageCount          = (cc == 0 && !p->shader->fs_needed_without_color) ? 1 : 2;
    gi.pStages             = stages;
    gi.pVertexInputState   = &vi;
    gi.pInputAssemblyState = &ia;
    gi.pViewportState      = &vp;
    gi.pRasterizationState = &rs;
    gi.pMultisampleState   = &ms;
    gi.pDepthStencilState  = vio_vk.cur_has_depth ? &ds : NULL;
    gi.pColorBlendState    = &cb;
    gi.pDynamicState       = &dyn;
    gi.layout              = p->shader->layout;
    gi.renderPass          = vio_vk.cur_render_pass;
    gi.subpass             = 0;
    gi.basePipelineIndex   = -1;

    VkPipeline pl = VK_NULL_HANDLE;
    VkResult r = vkCreateGraphicsPipelines(vio_vk.device, vio_vk.pipeline_cache, 1, &gi, NULL, &pl);
    if (r != VK_SUCCESS) {
        php_error_docref(NULL, E_WARNING, "Vulkan: vkCreateGraphicsPipelines failed (VkResult %d)", (int)r);
        return VK_NULL_HANDLE;
    }
    int slot = p->variant_count;
    if (slot >= VK3D_MAX_VARIANTS) {
        slot = (int)(key % VK3D_MAX_VARIANTS);
        vio_vk_defer_destroy(VIO_VK_GRAVE_PIPELINE, (uint64_t)p->variants[slot].pipeline, NULL);
    } else {
        p->variant_count++;
    }
    p->variants[slot].key = key;
    p->variants[slot].pipeline = pl;
    return pl;
}

#endif /* HAVE_VULKAN */

/*
 * php-vio - Metal backend: SPIR-V -> MSL transpilation
 *
 * Plain C (no Objective-C) so the resource renumbering and the tessellation
 * stage options can be checked against SPIRV-Cross on any host. Included by
 * vio_metal.m only; every function is static.
 */

#ifndef VIO_METAL_MSL_H
#define VIO_METAL_MSL_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define VIO_METAL_MAX_RES 16

/* One buffer-like resource of a shader stage after the MSL renumbering. */
typedef struct _vio_metal_res_buffer {
    int kind;       /* 0 = UBO, 1 = SSBO, 2 = push-constant block */
    int set;        /* original GLSL descriptor set */
    int binding;    /* original GLSL binding (what vio_bind_buffer / vio_bind_storage_buffer pass) */
    int msl_index;  /* [[buffer(N)]] the resource was pinned to */
} vio_metal_res_buffer;

/* One combined image-sampler of a shader stage. Listed in SPIRV-Cross's
 * sampled_images order — the SAME order vio_spirv_reflect() reports and the PHP
 * layer's gl_to_hlsl_sampler remap indexes (see vio_bind_texture_internal). */
typedef struct _vio_metal_res_texture {
    int binding;    /* original GLSL binding */
    int msl_index;  /* [[texture(N)]] == [[sampler(N)]] */
    int is_depth;   /* sampler2DShadow -> needs a compare sampler */
    int is_cube;    /* samplerCube */
} vio_metal_res_texture;

typedef struct _vio_metal_vs_input {
    int location;
    int components; /* 1..4 */
    int columns;    /* 1 for vectors, 4 for a mat4 (occupies location..location+3) */
} vio_metal_vs_input;

/* The vertex stage's [[stage_in]] attributes, reflected so the pipeline can
 * build a matching vertex (or stage-input) descriptor. */
typedef struct _vio_metal_vs_layout {
    vio_metal_vs_input inputs[VIO_METAL_MAX_RES];
    int                input_count;
    int                vertex_stride;          /* bytes of per-vertex attributes (locations outside 3..6) */
    int                uses_instance_attribs;  /* any input at location 3..6 (per-instance mat4 columns) */
} vio_metal_vs_layout;

typedef struct _vio_metal_stage_res {
    vio_metal_res_buffer  buffers[VIO_METAL_MAX_RES];
    int                   buffer_count;
    vio_metal_res_texture textures[VIO_METAL_MAX_RES];
    int                   texture_count;
    /* MSL buffer index of the stage's default uniform block — ubos[0], else the
     * push-constant block; -1 when the stage has neither. This is the block
     * vio_spirv_get_uniform_offsets() reflects into sh->cbuffer_data, so the
     * per-draw cbuffer slice is bound here. */
    int                   cbuffer_index;
} vio_metal_stage_res;

/* What one SPIR-V module is transpiled into. Metal has no hull / domain
 * stages: tessellation runs the vertex and control stages as compute kernels
 * that write their outputs into buffers, the fixed-function tessellator reads
 * the factors, and the evaluation stage becomes a post-tessellation vertex
 * function that reads the control points from those buffers. */
typedef enum {
    VIO_MSL_VERTEX = 0,
    VIO_MSL_FRAGMENT,
    VIO_MSL_VERTEX_TESS,   /* vertex stage as a kernel, one thread per (vertex, instance) */
    VIO_MSL_TESS_CONTROL,  /* tess control as a kernel, one thread per output control point */
    VIO_MSL_TESS_EVAL      /* tess evaluation as a [[patch]] vertex function */
} vio_msl_stage;

/* Buffer indices of the tessellation plumbing. Resources of every stage are
 * renumbered to 0..N-1 (N <= 2 * VIO_METAL_MAX_RES), so they stay below 19. */
#define VIO_METAL_TESS_PARAMS_INDEX     19  /* uint[2]: input control points per patch, patch count */
#define VIO_METAL_TESS_PATCH_IN_INDEX   20  /* TES: TCS patch outputs */
#define VIO_METAL_TESS_IN_INDEX         22  /* TCS: VS kernel outputs; TES: TCS control points */
#define VIO_METAL_TESS_FACTOR_INDEX     26  /* TCS: MTL{Triangle,Quad}TessellationFactorsHalf per patch */
#define VIO_METAL_TESS_PATCH_OUT_INDEX  27  /* TCS: patch outputs */
#define VIO_METAL_TESS_OUT_INDEX        28  /* VS kernel / TCS: per-vertex / per-control-point outputs */

#define VIO_MSL_DOMAIN_TRIANGLES 0
#define VIO_MSL_DOMAIN_QUADS     1
#define VIO_MSL_DOMAIN_ISOLINES  2

#define VIO_MSL_SPACING_EQUAL           0
#define VIO_MSL_SPACING_FRACTIONAL_EVEN 1
#define VIO_MSL_SPACING_FRACTIONAL_ODD  2

typedef struct _vio_metal_tess_info {
    int    domain;           /* VIO_MSL_DOMAIN_* (declared by the TES, or the TCS) */
    int    spacing;          /* VIO_MSL_SPACING_* */
    int    ccw;              /* vertex order of the generated triangles (GL default: ccw) */
    int    point_mode;
    int    output_vertices;  /* TCS layout(vertices = N) */
    /* Upper bounds of one output record, for sizing the per-draw buffers; the
     * kernels index the buffers with their own (smaller or equal) struct sizes. */
    size_t vs_out_stride;    /* one VS kernel output vertex */
    size_t cp_stride;        /* one TCS output control point */
    size_t patch_stride;     /* one TCS patch record */
} vio_metal_tess_info;

#ifdef HAVE_SPIRV_CROSS

static void metal_gfx_add_binding(spvc_compiler compiler, SpvExecutionModel stage,
                                  unsigned desc_set, unsigned binding, unsigned msl_index)
{
    spvc_msl_resource_binding_2 rb;
    spvc_msl_resource_binding_init_2(&rb);
    rb.stage       = stage;
    rb.desc_set    = desc_set;
    rb.binding     = binding;
    rb.count       = 1;
    /* One entry serves whichever resource kind lives at this (set, binding):
     * a buffer reads msl_buffer, a texture reads msl_texture + msl_sampler. */
    rb.msl_buffer  = msl_index;
    rb.msl_texture = msl_index;
    rb.msl_sampler = msl_index;
    spvc_compiler_msl_add_resource_binding_2(compiler, &rb);
}

/* Bytes one interface variable of `type_id` can take in an MSL struct, rounded
 * up generously: every vector or matrix column counts as 16 bytes (float3 /
 * float4 alignment), 32 for doubles. With skip_outer the outermost array
 * dimension is dropped — the per-vertex array of a TCS output / TES input. */
static size_t metal_varying_bytes(spvc_compiler compiler, spvc_type_id type_id, int skip_outer)
{
    spvc_type type = spvc_compiler_get_type_handle(compiler, type_id);
    if (!type) return 64;
    size_t count = 1;
    unsigned dims = spvc_type_get_num_array_dimensions(type);
    /* SPIRV-Cross keeps array dimensions innermost first: the last one is the
     * outermost. */
    for (unsigned d = 0; d < dims; d++) {
        if (skip_outer && d == dims - 1) continue;
        if (!spvc_type_array_dimension_is_literal(type, d)) continue;
        unsigned n = (unsigned)spvc_type_get_array_dimension(type, d);
        if (n > 1) count *= n;
    }
    size_t elem;
    if (spvc_type_get_basetype(type) == SPVC_BASETYPE_STRUCT) {
        elem = 0;
        unsigned members = spvc_type_get_num_member_types(type);
        for (unsigned m = 0; m < members; m++) {
            elem += metal_varying_bytes(compiler, spvc_type_get_member_type(type, m), 0);
        }
    } else {
        unsigned columns = spvc_type_get_columns(type);
        size_t column = spvc_type_get_basetype(type) == SPVC_BASETYPE_FP64 ? 32 : 16;
        elem = column * (columns > 0 ? columns : 1);
    }
    return elem * count;
}

/* Upper bound of one output record of a VS kernel / TCS: the stage outputs
 * plus slack for the builtins (gl_Position, gl_PointSize, clip distances). */
static void metal_output_strides(spvc_compiler compiler, spvc_resources resources, int arrayed,
                                 size_t *per_vertex, size_t *per_patch)
{
    const spvc_reflected_resource *list = NULL;
    size_t count = 0;
    size_t vtx = 128, patch = 16;
    spvc_resources_get_resource_list_for_type(resources, SPVC_RESOURCE_TYPE_STAGE_OUTPUT, &list, &count);
    for (size_t i = 0; i < count; i++) {
        if (spvc_compiler_has_decoration(compiler, list[i].id, SpvDecorationPatch)) {
            patch += metal_varying_bytes(compiler, list[i].type_id, 0);
        } else {
            vtx += metal_varying_bytes(compiler, list[i].type_id, arrayed);
        }
    }
    if (per_vertex) *per_vertex = vtx;
    if (per_patch) *per_patch = patch;
}

/* Read the tessellation execution modes: output control points from the TCS,
 * domain / spacing / vertex order / point mode from the TES (an HLSL-style TCS
 * may declare them instead). Returns 0 on success. */
static int vio_metal_tess_reflect(const uint32_t *tcs, size_t tcs_size,
                                  const uint32_t *tes, size_t tes_size,
                                  vio_metal_tess_info *info, char **error_msg)
{
    memset(info, 0, sizeof(*info));
    info->domain = VIO_MSL_DOMAIN_TRIANGLES;
    info->spacing = VIO_MSL_SPACING_EQUAL;
    info->ccw = 1;

    const uint32_t *mods[2] = { tcs, tes };
    const size_t    sizes[2] = { tcs_size, tes_size };
    for (int m = 0; m < 2; m++) {
        spvc_context ctx = NULL;
        spvc_parsed_ir ir = NULL;
        spvc_compiler compiler = NULL;
        if (spvc_context_create(&ctx) != SPVC_SUCCESS) {
            if (error_msg) *error_msg = strdup("Failed to create SPIRV-Cross context");
            return -1;
        }
        if (spvc_context_parse_spirv(ctx, mods[m], sizes[m] / sizeof(uint32_t), &ir) != SPVC_SUCCESS ||
            spvc_context_create_compiler(ctx, SPVC_BACKEND_NONE, ir, SPVC_CAPTURE_MODE_TAKE_OWNERSHIP,
                                         &compiler) != SPVC_SUCCESS) {
            if (error_msg) *error_msg = strdup(spvc_context_get_last_error_string(ctx));
            spvc_context_destroy(ctx);
            return -1;
        }
        const SpvExecutionMode *modes = NULL;
        size_t count = 0;
        spvc_compiler_get_execution_modes(compiler, &modes, &count);
        for (size_t i = 0; i < count; i++) {
            switch (modes[i]) {
                case SpvExecutionModeTriangles:            info->domain = VIO_MSL_DOMAIN_TRIANGLES; break;
                case SpvExecutionModeQuads:                info->domain = VIO_MSL_DOMAIN_QUADS; break;
                case SpvExecutionModeIsolines:             info->domain = VIO_MSL_DOMAIN_ISOLINES; break;
                case SpvExecutionModeSpacingEqual:         info->spacing = VIO_MSL_SPACING_EQUAL; break;
                case SpvExecutionModeSpacingFractionalEven: info->spacing = VIO_MSL_SPACING_FRACTIONAL_EVEN; break;
                case SpvExecutionModeSpacingFractionalOdd: info->spacing = VIO_MSL_SPACING_FRACTIONAL_ODD; break;
                case SpvExecutionModeVertexOrderCw:        info->ccw = 0; break;
                case SpvExecutionModeVertexOrderCcw:       info->ccw = 1; break;
                case SpvExecutionModePointMode:            info->point_mode = 1; break;
                case SpvExecutionModeOutputVertices:
                    if (m == 0) info->output_vertices = (int)spvc_compiler_get_execution_mode_argument(compiler, modes[i]);
                    break;
                default: break;
            }
        }
        spvc_context_destroy(ctx);
    }
    if (info->output_vertices < 1 || info->output_vertices > 32) {
        if (error_msg) *error_msg = strdup("tessellation control stage must declare layout(vertices = 1..32) out");
        return -1;
    }
    return 0;
}

/* Transpile one GRAPHICS stage to MSL with deterministic resource indices.
 *
 * glslang's AUTO_MAP_BINDINGS leaves every resource of an OpenGL-style shader
 * at (set 0, binding 0) — three sampler2Ds all report binding 0 — so the GLSL
 * bindings cannot be used as MSL indices directly (SPIRV-Cross keys explicit
 * bindings by (set, binding) and would collapse them). Instead every UBO, SSBO,
 * push-constant block and sampled image is renumbered to a unique binding k in
 * list order and pinned to [[buffer(k)]] / [[texture(k)]] / [[sampler(k)]]. The
 * resulting tables (vio_metal_stage_res) are what draw / bind_texture /
 * bind_storage_buffer consult at runtime. Mirrors what vio_spirv_to_hlsl does
 * with SpvDecorationBinding for the D3D register spaces.
 *
 * `vl` receives the [[stage_in]] layout of the vertex stages (may be NULL for
 * the others). The tessellation stages read `tess` (domain for the TCS, output
 * control points for the TES) and the kernels write their output strides back.
 *
 * Returns a malloc'd MSL string (caller frees) or NULL. */
static char *metal_gfx_spirv_to_msl(const uint32_t *spirv, size_t spirv_size, vio_msl_stage stage,
                                    vio_metal_stage_res *res, vio_metal_vs_layout *vl,
                                    unsigned frag_output_mask, vio_metal_tess_info *tess,
                                    char **error_msg)
{
    spvc_context   ctx = NULL;
    spvc_parsed_ir ir = NULL;
    spvc_compiler  compiler = NULL;
    spvc_compiler_options opts = NULL;
    spvc_resources resources = NULL;
    const char    *result = NULL;
    int is_vertex = (stage == VIO_MSL_VERTEX || stage == VIO_MSL_VERTEX_TESS);
    int is_tess = (stage == VIO_MSL_VERTEX_TESS || stage == VIO_MSL_TESS_CONTROL || stage == VIO_MSL_TESS_EVAL);

    memset(res, 0, sizeof(*res));
    res->cbuffer_index = -1;

    if (is_tess && !tess) {
        if (error_msg) *error_msg = strdup("tessellation stage without tessellation info");
        return NULL;
    }
    if (spvc_context_create(&ctx) != SPVC_SUCCESS) {
        if (error_msg) *error_msg = strdup("Failed to create SPIRV-Cross context");
        return NULL;
    }
    if (spvc_context_parse_spirv(ctx, spirv, spirv_size / sizeof(uint32_t), &ir) != SPVC_SUCCESS ||
        spvc_context_create_compiler(ctx, SPVC_BACKEND_MSL, ir, SPVC_CAPTURE_MODE_TAKE_OWNERSHIP,
                                     &compiler) != SPVC_SUCCESS) {
        if (error_msg) *error_msg = strdup(spvc_context_get_last_error_string(ctx));
        spvc_context_destroy(ctx);
        return NULL;
    }

    if (spvc_compiler_create_compiler_options(compiler, &opts) == SPVC_SUCCESS) {
        spvc_compiler_options_set_uint(opts, SPVC_COMPILER_OPTION_MSL_VERSION,
                                       is_tess ? SPVC_MAKE_MSL_VERSION(2, 1, 0) : SPVC_MAKE_MSL_VERSION(2, 0, 0));
        spvc_compiler_options_set_uint(opts, SPVC_COMPILER_OPTION_MSL_PLATFORM, SPVC_MSL_PLATFORM_MACOS);
        if (stage == VIO_MSL_FRAGMENT) {
            spvc_compiler_options_set_uint(opts, SPVC_COMPILER_OPTION_MSL_ENABLE_FRAG_OUTPUT_MASK, frag_output_mask);
        }
        if (is_tess) {
            spvc_compiler_options_set_uint(opts, SPVC_COMPILER_OPTION_MSL_INDIRECT_PARAMS_BUFFER_INDEX, VIO_METAL_TESS_PARAMS_INDEX);
            spvc_compiler_options_set_uint(opts, SPVC_COMPILER_OPTION_MSL_SHADER_INPUT_BUFFER_INDEX, VIO_METAL_TESS_IN_INDEX);
            spvc_compiler_options_set_uint(opts, SPVC_COMPILER_OPTION_MSL_SHADER_PATCH_INPUT_BUFFER_INDEX, VIO_METAL_TESS_PATCH_IN_INDEX);
            spvc_compiler_options_set_uint(opts, SPVC_COMPILER_OPTION_MSL_SHADER_OUTPUT_BUFFER_INDEX, VIO_METAL_TESS_OUT_INDEX);
            spvc_compiler_options_set_uint(opts, SPVC_COMPILER_OPTION_MSL_SHADER_PATCH_OUTPUT_BUFFER_INDEX, VIO_METAL_TESS_PATCH_OUT_INDEX);
            spvc_compiler_options_set_uint(opts, SPVC_COMPILER_OPTION_MSL_SHADER_TESS_FACTOR_OUTPUT_BUFFER_INDEX, VIO_METAL_TESS_FACTOR_INDEX);
        }
        if (stage == VIO_MSL_VERTEX_TESS) {
            /* Outputs go to spvOut[instance * vertex_count + vertex], in the
             * order the vertices arrive (the mesh is fed already de-indexed). */
            spvc_compiler_options_set_bool(opts, SPVC_COMPILER_OPTION_MSL_VERTEX_FOR_TESSELLATION, SPVC_TRUE);
            spvc_compiler_options_set_bool(opts, SPVC_COMPILER_OPTION_MSL_CAPTURE_OUTPUT_TO_BUFFER, SPVC_TRUE);
        } else if (stage == VIO_MSL_TESS_CONTROL) {
            /* One thread per output control point of every patch in a flat
             * grid; params[0] = input control points, params[1] = patch count. */
            spvc_compiler_options_set_bool(opts, SPVC_COMPILER_OPTION_MSL_MULTI_PATCH_WORKGROUP, SPVC_TRUE);
            spvc_compiler_options_set_bool(opts, SPVC_COMPILER_OPTION_MSL_CAPTURE_OUTPUT_TO_BUFFER, SPVC_TRUE);
        } else if (stage == VIO_MSL_TESS_EVAL) {
            /* Control points come from the TCS output buffers, not a stage_in
             * descriptor; gl_TessCoord keeps the GL origin (lower left). */
            spvc_compiler_options_set_bool(opts, SPVC_COMPILER_OPTION_MSL_RAW_BUFFER_TESE_INPUT, SPVC_TRUE);
            spvc_compiler_options_set_bool(opts, SPVC_COMPILER_OPTION_MSL_TESS_DOMAIN_ORIGIN_LOWER_LEFT, SPVC_TRUE);
        }
        spvc_compiler_install_compiler_options(compiler, opts);
    }

    /* The control stage writes the tessellation factors in the struct of the
     * evaluation stage's domain, which only the TES declares in GLSL; the
     * evaluation stage indexes the control points with the TCS's count. */
    if (stage == VIO_MSL_TESS_CONTROL) {
        spvc_compiler_set_execution_mode(compiler, tess->domain == VIO_MSL_DOMAIN_QUADS
                                                   ? SpvExecutionModeQuads : SpvExecutionModeTriangles);
    } else if (stage == VIO_MSL_TESS_EVAL) {
        spvc_compiler_set_execution_mode_with_arguments(compiler, SpvExecutionModeOutputVertices,
                                                        (unsigned)tess->output_vertices, 0, 0);
    }

    if (spvc_compiler_create_shader_resources(compiler, &resources) != SPVC_SUCCESS) {
        if (error_msg) *error_msg = strdup(spvc_context_get_last_error_string(ctx));
        spvc_context_destroy(ctx);
        return NULL;
    }

    SpvExecutionModel em = spvc_compiler_get_execution_model(compiler);
    unsigned next = 0;

    /* Buffers: UBOs first so ubos[0] becomes the default cbuffer (the block
     * vio_spirv_get_uniform_offsets picks), then SSBOs, then push constants. */
    static const struct { spvc_resource_type type; int kind; } buffer_kinds[] = {
        { SPVC_RESOURCE_TYPE_UNIFORM_BUFFER, 0 },
        { SPVC_RESOURCE_TYPE_STORAGE_BUFFER, 1 },
        { SPVC_RESOURCE_TYPE_PUSH_CONSTANT,  2 },
    };
    for (size_t k = 0; k < sizeof(buffer_kinds) / sizeof(buffer_kinds[0]); k++) {
        const spvc_reflected_resource *list = NULL;
        size_t count = 0;
        spvc_resources_get_resource_list_for_type(resources, buffer_kinds[k].type, &list, &count);
        for (size_t i = 0; i < count && res->buffer_count < VIO_METAL_MAX_RES; i++) {
            vio_metal_res_buffer *b = &res->buffers[res->buffer_count++];
            b->kind      = buffer_kinds[k].kind;
            b->set       = (int)spvc_compiler_get_decoration(compiler, list[i].id, SpvDecorationDescriptorSet);
            b->binding   = (int)spvc_compiler_get_decoration(compiler, list[i].id, SpvDecorationBinding);
            b->msl_index = (int)next;
            if (b->kind == 2) {
                metal_gfx_add_binding(compiler, em, SPVC_MSL_PUSH_CONSTANT_DESC_SET,
                                      SPVC_MSL_PUSH_CONSTANT_BINDING, next);
            } else {
                spvc_compiler_set_decoration(compiler, list[i].id, SpvDecorationDescriptorSet, 0);
                spvc_compiler_set_decoration(compiler, list[i].id, SpvDecorationBinding, next);
                metal_gfx_add_binding(compiler, em, 0, next, next);
            }
            if (res->cbuffer_index < 0 && b->kind != 1) {
                res->cbuffer_index = (int)next;
            }
            next++;
        }
    }

    /* Combined image-samplers, in sampled_images order. */
    {
        const spvc_reflected_resource *list = NULL;
        size_t count = 0;
        spvc_resources_get_resource_list_for_type(resources, SPVC_RESOURCE_TYPE_SAMPLED_IMAGE, &list, &count);
        for (size_t i = 0; i < count && res->texture_count < VIO_METAL_MAX_RES; i++) {
            vio_metal_res_texture *t = &res->textures[res->texture_count++];
            t->binding   = (int)spvc_compiler_get_decoration(compiler, list[i].id, SpvDecorationBinding);
            t->msl_index = (int)next;
            spvc_type type = spvc_compiler_get_type_handle(compiler, list[i].type_id);
            spvc_type image = type ? spvc_compiler_get_type_handle(compiler, spvc_type_get_base_type_id(type)) : NULL;
            if (image) {
                t->is_depth = spvc_type_get_image_is_depth(image) ? 1 : 0;
                t->is_cube  = (spvc_type_get_image_dimension(image) == SpvDimCube) ? 1 : 0;
            }
            spvc_compiler_set_decoration(compiler, list[i].id, SpvDecorationDescriptorSet, 0);
            spvc_compiler_set_decoration(compiler, list[i].id, SpvDecorationBinding, next);
            metal_gfx_add_binding(compiler, em, 0, next, next);
            next++;
        }
    }

    /* Vertex inputs: what the [[stage_in]] struct expects, so the pipeline can
     * build a matching MTLVertexDescriptor without trusting the caller's layout
     * (a mat4 instance attribute reflects as ONE input with 4 columns). */
    if (is_vertex && vl) {
        const spvc_reflected_resource *list = NULL;
        size_t count = 0;
        spvc_resources_get_resource_list_for_type(resources, SPVC_RESOURCE_TYPE_STAGE_INPUT, &list, &count);
        vl->input_count = 0;
        vl->vertex_stride = 0;
        vl->uses_instance_attribs = 0;
        for (size_t i = 0; i < count && vl->input_count < VIO_METAL_MAX_RES; i++) {
            vio_metal_vs_input *in = &vl->inputs[vl->input_count++];
            in->location = (int)spvc_compiler_get_decoration(compiler, list[i].id, SpvDecorationLocation);
            spvc_type type = spvc_compiler_get_type_handle(compiler, list[i].type_id);
            in->components = type ? (int)spvc_type_get_vector_size(type) : 3;
            in->columns    = type ? (int)spvc_type_get_columns(type) : 1;
            if (in->components < 1 || in->components > 4) in->components = 3;
            if (in->columns < 1) in->columns = 1;
            if (in->location >= 3 && in->location <= 6) {
                vl->uses_instance_attribs = 1;
            } else {
                vl->vertex_stride += in->components * in->columns * (int)sizeof(float);
            }
        }
    }

    if (stage == VIO_MSL_VERTEX_TESS) {
        metal_output_strides(compiler, resources, 0, &tess->vs_out_stride, NULL);
    } else if (stage == VIO_MSL_TESS_CONTROL) {
        metal_output_strides(compiler, resources, 1, &tess->cp_stride, &tess->patch_stride);
    }

    if (spvc_compiler_compile(compiler, &result) != SPVC_SUCCESS) {
        if (error_msg) *error_msg = strdup(spvc_context_get_last_error_string(ctx));
        spvc_context_destroy(ctx);
        return NULL;
    }

    if (getenv("VIO_DUMP_MSL")) {
        static const char *labels[] = { "vertex", "fragment", "vertex (tessellation kernel)",
                                        "tessellation control", "tessellation evaluation" };
        fprintf(stderr, "==== Metal %s MSL ====\n%s\n==== end ====\n", labels[stage], result);
        fflush(stderr);
    }

    char *output = strdup(result);
    spvc_context_destroy(ctx);
    return output;
}

#endif /* HAVE_SPIRV_CROSS */

#endif /* VIO_METAL_MSL_H */

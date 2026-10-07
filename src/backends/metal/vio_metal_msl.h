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
#include <ctype.h>

#define VIO_METAL_MAX_RES 16

/* MSL target of every transpile (graphics stages, kernels, compute): the rung
 * of the version ladder the device context picked (major * 10 + minor, see
 * metal_select_msl_version in vio_metal.m), and the platform. Set once per
 * context before any shader is built. */
static int metal_msl_target_version = 21;
static int metal_msl_target_ios = 0;
/* Multiview (vio_shader 'view_count'): set around the transpile of a multiview
 * shader's vertex / fragment stage. SPIRV-Cross emulates the views with
 * instancing - instance count x views, gl_ViewIndex from the instance, written
 * to [[render_target_array_index]] - and reads {base view, view count} from
 * [[buffer(VIO_METAL_VIEW_MASK_INDEX)]] (its default 24 is a tessellation slot). */
static int metal_msl_multiview = 0;
#define VIO_METAL_VIEW_MASK_INDEX 23

static void metal_msl_set_target(int version, int ios)
{
    metal_msl_target_version = version;
    metal_msl_target_ios = ios;
}

/* SPIRV-Cross encoding (major * 10000 + minor * 100) of the target, never
 * below `floor` (major * 10 + minor) - the tessellation stages need 2.1. */
static unsigned metal_msl_spvc_version(int floor)
{
    int v = metal_msl_target_version < floor ? floor : metal_msl_target_version;
    return (unsigned)((v / 10) * 10000 + (v % 10) * 100);
}


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
    VIO_MSL_TESS_EVAL,     /* tess evaluation as a [[patch]] vertex function */
    VIO_MSL_KERNEL,        /* vertex / geometry stage rebuilt as a GLSL compute kernel (vio_metal_kernel.h) */
    VIO_MSL_MESH,          /* mesh stage ([[mesh]], MSL 3.0) */
    VIO_MSL_TASK           /* task stage ([[object]], MSL 3.0) */
} vio_msl_stage;

/* LocalSize execution mode of a compute-like module (mesh / task stages):
 * the threads per threadgroup drawMeshThreadgroups needs. 1x1x1 when absent. */
static void metal_spirv_local_size(const uint32_t *w, size_t bytes, unsigned out[3])
{
    out[0] = out[1] = out[2] = 1;
    size_t n = bytes / 4;
    if (!w || n < 5 || w[0] != 0x07230203u) return;
    for (size_t i = 5; i < n; ) {
        uint32_t count = w[i] >> 16, op = w[i] & 0xFFFFu;
        if (count == 0) break;
        if (op == 16 && count >= 6 && w[i + 2] == 17) {   /* OpExecutionMode %entry LocalSize x y z */
            out[0] = w[i + 3]; out[1] = w[i + 4]; out[2] = w[i + 5];
            return;
        }
        i += count;
    }
}

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
    size_t tes_out_stride;   /* one TES output vertex (isolines / point_mode capture) */
} vio_metal_tess_info;

#ifdef HAVE_SPIRV_CROSS
static void metal_msl_apply_target(spvc_compiler_options opts, int floor)
{
    spvc_compiler_options_set_uint(opts, SPVC_COMPILER_OPTION_MSL_VERSION, metal_msl_spvc_version(floor));
    spvc_compiler_options_set_uint(opts, SPVC_COMPILER_OPTION_MSL_PLATFORM,
                                   metal_msl_target_ios ? SPVC_MSL_PLATFORM_IOS : SPVC_MSL_PLATFORM_MACOS);
}


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
        /* Mesh / task stages need MSL 3.0 (the ladder only reports the
         * capability from that rung). */
        metal_msl_apply_target(opts, (stage == VIO_MSL_MESH || stage == VIO_MSL_TASK) ? 30 : (is_tess ? 21 : 20));
        if (metal_msl_multiview && (stage == VIO_MSL_VERTEX || stage == VIO_MSL_FRAGMENT)) {
            spvc_compiler_options_set_bool(opts, SPVC_COMPILER_OPTION_MSL_MULTIVIEW, SPVC_TRUE);
            spvc_compiler_options_set_bool(opts, SPVC_COMPILER_OPTION_MSL_MULTIVIEW_LAYERED_RENDERING, SPVC_TRUE);
            spvc_compiler_options_set_uint(opts, SPVC_COMPILER_OPTION_MSL_VIEW_MASK_BUFFER_INDEX, VIO_METAL_VIEW_MASK_INDEX);
        }
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
             * descriptor. No TESS_DOMAIN_ORIGIN_LOWER_LEFT: Metal's tessellator
             * already has GL's domain coordinates, factor edges and winding
             * (test 144 on the macOS CI); that option flips v for quads, which
             * put outer[1] / outer[3] on the opposite edges. */
            spvc_compiler_options_set_bool(opts, SPVC_COMPILER_OPTION_MSL_RAW_BUFFER_TESE_INPUT, SPVC_TRUE);
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
    } else if (stage == VIO_MSL_TESS_EVAL) {
        metal_output_strides(compiler, resources, 0, &tess->tes_out_stride, NULL);
    }

    if (spvc_compiler_compile(compiler, &result) != SPVC_SUCCESS) {
        if (error_msg) *error_msg = strdup(spvc_context_get_last_error_string(ctx));
        spvc_context_destroy(ctx);
        return NULL;
    }

    if (getenv("VIO_DUMP_MSL")) {
        static const char *labels[] = { "vertex", "fragment", "vertex (tessellation kernel)",
                                        "tessellation control", "tessellation evaluation",
                                        "stage kernel", "mesh", "task" };
        fprintf(stderr, "==== Metal %s MSL ====\n%s\n==== end ====\n", labels[stage], result);
        fflush(stderr);
    }

    char *output = strdup(result);
    spvc_context_destroy(ctx);
    return output;
}

/* ── Isolines and point_mode ─────────────────────────────────────
 *
 * Metal's tessellator has neither. vio drives it with integer factors it
 * computed itself (quad domain for isolines) and runs the evaluation stage as
 * a capture function with rasterization off: every domain point vio needs is
 * written to its own slot of a capture buffer, gl_TessCoord remapped to GL's
 * spacing rule; points the integer tessellation adds beyond that set are
 * dropped. The draw then reads the slots through vio_pass (same library, same
 * output struct) as lines or points. Per-patch parameters (uint4 x 4 at
 * VIO_METAL_TESS_EMUL_INFO_INDEX):
 *   isolines   [0] = (slot base, lines, segments, segment level bits)
 *   quads      [0] = (base, m0, m1, m2) [1] = (m3, n0, n1, -) [2] = levels m0..m3 [3] = (inner0, inner1)
 *   triangles  [0] = (base, m0, m1, m2) [1] = (n, -, -, -)    [2] = levels m0..m2 [3] = (inner)
 * m = outer subdivisions, n = inner; 0 lines / m0 = 0 marks a culled patch. */

#define VIO_METAL_TESS_EMUL_INFO_INDEX 24
#define VIO_METAL_TESS_EMUL_CAP_INDEX  25

static const char *metal_tess_emul_helpers =
    "static inline float vio_sub(uint k, uint n, float f)\n"
    "{\n"
    "    if (VIO_SPACING == 0 || n <= 1u || abs(f - float(n)) < 1e-4) return float(k) / float(n);\n"
    "    if (k == 0u) return 0.0;\n"
    "    if (k >= n) return 1.0;\n"
    "    float L = 1.0 / f, sh = (1.0 - float(n - 2u) * L) * 0.5;   /* two short segments at the ends */\n"
    "    return sh + float(k - 1u) * L;\n"
    "}\n"
    "static inline bool vio_idx(float x, uint n, thread uint &k)\n"
    "{\n"
    "    float t = x * float(n), r = rint(t);\n"
    "    if (abs(t - r) > 0.01) return false;\n"
    "    k = uint(r);\n"
    "    return true;\n"
    "}\n";

static const char *metal_tess_emul_iso =
    "static inline bool vio_tess_point(float2 pic, const device uint4 *tp, uint pid, thread uint &slot, thread float2 &tc)\n"
    "{\n"
    "    uint4 a = tp[pid * 4u];\n"
    "    uint i, j;\n"
    "    if (a.y == 0u || !vio_idx(pic.y, a.y, i) || !vio_idx(pic.x, a.z, j) || i >= a.y || j > a.z) return false;\n"
    "    slot = a.x + i * (a.z + 1u) + j;\n"
    "    tc = float2(vio_sub(j, a.z, as_type<float>(a.w)), float(i) / float(a.y));\n"
    "    return true;\n"
    "}\n";

static const char *metal_tess_emul_quad =
    "static inline bool vio_tess_point(float2 pic, const device uint4 *tp, uint pid, thread uint &slot, thread float2 &tc)\n"
    "{\n"
    "    uint4 a = tp[pid * 4u], b = tp[pid * 4u + 1u];\n"
    "    float4 f = as_type<float4>(tp[pid * 4u + 2u]);\n"
    "    float2 fi = as_type<float2>(tp[pid * 4u + 3u].xy);\n"
    "    if (a.y == 0u) return false;\n"
    "    float u = pic.x, v = pic.y;\n"
    "    const float e = 1e-5;\n"
    "    bool u0 = u < e, u1 = u > 1.0 - e, v0 = v < e, v1 = v > 1.0 - e;\n"
    "    if ((u0 || u1) && (v0 || v1)) { slot = a.x + (u0 ? (v0 ? 0u : 3u) : (v0 ? 1u : 2u)); tc = float2(u1 ? 1.0 : 0.0, v1 ? 1.0 : 0.0); return true; }\n"
    "    uint k, off = a.x + 4u;\n"
    "    if (u0) { if (!vio_idx(v, a.y, k) || k == 0u || k >= a.y) return false; slot = off + k - 1u; tc = float2(0.0, vio_sub(k, a.y, f.x)); return true; }\n"
    "    off += a.y - 1u;\n"
    "    if (v0) { if (!vio_idx(u, a.z, k) || k == 0u || k >= a.z) return false; slot = off + k - 1u; tc = float2(vio_sub(k, a.z, f.y), 0.0); return true; }\n"
    "    off += a.z - 1u;\n"
    "    if (u1) { if (!vio_idx(v, a.w, k) || k == 0u || k >= a.w) return false; slot = off + k - 1u; tc = float2(1.0, vio_sub(k, a.w, f.z)); return true; }\n"
    "    off += a.w - 1u;\n"
    "    if (v1) { if (!vio_idx(u, b.x, k) || k == 0u || k >= b.x) return false; slot = off + k - 1u; tc = float2(vio_sub(k, b.x, f.w), 1.0); return true; }\n"
    "    off += b.x - 1u;\n"
    "    uint i, j;\n"
    "    if (!vio_idx(u, b.y, i) || !vio_idx(v, b.z, j) || i == 0u || i >= b.y || j == 0u || j >= b.z) return false;\n"
    "    slot = off + (j - 1u) * (b.y - 1u) + (i - 1u);\n"
    "    tc = float2(vio_sub(i, b.y, fi.x), vio_sub(j, b.z, fi.y));\n"
    "    return true;\n"
    "}\n";

/* Inner rings of a triangle with n inner subdivisions (equal spacing): ring k
 * has its corners at barycentric (1 - 4k/3n, 2k/3n, 2k/3n) and permutations,
 * n - 2k segments per side, a single centre point when n = 2k. */
static const char *metal_tess_emul_tri =
    "static inline bool vio_tess_point(float3 pic, const device uint4 *tp, uint pid, thread uint &slot, thread float3 &tc)\n"
    "{\n"
    "    uint4 a = tp[pid * 4u], b = tp[pid * 4u + 1u];\n"
    "    float4 f = as_type<float4>(tp[pid * 4u + 2u]);\n"
    "    if (a.y == 0u) return false;\n"
    "    float x = pic.x, y = pic.y, z = pic.z;\n"
    "    const float e = 1e-5;\n"
    "    tc = pic;\n"
    "    if (x > 1.0 - e) { slot = a.x; tc = float3(1.0, 0.0, 0.0); return true; }\n"
    "    if (y > 1.0 - e) { slot = a.x + 1u; tc = float3(0.0, 1.0, 0.0); return true; }\n"
    "    if (z > 1.0 - e) { slot = a.x + 2u; tc = float3(0.0, 0.0, 1.0); return true; }\n"
    "    uint k, off = a.x + 3u;\n"
    "    if (x < e) { if (!vio_idx(y, a.y, k) || k == 0u || k >= a.y) return false; slot = off + k - 1u; float s = vio_sub(k, a.y, f.x); tc = float3(0.0, s, 1.0 - s); return true; }\n"
    "    off += a.y - 1u;\n"
    "    if (y < e) { if (!vio_idx(z, a.z, k) || k == 0u || k >= a.z) return false; slot = off + k - 1u; float s = vio_sub(k, a.z, f.y); tc = float3(1.0 - s, 0.0, s); return true; }\n"
    "    off += a.z - 1u;\n"
    "    if (z < e) { if (!vio_idx(x, a.w, k) || k == 0u || k >= a.w) return false; slot = off + k - 1u; float s = vio_sub(k, a.w, f.z); tc = float3(s, 1.0 - s, 0.0); return true; }\n"
    "    off += a.w - 1u;\n"
    "    uint n = b.x;\n"
    "    float mn = min(x, min(y, z)), rk = mn * 3.0 * float(n) * 0.5;\n"
    "    uint r = uint(rint(rk));\n"
    "    if (abs(rk - float(r)) > 0.02 || r == 0u || 2u * r > n) return false;\n"
    "    for (uint q = 1u; q < r; q++) off += (n == 2u * q) ? 1u : 3u * (n - 2u * q);\n"
    "    uint len = n - 2u * r;\n"
    "    if (len == 0u) { slot = off; return true; }\n"
    "    uint ord; float t;\n"
    "    if (x <= y && x <= z) { ord = 0u; t = y; } else if (z <= y) { ord = 1u; t = x; } else { ord = 2u; t = z; }\n"
    "    float it = (t - 2.0 * float(r) / (3.0 * float(n))) * float(n);\n"
    "    uint ix = uint(rint(it));\n"
    "    if (abs(it - float(ix)) > 0.02 || ix > len) return false;\n"
    "    slot = off + (ord * len + ix) % (3u * len);\n"
    "    return true;\n"
    "}\n";

static char *metal_str_replace_all(const char *src, const char *from, const char *to)
{
    size_t fl = strlen(from), tl = strlen(to), n = 0;
    for (const char *p = strstr(src, from); p; p = strstr(p + fl, from)) n++;
    size_t len = strlen(src) + n * (tl > fl ? tl - fl : 0) + 1;
    char *out = (char *)malloc(len), *o = out;
    if (!out) return NULL;
    const char *p = src;
    for (const char *q = strstr(p, from); q; q = strstr(p, from)) {
        memcpy(o, p, (size_t)(q - p)); o += q - p;
        memcpy(o, to, tl); o += tl;
        p = q + fl;
    }
    strcpy(o, p);
    return out;
}

/* Rewrite the TES MSL (main0, a [[patch]] vertex function returning
 * main0_out) into the capture function and append vio_pass. `domain` is the
 * GLSL domain (VIO_MSL_DOMAIN_*, isolines already compiled as quads), spacing
 * VIO_MSL_SPACING_*. Returns malloc'd MSL or NULL. */
static char *metal_tess_emul_msl(const char *tes, int domain, int spacing, char **error_msg)
{
    const char *sig = strstr(tes, "vertex main0_out main0(");
    const char *pip = strstr(tes, "[[position_in_patch]]");
    const char *sb  = strstr(tes, "struct main0_out\n{");
    if (!sig || !pip || !sb || pip < sig) {
        if (error_msg) *error_msg = strdup("unexpected tessellation evaluation MSL");
        return NULL;
    }
    /* name and type of the position_in_patch parameter */
    const char *ne = pip;
    while (ne > sig && ne[-1] == ' ') ne--;
    const char *nb = ne;
    while (nb > sig && (isalnum((unsigned char)nb[-1]) || nb[-1] == '_')) nb--;
    char pname[64];
    size_t pl = (size_t)(ne - nb);
    if (pl == 0 || pl >= sizeof(pname)) { if (error_msg) *error_msg = strdup("position_in_patch parameter"); return NULL; }
    memcpy(pname, nb, pl);
    pname[pl] = '\0';
    const char *ptype = domain == VIO_MSL_DOMAIN_TRIANGLES ? "float3" : "float2";
    /* patch id parameter, added when the shader does not read gl_PrimitiveID */
    char pid[64] = "vio_pid";
    const char *pidp = strstr(sig, "[[patch_id]]");
    int add_pid = 1;
    if (pidp) {
        const char *e2 = pidp;
        while (e2 > sig && e2[-1] == ' ') e2--;
        const char *b2 = e2;
        while (b2 > sig && (isalnum((unsigned char)b2[-1]) || b2[-1] == '_')) b2--;
        if (e2 > b2 && (size_t)(e2 - b2) < sizeof(pid)) { memcpy(pid, b2, (size_t)(e2 - b2)); pid[e2 - b2] = '\0'; add_pid = 0; }
    }
    const char *body = strchr(pip, '{');
    if (!body) { if (error_msg) *error_msg = strdup("tessellation evaluation body"); return NULL; }
    const char *se = strstr(sb, "};");
    if (!se) { if (error_msg) *error_msg = strdup("main0_out struct"); return NULL; }
    int has_psize = strstr(sb, "[[point_size]]") && strstr(sb, "[[point_size]]") < se;

    size_t cap = strlen(tes) + 16384;
    char *out = (char *)malloc(cap);
    if (!out) return NULL;
    size_t o = 0;
#define EMIT(...) do { int w_ = snprintf(out + o, cap - o, __VA_ARGS__); if (w_ < 0 || (size_t)w_ >= cap - o) { free(out); return NULL; } o += (size_t)w_; } while (0)
    /* everything up to the signature's line, the helpers, the [[patch]] attribute */
    const char *line = sig;
    while (line > tes && line[-1] != '\n') line--;
    EMIT("%.*s", (int)(line - tes), tes);
    EMIT("#define VIO_SPACING %d\n%s%s", spacing, metal_tess_emul_helpers,
         domain == VIO_MSL_DOMAIN_ISOLINES ? metal_tess_emul_iso :
         domain == VIO_MSL_DOMAIN_QUADS ? metal_tess_emul_quad : metal_tess_emul_tri);
    EMIT("%.*s", (int)(sig - line), line);
    EMIT("vertex void main0(device main0_out* vio_cap [[buffer(%d)]], const device uint4* vio_tp [[buffer(%d)]], ",
         VIO_METAL_TESS_EMUL_CAP_INDEX, VIO_METAL_TESS_EMUL_INFO_INDEX);
    if (add_pid) EMIT("uint vio_pid [[patch_id]], ");
    /* the original parameters with position_in_patch renamed */
    const char *params = sig + strlen("vertex main0_out main0(");
    EMIT("%.*svio_pic %.*s", (int)(nb - params), params, (int)(body - pip), pip);
    EMIT("{\n    uint vio_slot;\n    %s %s;\n    if (!vio_tess_point(vio_pic, vio_tp, %s, vio_slot, %s)) return;\n",
         ptype, pname, pid, pname);
    /* body: every `return out;` stores the record */
    size_t blen = strlen(body + 1);
    char *rest = (char *)malloc(blen + 1);
    if (!rest) { free(out); return NULL; }
    memcpy(rest, body + 1, blen + 1);
    char *patched = metal_str_replace_all(rest, "return out;", "{ vio_cap[vio_slot] = out; return; }");
    free(rest);
    if (!patched) { free(out); return NULL; }
    if (o + strlen(patched) + 4096 > cap) {
        cap = o + strlen(patched) + 4096;
        char *g = (char *)realloc(out, cap);
        if (!g) { free(out); free(patched); return NULL; }
        out = g;
    }
    EMIT("%s", patched);
    free(patched);
    /* vio_pass: the captured record as the draw's vertex output. Lines must
     * not carry [[point_size]], points always do. */
    int points = domain != VIO_MSL_DOMAIN_ISOLINES;
    {
        const char *bs = sb + strlen("struct main0_out\n");
        size_t bl = (size_t)(se - bs);
        char *sbody = (char *)malloc(bl + 1);
        if (!sbody) { free(out); return NULL; }
        memcpy(sbody, bs, bl);
        sbody[bl] = '\0';
        char *clean = points ? strdup(sbody) : metal_str_replace_all(sbody, "[[point_size]]", "");
        free(sbody);
        if (!clean) { free(out); return NULL; }
        EMIT("\nstruct vio_pass_out\n%s", clean);
        free(clean);
    }
    if (points && !has_psize) EMIT("    float vio_psize [[point_size]];\n");
    EMIT("};\n\nvertex vio_pass_out vio_pass(const device main0_out* vio_cap [[buffer(%d)]], uint vio_vid [[vertex_id]])\n{\n"
         "    vio_pass_out o;\n    reinterpret_cast<thread main0_out&>(o) = vio_cap[vio_vid];\n%s    return o;\n}\n",
         VIO_METAL_TESS_EMUL_CAP_INDEX, (points && !has_psize) ? "    o.vio_psize = 1.0;\n" : "");
#undef EMIT
    return out;
}

#endif /* HAVE_SPIRV_CROSS */

#endif /* VIO_METAL_MSL_H */

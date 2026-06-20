/*
 * php-vio - PHP Video Input Output
 * Common type definitions shared between php-vio and backend extensions
 */

#ifndef VIO_TYPES_H
#define VIO_TYPES_H

#include <stddef.h>
#include <stdint.h>

/* ── Vertex format enums ──────────────────────────────────────────── */

typedef enum _vio_format {
    VIO_FLOAT1 = 1,
    VIO_FLOAT2 = 2,
    VIO_FLOAT3 = 3,
    VIO_FLOAT4 = 4,
    VIO_INT1   = 5,
    VIO_INT2   = 6,
    VIO_INT3   = 7,
    VIO_INT4   = 8,
    VIO_UINT1  = 9,
    VIO_UINT2  = 10,
    VIO_UINT3  = 11,
    VIO_UINT4  = 12,
} vio_format;

/* ── Topology ─────────────────────────────────────────────────────── */

typedef enum _vio_topology {
    VIO_TRIANGLES      = 0,
    VIO_TRIANGLE_STRIP = 1,
    VIO_TRIANGLE_FAN   = 2,
    VIO_LINES          = 3,
    VIO_LINE_STRIP     = 4,
    VIO_POINTS         = 5,
} vio_topology;

/* ── Cull mode ────────────────────────────────────────────────────── */

typedef enum _vio_cull_mode {
    VIO_CULL_NONE  = 0,
    VIO_CULL_BACK  = 1,
    VIO_CULL_FRONT = 2,
} vio_cull_mode;

/* ── Depth function ──────────────────────────────────────────────── */

typedef enum _vio_depth_func {
    VIO_DEPTH_LESS   = 0,
    VIO_DEPTH_LEQUAL = 1,
} vio_depth_func;

/* ── Blend mode ───────────────────────────────────────────────────── */

typedef enum _vio_blend_mode {
    VIO_BLEND_NONE     = 0,
    VIO_BLEND_ALPHA    = 1,
    VIO_BLEND_ADDITIVE = 2,
} vio_blend_mode;

/* ── Shader format ────────────────────────────────────────────────── */

typedef enum _vio_shader_format {
    VIO_SHADER_AUTO      = 0,
    VIO_SHADER_SPIRV     = 1,
    VIO_SHADER_GLSL      = 2,  /* GLSL -> SPIR-V -> cross-compile */
    VIO_SHADER_MSL       = 3,
    VIO_SHADER_GLSL_RAW  = 4,  /* GLSL -> compile directly (OpenGL only, no SPIR-V) */
    VIO_SHADER_HLSL      = 5,  /* HLSL -> compile directly (D3D11/D3D12 only) */
} vio_shader_format;

/* ── Texture filter / wrap ────────────────────────────────────────── */

typedef enum _vio_filter {
    VIO_FILTER_NEAREST = 0,
    VIO_FILTER_LINEAR  = 1,
} vio_filter;

typedef enum _vio_wrap {
    VIO_WRAP_REPEAT = 0,
    VIO_WRAP_CLAMP  = 1,
    VIO_WRAP_MIRROR = 2,
} vio_wrap;

/* ── Vertex usage hints ───────────────────────────────────────────── */

typedef enum _vio_usage {
    VIO_POSITION = 0,
    VIO_COLOR    = 1,
    VIO_TEXCOORD = 2,
    VIO_NORMAL   = 3,
    VIO_TANGENT  = 4,
} vio_usage;

/* ── Buffer type ──────────────────────────────────────────────────── */

typedef enum _vio_buffer_type {
    VIO_BUFFER_VERTEX  = 0,
    VIO_BUFFER_INDEX   = 1,
    VIO_BUFFER_UNIFORM = 2,
    VIO_BUFFER_STORAGE = 3,
} vio_buffer_type;

/* ── Uniform types (for set_uniform vtable) ──────────────────────── */

typedef enum _vio_uniform_type {
    VIO_UNIFORM_INT     = 0,
    VIO_UNIFORM_FLOAT   = 1,
    VIO_UNIFORM_VEC2    = 2,
    VIO_UNIFORM_VEC3    = 3,
    VIO_UNIFORM_VEC4    = 4,
    VIO_UNIFORM_MAT3    = 5,
    VIO_UNIFORM_MAT4    = 6,
} vio_uniform_type;

/* ── Feature flags ────────────────────────────────────────────────── */

typedef enum _vio_feature {
    VIO_FEATURE_COMPUTE      = 0,
    VIO_FEATURE_RAYTRACING   = 1,
    VIO_FEATURE_TESSELLATION = 2,
    VIO_FEATURE_GEOMETRY     = 3,
    VIO_FEATURE_MULTIVIEW    = 4,
    /* Backend has a wired 3D draw pipeline (create_pipeline / create_buffer /
     * create_texture / draw / draw_indexed all functional). Returns 0 when
     * the backend's 3D path is stubbed out, so callers can pick a different
     * backend instead of silently rendering black. 2D-only paths (vio_rect,
     * vio_sprite, vio_text) are independent and may work even when this
     * flag is 0 (e.g. Metal currently has 2D wired but no 3D). */
    VIO_FEATURE_3D_PIPELINE  = 5,
    /* Frame readback to RGBA on the CPU side. Used by vio_read_pixels,
     * vio_save_screenshot, vio_recorder_capture, vio_stream_push. Returns
     * 0 on backends where the readback path is stubbed (Vulkan, Metal). */
    VIO_FEATURE_READ_PIXELS  = 6,
    /* Hardware instancing — vio_draw_instanced. */
    VIO_FEATURE_INSTANCED_DRAW = 7,
    /* Offscreen render-target API (vio_render_target / vio_bind_render_target). */
    VIO_FEATURE_RENDER_TARGET      = 8,
    VIO_FEATURE_RENDER_TARGET_HDR  = 9,   /* 16F color attachment */
    VIO_FEATURE_RENDER_TARGET_DEPTH = 10, /* depth-only RT (shadow maps) */
    VIO_FEATURE_RENDER_TARGET_MSAA = 11,  /* multisampled RT */
    /* Cubemap textures — vio_cubemap / vio_bind_cubemap. */
    VIO_FEATURE_CUBEMAP            = 12,
    /* Pipeline depth-bias state. GL's glPolygonOffset; D3D/Vulkan/Metal
     * expose it via pipeline state. */
    VIO_FEATURE_DEPTH_BIAS         = 13,
    /* Scissor rectangle for 2D push/pop. */
    VIO_FEATURE_SCISSOR            = 14,
    /* GPU-side texture component swizzle (R8 → alpha for font atlases).
     * Backends without it (D3D11/12) have to CPU-expand to RGBA8. */
    VIO_FEATURE_TEXTURE_SWIZZLE    = 15,
    /* Backend ships its own 2D-batch renderer (vio_2d_<backend>_*). The
     * generic geo path in vio_2d.c is only invoked when this is 0. */
    VIO_FEATURE_NATIVE_2D_BATCH    = 16,
    /* OpenGL-specific feature flags filling out vio_gl_info(). All n/a for
     * non-GL backends. */
    VIO_FEATURE_DEBUG_OUTPUT       = 17,  /* GL 4.3 / KHR_debug */
    VIO_FEATURE_DSA                = 18,  /* GL 4.5 / ARB_direct_state_access */
    VIO_FEATURE_BUFFER_STORAGE     = 19,  /* GL 4.4 / ARB_buffer_storage */
    VIO_FEATURE_TEXTURE_STORAGE    = 20,  /* GL 4.2 / ARB_texture_storage */
    VIO_FEATURE_SEPARATE_SHADERS   = 21,  /* GL 4.1 / ARB_separate_shader_objects */
    /* 3D / volume textures (vio_texture_3d / sampler3D). Used by Fieldtracing
     * to store a baked Signed Distance Field volume on the GPU. Wired and
     * reported (1) on all backends: GL (glTexImage3D, core since 1.2), D3D11/
     * D3D12 (Texture3D), Metal (MTLTextureType3D), Vulkan (VK_IMAGE_TYPE_3D).
     * A backend that ever lacks the upload path reports 0 and vio_texture_3d
     * returns false there (graceful — the engine stays on the analytic trace
     * path). */
    VIO_FEATURE_TEXTURE_3D         = 22,
} vio_feature;

/* ── Input actions ────────────────────────────────────────────────── */

typedef enum _vio_action {
    VIO_RELEASE = 0,
    VIO_PRESS   = 1,
    VIO_REPEAT  = 2,
} vio_action;

/* ── Mouse buttons ────────────────────────────────────────────────── */

typedef enum _vio_mouse_button {
    VIO_MOUSE_LEFT   = 0,
    VIO_MOUSE_RIGHT  = 1,
    VIO_MOUSE_MIDDLE = 2,
} vio_mouse_button;

/* ── Configuration ────────────────────────────────────────────────── */

typedef struct _vio_config {
    int         width;
    int         height;
    const char *title;
    int         vsync;
    int         samples;    /* MSAA, 0 = off */
    int         debug;      /* Validation Layers / Debug Output */
    int         headless;   /* Offscreen rendering, no visible window */
} vio_config;

/* ── Descriptor structs ───────────────────────────────────────────── */

typedef struct _vio_vertex_attrib {
    int        location;
    vio_format format;
    vio_usage  usage;
} vio_vertex_attrib;

/* Mesh-level per-attribute description used by the create_mesh vtable slot.
 * Simpler than vio_vertex_attrib (no semantic usage), since mesh upload only
 * needs to know where each float-N attribute sits in the vertex layout. */
typedef struct _vio_mesh_attrib {
    int location;     /* glsl `layout(location = N)` */
    int components;   /* 1..4 floats */
    int offset;       /* bytes into the vertex */
} vio_mesh_attrib;

typedef struct _vio_pipeline_desc {
    void            *shader;
    vio_vertex_attrib *vertex_layout;
    int              vertex_attrib_count;
    vio_topology     topology;
    vio_cull_mode    cull_mode;
    int              depth_test;
    vio_depth_func   depth_func;    /* VIO_DEPTH_LESS (default 0) or VIO_DEPTH_LEQUAL */
    vio_blend_mode   blend;
    float            depth_bias;             /* constant depth bias (shadow mapping) */
    float            slope_scaled_depth_bias; /* slope-scaled bias (shadow mapping) */
    int              hdr_output;             /* 1 => render-target output format is
                                                FP16 (R16G16B16A16_FLOAT); 0 (default)
                                                => R8G8B8A8_UNORM. A PSO's RTV format
                                                must match the bound render target's
                                                format on D3D12, so a pipeline drawing
                                                into an hdr=true target must set this.
                                                Honored by D3D12 only this round; the
                                                other backends treat it as a no-op
                                                (they derive the color format from the
                                                bound target / render-pass instead). */
} vio_pipeline_desc;

typedef struct _vio_buffer_desc {
    vio_buffer_type type;
    const void     *data;
    size_t          size;
    int             binding;
} vio_buffer_desc;

typedef struct _vio_texture_desc {
    const void *data;
    size_t      data_size;
    int         width;
    int         height;
    int         depth;     /* 0 (default) => 2D texture; > 0 => 3D / volume texture
                              (data is width*height*depth*4 RGBA8, slices in +Z order) */
    vio_filter  filter;
    vio_wrap    wrap;
    int         mipmaps;
    int         single_channel; /* 1 => R8 (1 byte/px, e.g. font coverage atlas);
                                   0 => RGBA8. data must match the chosen format. */
} vio_texture_desc;

typedef struct _vio_shader_desc {
    const void       *vertex_data;
    size_t            vertex_size;
    const void       *fragment_data;
    size_t            fragment_size;
    vio_shader_format format;
} vio_shader_desc;

typedef struct _vio_draw_cmd {
    void *pipeline;
    void *vertex_buffer;
    int   vertex_count;
    int   first_vertex;
    int   instance_count;
    int   vertex_stride;    /* mesh stride in bytes (overrides pipeline stride if > 0) */
} vio_draw_cmd;

typedef struct _vio_draw_indexed_cmd {
    void *pipeline;
    void *vertex_buffer;
    void *index_buffer;
    int   index_count;
    int   first_index;
    int   vertex_offset;
    int   instance_count;
    int   vertex_stride;    /* mesh stride in bytes (overrides pipeline stride if > 0) */
} vio_draw_indexed_cmd;

typedef struct _vio_compute_cmd {
    void *pipeline;
    int   group_count_x;
    int   group_count_y;
    int   group_count_z;
} vio_compute_cmd;

#endif /* VIO_TYPES_H */

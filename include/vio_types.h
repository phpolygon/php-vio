/*
 * php-vio - PHP Video Input Output
 * Common type definitions shared between php-vio and backend extensions
 */

#ifndef VIO_TYPES_H
#define VIO_TYPES_H

/* Maximum colour attachments of one render target (MRT). */
#define VIO_MAX_COLOR_ATTACHMENTS 4

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

/* ── Comparison function (stencil test; GAP-PHASE5 Block 1) ───────── */
typedef enum _vio_compare_func {
    VIO_CMP_NEVER    = 0,
    VIO_CMP_LESS     = 1,
    VIO_CMP_EQUAL    = 2,
    VIO_CMP_LEQUAL   = 3,
    VIO_CMP_GREATER  = 4,
    VIO_CMP_NOTEQUAL = 5,
    VIO_CMP_GEQUAL   = 6,
    VIO_CMP_ALWAYS   = 7,
} vio_compare_func;

/* ── Stencil operation (what happens to the stencil value) ────────── */
typedef enum _vio_stencil_op {
    VIO_STENCIL_KEEP      = 0,
    VIO_STENCIL_ZERO      = 1,
    VIO_STENCIL_REPLACE   = 2,   /* write the reference value */
    VIO_STENCIL_INCR      = 3,   /* increment, clamp at 255 */
    VIO_STENCIL_DECR      = 4,   /* decrement, clamp at 0 */
    VIO_STENCIL_INVERT    = 5,
    VIO_STENCIL_INCR_WRAP = 6,
    VIO_STENCIL_DECR_WRAP = 7,
} vio_stencil_op;

/* ── Blend mode ───────────────────────────────────────────────────── */

typedef enum _vio_blend_mode {
    VIO_BLEND_NONE          = 0,
    VIO_BLEND_ALPHA         = 1,   /* src.a, 1-src.a */
    VIO_BLEND_ADDITIVE      = 2,   /* src.a, 1 */
    VIO_BLEND_PREMULTIPLIED = 3,   /* 1, 1-src.a (premultiplied alpha) */
    VIO_BLEND_MULTIPLY      = 4,   /* dst.rgb * src.rgb */
    VIO_BLEND_SCREEN        = 5,   /* 1, 1-src.rgb */
    VIO_BLEND_MIN           = 6,   /* min(src, dst) */
    VIO_BLEND_MAX           = 7,   /* max(src, dst) */
} vio_blend_mode;

/* ── Colour write mask (pipeline 'color_mask', bit flags) ─────────── */

#define VIO_COLOR_R    1
#define VIO_COLOR_G    2
#define VIO_COLOR_B    4
#define VIO_COLOR_A    8
#define VIO_COLOR_RGB  7
#define VIO_COLOR_RGBA 15

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
    /* Cubemap render targets: vio_render_target(['cube' => true]) + per-face
     * bind (vio_bind_render_target($ctx, $rt, $face)) + vio_render_target_cubemap.
     * The environment-probe path (render the sky into 6 faces, sample with
     * textureLod by roughness). 0 on backends without the face-attach path. */
    VIO_FEATURE_RENDER_TARGET_CUBE = 23,
    /* vio_generate_mipmaps() on textures / cubemaps / render targets. */
    VIO_FEATURE_MIPMAP_GEN         = 24,
    /* Multiple render targets: vio_render_target(['attachments' => [...]]) with
     * up to VIO_MAX_COLOR_ATTACHMENTS colour attachments, fragment
     * layout(location = n) out. */
    VIO_FEATURE_MRT                = 25,
    /* Compute storage images: vio_texture(['storage' => true]) +
     * vio_compute_bind_image() (GLSL image2D / image3D). */
    VIO_FEATURE_STORAGE_IMAGE      = 26,
    /* A storage buffer (SSBO / StructuredBuffer SRV) can be bound to the
     * GRAPHICS pipeline and read from the VERTEX stage — the primitive that
     * lets a vertex shader pull per-instance data via gl_InstanceIndex from a
     * compute-written buffer, with no GPU->CPU readback. Requires the vertex
     * stage to support storage-buffer reads: core everywhere on D3D11 (SM5
     * SRV-in-VS), D3D12, Vulkan and Metal; on OpenGL only from 4.3 (SSBOs are
     * core 4.3), so GL < 4.3 reports 0 and callers stay on the readback path.
     * Value 30 (leaves 23-29 free for unrelated features). */
    VIO_FEATURE_VERTEX_STORAGE     = 30,
    /* Stencil test / write through vio_pipeline(['stencil' => [...]]) — the
     * depth attachment carries 8 stencil bits and the pipeline state exposes
     * compare function, reference, masks and the three operations. */
    VIO_FEATURE_STENCIL            = 31,
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
    /* Backbuffers == how many frames the CPU may run ahead of the GPU.
     * 0 = backend default. Currently only D3D12 honours it (2 or 3); other
     * backends ignore it. See VIO_D3D12_FRAME_COUNT_DEFAULT. */
    int         frame_count;
} vio_config;

/* ── Descriptor structs ───────────────────────────────────────────── */

typedef struct _vio_vertex_attrib {
    int        location;
    vio_format format;
    vio_usage  usage;
    /* Matrix inputs (`layout(location = 3) in mat4 aModel`) are expanded into
     * one attribute per column at location + column. matrix_columns is the
     * column count of the source matrix (0 / 1 for plain vectors) and
     * matrix_column this attribute's column. HLSL from SPIRV-Cross names the
     * columns TEXCOORD{location}_{column}, so the D3D input layouts need both. */
    int        matrix_columns;
    int        matrix_column;
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
    int              depth_write;            /* 1 (default) => depth test writes; 0 => test only
                                                (sky, transparent geometry) */
    int              color_mask;             /* VIO_COLOR_* bits, default VIO_COLOR_RGBA */
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
    int              color_count;            /* MRT: number of colour outputs the PSO
                                                writes (0 => legacy single target from
                                                hdr_output). D3D12 needs the formats at
                                                PSO creation; Metal/GL/D3D11 derive them
                                                from the bound render target. */
    int              color_formats[VIO_MAX_COLOR_ATTACHMENTS]; /* vio_pixel_format */
    int              per_attachment;         /* 1 => attachment_blend[] / attachment_mask[]
                                                override blend / color_mask per colour
                                                attachment (MRT: alpha-blend colour while a
                                                data attachment stays untouched). */
    int              attachment_blend[VIO_MAX_COLOR_ATTACHMENTS]; /* vio_blend_mode per attachment */
    int              attachment_mask[VIO_MAX_COLOR_ATTACHMENTS];  /* VIO_COLOR_* bits per attachment */
    /* Stencil test (vio_pipeline(['stencil' => [...]]), VIO_FEATURE_STENCIL). The
     * same function / operations apply to front and back faces. The depth
     * attachments of every backend that reports the feature carry 8 stencil bits
     * (D24S8 on D3D, DEPTH24_STENCIL8 on GL); vio_clear resets them to 0. */
    int              stencil_enable;
    int              stencil_func;           /* vio_compare_func */
    int              stencil_ref;            /* 0..255 */
    int              stencil_read_mask;      /* 0..255 */
    int              stencil_write_mask;     /* 0..255 */
    int              stencil_pass_op;        /* vio_stencil_op: stencil + depth passed */
    int              stencil_fail_op;        /* vio_stencil_op: stencil test failed */
    int              stencil_depth_fail_op;  /* vio_stencil_op: stencil passed, depth failed */
} vio_pipeline_desc;

typedef struct _vio_buffer_desc {
    vio_buffer_type type;
    const void     *data;
    size_t          size;
    int             binding;
    int             stride;   /* element stride in bytes for STORAGE buffers
                                 (StructuredBuffer StructureByteStride). 0 (default)
                                 => treat as raw/4-byte elements. */
} vio_buffer_desc;

/* Colour attachment formats for render targets (vio_render_target
 * 'attachments' => [...]) and D3D12 PSO output formats. RGBA8 is the default
 * and the swapchain format; RGBA16F is what the legacy 'hdr' => true selects. */
typedef enum {
    VIO_FORMAT_RGBA8      = 0,
    VIO_FORMAT_RGBA16F    = 1,
    VIO_FORMAT_RGBA32F    = 2,
    VIO_FORMAT_R11G11B10F = 3,
    VIO_FORMAT_RG16F      = 4,
    VIO_FORMAT_R16F       = 5,
    VIO_FORMAT_R32F       = 6,
    VIO_FORMAT_R8         = 7,
} vio_pixel_format;

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
    int         storage;        /* 1 => usable as a compute storage image
                                   (image2D / image3D): Metal ShaderWrite usage,
                                   D3D UAV, GL image unit. Sampling still works. */
    int         anisotropy;     /* max anisotropic filtering, 0/1 = off (default),
                                   clamped to 16 and to the device limit. Only
                                   meaningful with filter = LINEAR. */
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
    int   async;   /* 1 => record into the open frame's command stream instead of
                      a fenced standalone submission; completion is observed via
                      compute_wait / read_buffer. Backends without an in-frame
                      path (or outside vio_begin/vio_end) run synchronously. */
} vio_compute_cmd;

#endif /* VIO_TYPES_H */

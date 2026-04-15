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

typedef struct _vio_pipeline_desc {
    void            *shader;
    vio_vertex_attrib *vertex_layout;
    int              vertex_attrib_count;
    vio_topology     topology;
    vio_cull_mode    cull_mode;
    int              depth_test;
    vio_blend_mode   blend;
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
    vio_filter  filter;
    vio_wrap    wrap;
    int         mipmaps;
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
} vio_draw_cmd;

typedef struct _vio_draw_indexed_cmd {
    void *pipeline;
    void *vertex_buffer;
    void *index_buffer;
    int   index_count;
    int   first_index;
    int   vertex_offset;
    int   instance_count;
} vio_draw_indexed_cmd;

typedef struct _vio_compute_cmd {
    void *pipeline;
    int   group_count_x;
    int   group_count_y;
    int   group_count_z;
} vio_compute_cmd;

#endif /* VIO_TYPES_H */

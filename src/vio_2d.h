/*
 * php-vio - 2D Rendering System
 */

#ifndef VIO_2D_H
#define VIO_2D_H

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "php.h"
#include "../include/vio_types.h"

/* Initial capacity — grows dynamically as needed */
#define VIO_2D_INITIAL_ITEMS     4096
#define VIO_2D_INITIAL_VERTICES  (VIO_2D_INITIAL_ITEMS * 6)
#define VIO_2D_MAX_VERTICES      (65536 * 6)  /* Fixed max for D3D vertex buffers */
#define VIO_2D_CIRCLE_SEGS   32
#define VIO_2D_MAX_TRANSFORM_STACK 32
#define VIO_2D_MAX_SCISSOR_STACK   32
#define VIO_2D_CORNER_SEGS   8  /* segments per rounded corner */

typedef enum _vio_2d_item_type {
    VIO_2D_RECT,
    VIO_2D_RECT_OUTLINE,
    VIO_2D_CIRCLE,
    VIO_2D_CIRCLE_OUTLINE,
    VIO_2D_LINE,
    VIO_2D_SPRITE,
    VIO_2D_TEXT,
    VIO_2D_ROUNDED_RECT,
    VIO_2D_ROUNDED_RECT_OUTLINE,
} vio_2d_item_type;

/* Single 2D vertex: position (2), texcoord (2), color (4) */
typedef struct _vio_2d_vertex {
    float x, y;
    float u, v;
    float r, g, b, a;
} vio_2d_vertex;

/* Scissor rectangle (pixel coordinates) */
typedef struct _vio_2d_scissor_rect {
    float x, y, w, h;
    int   enabled;
} vio_2d_scissor_rect;

typedef struct _vio_2d_item {
    vio_2d_item_type    type;
    float               z_order;
    unsigned int        texture_id;   /* 0 = no texture (shapes), GL texture name */
    void               *backend_texture; /* D3D11/D3D12 backend texture, NULL for shapes */
    int                 vertex_start;
    int                 vertex_count;
    vio_2d_scissor_rect scissor;      /* scissor state at time of push */
} vio_2d_item;

/* Backend type for 2D renderer */
typedef enum _vio_2d_backend {
    VIO_2D_BACKEND_NONE   = 0,
    VIO_2D_BACKEND_OPENGL = 1,
    VIO_2D_BACKEND_D3D11  = 2,
    VIO_2D_BACKEND_D3D12  = 3,
} vio_2d_backend;

typedef struct _vio_2d_state {
    vio_2d_vertex  *vertices;
    int             vertex_count;
    int             vertex_capacity;
    vio_2d_item    *items;
    int             item_count;
    int             item_capacity;
    float           projection[16];
    unsigned int    shader_shapes;
    unsigned int    shader_sprites;
    unsigned int    vao;
    unsigned int    vbo;
    int             vbo_capacity;     /* current GPU buffer size in vertices */
    int             initialized;
    vio_2d_backend  backend;
    int             width, height;
    int             fb_width, fb_height;  /* framebuffer pixels (for glScissor on HiDPI) */
    /* Transform stack: 2D affine matrices [a,b,c,d,e,f] */
    float           transform_stack[VIO_2D_MAX_TRANSFORM_STACK][6];
    int             transform_depth;
    /* Scissor stack */
    vio_2d_scissor_rect scissor_stack[VIO_2D_MAX_SCISSOR_STACK];
    int                 scissor_depth;
    /* D3D11/D3D12 state (opaque pointers to avoid header deps) */
    void           *d3d_state;
} vio_2d_state;

/* Initialize 2D state (called once after GL context is ready) */
int  vio_2d_init(vio_2d_state *state, int width, int height);

/* Shutdown and free 2D resources */
void vio_2d_shutdown(vio_2d_state *state);

/* Reset batch for new frame */
void vio_2d_begin(vio_2d_state *state);

/* Update projection when window resizes */
void vio_2d_set_size(vio_2d_state *state, int width, int height);

/* Add vertices to the batch, returns start index */
int  vio_2d_push_vertices(vio_2d_state *state, const vio_2d_vertex *verts, int count);

/* Add a draw item to the batch */
void vio_2d_push_item(vio_2d_state *state, vio_2d_item_type type, float z,
                       unsigned int texture_id, void *backend_texture,
                       int vert_start, int vert_count);

/* Flush all batched items sorted by z-order, then texture */
void vio_2d_flush(vio_2d_state *state);

/* Transform stack: push a 2D affine matrix [a,b,c,d,e,f] */
void vio_2d_push_transform(vio_2d_state *state, float a, float b, float c, float d, float e, float f);
void vio_2d_pop_transform(vio_2d_state *state);

/* Apply current transform to a vertex position in-place */
void vio_2d_apply_transform(vio_2d_state *state, float *x, float *y);

/* Scissor stack */
void vio_2d_push_scissor(vio_2d_state *state, float x, float y, float w, float h);
void vio_2d_pop_scissor(vio_2d_state *state);

/* Get current scissor rect (for stamping onto items) */
vio_2d_scissor_rect vio_2d_current_scissor(vio_2d_state *state);

/* Helper: unpack 0xAARRGGBB to floats */
static inline void vio_argb_unpack(uint32_t argb, float *r, float *g, float *b, float *a) {
    *a = ((argb >> 24) & 0xFF) / 255.0f;
    *r = ((argb >> 16) & 0xFF) / 255.0f;
    *g = ((argb >>  8) & 0xFF) / 255.0f;
    *b = ((argb      ) & 0xFF) / 255.0f;
}

#endif /* VIO_2D_H */

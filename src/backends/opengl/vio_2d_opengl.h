/*
 * php-vio - 2D rendering, OpenGL backend
 *
 * Mirrors the vio_2d_d3d11 / vio_2d_d3d12 split: the high-level batcher in
 * src/vio_2d.c remains backend-agnostic; this file holds the GL-specific
 * VAO/VBO/shader state plus the flush routine that turns the batched
 * vio_2d_state into glDrawArrays calls.
 */

#ifndef VIO_2D_OPENGL_H
#define VIO_2D_OPENGL_H

#ifdef HAVE_GLFW

#include "../../vio_2d.h"

typedef struct _vio_2d_opengl_state {
    unsigned int  shader_shapes;
    unsigned int  shader_sprites;
    unsigned int  vao;
    unsigned int  vbo;
    int           vbo_capacity;   /* current GPU buffer size in vertices */
} vio_2d_opengl_state;

int  vio_2d_opengl_init(vio_2d_opengl_state *state, int vertex_capacity);
void vio_2d_opengl_shutdown(vio_2d_opengl_state *state);
void vio_2d_opengl_flush(vio_2d_state *state);

#endif /* HAVE_GLFW */
#endif /* VIO_2D_OPENGL_H */

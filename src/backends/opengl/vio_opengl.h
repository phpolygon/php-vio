/*
 * php-vio - OpenGL 4.1 Core Backend
 */

#ifndef VIO_OPENGL_H
#define VIO_OPENGL_H

#include "../../../include/vio_backend.h"

void vio_backend_opengl_register(void);

/* Internal OpenGL state (shared between opengl source files) */
typedef struct _vio_opengl_state {
    unsigned int default_shader_program;
    unsigned int default_shader_pos_only;
    int          initialized;
    float        clear_r, clear_g, clear_b, clear_a;
} vio_opengl_state;

extern vio_opengl_state vio_gl;

/* Shader helpers */
unsigned int vio_opengl_compile_shader_source(const char *vert_src, const char *frag_src);
void vio_opengl_delete_program(unsigned int program);

#endif /* VIO_OPENGL_H */

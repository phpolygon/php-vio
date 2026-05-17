/*
 * php-vio - OpenGL Core Backend (runtime version detection, GL 3.3 floor)
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

    /* Runtime-detected capabilities. Filled by vio_opengl_setup_context()
     * after the window-system handed us a current GL context. The numeric
     * GLSL version (e.g. 330, 410, 460) matches GL_MAJOR_VERSION*100 +
     * GL_MINOR_VERSION*10 and is the version we ask SPIRV-Cross to emit. */
    int          gl_major;
    int          gl_minor;
    int          glsl_version;
} vio_opengl_state;

extern vio_opengl_state vio_gl;

/* Shader helpers */
unsigned int vio_opengl_compile_shader_source(const char *vert_src, const char *frag_src);
void vio_opengl_delete_program(unsigned int program);

/* Returns the GLSL version (e.g. 330) matching the active GL context.
 * Returns 330 if no context is initialized yet — that's the floor we ship. */
int vio_opengl_get_glsl_version(void);

#endif /* VIO_OPENGL_H */

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

    /* Per-feature capability cache. Filled once in vio_opengl_setup_context()
     * by combining a core-version check (gl_major / gl_minor) with an
     * extension-list lookup. opengl_supports_feature() reads these directly
     * so callers don't pay a query per call.
     *
     * Issue #3 part 2: each flag falls back to the equivalent ARB/KHR
     * extension so drivers that ship the feature outside of a higher core
     * version (typical on macOS 4.1 contexts with backports) still expose
     * it. The flags reflect actual driver capability, not aspirational core
     * level — a 3.3 context on a 4.6-capable driver only flips a flag if
     * the driver also exports the matching extension. */
    struct {
        int has_compute_shader;          /* core 4.3 / GL_ARB_compute_shader */
        int has_tessellation;            /* core 4.0 / GL_ARB_tessellation_shader */
        int has_separate_shaders;        /* core 4.1 / GL_ARB_separate_shader_objects */
        int has_debug_output;            /* core 4.3 / GL_KHR_debug */
        int has_dsa;                     /* core 4.5 / GL_ARB_direct_state_access */
        int has_buffer_storage;          /* core 4.4 / GL_ARB_buffer_storage */
        int has_texture_storage;         /* core 4.2 / GL_ARB_texture_storage */
        int has_texture_swizzle;         /* core 3.3 / GL_ARB_texture_swizzle */
    } caps;

    /* Cached extension list. NULL until setup; freed in shutdown. */
    int          extension_count;
    char       **extensions;
} vio_opengl_state;

extern vio_opengl_state vio_gl;

/* Shader helpers */
unsigned int vio_opengl_compile_shader_source(const char *vert_src, const char *frag_src);
void vio_opengl_delete_program(unsigned int program);

/* Returns the GLSL version (e.g. 330) matching the active GL context.
 * Returns 330 if no context is initialized yet — that's the floor we ship. */
int vio_opengl_get_glsl_version(void);

#endif /* VIO_OPENGL_H */

/*
 * php-vio - Context management internals
 */

#ifndef VIO_CONTEXT_H
#define VIO_CONTEXT_H

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "php.h"
#include "../include/vio_backend.h"
#include "vio_window.h"
#include "vio_input.h"
#include "vio_2d.h"

typedef struct _vio_context_object {
    const vio_backend *backend;
    vio_config         config;
    void              *surface;
#ifdef HAVE_GLFW
    GLFWwindow        *window;
#endif
    vio_input_state    input;
    vio_2d_state       state_2d;
    int                initialized;
    int                should_close;
    int                in_frame;         /* 1 between begin/end */
    /* Currently bound pipeline shader (0 = use default) */
    unsigned int       bound_shader_program;
    void              *bound_shader_object;  /* vio_shader_object* for uniform cbuffer */
    /* Headless FBO (OpenGL) */
    unsigned int       headless_fbo;
    unsigned int       headless_color_rb;
    unsigned int       headless_depth_rb;
    zend_object        std;
} vio_context_object;

extern zend_class_entry *vio_context_ce;

void vio_context_register(void);

static inline vio_context_object *vio_context_from_obj(zend_object *obj) {
    return (vio_context_object *)((char *)obj - XtOffsetOf(vio_context_object, std));
}

#define Z_VIO_CONTEXT_P(zv) vio_context_from_obj(Z_OBJ_P(zv))

#endif /* VIO_CONTEXT_H */

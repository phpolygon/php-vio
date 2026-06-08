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
    /* GLFWwindow* — typed as void* so this header doesn't depend on
     * HAVE_GLFW being visible to every translation unit that includes it.
     * Only translation units that actually drive GLFW need the cast. */
    void              *window;
    vio_input_state    input;
    vio_2d_state       state_2d;
    int                initialized;
    int                should_close;
    int                in_frame;         /* 1 between begin/end */
    /* Currently bound pipeline shader (0 = use default) */
    unsigned int       bound_shader_program;
    void              *bound_shader_object;  /* vio_shader_object* for uniform cbuffer */
    /* Backend-supplied offscreen render surface (OpenGL = FBO handle; other
     * backends leave 0 and use the swapchain backbuffer directly). */
    unsigned int       headless_fbo;
    /* Push/pop stack for vio_push_render_target / vio_pop_render_target
     * (Issue #4). Stores zval references to VioRenderTarget objects (or
     * NULL for "default target"); depth is capped to keep recursion
     * limited and to bound memory. */
    void              *rt_stack[8];   /* zval * — kept opaque to avoid php.h here */
    int                rt_stack_depth;
    /* Windowed-mode geometry captured before entering fullscreen/borderless so
     * vio_set_windowed can restore the exact pos/size. glfwSetWindowMonitor()
     * (real fullscreen) does not remember the prior windowed rect the way
     * glfwRestoreWindow() does for a maximized window, so we track it here. */
    int                saved_win_x, saved_win_y, saved_win_w, saved_win_h;
    int                has_saved_win_geometry;
    zend_object        std;
} vio_context_object;

extern zend_class_entry *vio_context_ce;

void vio_context_register(void);

static inline vio_context_object *vio_context_from_obj(zend_object *obj) {
    return (vio_context_object *)((char *)obj - XtOffsetOf(vio_context_object, std));
}

#define Z_VIO_CONTEXT_P(zv) vio_context_from_obj(Z_OBJ_P(zv))

#endif /* VIO_CONTEXT_H */

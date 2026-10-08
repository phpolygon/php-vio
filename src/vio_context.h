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
    /* vio_bind_buffer: uniform buffer per binding point (referenced), read by
     * the bound shader's uniform blocks at each draw. */
#define VIO_MAX_UBO_BINDINGS 16
    zend_object       *bound_ubo[VIO_MAX_UBO_BINDINGS];
    zend_object       *frag_storage[4];   /* vio_bind_fragment_storage_buffer (A15), held */
    /* Metal: last object bound per GL texture unit. Resolved against the
     * shader bound AT DRAW TIME (vio_flush_pending_textures), so vio_bind_texture
     * may precede vio_set_uniform('u_sampler', unit) and pipeline switches, the
     * way it does on OpenGL/D3D. Raw pointers — cleared by vio_begin/vio_end,
     * valid within one frame only. */
    void              *pending_tex_obj[16];
    int                pending_tex_kind[16];  /* 0 none, 1 VioTexture, 2 VioCubemap */
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
    /* vio_texture_index() table (VIO_FEATURE_BINDLESS): slot -> VioTexture,
     * each holding a reference until vio_destroy / free so a slot never points
     * at a freed texture. emalloc'd lazily (VIO_BINDLESS_MAX entries). */
    zend_object      **bindless;
    int                bindless_count;
    /* vio_texture_release_index: per slot the vio_begin count from which the
     * slot may be handed out again (0 = live or empty), and the free slots. */
    unsigned int      *bindless_retire;
    unsigned char     *bindless_kind;    /* VIO_BINDLESS_KIND_* per slot */
    int               *bindless_free;
    int                bindless_free_count;
    unsigned int       frame_no;   /* vio_begin calls on this context */
    /* vio_backend_info: how vio_create chose the backend ("explicit", "priority",
     * "score") and the ranked candidates of a scored 'auto' (NULL otherwise). */
    const char        *selected_by;
    zend_array        *candidates;
    zend_object        std;
} vio_context_object;

/* Drop the vio_texture_index() references (before the backend shuts down). */
void vio_context_bindless_clear(vio_context_object *ctx);
void vio_context_release_fragment_storage(vio_context_object *ctx);

extern zend_class_entry *vio_context_ce;

void vio_context_register(void);

static inline vio_context_object *vio_context_from_obj(zend_object *obj) {
    return (vio_context_object *)((char *)obj - XtOffsetOf(vio_context_object, std));
}

#define Z_VIO_CONTEXT_P(zv) vio_context_from_obj(Z_OBJ_P(zv))

#endif /* VIO_CONTEXT_H */

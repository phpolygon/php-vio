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
    /* vio_window_handle (include/vio_platform.h): whatever the active
     * platform created - a GLFWwindow* behind the GLFW platform, a native
     * window state behind the others. Opaque everywhere else. */
    void              *window;
    vio_input_state    input;
    vio_2d_state       state_2d;
    int                initialized;
    int                should_close;
    /* vio_set_window_size_limits, logical pixels; 0 = no limit. vio_set_window_size clamps to them. */
    int                size_min_w, size_min_h, size_max_w, size_max_h;
    int                in_frame;         /* 1 between begin/end */
    /* Currently bound pipeline shader (0 = use default) */
    unsigned int       bound_shader_program;
    void              *bound_shader_object;  /* vio_shader_object* for uniform cbuffer */
    /* vio_bind_buffer: uniform buffer per binding point (referenced), read by
     * the bound shader's uniform blocks at each draw. */
#define VIO_MAX_UBO_BINDINGS 16
    zend_object       *bound_ubo[VIO_MAX_UBO_BINDINGS];
    zend_object       *frag_storage[4];   /* vio_bind_fragment_storage_buffer (A15), held */
    zend_object       *fb_texture;        /* texture whose GLSL feedback map is bound (A15), held */
    /* Metal: last object bound per GL texture unit. Resolved against the
     * shader bound AT DRAW TIME (vio_flush_pending_textures), so vio_bind_texture
     * may precede vio_set_uniform('u_sampler', unit) and pipeline switches, the
     * way it does on OpenGL/D3D. Raw pointers — cleared by vio_begin/vio_end,
     * valid within one frame only. */
    void             **pending_tex_obj;      /* texture_units entries */
    int               *pending_tex_kind;     /* 0 none, 1 VioTexture, 2 VioCubemap */
    int                pending_tex_high;     /* highest bound unit + 1 (flush loops stop there) */
    int                pending_tex_cap;      /* entries allocated (grows to the highest unit bound) */
    /* Per-stage limits of the backend (vio_backend.texture_limits) and the
     * texture units the API offers: min(textures, samplers). */
    int                texture_limit;
    int                sampler_limit;
    int                texture_units;
    /* Backend-supplied offscreen render surface (OpenGL = FBO handle; other
     * backends leave 0 and use the swapchain backbuffer directly). */
    unsigned int       headless_fbo;
    /* vio_begin 'present' of the open / last frame: VIO_PRESENT_AUTO / _NO / _YES.
     * A _NO frame on OpenGL draws its screen part into the window's own back
     * buffer, never swapped, instead of the headless FBO vio_read_pixels reads. */
    int                frame_present;
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
    /* vio_upscale: passes, history and intermediates (php_vio.c), released
     * before the backend shuts down. */
    void              *upscale;
    /* vio_window (DESKTOP-APP-PLAN Phase 4): the context's further windows
     * (not referenced; each removes itself when freed), and the one the open
     * frame / readback renders into with what it displaced (frame_saved_*). */
#define VIO_MAX_WINDOWS 16
    struct _vio_window_object *windows[VIO_MAX_WINDOWS];
    int                window_count;
    struct _vio_window_object *frame_window;
    void              *frame_saved_window;
    unsigned int       frame_saved_fbo;
    vio_config         frame_saved_config;
    int                frame_saved_2d_w, frame_saved_2d_h, frame_saved_fb_w, frame_saved_fb_h;
    zend_object        std;
} vio_context_object;

/* VioWindow: a further window on a context's device. `view` is a context
 * record holding only the window-side state (platform window, input, close
 * flag, config sizes, backend pointer), so the window, input and size
 * functions run on it unchanged; GPU state stays in the owner. */
typedef struct _vio_window_object {
    vio_context_object  view;
    zend_object        *owner;       /* the VioContext, referenced while the window lives */
    void               *swapchain;   /* backend->create_swapchain */
    /* 'parent': another window of the context, or its main window (parent NULL,
     * has_parent 1); closed before its parent. 'modal': the parent's input is
     * blocked (input.blocked) while this window is open. */
    struct _vio_window_object *parent;
    int                 has_parent, modal;
    zend_object         std;
} vio_window_object;

extern zend_class_entry *vio_window_ce;

static inline vio_window_object *vio_window_from_obj(zend_object *obj) {
    return (vio_window_object *)((char *)obj - offsetof(vio_window_object, std));
}

#define Z_VIO_WINDOW_P(zv) vio_window_from_obj(Z_OBJ_P(zv))

/* Close the window: swapchain, platform window, the owner's reference. Idempotent. */
void vio_window_object_close(vio_window_object *w);
/* vio_destroy: close every window of the context. */
void vio_context_close_windows(vio_context_object *ctx);

/* Drop the vio_texture_index() references (before the backend shuts down). */
void vio_context_bindless_clear(vio_context_object *ctx);
/* Query the backend's texture / sampler limits and size the per-unit tables
 * (after the backend initialised). */
void vio_context_texture_units_init(vio_context_object *ctx);
/* Free the per-unit tables (the caller released the references they hold). */
void vio_context_texture_units_free(vio_context_object *ctx);
void vio_context_release_fragment_storage(vio_context_object *ctx);
void vio_upscale_release(vio_context_object *ctx);

extern zend_class_entry *vio_context_ce;

void vio_context_register(void);

static inline vio_context_object *vio_context_from_obj(zend_object *obj) {
    return (vio_context_object *)((char *)obj - offsetof(vio_context_object, std));
}

#define Z_VIO_CONTEXT_P(zv) vio_context_from_obj(Z_OBJ_P(zv))

static inline vio_context_object *vio_window_owner(vio_window_object *w) {
    return w->owner ? vio_context_from_obj(w->owner) : NULL;
}

#endif /* VIO_CONTEXT_H */

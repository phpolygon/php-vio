/*
 * php-vio - Context management implementation
 */

#include "vio_context.h"
#include "upscale/vio_upscaler.h"

zend_class_entry *vio_context_ce = NULL;
zend_class_entry *vio_window_ce = NULL;
static zend_object_handlers vio_context_handlers;
static zend_object_handlers vio_window_handlers;

static zend_object *vio_context_create_object(zend_class_entry *ce)
{
    vio_context_object *ctx = zend_object_alloc(sizeof(vio_context_object), ce);

    ctx->backend      = NULL;
    ctx->surface      = NULL;
    ctx->window       = NULL;
    ctx->initialized  = 0;
    ctx->should_close = 0;
    ctx->size_min_w = ctx->size_min_h = ctx->size_max_w = ctx->size_max_h = 0;
    ctx->in_frame     = 0;
    ctx->frame_present = -1;   /* VIO_PRESENT_AUTO */
    memset(&ctx->config, 0, sizeof(vio_config));
    vio_input_init(&ctx->input);
    memset(&ctx->state_2d, 0, sizeof(vio_2d_state));
    ctx->rt_stack_depth = 0;
    memset(ctx->rt_stack, 0, sizeof(ctx->rt_stack));
    ctx->saved_win_x = ctx->saved_win_y = 0;
    ctx->saved_win_w = ctx->saved_win_h = 0;
    ctx->has_saved_win_geometry = 0;
    ctx->selected_by = NULL;
    ctx->candidates = NULL;

    zend_object_std_init(&ctx->std, ce);
    object_properties_init(&ctx->std, ce);
    ctx->std.handlers = &vio_context_handlers;

    return &ctx->std;
}

void vio_context_bindless_clear(vio_context_object *ctx)
{
    if (!ctx->bindless) return;
    for (int i = 0; i < ctx->bindless_count; i++) {
        if (ctx->bindless[i]) OBJ_RELEASE(ctx->bindless[i]);
    }
    efree(ctx->bindless);
    ctx->bindless = NULL;
    ctx->bindless_count = 0;
    if (ctx->bindless_retire) { efree(ctx->bindless_retire); ctx->bindless_retire = NULL; }
    if (ctx->bindless_kind) { efree(ctx->bindless_kind); ctx->bindless_kind = NULL; }
    if (ctx->bindless_free) { efree(ctx->bindless_free); ctx->bindless_free = NULL; }
    ctx->bindless_free_count = 0;
}

/* Unbind and release the fragment-stage storage buffers (A15): the backend
 * keeps raw pointers to them, which must not outlive the context. */
void vio_context_release_fragment_storage(vio_context_object *ctx)
{
    for (int i = 0; i < 4; i++) {
        if (!ctx->frag_storage[i]) continue;
        if (ctx->initialized && ctx->backend && ctx->backend->bind_fragment_storage)
            ctx->backend->bind_fragment_storage(NULL, i);
        OBJ_RELEASE(ctx->frag_storage[i]);
        ctx->frag_storage[i] = NULL;
    }
    if (ctx->fb_texture) {
        if (ctx->initialized && ctx->backend && ctx->backend->bind_fragment_storage)
            ctx->backend->bind_fragment_storage(NULL, 3);
        OBJ_RELEASE(ctx->fb_texture);
        ctx->fb_texture = NULL;
    }
}

/* ── VioWindow ─────────────────────────────────────────────────────── */

static zend_object *vio_window_create_object(zend_class_entry *ce)
{
    vio_window_object *w = zend_object_alloc(sizeof(vio_window_object), ce);
    memset(&w->view, 0, sizeof(w->view));
    w->view.frame_present = -1;
    vio_input_init(&w->view.input);
    w->owner = NULL;
    w->swapchain = NULL;
    zend_object_std_init(&w->std, ce);
    object_properties_init(&w->std, ce);
    w->std.handlers = &vio_window_handlers;
    return &w->std;
}

void vio_window_object_close(vio_window_object *w)
{
    vio_context_object *ctx = vio_window_owner(w);
    if (!ctx) return;
    /* children first: a window system takes owned windows down with their owner */
    for (int i = ctx->window_count - 1; i >= 0; i--) {
        if (i < ctx->window_count && ctx->windows[i] != w && ctx->windows[i]->has_parent && ctx->windows[i]->parent == w) {
            vio_window_object_close(ctx->windows[i]);
            i = ctx->window_count;   /* the list changed */
        }
    }
    if (w->has_parent) {
        if (w->modal) {
            vio_input_state *pin = w->parent ? &w->parent->view.input : &ctx->input;
            if (pin->blocked > 0) pin->blocked--;
        }
        if (w->view.window && vio_plat()->set_parent) vio_plat()->set_parent(w->view.window, NULL, 0);
        w->has_parent = 0;
        w->parent = NULL;
    }
    for (int i = 0; i < ctx->window_count; i++) {
        if (ctx->windows[i] != w) continue;
        ctx->windows[i] = ctx->windows[--ctx->window_count];
        ctx->windows[ctx->window_count] = NULL;
        break;
    }
    if (w->swapchain && ctx->initialized && ctx->backend && ctx->backend->destroy_swapchain) {
        ctx->backend->destroy_swapchain(w->swapchain);
    }
    w->swapchain = NULL;
    if (w->view.window) {
        vio_window_destroy(w->view.window);
        w->view.window = NULL;
        /* OpenGL: destroying the window's context may have unbound the main one */
        if (ctx->initialized && ctx->window && ctx->backend && ctx->backend->swapchain_fbo && vio_plat()->gl_make_current)
            vio_plat()->gl_make_current(ctx->window);
    }
    w->view.initialized = 0;
    w->view.should_close = 1;
    zend_object *owner = w->owner;
    w->owner = NULL;
    OBJ_RELEASE(owner);
}

void vio_context_close_windows(vio_context_object *ctx)
{
    while (ctx->window_count > 0) {
        vio_window_object_close(ctx->windows[ctx->window_count - 1]);
    }
}

static void vio_window_free_object(zend_object *obj)
{
    vio_window_object *w = vio_window_from_obj(obj);
    vio_window_object_close(w);
    vio_input_shutdown(&w->view.input);
    zend_object_std_dtor(&w->std);
}

static void vio_context_free_object(zend_object *obj)
{
    vio_context_object *ctx = vio_context_from_obj(obj);
    vio_context_release_fragment_storage(ctx);
    vio_upscale_release(ctx);
    /* The table's textures free their GPU objects through the backend: before it shuts down. */
    vio_context_bindless_clear(ctx);
    if (ctx->candidates) {
        if (GC_DELREF(ctx->candidates) == 0) zend_array_destroy(ctx->candidates);
        ctx->candidates = NULL;
    }
    for (int i = 0; i < VIO_MAX_UBO_BINDINGS; i++) {
        if (ctx->bound_ubo[i]) { OBJ_RELEASE(ctx->bound_ubo[i]); ctx->bound_ubo[i] = NULL; }
    }

    /* Backend / surface / window cleanup runs only when the ctx is still
     * initialised — vio_destroy may have already run these, in which case
     * ctx->initialized is 0 and they would double-free. */
    if (ctx->initialized && ctx->backend) {
        if (ctx->surface && ctx->backend->destroy_surface) {
            ctx->backend->destroy_surface(ctx->surface);
        }
        /* Destroy the 2D renderer's backend resources while the backend device
         * is still alive (Vulkan creates its 2D pipelines/buffer/layouts against
         * vio_vk.device, which backend->shutdown() destroys). Idempotent, so the
         * unconditional call below — needed when vio_destroy already ran the
         * backend teardown — is a harmless no-op. */
        vio_2d_shutdown(&ctx->state_2d);
        vio_upscaler_sweep(ctx->backend);   /* upscaler contexts hold device objects */
        if (ctx->backend->shutdown) {
            ctx->backend->shutdown();
        }
        if (ctx->window) {
            vio_window_destroy(ctx->window);
            ctx->window = NULL;
        }
        ctx->initialized = 0;
    }

    /* Per-ctx PHP-heap buffers (2D vertex/item arrays, input callbacks)
     * are allocated unconditionally in vio_create and must be freed on
     * every destruction path. Previously these were inside the
     * initialised-only block, so calling vio_destroy() before PHP's GC
     * fired the free handler would skip them and leak ~1 MiB per ctx. */
    vio_input_shutdown(&ctx->input);
    vio_2d_shutdown(&ctx->state_2d);

    /* Drain the render-target push/pop stack so any held zvals don't leak. */
    for (int i = 0; i < ctx->rt_stack_depth; i++) {
        zval *slot = (zval *)ctx->rt_stack[i];
        if (slot) {
            zval_ptr_dtor(slot);
            efree(slot);
        }
    }
    ctx->rt_stack_depth = 0;

    zend_object_std_dtor(&ctx->std);
}

void vio_context_register(void)
{
    zend_class_entry ce;

    INIT_CLASS_ENTRY(ce, "VioContext", NULL);
    vio_context_ce = zend_register_internal_class(&ce);
    vio_context_ce->ce_flags |= ZEND_ACC_FINAL | ZEND_ACC_NO_DYNAMIC_PROPERTIES | ZEND_ACC_NOT_SERIALIZABLE;
    vio_context_ce->create_object = vio_context_create_object;

    memcpy(&vio_context_handlers, &std_object_handlers, sizeof(zend_object_handlers));
    vio_context_handlers.offset   = XtOffsetOf(vio_context_object, std);
    vio_context_handlers.free_obj = vio_context_free_object;
    vio_context_handlers.clone_obj = NULL;

    INIT_CLASS_ENTRY(ce, "VioWindow", NULL);
    vio_window_ce = zend_register_internal_class(&ce);
    vio_window_ce->ce_flags |= ZEND_ACC_FINAL | ZEND_ACC_NO_DYNAMIC_PROPERTIES | ZEND_ACC_NOT_SERIALIZABLE;
    vio_window_ce->create_object = vio_window_create_object;

    memcpy(&vio_window_handlers, &std_object_handlers, sizeof(zend_object_handlers));
    vio_window_handlers.offset   = XtOffsetOf(vio_window_object, std);
    vio_window_handlers.free_obj = vio_window_free_object;
    vio_window_handlers.clone_obj = NULL;
}

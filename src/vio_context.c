/*
 * php-vio - Context management implementation
 */

#include "vio_context.h"

zend_class_entry *vio_context_ce = NULL;
static zend_object_handlers vio_context_handlers;

static zend_object *vio_context_create_object(zend_class_entry *ce)
{
    vio_context_object *ctx = zend_object_alloc(sizeof(vio_context_object), ce);

    ctx->backend      = NULL;
    ctx->surface      = NULL;
#ifdef HAVE_GLFW
    ctx->window       = NULL;
#endif
    ctx->initialized  = 0;
    ctx->should_close = 0;
    ctx->in_frame     = 0;
    memset(&ctx->config, 0, sizeof(vio_config));
    vio_input_init(&ctx->input);
    memset(&ctx->state_2d, 0, sizeof(vio_2d_state));

    zend_object_std_init(&ctx->std, ce);
    object_properties_init(&ctx->std, ce);
    ctx->std.handlers = &vio_context_handlers;

    return &ctx->std;
}

static void vio_context_free_object(zend_object *obj)
{
    vio_context_object *ctx = vio_context_from_obj(obj);

    if (ctx->initialized && ctx->backend) {
        if (ctx->surface && ctx->backend->destroy_surface) {
            ctx->backend->destroy_surface(ctx->surface);
        }
        if (ctx->backend->shutdown) {
            ctx->backend->shutdown();
        }
    vio_input_shutdown(&ctx->input);
        vio_2d_shutdown(&ctx->state_2d);
#ifdef HAVE_GLFW
        if (ctx->window) {
            vio_window_destroy(ctx->window);
            ctx->window = NULL;
        }
#endif
        ctx->initialized = 0;
    }

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
}

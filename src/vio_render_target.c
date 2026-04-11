/*
 * php-vio - Render target (offscreen FBO) Zend object
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "vio_render_target.h"

#ifdef HAVE_GLFW
#include <glad/glad.h>
#endif

zend_class_entry *vio_render_target_ce = NULL;
static zend_object_handlers vio_render_target_handlers;

static zend_object *vio_render_target_create_object(zend_class_entry *ce)
{
    vio_render_target_object *rt = zend_object_alloc(sizeof(vio_render_target_object), ce);

    rt->fbo           = 0;
    rt->color_texture = 0;
    rt->depth_texture = 0;
    rt->width         = 0;
    rt->height        = 0;
    rt->depth_only    = 0;
    rt->valid         = 0;

    zend_object_std_init(&rt->std, ce);
    object_properties_init(&rt->std, ce);
    rt->std.handlers = &vio_render_target_handlers;

    return &rt->std;
}

static void vio_render_target_free_object(zend_object *obj)
{
    vio_render_target_object *rt = vio_render_target_from_obj(obj);

#ifdef HAVE_GLFW
    if (rt->fbo) {
        glDeleteFramebuffers(1, &rt->fbo);
        rt->fbo = 0;
    }
    if (rt->color_texture) {
        glDeleteTextures(1, &rt->color_texture);
        rt->color_texture = 0;
    }
    if (rt->depth_texture) {
        glDeleteTextures(1, &rt->depth_texture);
        rt->depth_texture = 0;
    }
#endif

    zend_object_std_dtor(&rt->std);
}

void vio_render_target_register(void)
{
    zend_class_entry ce;

    INIT_CLASS_ENTRY(ce, "VioRenderTarget", NULL);
    vio_render_target_ce = zend_register_internal_class(&ce);
    vio_render_target_ce->ce_flags |= ZEND_ACC_FINAL | ZEND_ACC_NO_DYNAMIC_PROPERTIES | ZEND_ACC_NOT_SERIALIZABLE;
    vio_render_target_ce->create_object = vio_render_target_create_object;

    memcpy(&vio_render_target_handlers, &std_object_handlers, sizeof(zend_object_handlers));
    vio_render_target_handlers.offset   = XtOffsetOf(vio_render_target_object, std);
    vio_render_target_handlers.free_obj = vio_render_target_free_object;
    vio_render_target_handlers.clone_obj = NULL;
}

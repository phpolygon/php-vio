/*
 * php-vio - Render target (offscreen FBO) Zend object
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "vio_render_target.h"
#include "../include/vio_backend.h"

zend_class_entry *vio_render_target_ce = NULL;
static zend_object_handlers vio_render_target_handlers;

static zend_object *vio_render_target_create_object(zend_class_entry *ce)
{
    vio_render_target_object *rt = zend_object_alloc(sizeof(vio_render_target_object), ce);

    rt->fbo           = 0;
    rt->color_texture = 0;
    rt->depth_texture = 0;
    rt->d3d11_rtv       = NULL;
    rt->d3d11_dsv       = NULL;
    rt->d3d11_color_tex = NULL;
    rt->d3d11_depth_tex = NULL;
    rt->d3d11_depth_srv = NULL;
    rt->d3d11_color_srv = NULL;
    rt->d3d11_color_backend_texture = NULL;
    rt->d3d11_depth_backend_texture = NULL;
    rt->d3d12_color_resource = NULL;
    rt->d3d12_depth_resource = NULL;
    rt->d3d12_rtv_heap       = NULL;
    rt->d3d12_dsv_heap       = NULL;
    rt->d3d12_color_backend_texture = NULL;
    rt->d3d12_depth_backend_texture = NULL;
    rt->metal_color_texture = NULL;
    rt->metal_depth_texture = NULL;
    rt->width         = 0;
    rt->height        = 0;
    rt->depth_only    = 0;
    rt->valid         = 0;
    rt->backend_type  = VIO_RT_BACKEND_NONE;
    rt->backend       = NULL;

    zend_object_std_init(&rt->std, ce);
    object_properties_init(&rt->std, ce);
    rt->std.handlers = &vio_render_target_handlers;

    return &rt->std;
}

static void vio_render_target_free_object(zend_object *obj)
{
    vio_render_target_object *rt = vio_render_target_from_obj(obj);

    if (rt->backend && rt->backend->destroy_render_target) {
        rt->backend->destroy_render_target(rt);
    }

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

/*
 * php-vio - Texture management implementation
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "vio_texture.h"

#ifdef HAVE_GLFW
#include <glad/glad.h>
#endif

#ifdef HAVE_METAL
#include "backends/metal/vio_metal.h"
#endif

zend_class_entry *vio_texture_ce = NULL;
static zend_object_handlers vio_texture_handlers;

static zend_object *vio_texture_create_object(zend_class_entry *ce)
{
    vio_texture_object *tex = zend_object_alloc(sizeof(vio_texture_object), ce);

    tex->texture_id = 0;
    tex->width      = 0;
    tex->height     = 0;
    tex->channels   = 0;
    tex->filter     = VIO_FILTER_LINEAR;
    tex->wrap       = VIO_WRAP_REPEAT;
    tex->valid      = 0;
    tex->borrowed   = 0;

    zend_object_std_init(&tex->std, ce);
    object_properties_init(&tex->std, ce);
    tex->std.handlers = &vio_texture_handlers;

    return &tex->std;
}

static void vio_texture_free_object(zend_object *obj)
{
    vio_texture_object *tex = vio_texture_from_obj(obj);

#ifdef HAVE_GLFW
    if (tex->texture_id && !tex->borrowed) {
        glDeleteTextures(1, &tex->texture_id);
        tex->texture_id = 0;
    }
#endif
#ifdef HAVE_METAL
    if (tex->texture_id && !tex->borrowed) {
        vio_metal_delete_texture(tex->texture_id);
        tex->texture_id = 0;
    }
#endif

    zend_object_std_dtor(&tex->std);
}

void vio_texture_register(void)
{
    zend_class_entry ce;

    INIT_CLASS_ENTRY(ce, "VioTexture", NULL);
    vio_texture_ce = zend_register_internal_class(&ce);
    vio_texture_ce->ce_flags |= ZEND_ACC_FINAL | ZEND_ACC_NO_DYNAMIC_PROPERTIES | ZEND_ACC_NOT_SERIALIZABLE;
    vio_texture_ce->create_object = vio_texture_create_object;

    memcpy(&vio_texture_handlers, &std_object_handlers, sizeof(zend_object_handlers));
    vio_texture_handlers.offset   = XtOffsetOf(vio_texture_object, std);
    vio_texture_handlers.free_obj = vio_texture_free_object;
    vio_texture_handlers.clone_obj = NULL;
}

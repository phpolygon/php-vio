/*
 * php-vio - Font management implementation
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "vio_font.h"

#ifdef HAVE_GLFW
#include <glad/glad.h>
#endif

zend_class_entry *vio_font_ce = NULL;
static zend_object_handlers vio_font_handlers;

static zend_object *vio_font_create_object(zend_class_entry *ce)
{
    vio_font_object *font = zend_object_alloc(sizeof(vio_font_object), ce);

    font->atlas_texture = 0;
    font->font_size     = 16.0f;
    font->atlas_w       = VIO_FONT_ATLAS_SIZE;
    font->atlas_h       = VIO_FONT_ATLAS_SIZE;
    font->ttf_data      = NULL;
    font->valid         = 0;
    memset(font->char_data, 0, sizeof(font->char_data));

    zend_object_std_init(&font->std, ce);
    object_properties_init(&font->std, ce);
    font->std.handlers = &vio_font_handlers;

    return &font->std;
}

static void vio_font_free_object(zend_object *obj)
{
    vio_font_object *font = vio_font_from_obj(obj);

#ifdef HAVE_GLFW
    if (font->atlas_texture) {
        glDeleteTextures(1, &font->atlas_texture);
        font->atlas_texture = 0;
    }
#endif

    if (font->ttf_data) {
        efree(font->ttf_data);
        font->ttf_data = NULL;
    }

    zend_object_std_dtor(&font->std);
}

void vio_font_register(void)
{
    zend_class_entry ce;

    INIT_CLASS_ENTRY(ce, "VioFont", NULL);
    vio_font_ce = zend_register_internal_class(&ce);
    vio_font_ce->ce_flags |= ZEND_ACC_FINAL | ZEND_ACC_NO_DYNAMIC_PROPERTIES | ZEND_ACC_NOT_SERIALIZABLE;
    vio_font_ce->create_object = vio_font_create_object;

    memcpy(&vio_font_handlers, &std_object_handlers, sizeof(zend_object_handlers));
    vio_font_handlers.offset   = XtOffsetOf(vio_font_object, std);
    vio_font_handlers.free_obj = vio_font_free_object;
    vio_font_handlers.clone_obj = NULL;
}

/*
 * php-vio - Font management implementation
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

/* Include stb_truetype BEFORE php.h to avoid macro conflicts on Windows */
#include "../vendor/stb/stb_truetype.h"

#include "vio_font.h"
#include "../include/vio_backend.h"

#ifdef HAVE_GLFW
#include <glad/glad.h>
#endif

zend_class_entry *vio_font_ce = NULL;
static zend_object_handlers vio_font_handlers;

static zend_object *vio_font_create_object(zend_class_entry *ce)
{
    vio_font_object *font = zend_object_alloc(sizeof(vio_font_object), ce);

    font->atlas_texture         = 0;
    font->atlas_backend_texture = NULL;
    font->font_size             = 16.0f;
    font->atlas_w               = VIO_FONT_ATLAS_SIZE;
    font->atlas_h               = VIO_FONT_ATLAS_SIZE;
    font->ttf_data              = NULL;
    font->valid                 = 0;
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

    if (font->atlas_backend_texture) {
        const vio_backend *b = NULL;
#ifdef HAVE_D3D11
        b = vio_find_backend("d3d11");
#endif
#ifdef HAVE_D3D12
        if (!b) b = vio_find_backend("d3d12");
#endif
        if (b && b->destroy_texture) {
            b->destroy_texture(font->atlas_backend_texture);
        }
        font->atlas_backend_texture = NULL;
    }

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

int vio_font_bake_bitmap(const unsigned char *data, int offset, float pixel_height,
                         unsigned char *pixels, int pw, int ph,
                         int first_char, int num_chars,
                         vio_stbtt_bakedchar *chardata)
{
    /* vio_stbtt_bakedchar and stbtt_bakedchar have identical layout */
    return stbtt_BakeFontBitmap(data, offset, pixel_height,
                                pixels, pw, ph,
                                first_char, num_chars,
                                (stbtt_bakedchar *)chardata);
}

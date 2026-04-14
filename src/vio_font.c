/*
 * php-vio - Font management implementation
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

/* Include stb_truetype BEFORE php.h to avoid macro conflicts on Windows */
#include "../vendor/stb/stb_truetype.h"

#include "vio_font.h"

#ifdef HAVE_GLFW
#include <glad/glad.h>
#endif

#ifdef HAVE_METAL
#include "backends/metal/vio_metal.h"
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
    font->ttf_len       = 0;
    font->valid         = 0;
    zend_hash_init(&font->glyph_map, 512, NULL, NULL, 0);

    zend_object_std_init(&font->std, ce);
    object_properties_init(&font->std, ce);
    font->std.handlers = &vio_font_handlers;

    return &font->std;
}

static void vio_font_free_object(zend_object *obj)
{
    vio_font_object *font = vio_font_from_obj(obj);

#ifdef HAVE_GLFW
    if (font->atlas_texture && glDeleteTextures) {
        glDeleteTextures(1, &font->atlas_texture);
        font->atlas_texture = 0;
    }
#endif
#ifdef HAVE_METAL
    if (font->atlas_texture) {
        vio_metal_delete_texture(font->atlas_texture);
        font->atlas_texture = 0;
    }
#endif

    zend_hash_destroy(&font->glyph_map);

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

int vio_font_pack_atlas(vio_font_object *font, unsigned char *atlas_bitmap, int atlas_size)
{
    stbtt_pack_context spc;
    if (!stbtt_PackBegin(&spc, atlas_bitmap, atlas_size, atlas_size, 0, 1, NULL)) {
        return 0;
    }
    stbtt_PackSetOversampling(&spc, 1, 1);
    stbtt_PackSetSkipMissingCodepoints(&spc, 1);

    /* Build pack_range array + allocate chardata per range */
    stbtt_pack_range ranges[VIO_FONT_NUM_RANGES];
    stbtt_packedchar *range_chardata[VIO_FONT_NUM_RANGES];

    for (int r = 0; r < VIO_FONT_NUM_RANGES; r++) {
        range_chardata[r] = ecalloc(vio_font_ranges[r].num_chars, sizeof(stbtt_packedchar));
        ranges[r].font_size                       = font->font_size;
        ranges[r].first_unicode_codepoint_in_range = vio_font_ranges[r].first_codepoint;
        ranges[r].array_of_unicode_codepoints      = NULL;
        ranges[r].num_chars                        = vio_font_ranges[r].num_chars;
        ranges[r].chardata_for_range               = range_chardata[r];
        ranges[r].h_oversample                     = 1;
        ranges[r].v_oversample                     = 1;
    }

    int pack_ok = stbtt_PackFontRanges(&spc, font->ttf_data, 0, ranges, VIO_FONT_NUM_RANGES);
    stbtt_PackEnd(&spc);

    /* Populate glyph hashmap from packed ranges */
    for (int r = 0; r < VIO_FONT_NUM_RANGES; r++) {
        int first = vio_font_ranges[r].first_codepoint;
        int count = vio_font_ranges[r].num_chars;
        for (int c = 0; c < count; c++) {
            stbtt_packedchar *pc = &range_chardata[r][c];
            /* Skip glyphs that were not packed (zero-size AND no advance, e.g. missing glyph).
             * Keep zero-size glyphs with xadvance > 0 (e.g. space). */
            if (pc->x1 == pc->x0 && pc->y1 == pc->y0 && pc->xadvance == 0.0f) continue;
            zend_long cp = (zend_long)(first + c);
            vio_stbtt_packedchar vpc;
            vpc.x0 = pc->x0; vpc.y0 = pc->y0;
            vpc.x1 = pc->x1; vpc.y1 = pc->y1;
            vpc.xoff = pc->xoff; vpc.yoff = pc->yoff;
            vpc.xadvance = pc->xadvance;
            vpc.xoff2 = pc->xoff2; vpc.yoff2 = pc->yoff2;
            zval zv;
            ZVAL_STRINGL(&zv, (char *)&vpc, sizeof(vio_stbtt_packedchar));
            zend_hash_index_update(&font->glyph_map, cp, &zv);
        }
        efree(range_chardata[r]);
    }

    return pack_ok;
}

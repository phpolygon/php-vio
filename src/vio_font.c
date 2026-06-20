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
    font->ttf_len               = 0;
    font->valid                 = 0;
    font->backend               = NULL;
    zend_hash_init(&font->glyph_map, 512, NULL, NULL, 0);

    zend_object_std_init(&font->std, ce);
    object_properties_init(&font->std, ce);
    font->std.handlers = &vio_font_handlers;

    return &font->std;
}

static void vio_font_free_object(zend_object *obj)
{
    vio_font_object *font = vio_font_from_obj(obj);

    if (font->backend && font->backend->destroy_font_atlas) {
        font->backend->destroy_font_atlas(font);
    }

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

int vio_font_pack_atlas_raw(const unsigned char *ttf_data, float font_size,
                            unsigned char *atlas_bitmap, int atlas_size,
                            vio_font_packed_glyph **out_glyphs, int *out_count)
{
    *out_glyphs = NULL;
    *out_count  = 0;

    stbtt_pack_context spc;
    if (!stbtt_PackBegin(&spc, atlas_bitmap, atlas_size, atlas_size, 0, 1, NULL)) {
        return 0;
    }
    stbtt_PackSetOversampling(&spc, 1, 1);
    stbtt_PackSetSkipMissingCodepoints(&spc, 1);

    /* Build pack_range array + allocate chardata per range. This path may run
     * on a worker thread, so it uses libc malloc/free (NOT emalloc/efree —
     * the Zend allocator is per-request and not thread-safe). */
    stbtt_pack_range ranges[VIO_FONT_NUM_RANGES];
    stbtt_packedchar *range_chardata[VIO_FONT_NUM_RANGES];
    int total_slots = 0;

    for (int r = 0; r < VIO_FONT_NUM_RANGES; r++) {
        range_chardata[r] = (stbtt_packedchar *)calloc(vio_font_ranges[r].num_chars, sizeof(stbtt_packedchar));
        if (!range_chardata[r]) {
            for (int k = 0; k < r; k++) free(range_chardata[k]);
            stbtt_PackEnd(&spc);
            return 0;
        }
        total_slots += vio_font_ranges[r].num_chars;
        ranges[r].font_size                       = font_size;
        ranges[r].first_unicode_codepoint_in_range = vio_font_ranges[r].first_codepoint;
        ranges[r].array_of_unicode_codepoints      = NULL;
        ranges[r].num_chars                        = vio_font_ranges[r].num_chars;
        ranges[r].chardata_for_range               = range_chardata[r];
        ranges[r].h_oversample                     = 1;
        ranges[r].v_oversample                     = 1;
    }

    int pack_ok = stbtt_PackFontRanges(&spc, (unsigned char *)ttf_data, 0, ranges, VIO_FONT_NUM_RANGES);
    stbtt_PackEnd(&spc);

    /* Collapse the per-range chardata into one flat array of (codepoint, pc)
     * pairs, skipping unpacked glyphs. total_slots is the upper bound. */
    vio_font_packed_glyph *glyphs = (vio_font_packed_glyph *)malloc((size_t)total_slots * sizeof(vio_font_packed_glyph));
    int n = 0;
    if (glyphs) {
        for (int r = 0; r < VIO_FONT_NUM_RANGES; r++) {
            int first = vio_font_ranges[r].first_codepoint;
            int count = vio_font_ranges[r].num_chars;
            for (int c = 0; c < count; c++) {
                stbtt_packedchar *pc = &range_chardata[r][c];
                /* Skip glyphs that were not packed (zero-size AND no advance,
                 * e.g. missing glyph). Keep zero-size glyphs with xadvance > 0
                 * (e.g. space). */
                if (pc->x1 == pc->x0 && pc->y1 == pc->y0 && pc->xadvance == 0.0f) continue;
                vio_font_packed_glyph *g = &glyphs[n++];
                g->codepoint   = first + c;
                g->pc.x0       = pc->x0; g->pc.y0 = pc->y0;
                g->pc.x1       = pc->x1; g->pc.y1 = pc->y1;
                g->pc.xoff     = pc->xoff; g->pc.yoff = pc->yoff;
                g->pc.xadvance = pc->xadvance;
                g->pc.xoff2    = pc->xoff2; g->pc.yoff2 = pc->yoff2;
            }
        }
    }

    for (int r = 0; r < VIO_FONT_NUM_RANGES; r++) free(range_chardata[r]);

    if (!glyphs) {
        return 0;
    }

    *out_glyphs = glyphs;
    *out_count  = n;
    return pack_ok;
}

void vio_font_finalize_glyphs(vio_font_object *font,
                              const vio_font_packed_glyph *glyphs, int count)
{
    for (int i = 0; i < count; i++) {
        zend_long cp = (zend_long)glyphs[i].codepoint;
        zval zv;
        ZVAL_STRINGL(&zv, (char *)&glyphs[i].pc, sizeof(vio_stbtt_packedchar));
        zend_hash_index_update(&font->glyph_map, cp, &zv);
    }
}

int vio_font_pack_atlas_dynamic(const unsigned char *ttf_data, float font_size,
                                unsigned char **out_bitmap, int *out_side,
                                vio_font_packed_glyph **out_glyphs, int *out_count)
{
    *out_bitmap = NULL; *out_side = 0; *out_glyphs = NULL; *out_count = 0;

    /* Decide the atlas size up front — we can NOT rely on stbtt_PackFontRanges'
     * return value: it reports the (intentionally) skipped CJK/Hangul ranges as
     * "missing", so it returns 0 even when every present glyph fit. Instead,
     * cheaply detect whether this font even has CJK (two cmap lookups) and size
     * accordingly. */
    int side;
    stbtt_fontinfo info;
    int has_font = stbtt_InitFont(&info, ttf_data, stbtt_GetFontOffsetForIndex(ttf_data, 0));
    int is_cjk = has_font && (stbtt_FindGlyphIndex(&info, 0x4E00) != 0   /* 一 */
                           || stbtt_FindGlyphIndex(&info, 0xAC00) != 0); /* 가 */

    if (!has_font || is_cjk) {
        /* CJK / Hangul face (tens of thousands of glyphs) — keep the full
         * atlas, exactly as before. */
        side = VIO_FONT_ATLAS_SIZE;
    } else {
        /* Latin / Cyrillic / Greek / Vietnamese face: at most ~1.1k present
         * glyphs. Their packed area is well under 1000·(size+2)² (≈800·size² in
         * practice), so the smallest power-of-two meeting that bound holds them
         * all with margin: 10px→512², 15-28px→1024², 32-48px→2048², 72px+→4096².
         * No retry — the pack always fits, and PackFontRanges' return value is
         * unreliable here anyway. */
        double need = 1000.0 * (font_size + 2.0) * (font_size + 2.0);
        side = 256;
        while ((double)side * (double)side < need && side < VIO_FONT_ATLAS_SIZE) {
            side <<= 1;
        }
    }

    unsigned char *bmp = (unsigned char *)calloc(1, (size_t)side * (size_t)side);
    if (!bmp) {
        return 0;
    }
    vio_font_packed_glyph *glyphs = NULL;
    int count = 0;
    int ok = vio_font_pack_atlas_raw(ttf_data, font_size, bmp, side, &glyphs, &count);
    *out_bitmap = bmp;
    *out_side   = side;
    *out_glyphs = glyphs;
    *out_count  = count;
    return (count > 0) ? 1 : ok;
}

int vio_font_pack_atlas(vio_font_object *font, unsigned char *atlas_bitmap, int atlas_size)
{
    vio_font_packed_glyph *glyphs = NULL;
    int count = 0;
    int pack_ok = vio_font_pack_atlas_raw(font->ttf_data, font->font_size,
                                          atlas_bitmap, atlas_size, &glyphs, &count);
    if (glyphs) {
        vio_font_finalize_glyphs(font, glyphs, count);
        free(glyphs);
    }
    return pack_ok;
}

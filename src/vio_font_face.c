/*
 * php-vio - Font face implementation: CPU text bitmaps (vio_text_bitmap)
 *
 * vio_text() draws through a font atlas on the GPU. Some consumers need the
 * same shaped text as pixels instead: textures baked from localized strings,
 * possibly in a build tool or a worker thread without a context. This file lays
 * a line out with the same pipeline as vio_text_shape.c (SheenBidi runs,
 * HarfBuzz shaping) over a fallback chain of faces and rasterizes the glyphs
 * with stb_truetype straight into a coverage bitmap.
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

/* stb before php.h (MSVC macro clashes), same rule as vio_font.c. */
#include "../vendor/stb/stb_truetype.h"

#include "vio_font_face.h"
#include "vio_text_shape.h"   /* php.h + build config -> HAVE_HARFBUZZ, bidi spans */

#include <math.h>

#ifdef HAVE_HARFBUZZ
#include <hb.h>
#endif

zend_class_entry *vio_font_face_ce = NULL;
static zend_object_handlers vio_font_face_handlers;

static zend_object *vio_font_face_create_object(zend_class_entry *ce)
{
    vio_font_face_object *face = zend_object_alloc(sizeof(vio_font_face_object), ce);

    face->ttf_data = NULL;
    face->ttf_len  = 0;
    face->stb_info = NULL;
    face->hb_font  = NULL;
    face->valid    = 0;

    zend_object_std_init(&face->std, ce);
    object_properties_init(&face->std, ce);
    face->std.handlers = &vio_font_face_handlers;

    return &face->std;
}

static void vio_font_face_free_object(zend_object *obj)
{
    vio_font_face_object *face = vio_font_face_from_obj(obj);

#ifdef HAVE_HARFBUZZ
    /* The HarfBuzz blob points into ttf_data: destroy the font first. */
    if (face->hb_font) {
        hb_font_destroy((hb_font_t *)face->hb_font);
    }
#endif
    face->hb_font = NULL;
    if (face->stb_info) {
        efree(face->stb_info);
        face->stb_info = NULL;
    }
    if (face->ttf_data) {
        efree(face->ttf_data);
        face->ttf_data = NULL;
    }

    zend_object_std_dtor(&face->std);
}

void vio_font_face_register(void)
{
    zend_class_entry ce;

    INIT_CLASS_ENTRY(ce, "VioFontFace", NULL);
    vio_font_face_ce = zend_register_internal_class(&ce);
    vio_font_face_ce->ce_flags |= ZEND_ACC_FINAL | ZEND_ACC_NO_DYNAMIC_PROPERTIES | ZEND_ACC_NOT_SERIALIZABLE;
    vio_font_face_ce->create_object = vio_font_face_create_object;

    memcpy(&vio_font_face_handlers, &std_object_handlers, sizeof(zend_object_handlers));
    vio_font_face_handlers.offset    = XtOffsetOf(vio_font_face_object, std);
    vio_font_face_handlers.free_obj  = vio_font_face_free_object;
    vio_font_face_handlers.clone_obj = NULL;
}

int vio_font_face_load(vio_font_face_object *face, const char *bytes, size_t len)
{
    if (!bytes || len == 0) return 0;

    face->ttf_data = (unsigned char *)emalloc(len);
    memcpy(face->ttf_data, bytes, len);
    face->ttf_len = len;

    /* Index 0 also picks the first face of a collection (.ttc). */
    int offset = stbtt_GetFontOffsetForIndex(face->ttf_data, 0);
    stbtt_fontinfo *info = (stbtt_fontinfo *)ecalloc(1, sizeof(stbtt_fontinfo));
    if (offset < 0 || !stbtt_InitFont(info, face->ttf_data, offset)) {
        efree(info);
        return 0;
    }
    face->stb_info = info;

#ifdef HAVE_HARFBUZZ
    hb_blob_t *blob = hb_blob_create((const char *)face->ttf_data, (unsigned int)face->ttf_len,
                                     HB_MEMORY_MODE_READONLY, NULL, NULL);
    hb_face_t *hface = hb_face_create(blob, 0);
    hb_blob_destroy(blob);
    hb_font_t *hbf = hb_font_create(hface);
    hb_face_destroy(hface);
    if (hbf && hbf != hb_font_get_empty()) {
        face->hb_font = hbf;
    } else if (hbf) {
        hb_font_destroy(hbf);
    }
#endif

    face->valid = 1;
    return 1;
}

int vio_font_face_has_glyph(const vio_font_face_object *face, uint32_t codepoint)
{
    if (!face || !face->valid) return 0;
#ifdef HAVE_HARFBUZZ
    if (face->hb_font) {
        hb_codepoint_t glyph = 0;
        return hb_font_get_nominal_glyph((hb_font_t *)face->hb_font, codepoint, &glyph) ? 1 : 0;
    }
#endif
    return stbtt_FindGlyphIndex((const stbtt_fontinfo *)face->stb_info, (int)codepoint) != 0;
}

/* ── Layout ─────────────────────────────────────────────────────────── */

/* A glyph placed on the line: origin x to the right, y down from the baseline. */
typedef struct {
    vio_font_face_object *face;
    int   gid;
    float x, y;
} vio_placed_glyph;

typedef struct {
    vio_placed_glyph *items;
    int   count, cap;
    float pen;
} vio_glyph_line;

/* A same-face stretch of a bidi run, in logical order. */
typedef struct {
    size_t off, len;
    int    face;
} vio_face_segment;

static void line_push(vio_glyph_line *line, vio_font_face_object *face, int gid, float x, float y)
{
    if (line->count == line->cap) {
        line->cap = line->cap ? line->cap * 2 : 64;
        line->items = (vio_placed_glyph *)erealloc(line->items, sizeof(vio_placed_glyph) * (size_t)line->cap);
    }
    vio_placed_glyph *g = &line->items[line->count++];
    g->face = face;
    g->gid  = gid;
    g->x    = x;
    g->y    = y;
}

/* Decode one UTF-8 codepoint from s[*i..end); advance *i. U+FFFD on a malformed
 * sequence (still advancing, so callers cannot loop). */
static uint32_t face_utf8_next(const char *s, size_t end, size_t *i)
{
    unsigned char c = (unsigned char)s[*i];
    uint32_t cp; int n;
    if (c < 0x80)            { cp = c;        n = 1; }
    else if ((c >> 5) == 6)  { cp = c & 0x1F; n = 2; }
    else if ((c >> 4) == 14) { cp = c & 0x0F; n = 3; }
    else if ((c >> 3) == 30) { cp = c & 0x07; n = 4; }
    else { (*i)++; return 0xFFFD; }
    if (*i + (size_t)n > end) { *i = end; return 0xFFFD; }
    for (int k = 1; k < n; k++) cp = (cp << 6) | (((unsigned char)s[*i + k]) & 0x3F);
    *i += (size_t)n;
    return cp;
}

/* The face for `cp`: the current segment's face while it covers the codepoint,
 * otherwise the first face in the chain that does. A codepoint no face covers
 * stays with the current face (its .notdef renders where the pen is). */
static int pick_face(vio_font_face_object **faces, int count, int current, uint32_t cp)
{
    if (current >= 0 && vio_font_face_has_glyph(faces[current], cp)) return current;
    for (int i = 0; i < count; i++) {
        if (vio_font_face_has_glyph(faces[i], cp)) return i;
    }
    return current >= 0 ? current : 0;
}

#ifdef HAVE_HARFBUZZ
static void shape_segment_hb(vio_glyph_line *line, vio_font_face_object *face,
                             const char *text, size_t len, size_t off, size_t n,
                             int rtl, float px_per_em)
{
    hb_font_t *hbf = (hb_font_t *)face->hb_font;
    int px64 = (int)lroundf(px_per_em * 64.0f);
    hb_font_set_scale(hbf, px64, px64);

    hb_buffer_t *buf = hb_buffer_create();
    /* The whole text is the pre/post context, only [off, off+n) is shaped. */
    hb_buffer_add_utf8(buf, text, (int)len, (unsigned int)off, (int)n);
    hb_buffer_guess_segment_properties(buf);
    hb_buffer_set_direction(buf, rtl ? HB_DIRECTION_RTL : HB_DIRECTION_LTR);
    hb_shape(hbf, buf, NULL, 0);

    unsigned int count = 0;
    hb_glyph_info_t     *info = hb_buffer_get_glyph_infos(buf, &count);
    hb_glyph_position_t *pos  = hb_buffer_get_glyph_positions(buf, &count);
    /* Glyphs come out in visual order for the buffer direction. */
    for (unsigned int i = 0; i < count; i++) {
        line_push(line, face, (int)info[i].codepoint,
                  line->pen + pos[i].x_offset / 64.0f,
                  -pos[i].y_offset / 64.0f);   /* HarfBuzz y is up */
        line->pen += pos[i].x_advance / 64.0f;
    }
    hb_buffer_destroy(buf);
}
#endif

static void shape_segment_stb(vio_glyph_line *line, vio_font_face_object *face,
                              const char *text, size_t off, size_t n, float px_per_em)
{
    const stbtt_fontinfo *info = (const stbtt_fontinfo *)face->stb_info;
    float scale = stbtt_ScaleForMappingEmToPixels(info, px_per_em);
    size_t i = off, end = off + n;
    int prev = 0;
    while (i < end) {
        uint32_t cp = face_utf8_next(text, end, &i);
        int gid = stbtt_FindGlyphIndex(info, (int)cp);
        if (prev) line->pen += scale * (float)stbtt_GetGlyphKernAdvance(info, prev, gid);
        line_push(line, face, gid, line->pen, 0.0f);
        int advance, lsb;
        stbtt_GetGlyphHMetrics(info, gid, &advance, &lsb);
        line->pen += scale * (float)advance;
        prev = gid;
    }
}

/* Pixel box of a placed glyph, in line coordinates (y down). 0 for no ink. */
static int glyph_box(const vio_placed_glyph *g, float px_per_em,
                     float *scale, float *shift_x, float *shift_y,
                     int *x0, int *y0, int *x1, int *y1)
{
    const stbtt_fontinfo *info = (const stbtt_fontinfo *)g->face->stb_info;
    float fx = (float)floor(g->x);
    float fy = (float)floor(g->y);
    *scale   = stbtt_ScaleForMappingEmToPixels(info, px_per_em);
    *shift_x = g->x - fx;
    *shift_y = g->y - fy;
    stbtt_GetGlyphBitmapBoxSubpixel(info, g->gid, *scale, *scale, *shift_x, *shift_y, x0, y0, x1, y1);
    if (*x1 <= *x0 || *y1 <= *y0) return 0;
    *x0 += (int)fx; *x1 += (int)fx;
    *y0 += (int)fy; *y1 += (int)fy;
    return 1;
}

int vio_font_face_render_text(vio_font_face_object **faces, int face_count,
                              const char *text, size_t len, float px_per_em,
                              int want_data, vio_text_bitmap_result *out)
{
    memset(out, 0, sizeof(*out));
    if (face_count <= 0 || !(px_per_em > 0.0f)) return 0;
    for (int i = 0; i < face_count; i++) {
        if (!faces[i] || !faces[i]->valid) return 0;
    }
    if (len == 0) return 1;

    int shaped = 0;
#ifdef HAVE_HARFBUZZ
    shaped = 1;
    for (int i = 0; i < face_count; i++) {
        if (!faces[i]->hb_font) { shaped = 0; break; }
    }
#endif

    vio_text_bidi_span *spans = NULL;
    int span_count = 0;
#ifdef HAVE_HARFBUZZ
    if (shaped) span_count = vio_text_bidi_spans(text, len, &spans);
#endif
    if (span_count <= 0) {
        spans = (vio_text_bidi_span *)emalloc(sizeof(vio_text_bidi_span));
        spans[0].offset = 0;
        spans[0].length = len;
        spans[0].rtl    = 0;
        span_count = 1;
    }

    vio_glyph_line line = {0};
    for (int s = 0; s < span_count; s++) {
        size_t end = spans[s].offset + spans[s].length;
        size_t i = spans[s].offset;
        int cap = 8, count = 0, current = -1;
        vio_face_segment *segs = (vio_face_segment *)emalloc(sizeof(vio_face_segment) * (size_t)cap);
        while (i < end) {
            size_t at = i;
            uint32_t cp = face_utf8_next(text, end, &i);
            int f = pick_face(faces, face_count, current, cp);
            if (f != current) {
                if (count == cap) {
                    cap *= 2;
                    segs = (vio_face_segment *)erealloc(segs, sizeof(vio_face_segment) * (size_t)cap);
                }
                segs[count].off  = at;
                segs[count].face = f;
                count++;
                current = f;
            }
            segs[count - 1].len = i - segs[count - 1].off;
        }
        /* A right-to-left run reads right to left: its logical segments are
         * placed in reverse. */
        for (int k = 0; k < count; k++) {
            vio_face_segment *seg = &segs[spans[s].rtl ? count - 1 - k : k];
#ifdef HAVE_HARFBUZZ
            if (shaped) {
                shape_segment_hb(&line, faces[seg->face], text, len, seg->off, seg->len,
                                 spans[s].rtl, px_per_em);
                continue;
            }
#endif
            shape_segment_stb(&line, faces[seg->face], text, seg->off, seg->len, px_per_em);
        }
        efree(segs);
    }
    efree(spans);

    out->advance = line.pen;

    int have = 0, min_x = 0, min_y = 0, max_x = 0, max_y = 0;
    for (int g = 0; g < line.count; g++) {
        float scale, sx, sy;
        int x0, y0, x1, y1;
        if (!glyph_box(&line.items[g], px_per_em, &scale, &sx, &sy, &x0, &y0, &x1, &y1)) continue;
        if (!have) {
            min_x = x0; min_y = y0; max_x = x1; max_y = y1;
            have = 1;
        } else {
            if (x0 < min_x) min_x = x0;
            if (y0 < min_y) min_y = y0;
            if (x1 > max_x) max_x = x1;
            if (y1 > max_y) max_y = y1;
        }
    }

    if (have) {
        out->width    = max_x - min_x;
        out->height   = max_y - min_y;
        out->origin_x = -min_x;
        out->baseline = -min_y;

        if (want_data) {
            out->data = (unsigned char *)ecalloc(1, (size_t)out->width * (size_t)out->height);
            for (int g = 0; g < line.count; g++) {
                const vio_placed_glyph *pg = &line.items[g];
                float scale, sx, sy;
                int x0, y0, x1, y1;
                if (!glyph_box(pg, px_per_em, &scale, &sx, &sy, &x0, &y0, &x1, &y1)) continue;
                int gw = x1 - x0, gh = y1 - y0;
                unsigned char *tmp = (unsigned char *)emalloc((size_t)gw * (size_t)gh);
                stbtt_MakeGlyphBitmapSubpixel((const stbtt_fontinfo *)pg->face->stb_info, tmp,
                                              gw, gh, gw, scale, scale, sx, sy, pg->gid);
                int ox = x0 - min_x, oy = y0 - min_y;
                for (int r = 0; r < gh; r++) {
                    unsigned char *dst = out->data + (size_t)(oy + r) * (size_t)out->width + (size_t)ox;
                    const unsigned char *src = tmp + (size_t)r * (size_t)gw;
                    /* Overlapping glyphs (joined scripts, marks) keep the
                     * stronger coverage instead of summing into dark seams. */
                    for (int c = 0; c < gw; c++) {
                        if (src[c] > dst[c]) dst[c] = src[c];
                    }
                }
                efree(tmp);
            }
        }
    }

    if (line.items) efree(line.items);
    return 1;
}

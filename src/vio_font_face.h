/*
 * php-vio - Font face: CPU text bitmaps without a GPU atlas (vio_text_bitmap)
 */

#ifndef VIO_FONT_FACE_H
#define VIO_FONT_FACE_H

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "php.h"
#include "../include/vio_types.h"

/* A parsed font file for vio_text_bitmap(): the TTF/OTF bytes, their
 * stb_truetype info and, with HarfBuzz, a shaping font. Unlike VioFont it owns
 * no glyph atlas and belongs to no context, so creating one is cheap even for a
 * large CJK face and needs no GPU (build tools, worker threads). */
typedef struct _vio_font_face_object {
    unsigned char *ttf_data;
    size_t         ttf_len;
    void          *stb_info;  /* stbtt_fontinfo* (opaque: stb clashes with php.h on MSVC) */
    void          *hb_font;   /* hb_font_t*, NULL without HarfBuzz */
    int            valid;
    zend_object    std;
} vio_font_face_object;

extern zend_class_entry *vio_font_face_ce;

void vio_font_face_register(void);

/* Parse `bytes` (copied) into `face`. Returns 1 on success. */
int vio_font_face_load(vio_font_face_object *face, const char *bytes, size_t len);

/* True if `face` maps `codepoint` to a real (non-.notdef) glyph. */
int vio_font_face_has_glyph(const vio_font_face_object *face, uint32_t codepoint);

typedef struct {
    int            width, height;  /* bitmap size in px; 0 when nothing has ink */
    int            origin_x;       /* bitmap column of the pen origin */
    int            baseline;       /* bitmap row of the baseline */
    float          advance;        /* total pen advance in px */
    unsigned char *data;           /* width*height coverage bytes (emalloc), NULL when measuring */
} vio_text_bitmap_result;

/* Lay out one line of UTF-8 `text` at `px_per_em` pixels per em with the
 * fallback chain `faces` and, when `want_data` is set, rasterize it.
 *
 * The first face that covers a codepoint claims it, and a segment keeps its
 * face while that face covers the next codepoint, so spaces and punctuation stay
 * inside the script run around them. With HarfBuzz every face is shaped per
 * segment on bidi runs in visual order (SheenBidi); without it the line is laid
 * out left to right, glyph by glyph, with kerning. Returns 1 on success. */
int vio_font_face_render_text(vio_font_face_object **faces, int face_count,
                              const char *text, size_t len, float px_per_em,
                              int want_data, vio_text_bitmap_result *out);

static inline vio_font_face_object *vio_font_face_from_obj(zend_object *obj) {
    return (vio_font_face_object *)((char *)obj - XtOffsetOf(vio_font_face_object, std));
}

#define Z_VIO_FONT_FACE_P(zv) vio_font_face_from_obj(Z_OBJ_P(zv))

#endif /* VIO_FONT_FACE_H */

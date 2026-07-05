/*
 * php-vio - Text shaping (HarfBuzz + SheenBidi)
 *
 * This whole subsystem is gated behind HAVE_HARFBUZZ. When HarfBuzz is not
 * compiled in, none of this exists and vio_text() / vio_text_measure() keep
 * using the legacy codepoint-per-glyph path unchanged.
 *
 * When present, the font's atlas switches from the static codepoint-range
 * pack (stbtt_PackFontRanges) to a lazily-populated, glyph-index-keyed atlas:
 * glyphs are rasterized one at a time (stbtt_MakeGlyphBitmap) into a
 * stb_rect_pack'd bitmap the first time shaping asks for them. This is the
 * only atlas layout that can hold the glyphs HarfBuzz produces (ligatures,
 * Arabic positional forms, ...) which no single codepoint addresses.
 *
 * Pipeline per string:
 *   1. BiDi          — SheenBidi resolves levels and hands back runs in
 *                      *visual* order (SBLine).
 *   2. Itemize+shape — each run is shaped by HarfBuzz with the bidi-resolved
 *                      direction; script/language are guessed from content.
 *   3. Emit          — shaped glyphs are rasterized on demand and pushed into
 *                      the existing 2D batch as VIO_2D_TEXT quads.
 */

#ifndef VIO_TEXT_SHAPE_H
#define VIO_TEXT_SHAPE_H

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

/* Included unconditionally so that pulling in this header also pulls in php.h
 * (via vio_font.h) and therefore the build config — this is what makes
 * HAVE_HARFBUZZ defined for consumers. On the Windows build HAVE_HARFBUZZ lives
 * in config.w32.h, which only becomes visible through php.h. */
#include "vio_font.h"
#include "vio_context.h"

#ifdef HAVE_HARFBUZZ

/* Build the HarfBuzz font + an empty lazy atlas for `font` (which must already
 * have ttf_data/ttf_len/font_size populated). Returns 1 on success, 0 on
 * failure — on failure the font simply has no shaping state and callers fall
 * back to the legacy path. */
int  vio_text_shape_init_font(vio_font_object *font);

/* Tear down HarfBuzz font + lazy atlas. Safe to call on a font that was never
 * initialized. Must run before font->ttf_data is freed (the HarfBuzz blob
 * references it). */
void vio_text_shape_free_font(vio_font_object *font);

/* True if `font` has a live shaping/atlas state (init succeeded). */
int  vio_text_shape_available(const vio_font_object *font);

/* Shape `text` (UTF-8, `len` bytes) and push glyph quads into ctx->state_2d.
 * Origin (x, y) is in logical units, y is the baseline of the FIRST line.
 * Hard breaks ('\n') always start a new line; when max_width > 0 the text also
 * soft-wraps at word boundaries so no line exceeds max_width logical px. Each
 * subsequent line drops by line_height logical px (or the font's natural line
 * height when line_height <= 0). Color is straight RGBA in [0,1]; z is the 2D
 * sort key. */
void vio_text_shape_draw(vio_context_object *ctx, vio_font_object *font,
                         const char *text, size_t len,
                         float x, float y, float z,
                         float cr, float cg, float cb, float ca,
                         float max_width, float line_height);

/* Measure `text` without drawing (same wrapping rules as the draw path). Writes
 * logical width (widest line) + total height, and the line count when out_lines
 * is non-NULL. No atlas side effects. */
void vio_text_shape_measure(vio_font_object *font,
                            const char *text, size_t len,
                            float max_width, float line_height,
                            float *out_width, float *out_height, int *out_lines);

#endif /* HAVE_HARFBUZZ */

#endif /* VIO_TEXT_SHAPE_H */

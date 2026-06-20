/*
 * php-vio - Font management (TTF via stb_truetype)
 */

#ifndef VIO_FONT_H
#define VIO_FONT_H

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "php.h"
#include "../include/vio_types.h"

/* stbtt_packedchar definition matching stb_truetype.h - avoids including
 * stb_truetype.h in this header which causes macro conflicts with PHP
 * headers on Windows (MSVC). */
typedef struct
{
   unsigned short x0,y0,x1,y1;
   float xoff,yoff,xadvance;
   float xoff2,yoff2;
} vio_stbtt_packedchar;

/* Legacy bakedchar (same layout as stbtt_bakedchar) for vio_font_bake_bitmap */
typedef struct
{
   unsigned short x0,y0,x1,y1;
   float xoff,yoff,xadvance;
} vio_stbtt_bakedchar;

#define VIO_FONT_ATLAS_SIZE 4096

/* Unicode block ranges packed into the atlas */
#define VIO_FONT_NUM_RANGES 9

typedef struct {
    int first_codepoint;
    int num_chars;
} vio_font_range_def;

/* Unicode ranges baked into the font atlas. Missing glyphs are skipped. */
static const vio_font_range_def vio_font_ranges[VIO_FONT_NUM_RANGES] = {
    { 0x0020,   224 },  /* Latin (Basic + Supplement): U+0020..U+00FF */
    { 0x0100,   256 },  /* Latin Extended-A+B: U+0100..U+01FF */
    { 0x0370,   144 },  /* Greek and Coptic: U+0370..U+03FF */
    { 0x0400,   256 },  /* Cyrillic: U+0400..U+04FF */
    { 0x1E00,   256 },  /* Latin Extended Additional (Vietnamese): U+1E00..U+1EFF */
    { 0x3000,   256 },  /* CJK Symbols + Hiragana + Katakana: U+3000..U+30FF */
    { 0x4E00, 20992 },  /* CJK Unified Ideographs: U+4E00..U+9FFF */
    { 0xAC00, 11172 },  /* Hangul Syllables: U+AC00..U+D7A3 */
    { 0xFF00,   240 },  /* Fullwidth Forms: U+FF00..U+FFEF */
};

struct _vio_backend;

typedef struct _vio_font_object {
    HashTable          glyph_map;     /* codepoint (zend_long) -> vio_stbtt_packedchar */
    unsigned int       atlas_texture;          /* GL texture / Metal handle */
    void              *atlas_backend_texture;   /* D3D11/D3D12 backend texture */
    float              font_size;
    int                atlas_w, atlas_h;
    unsigned char     *ttf_data;      /* raw TTF file data */
    size_t             ttf_len;       /* TTF data length */
    int                valid;
    const struct _vio_backend *backend;  /* Backend that owns the atlas */
    zend_object        std;
} vio_font_object;

extern zend_class_entry *vio_font_ce;

void vio_font_register(void);
int vio_font_pack_atlas(vio_font_object *font, unsigned char *atlas_bitmap, int atlas_size);

/* ── Thread-safe (Zend-free) atlas packing ───────────────────────────
 *
 * vio_font_pack_atlas() above both rasterizes the atlas AND populates the
 * font's Zend glyph_map hashtable in one pass. That hashtable population uses
 * the Zend memory manager (emalloc / ZVAL_STRINGL / zend_hash_*), which is
 * per-request and NOT safe to touch from a worker thread.
 *
 * For async loading the work is split:
 *   1. vio_font_pack_atlas_raw() runs on the worker thread. It rasterizes the
 *      atlas into a caller-provided bitmap and writes the packed glyph metrics
 *      into a flat, malloc-backed vio_font_packed_glyph array. It touches only
 *      libc malloc + stb_truetype — no Zend, no GL.
 *   2. vio_font_finalize_glyphs() runs on the render thread at poll time and
 *      copies that flat array into the font's Zend glyph_map. */

/* One packed glyph plus its codepoint — flat, Zend-free transport struct used
 * to hand worker-thread pack results back to the render thread. */
typedef struct {
    int                  codepoint;
    vio_stbtt_packedchar pc;
} vio_font_packed_glyph;

/* Rasterize the atlas for `font_size` from `ttf_data` into `atlas_bitmap`
 * (atlas_size*atlas_size R8 bytes, caller-allocated). Allocates *out_glyphs
 * with malloc() (caller frees with free()) and writes the packed glyph count
 * into *out_count. Returns 1 on success, 0 on failure. Thread-safe: uses no
 * Zend allocator and no GPU calls. */
int vio_font_pack_atlas_raw(const unsigned char *ttf_data, float font_size,
                            unsigned char *atlas_bitmap, int atlas_size,
                            vio_font_packed_glyph **out_glyphs, int *out_count);

/* Like vio_font_pack_atlas_raw(), but sizes the atlas dynamically to the font's
 * actual present-glyph set instead of a fixed VIO_FONT_ATLAS_SIZE². Allocates
 * *out_bitmap (malloc, *out_side × *out_side R8 bytes) and *out_glyphs (malloc);
 * the caller frees both with free(). Falls back to VIO_FONT_ATLAS_SIZE on
 * overflow. Thread-safe (libc only, no Zend, no GPU). Returns 1 on success. */
int vio_font_pack_atlas_dynamic(const unsigned char *ttf_data, float font_size,
                                unsigned char **out_bitmap, int *out_side,
                                vio_font_packed_glyph **out_glyphs, int *out_count);

/* Populate font->glyph_map from a flat packed-glyph array produced by
 * vio_font_pack_atlas_raw(). Must run on the thread that owns the Zend heap
 * (the render/request thread). */
void vio_font_finalize_glyphs(vio_font_object *font,
                              const vio_font_packed_glyph *glyphs, int count);

static inline vio_font_object *vio_font_from_obj(zend_object *obj) {
    return (vio_font_object *)((char *)obj - XtOffsetOf(vio_font_object, std));
}

#define Z_VIO_FONT_P(zv) vio_font_from_obj(Z_OBJ_P(zv))

#endif /* VIO_FONT_H */

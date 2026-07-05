/*
 * php-vio - Text shaping implementation (HarfBuzz + SheenBidi)
 *
 * See vio_text_shape.h for the overall design. Everything here is gated behind
 * HAVE_HARFBUZZ; without it this file compiles to nothing.
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

/* stb declaration headers must precede php.h to avoid macro conflicts on MSVC
 * (same rule as vio_font.c). Included UNCONDITIONALLY and before the vio/php
 * headers: on Windows HAVE_HARFBUZZ is only defined once php.h (→ config.w32.h)
 * is included, so the HAVE_HARFBUZZ guard below must come *after* that, not
 * before these includes. Implementations live in the stb_*_impl.c TUs. */
#include "../vendor/stb/stb_truetype.h"
#include "../vendor/stb/stb_rect_pack.h"

#include "vio_text_shape.h"   /* pulls php.h + build config → HAVE_HARFBUZZ */
#include "vio_2d.h"
#include "../include/vio_backend.h"

#ifdef HAVE_HARFBUZZ

#include <hb.h>
#include <SheenBidi/SheenBidi.h>

/* One glyph's place in the atlas (metrics in physical/atlas px). */
typedef struct {
    unsigned short x, y, w, h;   /* atlas rect; w==h==0 for blank glyphs (space) */
    short bearing_x, bearing_y;  /* stbtt_GetGlyphBitmapBox ix0 / iy0 (y-down) */
} vio_glyph_slot;

/* Per-font atlas state kept behind font->shape_atlas after the one-time build.
 * The atlas is packed once (all glyphs, by index) at font creation and uploaded
 * exactly like the legacy path — so the GPU texture handle is stable for the
 * font's whole life. No runtime rasterization, no re-upload, no destroy race
 * (which would be unsafe on deferred backends like D3D12/Vulkan). */
typedef struct {
    HashTable glyphs;      /* glyph_id (zend_long) -> vio_glyph_slot blob */
    float     line_height; /* natural line advance, physical px (asc-desc+gap) */
} vio_shape_atlas;

/* Choose the smallest power-of-two atlas side (>=512, <=4096) that comfortably
 * holds `num_glyphs` glyphs at `size` px. Mirrors the legacy dynamic sizing. */
static int shape_atlas_side(int num_glyphs, float size)
{
    double need = (double)num_glyphs * ((double)size + 2.0) * ((double)size + 2.0);
    int side = 512;
    while ((double)side * (double)side < need && side < 4096) side <<= 1;
    return side;
}

/* ── Lifecycle ──────────────────────────────────────────────────────── */

int vio_text_shape_init_font(vio_font_object *font)
{
    if (!font->ttf_data || font->ttf_len == 0) return 0;

    /* HarfBuzz font over the (read-only) TTF bytes. hb keeps the pointer, so it
     * must be torn down before font->ttf_data is freed. */
    hb_blob_t *blob = hb_blob_create((const char *)font->ttf_data,
                                     (unsigned int)font->ttf_len,
                                     HB_MEMORY_MODE_READONLY, NULL, NULL);
    hb_face_t *face = hb_face_create(blob, 0);
    hb_blob_destroy(blob);
    hb_font_t *hbf = hb_font_create(face);
    hb_face_destroy(face);
    if (!hbf || hb_font_get_empty() == hbf) {
        if (hbf) hb_font_destroy(hbf);
        return 0;
    }

    /* HarfBuzz positions come out in 26.6 fixed point at this scale, i.e. in
     * physical pixels once divided by 64. */
    int px64 = (int)(font->font_size * 64.0f + 0.5f);
    hb_font_set_scale(hbf, px64, px64);

    stbtt_fontinfo info;
    if (!stbtt_InitFont(&info, font->ttf_data,
                        stbtt_GetFontOffsetForIndex(font->ttf_data, 0))) {
        hb_font_destroy(hbf);
        return 0;
    }
    float scale = stbtt_ScaleForPixelHeight(&info, font->font_size);
    int num_glyphs = info.numGlyphs > 0 ? info.numGlyphs : 1;
    int side = shape_atlas_side(num_glyphs, font->font_size);

    unsigned char *bmp   = (unsigned char *)ecalloc(1, (size_t)side * (size_t)side);
    stbrp_node    *nodes = (stbrp_node *)emalloc(sizeof(stbrp_node) * (size_t)side);
    stbrp_context  packer;
    stbrp_init_target(&packer, side, side, nodes, side);

    /* Natural line height (physical px): ascent - descent + line gap. */
    int v_asc, v_desc, v_gap;
    stbtt_GetFontVMetrics(&info, &v_asc, &v_desc, &v_gap);

    vio_shape_atlas *a = (vio_shape_atlas *)ecalloc(1, sizeof(vio_shape_atlas));
    a->line_height = (float)(v_asc - v_desc + v_gap) * scale;
    zend_hash_init(&a->glyphs, num_glyphs < 4096 ? num_glyphs : 4096, NULL, NULL, 0);

    /* Rasterize every glyph the font has, keyed by glyph index — this is the
     * one layout that can hold HarfBuzz output (ligatures, positional forms)
     * since those glyphs have indices but no addressing codepoint. */
    for (int gid = 0; gid < num_glyphs; gid++) {
        int ix0, iy0, ix1, iy1;
        stbtt_GetGlyphBitmapBox(&info, gid, scale, scale, &ix0, &iy0, &ix1, &iy1);
        int gw = ix1 - ix0, gh = iy1 - iy0;

        vio_glyph_slot slot;
        slot.bearing_x = (short)ix0;
        slot.bearing_y = (short)iy0;

        if (gw <= 0 || gh <= 0) {
            slot.x = slot.y = slot.w = slot.h = 0;  /* blank (space, control) */
        } else {
            stbrp_rect r;                            /* 1px gutter vs bleed */
            r.id = gid; r.w = (stbrp_coord)(gw + 1); r.h = (stbrp_coord)(gh + 1);
            r.x = r.y = 0; r.was_packed = 0;
            stbrp_pack_rects(&packer, &r, 1);
            if (!r.was_packed) {
                /* Atlas full — drop this glyph (same "no retry" bound as the
                 * legacy CJK path). Store a blank slot so lookups still hit. */
                slot.x = slot.y = slot.w = slot.h = 0;
            } else {
                stbtt_MakeGlyphBitmap(&info, bmp + (size_t)r.y * side + r.x,
                                      gw, gh, side, scale, scale, gid);
                slot.x = (unsigned short)r.x; slot.y = (unsigned short)r.y;
                slot.w = (unsigned short)gw;  slot.h = (unsigned short)gh;
            }
        }
        zval zv;
        ZVAL_STRINGL(&zv, (char *)&slot, sizeof(slot));
        zend_hash_index_update(&a->glyphs, (zend_long)gid, &zv);
    }

    font->hb_font     = hbf;
    font->shape_atlas = a;
    font->atlas_w = font->atlas_h = side;

    /* Single upload, exactly like the legacy path — stable handle thereafter. */
    vio_font_upload_atlas_to_gpu(font, font->backend, bmp);

    efree(nodes);
    efree(bmp);
    return 1;
}

void vio_text_shape_free_font(vio_font_object *font)
{
    if (font->shape_atlas) {
        vio_shape_atlas *a = (vio_shape_atlas *)font->shape_atlas;
        zend_hash_destroy(&a->glyphs);
        efree(a);
        font->shape_atlas = NULL;
    }
    if (font->hb_font) {
        hb_font_destroy((hb_font_t *)font->hb_font);
        font->hb_font = NULL;
    }
}

int vio_text_shape_available(const vio_font_object *font)
{
    return font->hb_font != NULL && font->shape_atlas != NULL;
}

/* Look up a glyph slot (always present after build; NULL only if the id somehow
 * exceeds numGlyphs). */
static const vio_glyph_slot *shape_atlas_glyph(vio_shape_atlas *a, unsigned int gid)
{
    zval *found = zend_hash_index_find(&a->glyphs, (zend_long)gid);
    return found ? (const vio_glyph_slot *)Z_STRVAL_P(found) : NULL;
}

/* ── BiDi + shaping ─────────────────────────────────────────────────── */

/* A visual-order run handed back by SheenBidi. */
typedef struct { size_t offset, length; int rtl; } vio_bidi_run;

/* Resolve `text` into visual-order runs. On success returns a SheenBidi line
 * handle (caller releases it) and fills *out_runs (points into SheenBidi's
 * memory, valid until the line is released) + *out_count. Returns NULL on any
 * failure, in which case the caller should treat the whole string as one LTR
 * run. */
static SBLineRef bidi_resolve(const char *text, size_t len,
                              const SBRun **out_runs, SBUInteger *out_count)
{
    *out_runs = NULL; *out_count = 0;
    SBCodepointSequence seq;
    seq.stringEncoding = SBStringEncodingUTF8;
    seq.stringBuffer   = (void *)text;
    seq.stringLength   = (SBUInteger)len;

    SBAlgorithmRef algo = SBAlgorithmCreate(&seq);
    if (!algo) return NULL;
    SBParagraphRef para = SBAlgorithmCreateParagraph(algo, 0, (SBUInteger)len,
                                                     SBLevelDefaultLTR);
    if (!para) { SBAlgorithmRelease(algo); return NULL; }
    SBUInteger plen = SBParagraphGetLength(para);
    SBLineRef line = SBParagraphCreateLine(para, 0, plen);
    SBParagraphRelease(para);
    SBAlgorithmRelease(algo);
    if (!line) return NULL;

    *out_runs  = SBLineGetRunsPtr(line);
    *out_count = SBLineGetRunCount(line);
    return line;
}

/* Shape one run and invoke `emit(user, gid, pen_x, y_off)` per glyph, where
 * pen_x is the glyph origin within the run (accumulated advance + this glyph's
 * x_offset) and y_off is HarfBuzz's y-up offset — both physical px. Returns the
 * run's total advance (physical px). */
typedef void (*vio_glyph_emit)(void *user, unsigned int gid,
                               float pen_x, float y_off);

static float shape_run(hb_font_t *hbf, const char *text, size_t len,
                       const vio_bidi_run *run, vio_glyph_emit emit, void *user)
{
    hb_buffer_t *buf = hb_buffer_create();
    hb_buffer_add_utf8(buf, text, (int)len, (unsigned int)run->offset, (int)run->length);
    /* Guess script + language from content, then force the bidi-resolved
     * direction (guessing alone can disagree inside mixed-direction text). */
    hb_buffer_guess_segment_properties(buf);
    hb_buffer_set_direction(buf, run->rtl ? HB_DIRECTION_RTL : HB_DIRECTION_LTR);

    hb_shape(hbf, buf, NULL, 0);

    unsigned int n = 0;
    hb_glyph_info_t     *info = hb_buffer_get_glyph_infos(buf, &n);
    hb_glyph_position_t *pos  = hb_buffer_get_glyph_positions(buf, &n);

    /* HarfBuzz emits glyphs already in visual (left-to-right) order for the
     * buffer's direction, so the pen always advances positively. */
    float advance = 0.0f;
    for (unsigned int i = 0; i < n; i++) {
        float xo = pos[i].x_offset  / 64.0f;
        float yo = pos[i].y_offset  / 64.0f;  /* y-up */
        float xa = pos[i].x_advance / 64.0f;
        emit(user, info[i].codepoint, advance + xo, yo);
        advance += xa;
    }
    hb_buffer_destroy(buf);
    return advance;
}

/* ── Line breaking (hard '\n' + greedy word wrap) ───────────────────── */

static void noop_emit(void *user, unsigned int gid, float pen_x, float y_off)
{ (void)user; (void)gid; (void)pen_x; (void)y_off; }

/* Total advance (physical px) of a shaped sub-range [off, off+n) of `text`.
 * Direction-independent, so it is measured as a single (LTR) run. */
static float shape_range_advance(hb_font_t *hbf, const char *text, size_t len,
                                 size_t off, size_t n)
{
    if (n == 0) return 0.0f;
    vio_bidi_run r; r.offset = off; r.length = n; r.rtl = 0;
    return shape_run(hbf, text, len, &r, noop_emit, NULL);
}

/* Chars that are wrap opportunities / collapse at line ends (NOT '\n', which is
 * a hard break handled separately). */
static int is_wrap_space(char c) { return c == ' ' || c == '\t' || c == '\r'; }

/* Decode one UTF-8 codepoint from s[*i..end); advance *i. Returns U+FFFD on a
 * malformed sequence (still advancing so callers can't loop). */
static uint32_t utf8_next(const char *s, size_t end, size_t *i)
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

/* Compact UAX #14-lite line-break class — only what the wrapper needs. */
enum {
    LB_OTHER = 0,  /* Latin/digits/default: interior of a word, no break */
    LB_IDEO,       /* CJK ideograph / kana / Hangul: breakable both sides */
    LB_OPEN,       /* open punctuation: no break *after* */
    LB_CLOSE,      /* close punctuation / CJK non-starter: no break *before* */
    LB_THAI_BASE,  /* Thai consonant / following vowel: cluster starter */
    LB_THAI_LEAD,  /* Thai leading vowel: binds to the following consonant */
    LB_THAI_MARK   /* Thai combining vowel/tone: never break before it */
};

static int lb_class(uint32_t cp)
{
    /* Kinsoku sets first (some live inside the CJK-symbol ranges below). */
    switch (cp) {
        case 0x0028: case 0x005B: case 0x007B:                 /* ( [ { */
        case 0x3008: case 0x300A: case 0x300C: case 0x300E:    /* 〈《「『 */
        case 0x3010: case 0x3014: case 0x3016: case 0x3018:    /* 【〔〖〘 */
        case 0xFF08: case 0xFF3B: case 0xFF5B:                 /* （［｛ */
        case 0x201C: case 0x2018:                              /* “ ‘ */
            return LB_OPEN;
        case 0x0029: case 0x005D: case 0x007D:                 /* ) ] } */
        case 0x3009: case 0x300B: case 0x300D: case 0x300F:    /* 〉》」』 */
        case 0x3011: case 0x3015: case 0x3017: case 0x3019:    /* 】〕〗〙 */
        case 0x3001: case 0x3002: case 0x30FB: case 0x30FC:    /* 、。・ー */
        case 0xFF09: case 0xFF3D: case 0xFF5D: case 0xFF60:    /* ）］｝｠ */
        case 0xFF0C: case 0xFF0E: case 0xFF01: case 0xFF1F:    /* ，．！？ */
        case 0xFF1A: case 0xFF1B:                              /* ：； */
        case 0x201D: case 0x2019:                              /* ” ’ */
            return LB_CLOSE;
    }
    /* Thai (U+0E00..U+0E7F). */
    if (cp >= 0x0E00 && cp <= 0x0E7F) {
        if (cp >= 0x0E40 && cp <= 0x0E44) return LB_THAI_LEAD;   /* เ แ โ ใ ไ (pre-base) */
        /* Spacing + non-spacing vowels/tones that follow their consonant and
         * must not carry a break before them. */
        if (cp == 0x0E30 || cp == 0x0E31 || cp == 0x0E32 || cp == 0x0E33 ||
            cp == 0x0E45 || (cp >= 0x0E34 && cp <= 0x0E3A) ||
            (cp >= 0x0E47 && cp <= 0x0E4E))
            return LB_THAI_MARK;
        return LB_THAI_BASE;                                      /* consonants + cluster starters */
    }
    /* Ideographic (breakable) blocks. */
    if ((cp >= 0x3040 && cp <= 0x30FF) ||   /* Hiragana + Katakana */
        (cp >= 0x3400 && cp <= 0x9FFF) ||   /* CJK Ext-A + Unified */
        (cp >= 0xF900 && cp <= 0xFAFF) ||   /* CJK Compat Ideographs */
        (cp >= 0xAC00 && cp <= 0xD7A3) ||   /* Hangul Syllables */
        (cp >= 0x3000 && cp <= 0x303F) ||   /* CJK Symbols & Punct */
        (cp >= 0xFF00 && cp <= 0xFFEF) ||   /* Halfwidth/Fullwidth Forms */
        (cp >= 0x20000 && cp <= 0x2FFFF))   /* CJK Ext-B..F */
        return LB_IDEO;
    return LB_OTHER;
}

/* Is a line break allowed between codepoints `a` and `b` (a immediately before
 * b, neither a space)? Implements the CJK + Thai subset of UAX #14. */
static int lb_break_between(uint32_t a, uint32_t b)
{
    int ca = lb_class(a), cb = lb_class(b);
    if (ca == LB_OPEN)       return 0;   /* no break after open punctuation */
    if (cb == LB_CLOSE)      return 0;   /* no break before close punctuation */
    if (cb == LB_THAI_MARK)  return 0;   /* never split a combining mark off */
    if (ca == LB_IDEO || cb == LB_IDEO) return 1;   /* ideographic: break freely */
    /* Thai: break only before a cluster starter, and never right after a
     * leading vowel (it must stay glued to its consonant). */
    if ((ca == LB_THAI_BASE || ca == LB_THAI_MARK || ca == LB_THAI_LEAD) &&
        (cb == LB_THAI_BASE || cb == LB_THAI_LEAD)) {
        return ca != LB_THAI_LEAD;
    }
    return 0;   /* default: keep together (Latin word interior) */
}

/* Callback invoked once per laid-out line: its byte range [off, off+n) into
 * `text` and its 0-based line index. */
typedef void (*vio_line_cb)(void *user, const char *text, size_t off, size_t n, int index);

/* Break `text` into lines: hard breaks on '\n' always; within a paragraph, when
 * max_w > 0 (physical px) greedily wrap at spaces so no line exceeds max_w. A
 * single word wider than max_w is left to overflow (no mid-word break in v1).
 * Returns the number of lines emitted. */
static int break_lines(hb_font_t *hbf, const char *text, size_t len,
                       float max_w, vio_line_cb cb, void *user)
{
    int index = 0;
    size_t p = 0;
    for (;;) {
        size_t pe = p;
        while (pe < len && text[pe] != '\n') pe++;   /* paragraph [p, pe) */

        if (max_w <= 0.0f) {
            cb(user, text, p, pe - p, index++);       /* no soft wrap */
        } else {
            size_t line_start = p, line_end = p;
            float  line_w = 0.0f;
            int    have = 0;
            size_t i = p;
            while (i < pe) {
                size_t ws = i;                        /* run of spaces before word */
                while (i < pe && is_wrap_space(text[i])) i++;
                float sp_w = shape_range_advance(hbf, text, len, ws, i - ws);
                if (i >= pe) break;                   /* trailing spaces: drop */
                /* Next minimal unbreakable segment: a Latin word (stops at the
                 * next space), a single CJK ideograph, or one Thai cluster —
                 * whichever the UAX #14-lite rules delimit first. */
                size_t wstart = i;
                uint32_t prev = utf8_next(text, pe, &i);
                while (i < pe && !is_wrap_space(text[i])) {
                    size_t j = i;
                    uint32_t cur = utf8_next(text, pe, &j);
                    if (lb_break_between(prev, cur)) break;   /* break opportunity */
                    i = j; prev = cur;
                }
                float w_w = shape_range_advance(hbf, text, len, wstart, i - wstart);

                if (!have) {
                    line_start = wstart; line_end = i; line_w = w_w; have = 1;
                } else if (line_w + sp_w + w_w <= max_w) {
                    line_end = i; line_w += sp_w + w_w;
                } else {
                    cb(user, text, line_start, line_end - line_start, index++);
                    line_start = wstart; line_end = i; line_w = w_w;
                }
            }
            cb(user, text, line_start, have ? (line_end - line_start) : 0, index++);
        }

        if (pe >= len) break;   /* end of text */
        p = pe + 1;             /* skip the '\n' */
        if (p == len) break;    /* a trailing '\n' adds no extra empty line */
    }
    return index;
}

/* ── Draw ───────────────────────────────────────────────────────────── */

typedef struct {
    vio_context_object *ctx;
    vio_font_object    *font;
    vio_shape_atlas    *atlas;
    float baseline_y;   /* logical */
    float pen_x;        /* logical, run origin */
    float z;
    float inv_w, inv_h; /* atlas texel size */
    float inv_rs;       /* physical -> logical */
    float cr, cg, cb, ca;
} vio_draw_ctx;

static void draw_emit(void *user, unsigned int gid, float pen_x, float y_off)
{
    vio_draw_ctx *d = (vio_draw_ctx *)user;
    const vio_glyph_slot *s = shape_atlas_glyph(d->atlas, gid);
    if (!s || s->w == 0 || s->h == 0) return; /* blank / dropped */

    /* Baseline convention matches the legacy path: y is the baseline, glyph
     * top = baseline + bearing_y. HarfBuzz y_offset is y-up, so it subtracts. */
    float px = d->pen_x + (pen_x + s->bearing_x) * d->inv_rs;
    float py = d->baseline_y + (s->bearing_y - y_off) * d->inv_rs;
    float pw = s->w * d->inv_rs;
    float ph = s->h * d->inv_rs;

    float u0 = s->x * d->inv_w;
    float v0 = s->y * d->inv_h;
    float u1 = (s->x + s->w) * d->inv_w;
    float v1 = (s->y + s->h) * d->inv_h;

    float g0x = px,      g0y = py;
    float g1x = px + pw, g1y = py;
    float g2x = px + pw, g2y = py + ph;
    float g3x = px,      g3y = py + ph;
    vio_2d_apply_transform(&d->ctx->state_2d, &g0x, &g0y);
    vio_2d_apply_transform(&d->ctx->state_2d, &g1x, &g1y);
    vio_2d_apply_transform(&d->ctx->state_2d, &g2x, &g2y);
    vio_2d_apply_transform(&d->ctx->state_2d, &g3x, &g3y);

    vio_2d_vertex verts[6] = {
        {g0x, g0y, u0, v0, d->cr, d->cg, d->cb, d->ca},
        {g1x, g1y, u1, v0, d->cr, d->cg, d->cb, d->ca},
        {g2x, g2y, u1, v1, d->cr, d->cg, d->cb, d->ca},
        {g0x, g0y, u0, v0, d->cr, d->cg, d->cb, d->ca},
        {g2x, g2y, u1, v1, d->cr, d->cg, d->cb, d->ca},
        {g3x, g3y, u0, v1, d->cr, d->cg, d->cb, d->ca},
    };
    int start = vio_2d_push_vertices(&d->ctx->state_2d, verts, 6);
    if (start >= 0) {
        vio_2d_push_item(&d->ctx->state_2d, VIO_2D_TEXT, d->z,
                         d->font->atlas_texture, d->font->atlas_backend_texture,
                         start, 6);
    }
}

/* Shape one already-broken line (sub-buffer [text, text+n)) and emit its glyphs
 * starting at d->pen_x / d->baseline_y. BiDi is resolved per line, so each line
 * reorders independently (correct for wrapped mixed-direction paragraphs). */
static void shape_draw_line(vio_draw_ctx *d, hb_font_t *hbf,
                            const char *text, size_t n)
{
    if (n == 0) return;
    const SBRun *runs; SBUInteger rcount;
    SBLineRef line = bidi_resolve(text, n, &runs, &rcount);
    if (line) {
        for (SBUInteger r = 0; r < rcount; r++) {
            vio_bidi_run br;
            br.offset = (size_t)runs[r].offset;
            br.length = (size_t)runs[r].length;
            br.rtl    = (runs[r].level & 1) != 0;
            float adv = shape_run(hbf, text, n, &br, draw_emit, d);
            d->pen_x += adv * d->inv_rs;
        }
        SBLineRelease(line);
    } else {
        vio_bidi_run br = { 0, n, 0 };
        shape_run(hbf, text, n, &br, draw_emit, d);
    }
}

typedef struct {
    vio_draw_ctx *d;
    hb_font_t    *hbf;
    float x;      /* logical line origin x */
    float y0;     /* logical baseline of line 0 */
    float step;   /* logical line advance */
} vio_draw_lines_ctx;

static void draw_line_cb(void *user, const char *text, size_t off, size_t n, int index)
{
    vio_draw_lines_ctx *c = (vio_draw_lines_ctx *)user;
    c->d->pen_x      = c->x;
    c->d->baseline_y = c->y0 + (float)index * c->step;
    shape_draw_line(c->d, c->hbf, text + off, n);
}

void vio_text_shape_draw(vio_context_object *ctx, vio_font_object *font,
                         const char *text, size_t len,
                         float x, float y, float z,
                         float cr, float cg, float cb, float ca,
                         float max_width, float line_height)
{
    vio_shape_atlas *a = (vio_shape_atlas *)font->shape_atlas;
    if (!a || len == 0) return;

    vio_draw_ctx d;
    d.ctx = ctx; d.font = font; d.atlas = a; d.z = z;
    d.baseline_y = y; d.pen_x = x;
    d.inv_w = 1.0f / (float)font->atlas_w;
    d.inv_h = 1.0f / (float)font->atlas_h;
    d.inv_rs = (font->render_scale > 0.0f) ? (1.0f / font->render_scale) : 1.0f;
    d.cr = cr; d.cg = cg; d.cb = cb; d.ca = ca;

    hb_font_t *hbf = (hb_font_t *)font->hb_font;
    float rs = (font->render_scale > 0.0f) ? font->render_scale : 1.0f;
    float max_w_phys = (max_width > 0.0f) ? max_width * rs : 0.0f;
    float step = (line_height > 0.0f) ? line_height : (a->line_height * d.inv_rs);

    vio_draw_lines_ctx c;
    c.d = &d; c.hbf = hbf; c.x = x; c.y0 = y; c.step = step;
    break_lines(hbf, text, len, max_w_phys, draw_line_cb, &c);
    /* No upload here: the atlas was fully built + uploaded once at font
     * creation, so its GPU handle is stable and already holds every glyph. */
}

/* ── Measure ────────────────────────────────────────────────────────── */

typedef struct { vio_shape_atlas *atlas; float width, min_y, max_y; } vio_measure_ctx;

static void measure_emit(void *user, unsigned int gid, float pen_x, float y_off)
{
    (void)pen_x; (void)y_off;
    vio_measure_ctx *m = (vio_measure_ctx *)user;
    /* Vertical extents from the glyph's stored slot metrics (y-down): top =
     * bearing_y, bottom = bearing_y + h. */
    const vio_glyph_slot *s = shape_atlas_glyph(m->atlas, gid);
    if (!s) return;
    float top = (float)s->bearing_y;
    float bot = (float)s->bearing_y + (float)s->h;
    if (top < m->min_y) m->min_y = top;
    if (bot > m->max_y) m->max_y = bot;
}

/* Per-line measure: shape the line once — the returned advance gives its width,
 * measure_emit accumulates vertical extents across all lines (used for the
 * single-line height case). */
typedef struct {
    vio_shape_atlas *atlas;
    hb_font_t       *hbf;
    const char      *text;
    size_t           len;      /* full-buffer length (shaping context) */
    float            inv_rs;
    float            max_line_w; /* logical, widest line */
    float            min_y, max_y;
} vio_measure_lines_ctx;

static void measure_line_cb(void *user, const char *text, size_t off, size_t n, int index)
{
    (void)text; (void)index;
    vio_measure_lines_ctx *c = (vio_measure_lines_ctx *)user;
    vio_measure_ctx m; m.atlas = c->atlas; m.width = 0.0f;
    m.min_y = c->min_y; m.max_y = c->max_y;
    vio_bidi_run br; br.offset = off; br.length = n; br.rtl = 0;
    float adv = (n > 0) ? shape_run(c->hbf, c->text, c->len, &br, measure_emit, &m) : 0.0f;
    float w = adv * c->inv_rs;
    if (w > c->max_line_w) c->max_line_w = w;
    c->min_y = m.min_y; c->max_y = m.max_y;
}

void vio_text_shape_measure(vio_font_object *font,
                            const char *text, size_t len,
                            float max_width, float line_height,
                            float *out_width, float *out_height, int *out_lines)
{
    *out_width = 0.0f; *out_height = 0.0f;
    if (out_lines) *out_lines = 0;
    vio_shape_atlas *a = (vio_shape_atlas *)font->shape_atlas;
    if (!a || len == 0) return;

    hb_font_t *hbf = (hb_font_t *)font->hb_font;
    float rs = (font->render_scale > 0.0f) ? font->render_scale : 1.0f;
    float inv_rs = 1.0f / rs;
    float max_w_phys = (max_width > 0.0f) ? max_width * rs : 0.0f;

    vio_measure_lines_ctx c;
    c.atlas = a; c.hbf = hbf; c.text = text; c.len = len; c.inv_rs = inv_rs;
    c.max_line_w = 0.0f; c.min_y = 0.0f; c.max_y = 0.0f;
    int count = break_lines(hbf, text, len, max_w_phys, measure_line_cb, &c);

    /* Single line keeps the tight glyph-extent height (backward compatible);
     * multi-line uses whole line boxes (natural or overridden line height). */
    float step = (line_height > 0.0f) ? line_height : (a->line_height * inv_rs);
    float height = (count <= 1) ? ((c.max_y - c.min_y) * inv_rs)
                                : ((float)count * step);
    if (height < 0.0f) height = 0.0f;

    *out_width  = c.max_line_w;
    *out_height = height;
    if (out_lines) *out_lines = count;
}

#endif /* HAVE_HARFBUZZ */

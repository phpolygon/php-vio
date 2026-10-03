/*
 * php-vio - GLSL tessellation stages as HLSL hull / domain shaders.
 *
 * SPIRV-Cross's HLSL backend has no hull / domain shaders yet
 * (KhronosGroup/SPIRV-Cross#2693 / #2694 add them). Until a SPIRV-Cross with
 * them is what vio links against, vio builds them itself:
 *
 *   1. The stage's SPIR-V is rewritten into a vertex stage: the execution
 *      model becomes Vertex, the tessellation execution modes and (in the
 *      control stage) barriers are dropped, and the stage's interface
 *      variables become Private globals - all of them in the control stage,
 *      the inputs in the evaluation stage, whose outputs stay real vertex
 *      outputs (SPIRV-Cross then writes SPIRV_Cross_Output and the depth
 *      fixup itself).
 *   2. SPIRV-Cross translates that to HLSL with vio's usual options and
 *      register assignment: statics, the body as vert_main(), a trivial main.
 *   3. vio replaces main with the hull / domain entry points, using the same
 *      layout as the upstream PRs: a hull shader is a patch constant function
 *      that loads the output patch and runs the body for every invocation,
 *      plus a control point function that runs it for its own invocation.
 *
 * The control-point struct and the patch-constant struct are built from the
 * control stage's outputs and are identical in hull and domain shader, so
 * the two signatures always link, even when the evaluation stage reads only
 * part of them.
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "../php_vio.h"
#include "vio_shader.h"
#include "vio_tess_hlsl.h"
#include "vio_shader_compiler.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef HAVE_SPIRV_CROSS

#include "vio_hlsl_internal.h"

/* ── String builder ──────────────────────────────────────────────── */

typedef struct { char *s; size_t n, cap; int oom; } tess_sb;

static void sb_cat(tess_sb *b, const char *t)
{
    size_t l = strlen(t);
    if (b->oom) return;
    if (b->n + l + 1 > b->cap) {
        size_t cap = b->cap ? b->cap : 1024;
        while (b->n + l + 1 > cap) cap *= 2;
        char *g = (char *)realloc(b->s, cap);
        if (!g) { b->oom = 1; return; }
        b->s = g;
        b->cap = cap;
    }
    memcpy(b->s + b->n, t, l + 1);
    b->n += l;
}

static void sb_printf(tess_sb *b, const char *fmt, ...)
{
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n < 0) { b->oom = 1; return; }
    if ((size_t)n < sizeof(buf)) { sb_cat(b, buf); return; }
    char *big = (char *)malloc((size_t)n + 1);
    if (!big) { b->oom = 1; return; }
    va_start(ap, fmt);
    vsnprintf(big, (size_t)n + 1, fmt, ap);
    va_end(ap);
    sb_cat(b, big);
    free(big);
}

static char *tess_error(char **error_msg, const char *fmt, ...)
{
    if (error_msg) {
        char buf[512];
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(buf, sizeof(buf), fmt, ap);
        va_end(ap);
        *error_msg = strdup(buf);
    }
    return NULL;
}

/* ── SPIR-V analysis ─────────────────────────────────────────────── */

enum {
    SPV_EXEC_TESS_CONTROL = 1, SPV_EXEC_TESS_EVAL = 2,
    SPV_STORAGE_INPUT = 1, SPV_STORAGE_OUTPUT = 3, SPV_STORAGE_PRIVATE = 6,
    SPV_BUILTIN_POSITION = 0, SPV_BUILTIN_PRIMITIVE_ID = 7, SPV_BUILTIN_INVOCATION_ID = 8,
    SPV_BUILTIN_TESS_LEVEL_OUTER = 11, SPV_BUILTIN_TESS_LEVEL_INNER = 12,
    SPV_BUILTIN_TESS_COORD = 13, SPV_BUILTIN_PATCH_VERTICES = 14
};

enum { TESS_DOMAIN_NONE = 0, TESS_DOMAIN_TRI, TESS_DOMAIN_QUAD, TESS_DOMAIN_ISO };
enum { TV_USER = 0, TV_PERVERTEX, TV_BUILTIN };

#define TESS_MAX_VARS 96

typedef struct {
    uint32_t id, storage;
    int      kind;            /* TV_USER / TV_PERVERTEX / TV_BUILTIN */
    int      builtin;         /* TV_BUILTIN: SPIR-V BuiltIn */
    int      location;        /* TV_USER */
    int      patch;           /* patch in / patch out */
    uint32_t pv_struct;       /* TV_PERVERTEX: the gl_PerVertex struct type */
    int      pv_pos;          /* TV_PERVERTEX: member index of gl_Position, -1 if none */
    char     type[24];        /* TV_USER: HLSL element type (per control point) */
    char     dims[32];        /* TV_USER: array dimensions behind the name, e.g. "[2]" */
    char     name[128];       /* final HLSL name of the static (after compile) */
} tess_var;

typedef struct {
    const uint32_t *w;
    size_t          n;
    uint32_t        bound;
    size_t         *def;          /* id -> offset of the defining instruction */
    int32_t        *builtin;      /* id -> BuiltIn decoration, -1 */
    int32_t        *location;     /* id -> Location decoration, -1 */
    unsigned char  *patch;        /* id -> Patch decoration */
    unsigned char  *pv;           /* struct id -> has a BuiltIn member */
    int32_t        *pv_pos;       /* struct id -> member index of BuiltIn Position, -1 */
    uint32_t        model, ep;
    int             domain, spacing, order, point_mode;
    uint32_t        output_vertices;
    tess_var        vars[TESS_MAX_VARS];
    int             var_count;
} tess_module;

static void tess_module_free(tess_module *m)
{
    free(m->def); free(m->builtin); free(m->location); free(m->patch); free(m->pv); free(m->pv_pos);
    memset(m, 0, sizeof(*m));
}

static uint32_t tess_constant_u32(const tess_module *m, uint32_t id)
{
    size_t c = id < m->bound ? m->def[id] : 0;
    if (!c || (m->w[c] & 0xFFFF) != 43 /* OpConstant */ || (m->w[c] >> 16) < 4) return 0;
    return m->w[c + 3];
}

/* HLSL type of a scalar / vector type id; 0 if unsupported. */
static int tess_scalar_vector(const tess_module *m, uint32_t id, char *out, size_t cap)
{
    size_t t = id < m->bound ? m->def[id] : 0;
    if (!t) return 0;
    uint32_t op = m->w[t] & 0xFFFF;
    uint32_t count = 1;
    if (op == 23 /* OpTypeVector */) {
        count = m->w[t + 3];
        t = m->w[t + 2] < m->bound ? m->def[m->w[t + 2]] : 0;
        if (!t) return 0;
        op = m->w[t] & 0xFFFF;
    }
    const char *base;
    if (op == 22 /* OpTypeFloat */ && m->w[t + 2] == 32) base = "float";
    else if (op == 21 /* OpTypeInt */ && m->w[t + 2] == 32) base = m->w[t + 3] ? "int" : "uint";
    else return 0;
    if (count == 1) snprintf(out, cap, "%s", base);
    else snprintf(out, cap, "%s%u", base, count);
    return 1;
}

/* Type of an interface variable: strips the outermost (per control point)
 * array when `per_cp`, returns the element type and the remaining array
 * dimensions. Struct elements are reported through *struct_id. */
static int tess_var_type(const tess_module *m, uint32_t pointee, int per_cp, char *type, size_t tcap,
                         char *dims, size_t dcap, uint32_t *struct_id)
{
    uint32_t lens[8];
    int nl = 0;
    uint32_t t = pointee;
    for (;;) {
        size_t d = t < m->bound ? m->def[t] : 0;
        if (!d) return 0;
        if ((m->w[d] & 0xFFFF) != 28 /* OpTypeArray */) break;
        if (nl < 8) lens[nl++] = tess_constant_u32(m, m->w[d + 3]);
        t = m->w[d + 2];
    }
    int first = per_cp ? 1 : 0;
    if (per_cp && nl == 0) return 0;
    dims[0] = '\0';
    for (int k = first; k < nl; k++) {
        size_t l = strlen(dims);
        snprintf(dims + l, dcap - l, "[%u]", lens[k]);
    }
    size_t d = t < m->bound ? m->def[t] : 0;
    if (d && (m->w[d] & 0xFFFF) == 30 /* OpTypeStruct */) {
        *struct_id = t;
        return 1;
    }
    *struct_id = 0;
    return tess_scalar_vector(m, t, type, tcap);
}

static const char *tess_name_of(const tess_module *m, uint32_t id)
{
    /* OpName id "..." - only used in error messages. */
    for (size_t i = 5; i < m->n; ) {
        uint32_t op = m->w[i] & 0xFFFF, wc = m->w[i] >> 16;
        if (!wc) break;
        if (op == 5 && wc >= 3 && m->w[i + 1] == id) return (const char *)&m->w[i + 2];
        i += wc;
    }
    return "?";
}

static int tess_parse(tess_module *m, const uint32_t *w, size_t n, char **error_msg)
{
    memset(m, 0, sizeof(*m));
    if (n < 5 || w[0] != 0x07230203) { tess_error(error_msg, "not a SPIR-V module"); return 0; }
    m->w = w;
    m->n = n;
    m->bound = w[3];
    m->def = (size_t *)calloc(m->bound, sizeof(size_t));
    m->builtin = (int32_t *)malloc(m->bound * sizeof(int32_t));
    m->location = (int32_t *)malloc(m->bound * sizeof(int32_t));
    m->patch = (unsigned char *)calloc(m->bound, 1);
    m->pv = (unsigned char *)calloc(m->bound, 1);
    m->pv_pos = (int32_t *)malloc(m->bound * sizeof(int32_t));
    if (!m->def || !m->builtin || !m->location || !m->patch || !m->pv || !m->pv_pos) {
        tess_module_free(m);
        tess_error(error_msg, "out of memory");
        return 0;
    }
    for (uint32_t i = 0; i < m->bound; i++) { m->builtin[i] = -1; m->location[i] = -1; m->pv_pos[i] = -1; }

    for (size_t i = 5; i < n; ) {
        uint32_t op = w[i] & 0xFFFF, wc = w[i] >> 16;
        if (wc == 0 || i + wc > n) { tess_module_free(m); tess_error(error_msg, "malformed SPIR-V"); return 0; }
        switch (op) {
            case 15: /* OpEntryPoint model id name ... */
                if (!m->ep && wc >= 3) { m->model = w[i + 1]; m->ep = w[i + 2]; }
                break;
            case 16: /* OpExecutionMode ep mode ... */
                if (wc >= 3 && w[i + 1] == m->ep) {
                    switch (w[i + 2]) {
                        case 1:  m->spacing = 1; break;          /* SpacingEqual */
                        case 2:  m->spacing = 2; break;          /* SpacingFractionalEven */
                        case 3:  m->spacing = 3; break;          /* SpacingFractionalOdd */
                        case 4:  m->order = 1; break;            /* VertexOrderCw */
                        case 5:  m->order = 2; break;            /* VertexOrderCcw */
                        case 10: m->point_mode = 1; break;       /* PointMode */
                        case 22: m->domain = TESS_DOMAIN_TRI; break;
                        case 24: m->domain = TESS_DOMAIN_QUAD; break;
                        case 25: m->domain = TESS_DOMAIN_ISO; break;
                        case 26: if (wc >= 4) m->output_vertices = w[i + 3]; break;
                        default: break;
                    }
                }
                break;
            case 71: /* OpDecorate target decoration literals... */
                if (wc >= 3 && w[i + 1] < m->bound) {
                    uint32_t t = w[i + 1], dec = w[i + 2];
                    if (dec == 11 && wc >= 4) m->builtin[t] = (int32_t)w[i + 3];
                    else if (dec == 30 && wc >= 4) m->location[t] = (int32_t)w[i + 3];
                    else if (dec == 15) m->patch[t] = 1;
                }
                break;
            case 72: /* OpMemberDecorate struct member decoration literals... */
                if (wc >= 5 && w[i + 1] < m->bound && w[i + 3] == 11) {
                    m->pv[w[i + 1]] = 1;
                    if (w[i + 4] == SPV_BUILTIN_POSITION) m->pv_pos[w[i + 1]] = (int32_t)w[i + 2];
                }
                break;
            case 19: case 20: case 21: case 22: case 23: case 24: case 25: case 26: case 27:
            case 28: case 29: case 30: case 32: /* types: result id is operand 1 */
                if (wc >= 2 && w[i + 1] < m->bound) m->def[w[i + 1]] = i;
                break;
            case 43: case 59: /* OpConstant / OpVariable: type result ... */
                if (wc >= 3 && w[i + 2] < m->bound) m->def[w[i + 2]] = i;
                break;
            default: break;
        }
        i += wc;
    }
    if (m->model != SPV_EXEC_TESS_CONTROL && m->model != SPV_EXEC_TESS_EVAL) {
        tess_module_free(m);
        tess_error(error_msg, "not a tessellation stage");
        return 0;
    }

    /* Interface variables that become Private: every input, and the outputs
     * of the control stage. */
    for (size_t i = 5; i < n; i += w[i] >> 16) {
        if ((w[i] & 0xFFFF) != 59 || (w[i] >> 16) < 4) continue;
        uint32_t storage = w[i + 3];
        if (storage != SPV_STORAGE_INPUT && !(storage == SPV_STORAGE_OUTPUT && m->model == SPV_EXEC_TESS_CONTROL)) continue;
        if (m->var_count >= TESS_MAX_VARS) { tess_module_free(m); tess_error(error_msg, "too many stage variables"); return 0; }
        tess_var *v = &m->vars[m->var_count];
        memset(v, 0, sizeof(*v));
        v->id = w[i + 2];
        v->storage = storage;
        v->location = m->location[v->id];
        v->patch = m->patch[v->id];
        v->pv_pos = -1;
        size_t pt = w[i + 1] < m->bound ? m->def[w[i + 1]] : 0;
        if (!pt || (w[pt] & 0xFFFF) != 32) { tess_module_free(m); tess_error(error_msg, "malformed variable type"); return 0; }
        uint32_t pointee = w[pt + 3];

        if (m->builtin[v->id] >= 0) {
            v->kind = TV_BUILTIN;
            v->builtin = m->builtin[v->id];
            switch (v->builtin) {
                case SPV_BUILTIN_PRIMITIVE_ID: case SPV_BUILTIN_INVOCATION_ID: case SPV_BUILTIN_PATCH_VERTICES:
                case SPV_BUILTIN_TESS_LEVEL_OUTER: case SPV_BUILTIN_TESS_LEVEL_INNER: case SPV_BUILTIN_TESS_COORD:
                    break;
                default:
                    tess_module_free(m);
                    tess_error(error_msg, "unsupported built-in %d in a tessellation stage", v->builtin);
                    return 0;
            }
            m->var_count++;
            continue;
        }

        int per_cp = !v->patch;
        uint32_t st = 0;
        if (!tess_var_type(m, pointee, per_cp, v->type, sizeof(v->type), v->dims, sizeof(v->dims), &st)) {
            tess_error(error_msg, "'%s': unsupported varying type (scalars, vectors and arrays of them)", tess_name_of(m, v->id));
            tess_module_free(m);
            return 0;
        }
        if (st) {
            if (!m->pv[st] || v->patch) {
                tess_error(error_msg, "'%s': interface blocks and struct varyings are not supported between tessellation stages", tess_name_of(m, v->id));
                tess_module_free(m);
                return 0;
            }
            v->kind = TV_PERVERTEX;
            v->pv_struct = st;
            v->pv_pos = m->pv_pos[st];
        } else {
            v->kind = TV_USER;
            if (v->location < 0) {
                tess_error(error_msg, "'%s': tessellation varyings need a location", tess_name_of(m, v->id));
                tess_module_free(m);
                return 0;
            }
        }
        m->var_count++;
    }
    return 1;
}

static const tess_var *tess_find(const tess_module *m, uint32_t storage, int kind, int builtin_or_location, int patch)
{
    for (int i = 0; i < m->var_count; i++) {
        const tess_var *v = &m->vars[i];
        if (v->storage != storage || v->kind != kind) continue;
        if (kind == TV_BUILTIN && v->builtin != builtin_or_location) continue;
        if (kind == TV_USER && (v->location != builtin_or_location || v->patch != patch)) continue;
        return v;
    }
    return NULL;
}

static int tess_is_converted(const tess_module *m, uint32_t id)
{
    for (int i = 0; i < m->var_count; i++) if (m->vars[i].id == id) return 1;
    return 0;
}

static int tess_is_converted_struct(const tess_module *m, uint32_t id)
{
    for (int i = 0; i < m->var_count; i++) if (m->vars[i].kind == TV_PERVERTEX && m->vars[i].pv_struct == id) return 1;
    return 0;
}

/* ── SPIR-V rewrite ──────────────────────────────────────────────── */

static uint32_t *tess_rewrite(const tess_module *m, size_t *out_n)
{
    const uint32_t *w = m->w;
    /* Room for the renamed structs (OpName below). */
    uint32_t *out = (uint32_t *)malloc((m->n + 8 * TESS_MAX_VARS) * sizeof(uint32_t));
    if (!out) return NULL;
    size_t o = 0;
    for (int h = 0; h < 5; h++) out[o++] = w[h];
    int convert_outputs = m->model == SPV_EXEC_TESS_CONTROL;
    for (size_t i = 5; i < m->n; ) {
        uint32_t op = w[i] & 0xFFFF, wc = w[i] >> 16;
        int drop = 0;
        size_t start = o;
        switch (op) {
            case 16: case 331: /* OpExecutionMode(Id): tessellation modes mean nothing to a vertex stage */
                drop = wc >= 2 && w[i + 1] == m->ep;
                break;
            case 71: case 5632: /* OpDecorate(String) */
                if (wc >= 3 && tess_is_converted(m, w[i + 1])) drop = 1;
                else if (wc >= 3 && w[i + 2] == 2 /* Block */ && tess_is_converted_struct(m, w[i + 1])) drop = 1;
                break;
            case 72: case 5633: /* OpMemberDecorate(String) */
                drop = wc >= 2 && tess_is_converted_struct(m, w[i + 1]);
                break;
            case 224: case 225: /* OpControlBarrier / OpMemoryBarrier */
                drop = m->model == SPV_EXEC_TESS_CONTROL;
                break;
            case 5: /* OpName */
                /* SPIRV-Cross merges structurally identical structs of the
                 * same name: the evaluation stage's input gl_PerVertex would
                 * become an alias of the output block again. A struct that
                 * became Private gets its own name in the module itself
                 * (spvc_compiler_set_name comes after parsing). */
                if (wc >= 2 && tess_is_converted_struct(m, w[i + 1])) {
                    static const char name[] = "vio_PerVertex";   /* 13 chars + NUL = 4 words */
                    out[o++] = (6u << 16) | 5u;
                    out[o++] = w[i + 1];
                    uint32_t packed[4] = { 0, 0, 0, 0 };
                    memcpy(packed, name, sizeof(name));
                    for (int k = 0; k < 4; k++) out[o++] = packed[k];
                    drop = 1;
                }
                break;
            default: break;
        }
        if (!drop) {
            for (uint32_t k = 0; k < wc; k++) out[o++] = w[i + k];
            if (op == 15 /* OpEntryPoint */) out[start + 1] = 0;   /* Vertex */
            else if (op == 32 /* OpTypePointer */ && wc >= 4 &&
                     (w[i + 2] == SPV_STORAGE_INPUT || (convert_outputs && w[i + 2] == SPV_STORAGE_OUTPUT)))
                out[start + 2] = SPV_STORAGE_PRIVATE;
            else if (op == 59 /* OpVariable */ && wc >= 4 && tess_is_converted(m, w[i + 2]))
                out[start + 3] = SPV_STORAGE_PRIVATE;
        }
        i += wc;
    }
    *out_n = o;
    return out;
}

/* ── HLSL wrappers ───────────────────────────────────────────────── */

typedef struct {
    int             stage;          /* VIO_STAGE_TESS_CONTROL / _EVAL */
    tess_module    *self;           /* the module being translated */
    const tess_module *tcs;         /* layout source */
    const tess_module *tes;         /* domain / spacing / winding source */
    uint32_t        output_points, input_points;
} tess_ctx;

static const char *tess_builtin_name(int builtin)
{
    switch (builtin) {
        case SPV_BUILTIN_PRIMITIVE_ID:     return "vio_PrimitiveID";
        case SPV_BUILTIN_INVOCATION_ID:    return "vio_InvocationID";
        case SPV_BUILTIN_PATCH_VERTICES:   return "vio_PatchVerticesIn";
        case SPV_BUILTIN_TESS_LEVEL_OUTER: return "vio_TessLevelOuter";
        case SPV_BUILTIN_TESS_LEVEL_INNER: return "vio_TessLevelInner";
        case SPV_BUILTIN_TESS_COORD:       return "vio_TessCoord";
        default:                           return "vio_builtin";
    }
}

static void tess_configure(spvc_compiler c, spvc_compiler_options options, void *user)
{
    (void)options;
    tess_ctx *t = (tess_ctx *)user;
    for (int i = 0; i < t->self->var_count; i++) {
        tess_var *v = &t->self->vars[i];
        if (v->kind == TV_BUILTIN) spvc_compiler_set_name(c, v->id, tess_builtin_name(v->builtin));
        else if (v->kind == TV_PERVERTEX) {
            spvc_compiler_set_name(c, v->id, v->storage == SPV_STORAGE_INPUT ? "vio_gl_in" : "vio_gl_out");
            spvc_compiler_set_name(c, v->pv_struct, v->storage == SPV_STORAGE_INPUT ? "vio_PerVertexIn" : "vio_PerVertexOut");
            /* gl_Position / gl_PointSize / ... are reserved names. */
            size_t d = t->self->def[v->pv_struct];
            uint32_t members = (t->self->w[d] >> 16) - 2;
            for (uint32_t k = 0; k < members; k++) {
                char nm[32];
                if ((int)k == v->pv_pos) snprintf(nm, sizeof(nm), "vio_Position");
                else snprintf(nm, sizeof(nm), "vio_m%u", k);
                spvc_compiler_set_member_name(c, v->pv_struct, k, nm);
            }
        }
    }
}

static int tess_outer_count(int domain) { return domain == TESS_DOMAIN_QUAD ? 4 : domain == TESS_DOMAIN_ISO ? 2 : 3; }

/* Sorted (by location) indices of a module's user varyings with the given storage / patch flag. */
static int tess_sorted_user(const tess_module *m, uint32_t storage, int patch, int *idx)
{
    int n = 0;
    for (int i = 0; i < m->var_count; i++) {
        const tess_var *v = &m->vars[i];
        if (v->kind != TV_USER || v->storage != storage || v->patch != patch) continue;
        int k = n++;
        while (k > 0 && m->vars[idx[k - 1]].location > v->location) { idx[k] = idx[k - 1]; k--; }
        idx[k] = i;
    }
    return n;
}

/* The shared control-point and patch-constant structs (from the control
 * stage's outputs), and in a hull shader also its input struct. */
static void tess_emit_structs(tess_sb *b, const tess_ctx *t)
{
    int idx[TESS_MAX_VARS], n;
    const tess_module *c = t->tcs;
    int domain = t->tes->domain;

    if (t->stage == VIO_STAGE_TESS_CONTROL) {
        sb_cat(b, "struct VIO_HS_Input\n{\n");
        n = tess_sorted_user(c, SPV_STORAGE_INPUT, 0, idx);
        for (int i = 0; i < n; i++) {
            const tess_var *v = &c->vars[idx[i]];
            sb_printf(b, "    %s vio_l%d%s : TEXCOORD%d;\n", v->type, v->location, v->dims, v->location);
        }
        if (tess_find(c, SPV_STORAGE_INPUT, TV_PERVERTEX, 0, 0)) sb_cat(b, "    float4 vio_pos : SV_Position;\n");
        sb_cat(b, "};\n\n");
    }

    sb_cat(b, "struct VIO_ControlPoint\n{\n");
    n = tess_sorted_user(c, SPV_STORAGE_OUTPUT, 0, idx);
    for (int i = 0; i < n; i++) {
        const tess_var *v = &c->vars[idx[i]];
        sb_printf(b, "    %s vio_l%d%s : TEXCOORD%d;\n", v->type, v->location, v->dims, v->location);
    }
    if (tess_find(c, SPV_STORAGE_OUTPUT, TV_PERVERTEX, 0, 0)) sb_cat(b, "    float4 vio_pos : SV_Position;\n");
    sb_cat(b, "};\n\n");

    sb_cat(b, "struct VIO_PatchConstant\n{\n");
    sb_printf(b, "    float vio_outer[%d] : SV_TessFactor;\n", tess_outer_count(domain));
    if (domain == TESS_DOMAIN_QUAD) sb_cat(b, "    float vio_inner[2] : SV_InsideTessFactor;\n");
    else if (domain == TESS_DOMAIN_TRI) sb_cat(b, "    float vio_inner : SV_InsideTessFactor;\n");
    n = tess_sorted_user(c, SPV_STORAGE_OUTPUT, 1, idx);
    for (int i = 0; i < n; i++) {
        const tess_var *v = &c->vars[idx[i]];
        sb_printf(b, "    %s vio_p%d%s : PATCH%d;\n", v->type, v->location, v->dims, v->location);
    }
    sb_cat(b, "};\n\n");
}

static int tess_has_control_point_outputs(const tess_module *c)
{
    for (int i = 0; i < c->var_count; i++) {
        const tess_var *v = &c->vars[i];
        if (v->storage == SPV_STORAGE_OUTPUT && !v->patch && (v->kind == TV_USER || v->kind == TV_PERVERTEX)) return 1;
    }
    return 0;
}

/* Copy the hull shader's input patch into the statics. */
static void tess_hs_input_copies(tess_sb *b, const tess_ctx *t)
{
    const tess_module *c = t->tcs;
    const tess_var *v;
    if ((v = tess_find(c, SPV_STORAGE_INPUT, TV_BUILTIN, SPV_BUILTIN_PRIMITIVE_ID, 0)))
        sb_printf(b, "    %s = int(vio_prim);\n", v->name);
    if ((v = tess_find(c, SPV_STORAGE_INPUT, TV_BUILTIN, SPV_BUILTIN_PATCH_VERTICES, 0)))
        sb_printf(b, "    %s = %u;\n", v->name, t->input_points);
    int any = 0;
    for (int i = 0; i < c->var_count; i++) {
        v = &c->vars[i];
        if (v->storage != SPV_STORAGE_INPUT || v->kind == TV_BUILTIN) continue;
        if (v->kind == TV_PERVERTEX && v->pv_pos < 0) continue;
        if (!any) { sb_printf(b, "    for (int i = 0; i < %u; i++)\n    {\n", t->input_points); any = 1; }
        if (v->kind == TV_USER) sb_printf(b, "        %s[i] = stage_input[i].vio_l%d;\n", v->name, v->location);
        else sb_printf(b, "        %s[i].vio_Position = stage_input[i].vio_pos;\n", v->name);
    }
    if (any) sb_cat(b, "    }\n");
}

static char *tess_finish_hull(spvc_compiler compiler, char *hlsl, char **error_msg, tess_ctx *t)
{
    const tess_module *c = t->tcs, *e = t->tes;
    char *body_main = strstr(hlsl, "\nvoid main()");
    if (!strstr(hlsl, "void vert_main()") || !body_main) {
        free(hlsl);
        return tess_error(error_msg, "unexpected SPIRV-Cross output for the control stage");
    }

    const char *domain = e->domain == TESS_DOMAIN_TRI ? "tri" : e->domain == TESS_DOMAIN_QUAD ? "quad" : "isoline";
    const char *partitioning = e->spacing == 2 ? "fractional_even" : e->spacing == 3 ? "fractional_odd" : "integer";
    /* OpenGL's ccw is D3D's triangle_cw (same domain coordinates, mirrored
     * origin); no vertex order means ccw. */
    const char *topology = e->point_mode ? "point" : e->domain == TESS_DOMAIN_ISO ? "line"
                         : e->order == 1 ? "triangle_ccw" : "triangle_cw";
    int cp_outputs = tess_has_control_point_outputs(c);
    const tess_var *prim = tess_find(c, SPV_STORAGE_INPUT, TV_BUILTIN, SPV_BUILTIN_PRIMITIVE_ID, 0);
    const tess_var *inv = tess_find(c, SPV_STORAGE_INPUT, TV_BUILTIN, SPV_BUILTIN_INVOCATION_ID, 0);
    const tess_var *outer = tess_find(c, SPV_STORAGE_OUTPUT, TV_BUILTIN, SPV_BUILTIN_TESS_LEVEL_OUTER, 0);
    const tess_var *inner = tess_find(c, SPV_STORAGE_OUTPUT, TV_BUILTIN, SPV_BUILTIN_TESS_LEVEL_INNER, 0);
    (void)compiler;

    tess_sb b = { NULL, 0, 0, 0 };
    *body_main = '\0';
    sb_cat(&b, hlsl);
    sb_cat(&b, "\n");
    tess_emit_structs(&b, t);

    /* Patch constant function: all output control points first, then the body
     * once per invocation, so reads of other control points see final values
     * and patch writes of any invocation land. */
    sb_printf(&b, "VIO_PatchConstant vio_patch_constants(InputPatch<VIO_HS_Input, %u> stage_input", t->input_points);
    if (cp_outputs) sb_printf(&b, ", const OutputPatch<VIO_ControlPoint, %u> stage_output", t->output_points);
    if (prim) sb_cat(&b, ", uint vio_prim : SV_PrimitiveID");
    sb_cat(&b, ")\n{\n");
    tess_hs_input_copies(&b, t);
    if (cp_outputs) {
        sb_printf(&b, "    for (int j = 0; j < %u; j++)\n    {\n", t->output_points);
        for (int i = 0; i < c->var_count; i++) {
            const tess_var *v = &c->vars[i];
            if (v->storage != SPV_STORAGE_OUTPUT || v->patch) continue;
            if (v->kind == TV_USER) sb_printf(&b, "        %s[j] = stage_output[j].vio_l%d;\n", v->name, v->location);
            else if (v->kind == TV_PERVERTEX && v->pv_pos >= 0) sb_printf(&b, "        %s[j].vio_Position = stage_output[j].vio_pos;\n", v->name);
        }
        sb_cat(&b, "    }\n");
    }
    sb_printf(&b, "    for (int vio_invocation = 0; vio_invocation < %u; vio_invocation++)\n    {\n", t->output_points);
    if (inv) sb_printf(&b, "        %s = vio_invocation;\n", inv->name);
    sb_cat(&b, "        vert_main();\n    }\n");
    sb_cat(&b, "    VIO_PatchConstant patch_output;\n");
    for (int k = 0; k < tess_outer_count(e->domain); k++) {
        if (outer) sb_printf(&b, "    patch_output.vio_outer[%d] = %s[%d];\n", k, outer->name, k);
        else sb_printf(&b, "    patch_output.vio_outer[%d] = 0.0f;\n", k);
    }
    if (e->domain == TESS_DOMAIN_QUAD) {
        for (int k = 0; k < 2; k++) {
            if (inner) sb_printf(&b, "    patch_output.vio_inner[%d] = %s[%d];\n", k, inner->name, k);
            else sb_printf(&b, "    patch_output.vio_inner[%d] = 0.0f;\n", k);
        }
    } else if (e->domain == TESS_DOMAIN_TRI) {
        if (inner) sb_printf(&b, "    patch_output.vio_inner = %s[0];\n", inner->name);
        else sb_cat(&b, "    patch_output.vio_inner = 0.0f;\n");
    }
    for (int i = 0; i < c->var_count; i++) {
        const tess_var *v = &c->vars[i];
        if (v->kind == TV_USER && v->storage == SPV_STORAGE_OUTPUT && v->patch)
            sb_printf(&b, "    patch_output.vio_p%d = %s;\n", v->location, v->name);
    }
    sb_cat(&b, "    return patch_output;\n}\n\n");

    /* Control point function. */
    sb_printf(&b, "[domain(\"%s\")]\n[partitioning(\"%s\")]\n[outputtopology(\"%s\")]\n[outputcontrolpoints(%u)]\n"
                  "[patchconstantfunc(\"vio_patch_constants\")]\n",
              domain, partitioning, topology, t->output_points);
    sb_printf(&b, "%s main(InputPatch<VIO_HS_Input, %u> stage_input, uint vio_cp : SV_OutputControlPointID",
              cp_outputs ? "VIO_ControlPoint" : "void", t->input_points);
    if (prim) sb_cat(&b, ", uint vio_prim : SV_PrimitiveID");
    sb_cat(&b, ")\n{\n");
    tess_hs_input_copies(&b, t);
    if (inv) sb_printf(&b, "    %s = int(vio_cp);\n", inv->name);
    sb_cat(&b, "    vert_main();\n");
    if (cp_outputs) {
        sb_cat(&b, "    VIO_ControlPoint stage_output;\n");
        for (int i = 0; i < c->var_count; i++) {
            const tess_var *v = &c->vars[i];
            if (v->storage != SPV_STORAGE_OUTPUT || v->patch) continue;
            if (v->kind == TV_USER) sb_printf(&b, "    stage_output.vio_l%d = %s[vio_cp];\n", v->location, v->name);
            else if (v->kind == TV_PERVERTEX && v->pv_pos >= 0) sb_printf(&b, "    stage_output.vio_pos = %s[vio_cp].vio_Position;\n", v->name);
        }
        sb_cat(&b, "    return stage_output;\n");
    }
    sb_cat(&b, "}\n");
    free(hlsl);
    if (b.oom) { free(b.s); return tess_error(error_msg, "out of memory"); }
    return b.s;
}

static char *tess_finish_domain(spvc_compiler compiler, char *hlsl, char **error_msg, tess_ctx *t)
{
    const tess_module *c = t->tcs, *e = t->tes;
    (void)compiler;
    const char *sig = NULL;
    char *at = NULL;
    static const char *const sigs[] = { "\nSPIRV_Cross_Output main()\n{\n", "\nvoid main()\n{\n" };
    for (int k = 0; k < 2 && !at; k++) { at = strstr(hlsl, sigs[k]); sig = sigs[k]; }
    if (!strstr(hlsl, "void vert_main()") || !at) {
        free(hlsl);
        return tess_error(error_msg, "unexpected SPIRV-Cross output for the evaluation stage");
    }

    /* Everything the evaluation stage reads must come from the control stage. */
    for (int i = 0; i < e->var_count; i++) {
        const tess_var *v = &e->vars[i];
        if (v->kind == TV_USER && !tess_find(c, SPV_STORAGE_OUTPUT, TV_USER, v->location, v->patch)) {
            free(hlsl);
            return tess_error(error_msg, "the evaluation stage reads %s location %d, which the control stage does not write",
                              v->patch ? "patch" : "per-vertex", v->location);
        }
        if (v->kind == TV_USER) {
            const tess_var *src = tess_find(c, SPV_STORAGE_OUTPUT, TV_USER, v->location, v->patch);
            if (strcmp(src->type, v->type) != 0 || strcmp(src->dims, v->dims) != 0) {
                free(hlsl);
                return tess_error(error_msg, "location %d is %s%s in the control stage but %s%s in the evaluation stage",
                                  v->location, src->type, src->dims, v->type, v->dims);
            }
        }
        if (v->kind == TV_PERVERTEX && v->pv_pos >= 0 && !tess_find(c, SPV_STORAGE_OUTPUT, TV_PERVERTEX, 0, 0)) {
            free(hlsl);
            return tess_error(error_msg, "the evaluation stage reads gl_in[].gl_Position, which the control stage does not write");
        }
    }

    int cp_outputs = tess_has_control_point_outputs(c);
    const tess_var *coord = tess_find(e, SPV_STORAGE_INPUT, TV_BUILTIN, SPV_BUILTIN_TESS_COORD, 0);
    const tess_var *prim = tess_find(e, SPV_STORAGE_INPUT, TV_BUILTIN, SPV_BUILTIN_PRIMITIVE_ID, 0);
    const tess_var *pverts = tess_find(e, SPV_STORAGE_INPUT, TV_BUILTIN, SPV_BUILTIN_PATCH_VERTICES, 0);
    const tess_var *outer = tess_find(e, SPV_STORAGE_INPUT, TV_BUILTIN, SPV_BUILTIN_TESS_LEVEL_OUTER, 0);
    const tess_var *inner = tess_find(e, SPV_STORAGE_INPUT, TV_BUILTIN, SPV_BUILTIN_TESS_LEVEL_INNER, 0);
    const char *ret = sig[1] == 'S' ? "SPIRV_Cross_Output" : "void";

    tess_sb b = { NULL, 0, 0, 0 };
    *at = '\0';
    sb_cat(&b, hlsl);
    sb_cat(&b, "\n");
    tess_emit_structs(&b, t);
    sb_printf(&b, "[domain(\"%s\")]\n", e->domain == TESS_DOMAIN_TRI ? "tri" : e->domain == TESS_DOMAIN_QUAD ? "quad" : "isoline");
    sb_printf(&b, "%s main(", ret);
    int first = 1;
    if (cp_outputs) { sb_printf(&b, "const OutputPatch<VIO_ControlPoint, %u> stage_input", t->output_points); first = 0; }
    sb_printf(&b, "%sconst VIO_PatchConstant patch_input", first ? "" : ", ");
    if (coord) sb_printf(&b, ", %s vio_dl : SV_DomainLocation", e->domain == TESS_DOMAIN_TRI ? "float3" : "float2");
    if (prim) sb_cat(&b, ", uint vio_prim : SV_PrimitiveID");
    sb_cat(&b, ")\n{\n");

    if (coord) {
        if (e->domain == TESS_DOMAIN_TRI) sb_printf(&b, "    %s = vio_dl;\n", coord->name);
        else sb_printf(&b, "    %s = float3(vio_dl, 0.0f);\n", coord->name);
    }
    if (prim) sb_printf(&b, "    %s = int(vio_prim);\n", prim->name);
    if (pverts) sb_printf(&b, "    %s = %u;\n", pverts->name, t->output_points);
    if (outer) for (int k = 0; k < tess_outer_count(e->domain); k++)
        sb_printf(&b, "    %s[%d] = patch_input.vio_outer[%d];\n", outer->name, k, k);
    if (inner) {
        if (e->domain == TESS_DOMAIN_QUAD) {
            sb_printf(&b, "    %s[0] = patch_input.vio_inner[0];\n", inner->name);
            sb_printf(&b, "    %s[1] = patch_input.vio_inner[1];\n", inner->name);
        } else if (e->domain == TESS_DOMAIN_TRI) sb_printf(&b, "    %s[0] = patch_input.vio_inner;\n", inner->name);
    }
    int any = 0;
    for (int i = 0; i < e->var_count; i++) {
        const tess_var *v = &e->vars[i];
        if (v->kind == TV_USER && v->patch) sb_printf(&b, "    %s = patch_input.vio_p%d;\n", v->name, v->location);
    }
    for (int i = 0; i < e->var_count; i++) {
        const tess_var *v = &e->vars[i];
        if (v->kind == TV_BUILTIN || v->patch || (v->kind == TV_PERVERTEX && v->pv_pos < 0)) continue;
        if (!any) { sb_printf(&b, "    for (int i = 0; i < %u; i++)\n    {\n", t->output_points); any = 1; }
        if (v->kind == TV_USER) sb_printf(&b, "        %s[i] = stage_input[i].vio_l%d;\n", v->name, v->location);
        else sb_printf(&b, "        %s[i].vio_Position = stage_input[i].vio_pos;\n", v->name);
    }
    if (any) sb_cat(&b, "    }\n");
    /* The rest of SPIRV-Cross's main: vert_main(), outputs, return. */
    sb_cat(&b, at + strlen(sig));
    free(hlsl);
    if (b.oom) { free(b.s); return tess_error(error_msg, "out of memory"); }
    return b.s;
}

static char *tess_finish(spvc_compiler compiler, char *hlsl, char **error_msg, void *user)
{
    tess_ctx *t = (tess_ctx *)user;
    for (int i = 0; i < t->self->var_count; i++) {
        tess_var *v = &t->self->vars[i];
        const char *nm = spvc_compiler_get_name(compiler, v->id);
        snprintf(v->name, sizeof(v->name), "%s", nm ? nm : "");
        if (!v->name[0]) { free(hlsl); return tess_error(error_msg, "unnamed stage variable %u", v->id); }
    }
    return t->stage == VIO_STAGE_TESS_CONTROL ? tess_finish_hull(compiler, hlsl, error_msg, t)
                                              : tess_finish_domain(compiler, hlsl, error_msg, t);
}

char *vio_tess_to_hlsl(int stage, const vio_tess_hlsl_desc *desc, char **error_msg)
{
    if (!desc || !desc->tcs || !desc->tes) return tess_error(error_msg, "both tessellation stages are required");
    if (stage != VIO_STAGE_TESS_CONTROL && stage != VIO_STAGE_TESS_EVAL) return tess_error(error_msg, "not a tessellation stage");

    tess_module tcs, tes;
    if (!tess_parse(&tcs, (const uint32_t *)desc->tcs, desc->tcs_size / 4, error_msg)) return NULL;
    if (!tess_parse(&tes, (const uint32_t *)desc->tes, desc->tes_size / 4, error_msg)) { tess_module_free(&tcs); return NULL; }

    char *result = NULL;
    if (tcs.model != SPV_EXEC_TESS_CONTROL || tes.model != SPV_EXEC_TESS_EVAL) {
        tess_error(error_msg, "expected a tessellation control and a tessellation evaluation stage");
    } else if (!tcs.output_vertices) {
        tess_error(error_msg, "the control stage declares no output vertices");
    } else if (tes.domain == TESS_DOMAIN_NONE) {
        tess_error(error_msg, "the evaluation stage declares no triangles / quads / isolines domain");
    } else {
        tess_ctx t;
        t.stage = stage;
        t.self = stage == VIO_STAGE_TESS_CONTROL ? &tcs : &tes;
        t.tcs = &tcs;
        t.tes = &tes;
        t.output_points = tcs.output_vertices;
        t.input_points = desc->input_points ? desc->input_points : tcs.output_vertices;
        size_t words = 0;
        uint32_t *rewritten = tess_rewrite(t.self, &words);
        if (!rewritten) tess_error(error_msg, "out of memory");
        else {
            const char *dump = getenv("VIO_DUMP_TESS_SPV");
            if (dump) {
                /* The rewritten module, for spirv-dis / spirv-cross by hand. */
                char path[512];
                snprintf(path, sizeof(path), "%s.%s.spv", dump, stage == VIO_STAGE_TESS_CONTROL ? "tesc" : "tese");
                FILE *f = fopen(path, "wb");
                if (f) { fwrite(rewritten, 4, words, f); fclose(f); }
            }
            vio_hlsl_hooks hooks = { tess_configure, tess_finish, &t };
            result = vio_spirv_to_hlsl_hooked(rewritten, words, desc->shader_model,
                                              stage == VIO_STAGE_TESS_EVAL ? desc->fixup_depth : 0, &hooks, error_msg);
            free(rewritten);
            if (result && getenv("VIO_DUMP_HLSL")) {
                fprintf(stderr, "==== %s HLSL (vio) ====\n%s\n==== end ====\n",
                        stage == VIO_STAGE_TESS_CONTROL ? "hull" : "domain", result);
                fflush(stderr);
            }
        }
    }
    tess_module_free(&tcs);
    tess_module_free(&tes);
    return result;
}

uint32_t vio_tess_output_vertices(const void *tcs, size_t tcs_size)
{
    const uint32_t *w = (const uint32_t *)tcs;
    size_t n = tcs_size / 4;
    if (!w || n < 5 || w[0] != 0x07230203) return 0;
    for (size_t i = 5; i < n; ) {
        uint32_t op = w[i] & 0xFFFF, wc = w[i] >> 16;
        if (wc == 0 || i + wc > n) return 0;
        if (op == 16 && wc >= 4 && w[i + 2] == 26 /* OutputVertices */) return w[i + 3];
        i += wc;
    }
    return 0;
}

#endif /* HAVE_SPIRV_CROSS */

uint32_t *vio_tess_stage_spirv(const void *data, size_t size, int stage, size_t *out_size, char **error_msg)
{
    if (!data || !size) { if (error_msg) *error_msg = strdup("empty stage"); return NULL; }
    if (size >= 4 && *(const uint32_t *)data == 0x07230203) {
        uint32_t *copy = (uint32_t *)malloc(size);
        if (!copy) { if (error_msg) *error_msg = strdup("out of memory"); return NULL; }
        memcpy(copy, data, size);
        *out_size = size;
        return copy;
    }
    return vio_compile_glsl_stage_to_spirv((const char *)data, stage, out_size, error_msg);
}

#ifndef HAVE_SPIRV_CROSS

char *vio_tess_to_hlsl(int stage, const vio_tess_hlsl_desc *desc, char **error_msg)
{
    (void)stage; (void)desc;
    if (error_msg) *error_msg = strdup("spirv-cross not available (compile with --with-spirv-cross)");
    return NULL;
}

uint32_t vio_tess_output_vertices(const void *tcs, size_t tcs_size)
{
    (void)tcs; (void)tcs_size;
    return 0;
}

#endif /* HAVE_SPIRV_CROSS */

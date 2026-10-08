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
 * Varyings are lists of fields (OPEN-ITEMS-PLAN A29): a plain varying is one
 * field, an interface block or struct varying one field per member (its
 * location), matrices take one location per column. gl_ClipDistance travels
 * through gl_out / gl_in as packed SV_ClipDistance vectors when the stages
 * use it. A control stage with barrier() runs every invocation once per
 * barrier phase in each control point, so reads of other control points see
 * their writes.
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
    SPV_BUILTIN_POSITION = 0, SPV_BUILTIN_CLIP_DISTANCE = 3, SPV_BUILTIN_PRIMITIVE_ID = 7, SPV_BUILTIN_INVOCATION_ID = 8,
    SPV_BUILTIN_TESS_LEVEL_OUTER = 11, SPV_BUILTIN_TESS_LEVEL_INNER = 12,
    SPV_BUILTIN_TESS_COORD = 13, SPV_BUILTIN_PATCH_VERTICES = 14
};

enum { TESS_DOMAIN_NONE = 0, TESS_DOMAIN_TRI, TESS_DOMAIN_QUAD, TESS_DOMAIN_ISO };
enum { TV_USER = 0, TV_PERVERTEX, TV_BUILTIN, TV_STRUCT };

#define TESS_MAX_VARS 96
#define TESS_MAX_FIELDS 160

/* One location range of a user varying: the varying itself, or one member of
 * an interface block / struct varying. */
typedef struct {
    int  location;
    char type[24];            /* HLSL element type */
    char dims[32];            /* array dimensions behind the name */
    char access[16];          /* "" or ".vio_m<k>" (struct member) */
} tess_field;
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
    int      first_field, field_count;   /* TV_USER / TV_STRUCT: its fields in tess_module.fields */
    int      clip_member, clip_len;      /* TV_PERVERTEX: gl_ClipDistance member, -1 if none */
    int      clip_used;                  /* TV_PERVERTEX: the stage accesses gl_ClipDistance */
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
    int32_t        *pv_clip;      /* struct id -> member index of BuiltIn ClipDistance, -1 */
    unsigned char  *spatch;       /* struct id -> a member carries Patch */
    uint32_t       *mloc;         /* (struct, member, location) triples */
    int             mloc_count, mloc_cap;
    int             barriers;     /* OpControlBarrier count */
    uint32_t        model, ep;
    int             domain, spacing, order, point_mode;
    uint32_t        output_vertices;
    tess_var        vars[TESS_MAX_VARS];
    int             var_count;
    tess_field      fields[TESS_MAX_FIELDS];
    int             field_count;
} tess_module;

static void tess_module_free(tess_module *m)
{
    free(m->def); free(m->builtin); free(m->location); free(m->patch); free(m->pv); free(m->pv_pos);
    free(m->pv_clip); free(m->spatch); free(m->mloc);
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
    if (op == 24 /* OpTypeMatrix: floatCxR like SPIRV-Cross (columns x column size) */) {
        uint32_t cols = m->w[t + 3];
        size_t ct = m->w[t + 2] < m->bound ? m->def[m->w[t + 2]] : 0;
        if (!ct || (m->w[ct] & 0xFFFF) != 23) return 0;
        uint32_t rows = m->w[ct + 3];
        size_t ft = m->w[ct + 2] < m->bound ? m->def[m->w[ct + 2]] : 0;
        if (!ft || (m->w[ft] & 0xFFFF) != 22 || m->w[ft + 2] != 32) return 0;
        snprintf(out, cap, "float%ux%u", cols, rows);
        return 1;
    }
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

static int tess_is_converted(const tess_module *m, uint32_t id);

static int tess_member_location(const tess_module *m, uint32_t st, uint32_t mem)
{
    for (int i = 0; i < m->mloc_count; i++)
        if (m->mloc[3 * i] == st && m->mloc[3 * i + 1] == mem) return (int)m->mloc[3 * i + 2];
    return -1;
}

/* Locations a type takes: one per column of a matrix, times every array length. */
static int tess_type_locations(const tess_module *m, uint32_t t)
{
    size_t d = t < m->bound ? m->def[t] : 0;
    if (!d) return 1;
    uint32_t op = m->w[d] & 0xFFFF;
    if (op == 28) return (int)tess_constant_u32(m, m->w[d + 3]) * tess_type_locations(m, m->w[d + 2]);
    if (op == 24) return (int)m->w[d + 3];
    return 1;
}

/* The fields of an interface block / struct varying: members in order, each at
 * its Location decoration or right behind the previous member. */
static int tess_struct_fields(tess_module *m, tess_var *v, char **error_msg)
{
    size_t d = m->def[v->pv_struct];
    uint32_t members = (m->w[d] >> 16) - 2;
    int loc = v->location;
    v->first_field = m->field_count;
    for (uint32_t k = 0; k < members; k++) {
        uint32_t mt = m->w[d + 2 + k];
        int ml = tess_member_location(m, v->pv_struct, k);
        if (ml >= 0) loc = ml;
        if (loc < 0) { tess_error(error_msg, "'%s': tessellation varyings need a location", tess_name_of(m, v->id)); return 0; }
        if (m->field_count >= TESS_MAX_FIELDS) { tess_error(error_msg, "too many tessellation varyings"); return 0; }
        tess_field *f = &m->fields[m->field_count];
        uint32_t nested = 0;
        if (!tess_var_type(m, mt, 0, f->type, sizeof(f->type), f->dims, sizeof(f->dims), &nested) || nested) {
            tess_error(error_msg, "'%s': member %u: unsupported varying type (scalars, vectors, matrices and arrays of them)",
                       tess_name_of(m, v->id), k);
            return 0;
        }
        f->location = loc;
        snprintf(f->access, sizeof(f->access), ".vio_m%u", k);
        m->field_count++;
        loc += tess_type_locations(m, mt);
    }
    v->field_count = m->field_count - v->first_field;
    return 1;
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
    m->pv_clip = (int32_t *)malloc(m->bound * sizeof(int32_t));
    m->spatch = (unsigned char *)calloc(m->bound, 1);
    if (!m->def || !m->builtin || !m->location || !m->patch || !m->pv || !m->pv_pos || !m->pv_clip || !m->spatch) {
        tess_module_free(m);
        tess_error(error_msg, "out of memory");
        return 0;
    }
    for (uint32_t i = 0; i < m->bound; i++) { m->builtin[i] = -1; m->location[i] = -1; m->pv_pos[i] = -1; m->pv_clip[i] = -1; }

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
                if (wc >= 4 && w[i + 1] < m->bound) {
                    uint32_t st = w[i + 1], mem = w[i + 2], dec = w[i + 3];
                    if (dec == 11 && wc >= 5) {
                        m->pv[st] = 1;
                        if (w[i + 4] == SPV_BUILTIN_POSITION) m->pv_pos[st] = (int32_t)mem;
                        else if (w[i + 4] == SPV_BUILTIN_CLIP_DISTANCE) m->pv_clip[st] = (int32_t)mem;
                    } else if (dec == 15) {
                        m->spatch[st] = 1;
                    } else if (dec == 30 && wc >= 5) {
                        if (m->mloc_count == m->mloc_cap) {
                            int cap = m->mloc_cap ? m->mloc_cap * 2 : 32;
                            uint32_t *g = (uint32_t *)realloc(m->mloc, (size_t)cap * 3 * sizeof(uint32_t));
                            if (!g) { tess_module_free(m); tess_error(error_msg, "out of memory"); return 0; }
                            m->mloc = g;
                            m->mloc_cap = cap;
                        }
                        m->mloc[3 * m->mloc_count] = st;
                        m->mloc[3 * m->mloc_count + 1] = mem;
                        m->mloc[3 * m->mloc_count + 2] = w[i + 4];
                        m->mloc_count++;
                    }
                }
                break;
            case 224: /* OpControlBarrier */
                m->barriers++;
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

        /* A patch block may carry Patch on its members instead of the variable. */
        {
            size_t pd = pointee < m->bound ? m->def[pointee] : 0;
            if (pd && (w[pd] & 0xFFFF) == 30 && m->spatch[pointee]) v->patch = 1;
        }
        int per_cp = !v->patch;
        uint32_t st = 0;
        v->clip_member = -1;
        if (!tess_var_type(m, pointee, per_cp, v->type, sizeof(v->type), v->dims, sizeof(v->dims), &st)) {
            tess_error(error_msg, "'%s': unsupported varying type (scalars, vectors, matrices and arrays of them)", tess_name_of(m, v->id));
            tess_module_free(m);
            return 0;
        }
        if (st && m->pv[st]) {
            if (v->patch) {
                tess_error(error_msg, "'%s': gl_PerVertex cannot be a patch varying", tess_name_of(m, v->id));
                tess_module_free(m);
                return 0;
            }
            v->kind = TV_PERVERTEX;
            v->pv_struct = st;
            v->pv_pos = m->pv_pos[st];
            v->clip_member = m->pv_clip[st];
            if (v->clip_member >= 0) {
                size_t sd = m->def[st];
                uint32_t ct = m->w[sd + 2 + v->clip_member];
                size_t ad = ct < m->bound ? m->def[ct] : 0;
                v->clip_len = ad && (m->w[ad] & 0xFFFF) == 28 ? (int)tess_constant_u32(m, m->w[ad + 3]) : 1;
            }
        } else if (st) {
            /* Interface block or struct varying: one field per member. */
            if (v->dims[0]) {
                tess_error(error_msg, "'%s': arrays of blocks / structs are not supported between tessellation stages", tess_name_of(m, v->id));
                tess_module_free(m);
                return 0;
            }
            v->kind = TV_STRUCT;
            v->pv_struct = st;
            if (!tess_struct_fields(m, v, error_msg)) { tess_module_free(m); return 0; }
        } else {
            v->kind = TV_USER;
            if (v->location < 0) {
                tess_error(error_msg, "'%s': tessellation varyings need a location", tess_name_of(m, v->id));
                tess_module_free(m);
                return 0;
            }
            if (m->field_count >= TESS_MAX_FIELDS) {
                tess_error(error_msg, "too many tessellation varyings");
                tess_module_free(m);
                return 0;
            }
            tess_field *f = &m->fields[m->field_count];
            f->location = v->location;
            snprintf(f->type, sizeof(f->type), "%s", v->type);
            snprintf(f->dims, sizeof(f->dims), "%s", v->dims);
            f->access[0] = '\0';
            v->first_field = m->field_count++;
            v->field_count = 1;
        }
        m->var_count++;
    }

    /* Which gl_PerVertex variables touch gl_ClipDistance (access chains through it). */
    for (size_t i = 5; i < n; i += w[i] >> 16) {
        uint32_t op = w[i] & 0xFFFF, wc = w[i] >> 16;
        if ((op != 65 && op != 66) || wc < 6) continue;   /* OpAccessChain / OpInBoundsAccessChain base cp member */
        for (int k = 0; k < m->var_count; k++) {
            tess_var *v = &m->vars[k];
            if (v->kind != TV_PERVERTEX || v->id != w[i + 3] || v->clip_member < 0) continue;
            if (tess_constant_u32(m, w[i + 5]) == (uint32_t)v->clip_member) v->clip_used = 1;
        }
    }

    /* A struct type that became Private must not also type a real output (its
     * member decorations are dropped). */
    for (size_t i = 5; i < n; i += w[i] >> 16) {
        if ((w[i] & 0xFFFF) != 59 || (w[i] >> 16) < 4 || w[i + 3] != SPV_STORAGE_OUTPUT || tess_is_converted(m, w[i + 2])) continue;
        size_t pt = w[i + 1] < m->bound ? m->def[w[i + 1]] : 0;
        uint32_t t = pt ? w[pt + 3] : 0;
        for (size_t d = t < m->bound ? m->def[t] : 0; d && (w[d] & 0xFFFF) == 28; d = t < m->bound ? m->def[t] : 0) t = w[d + 2];
        for (int k = 0; k < m->var_count; k++) {
            if (m->vars[k].kind == TV_STRUCT && m->vars[k].pv_struct == t) {
                tess_error(error_msg, "'%s': the evaluation stage's struct type is used for an input and an output", tess_name_of(m, m->vars[k].id));
                tess_module_free(m);
                return 0;
            }
        }
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
    for (int i = 0; i < m->var_count; i++)
        if ((m->vars[i].kind == TV_PERVERTEX || m->vars[i].kind == TV_STRUCT) && m->vars[i].pv_struct == id)
            return m->vars[i].kind;
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
                    char name[24];
                    if (tess_is_converted_struct(m, w[i + 1]) == TV_PERVERTEX) snprintf(name, sizeof(name), "vio_PerVertex");
                    else snprintf(name, sizeof(name), "vio_S%u", w[i + 1]);
                    size_t len = strlen(name) + 1;
                    uint32_t words = (uint32_t)((len + 3) / 4);
                    out[o++] = ((2u + words) << 16) | 5u;
                    out[o++] = w[i + 1];
                    uint32_t packed[6] = { 0, 0, 0, 0, 0, 0 };
                    memcpy(packed, name, len);
                    for (uint32_t k = 0; k < words; k++) out[o++] = packed[k];
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
        else if (v->kind == TV_STRUCT) {
            uint32_t members = (t->self->w[t->self->def[v->pv_struct]] >> 16) - 2;
            for (uint32_t k = 0; k < members; k++) {
                char nm[16];
                snprintf(nm, sizeof(nm), "vio_m%u", k);
                spvc_compiler_set_member_name(c, v->pv_struct, k, nm);
            }
        }
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

/* The fields of a module's user varyings (plain ones and struct / block
 * members) with the given storage / patch flag, sorted by location. */
typedef struct { const tess_var *v; const tess_field *f; } tess_fref;

static int tess_sorted_fields(const tess_module *m, uint32_t storage, int patch, tess_fref *out)
{
    int n = 0;
    for (int i = 0; i < m->var_count; i++) {
        const tess_var *v = &m->vars[i];
        if ((v->kind != TV_USER && v->kind != TV_STRUCT) || v->storage != storage || v->patch != patch) continue;
        for (int j = 0; j < v->field_count && n < TESS_MAX_FIELDS; j++) {
            const tess_field *f = &m->fields[v->first_field + j];
            int k = n++;
            while (k > 0 && out[k - 1].f->location > f->location) { out[k] = out[k - 1]; k--; }
            out[k].v = v;
            out[k].f = f;
        }
    }
    return n;
}

static const tess_field *tess_find_field(const tess_module *m, uint32_t storage, int location, int patch)
{
    tess_fref fr[TESS_MAX_FIELDS];
    int n = tess_sorted_fields(m, storage, patch, fr);
    for (int i = 0; i < n; i++) if (fr[i].f->location == location) return fr[i].f;
    return NULL;
}

/* gl_ClipDistance as SPIRV-Cross packs it: float / float2..4 vectors, index c / 4. */
static void tess_emit_clip_fields(tess_sb *b, int len, const char *semantic)
{
    for (int c = 0; c < len; c += 4) {
        int k = len - c > 4 ? 4 : len - c;
        if (k == 1) sb_printf(b, "    float vio_clip%d : %s%d;\n", c / 4, semantic, c / 4);
        else sb_printf(b, "    float%d vio_clip%d : %s%d;\n", k, c / 4, semantic, c / 4);
    }
}

static void tess_clip_ref(char *out, size_t cap, int len, int c)
{
    int width = len - (c / 4) * 4;
    if (width > 4) width = 4;
    if (width == 1) snprintf(out, cap, "vio_clip%d", c / 4);
    else snprintf(out, cap, "vio_clip%d.%c", c / 4, "xyzw"[c % 4]);
}

/* The control stage's gl_out, when it carries gl_ClipDistance to the evaluation stage. */
static const tess_var *tess_clip_out(const tess_module *c)
{
    const tess_var *pv = tess_find(c, SPV_STORAGE_OUTPUT, TV_PERVERTEX, 0, 0);
    return pv && pv->clip_used ? pv : NULL;
}

/* The shared control-point and patch-constant structs (from the control
 * stage's outputs), and in a hull shader also its input struct. */
static void tess_emit_structs(tess_sb *b, const tess_ctx *t)
{
    tess_fref fr[TESS_MAX_FIELDS];
    int n;
    const tess_module *c = t->tcs;
    int domain = t->tes->domain;

    if (t->stage == VIO_STAGE_TESS_CONTROL) {
        sb_cat(b, "struct VIO_HS_Input\n{\n");
        n = tess_sorted_fields(c, SPV_STORAGE_INPUT, 0, fr);
        for (int i = 0; i < n; i++)
            sb_printf(b, "    %s vio_l%d%s : TEXCOORD%d;\n", fr[i].f->type, fr[i].f->location, fr[i].f->dims, fr[i].f->location);
        const tess_var *pv = tess_find(c, SPV_STORAGE_INPUT, TV_PERVERTEX, 0, 0);
        if (pv) sb_cat(b, "    float4 vio_pos : SV_Position;\n");
        if (pv && pv->clip_used) tess_emit_clip_fields(b, pv->clip_len, "SV_ClipDistance");
        sb_cat(b, "};\n\n");
    }

    sb_cat(b, "struct VIO_ControlPoint\n{\n");
    n = tess_sorted_fields(c, SPV_STORAGE_OUTPUT, 0, fr);
    for (int i = 0; i < n; i++)
        sb_printf(b, "    %s vio_l%d%s : TEXCOORD%d;\n", fr[i].f->type, fr[i].f->location, fr[i].f->dims, fr[i].f->location);
    if (tess_find(c, SPV_STORAGE_OUTPUT, TV_PERVERTEX, 0, 0)) sb_cat(b, "    float4 vio_pos : SV_Position;\n");
    if (tess_clip_out(c)) tess_emit_clip_fields(b, tess_clip_out(c)->clip_len, "VIO_CLIP");
    sb_cat(b, "};\n\n");

    sb_cat(b, "struct VIO_PatchConstant\n{\n");
    sb_printf(b, "    float vio_outer[%d] : SV_TessFactor;\n", tess_outer_count(domain));
    if (domain == TESS_DOMAIN_QUAD) sb_cat(b, "    float vio_inner[2] : SV_InsideTessFactor;\n");
    else if (domain == TESS_DOMAIN_TRI) sb_cat(b, "    float vio_inner : SV_InsideTessFactor;\n");
    n = tess_sorted_fields(c, SPV_STORAGE_OUTPUT, 1, fr);
    for (int i = 0; i < n; i++)
        sb_printf(b, "    %s vio_p%d%s : PATCH%d;\n", fr[i].f->type, fr[i].f->location, fr[i].f->dims, fr[i].f->location);
    sb_cat(b, "};\n\n");
}

static int tess_has_control_point_outputs(const tess_module *c)
{
    for (int i = 0; i < c->var_count; i++) {
        const tess_var *v = &c->vars[i];
        if (v->storage == SPV_STORAGE_OUTPUT && !v->patch && (v->kind == TV_USER || v->kind == TV_STRUCT || v->kind == TV_PERVERTEX)) return 1;
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
    tess_fref fr[TESS_MAX_FIELDS];
    int n = tess_sorted_fields(c, SPV_STORAGE_INPUT, 0, fr);
    const tess_var *pv = tess_find(c, SPV_STORAGE_INPUT, TV_PERVERTEX, 0, 0);
    if (!n && !(pv && (pv->pv_pos >= 0 || pv->clip_used))) return;
    sb_printf(b, "    for (int i = 0; i < %u; i++)\n    {\n", t->input_points);
    for (int i = 0; i < n; i++)
        sb_printf(b, "        %s[i]%s = stage_input[i].vio_l%d;\n", fr[i].v->name, fr[i].f->access, fr[i].f->location);
    if (pv && pv->pv_pos >= 0) sb_printf(b, "        %s[i].vio_Position = stage_input[i].vio_pos;\n", pv->name);
    if (pv && pv->clip_used) {
        for (int k = 0; k < pv->clip_len; k++) {
            char ref[24];
            tess_clip_ref(ref, sizeof(ref), pv->clip_len, k);
            sb_printf(b, "        %s[i].vio_m%d[%d] = stage_input[i].%s;\n", pv->name, pv->clip_member, k, ref);
        }
    }
    sb_cat(b, "    }\n");
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
        tess_fref fr[TESS_MAX_FIELDS];
        int nf = tess_sorted_fields(c, SPV_STORAGE_OUTPUT, 0, fr);
        for (int i = 0; i < nf; i++)
            sb_printf(&b, "        %s[j]%s = stage_output[j].vio_l%d;\n", fr[i].v->name, fr[i].f->access, fr[i].f->location);
        const tess_var *pvo = tess_find(c, SPV_STORAGE_OUTPUT, TV_PERVERTEX, 0, 0);
        if (pvo && pvo->pv_pos >= 0) sb_printf(&b, "        %s[j].vio_Position = stage_output[j].vio_pos;\n", pvo->name);
        if (pvo && pvo->clip_used) {
            for (int k = 0; k < pvo->clip_len; k++) {
                char ref[24];
                tess_clip_ref(ref, sizeof(ref), pvo->clip_len, k);
                sb_printf(&b, "        %s[j].vio_m%d[%d] = stage_output[j].%s;\n", pvo->name, pvo->clip_member, k, ref);
            }
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
    {
        tess_fref fr[TESS_MAX_FIELDS];
        int nf = tess_sorted_fields(c, SPV_STORAGE_OUTPUT, 1, fr);
        for (int i = 0; i < nf; i++)
            sb_printf(&b, "    patch_output.vio_p%d = %s%s;\n", fr[i].f->location, fr[i].v->name, fr[i].f->access);
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
    if (c->barriers > 0) {
        /* barrier(): a control point may read what other invocations wrote
         * before it. Run every invocation once per barrier phase - the last
         * pass sees all writes of the one before - then keep this one's. */
        sb_printf(&b, "    for (int vio_pass = 0; vio_pass <= %d; vio_pass++)\n", c->barriers);
        sb_printf(&b, "        for (int vio_invocation = 0; vio_invocation < %u; vio_invocation++)\n        {\n", t->output_points);
        if (inv) sb_printf(&b, "            %s = vio_invocation;\n", inv->name);
        sb_cat(&b, "            vert_main();\n        }\n");
    } else {
        if (inv) sb_printf(&b, "    %s = int(vio_cp);\n", inv->name);
        sb_cat(&b, "    vert_main();\n");
    }
    if (cp_outputs) {
        sb_cat(&b, "    VIO_ControlPoint stage_output;\n");
        tess_fref fr[TESS_MAX_FIELDS];
        int nf = tess_sorted_fields(c, SPV_STORAGE_OUTPUT, 0, fr);
        for (int i = 0; i < nf; i++)
            sb_printf(&b, "    stage_output.vio_l%d = %s[vio_cp]%s;\n", fr[i].f->location, fr[i].v->name, fr[i].f->access);
        const tess_var *pvo = tess_find(c, SPV_STORAGE_OUTPUT, TV_PERVERTEX, 0, 0);
        if (pvo && pvo->pv_pos >= 0) sb_printf(&b, "    stage_output.vio_pos = %s[vio_cp].vio_Position;\n", pvo->name);
        if (pvo && pvo->clip_used) {
            for (int k = 0; k < pvo->clip_len; k++) {
                char ref[24];
                tess_clip_ref(ref, sizeof(ref), pvo->clip_len, k);
                sb_printf(&b, "    stage_output.%s = %s[vio_cp].vio_m%d[%d];\n", ref, pvo->name, pvo->clip_member, k);
            }
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
        for (int j = 0; (v->kind == TV_USER || v->kind == TV_STRUCT) && j < v->field_count; j++) {
            const tess_field *f = &e->fields[v->first_field + j];
            const tess_field *src = tess_find_field(c, SPV_STORAGE_OUTPUT, f->location, v->patch);
            if (!src) {
                free(hlsl);
                return tess_error(error_msg, "the evaluation stage reads %s location %d, which the control stage does not write",
                                  v->patch ? "patch" : "per-vertex", f->location);
            }
            if (strcmp(src->type, f->type) != 0 || strcmp(src->dims, f->dims) != 0) {
                free(hlsl);
                return tess_error(error_msg, "location %d is %s%s in the control stage but %s%s in the evaluation stage",
                                  f->location, src->type, src->dims, f->type, f->dims);
            }
        }
        if (v->kind == TV_PERVERTEX && v->pv_pos >= 0 && !tess_find(c, SPV_STORAGE_OUTPUT, TV_PERVERTEX, 0, 0)) {
            free(hlsl);
            return tess_error(error_msg, "the evaluation stage reads gl_in[].gl_Position, which the control stage does not write");
        }
        if (v->kind == TV_PERVERTEX && v->clip_used && (!tess_clip_out(c) || tess_clip_out(c)->clip_len < v->clip_len)) {
            free(hlsl);
            return tess_error(error_msg, "the evaluation stage reads gl_in[].gl_ClipDistance, which the control stage does not write");
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
    {
        tess_fref fr[TESS_MAX_FIELDS];
        int nf = tess_sorted_fields(e, SPV_STORAGE_INPUT, 1, fr);
        for (int i = 0; i < nf; i++)
            sb_printf(&b, "    %s%s = patch_input.vio_p%d;\n", fr[i].v->name, fr[i].f->access, fr[i].f->location);
        nf = tess_sorted_fields(e, SPV_STORAGE_INPUT, 0, fr);
        const tess_var *pv = tess_find(e, SPV_STORAGE_INPUT, TV_PERVERTEX, 0, 0);
        if (nf || (pv && (pv->pv_pos >= 0 || pv->clip_used))) {
            sb_printf(&b, "    for (int i = 0; i < %u; i++)\n    {\n", t->output_points);
            for (int i = 0; i < nf; i++)
                sb_printf(&b, "        %s[i]%s = stage_input[i].vio_l%d;\n", fr[i].v->name, fr[i].f->access, fr[i].f->location);
            if (pv && pv->pv_pos >= 0) sb_printf(&b, "        %s[i].vio_Position = stage_input[i].vio_pos;\n", pv->name);
            if (pv && pv->clip_used) {
                int src_len = tess_clip_out(c)->clip_len;
                for (int k = 0; k < pv->clip_len; k++) {
                    char ref[24];
                    tess_clip_ref(ref, sizeof(ref), src_len, k);
                    sb_printf(&b, "        %s[i].vio_m%d[%d] = stage_input[i].%s;\n", pv->name, pv->clip_member, k, ref);
                }
            }
            sb_cat(&b, "    }\n");
        }
    }
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

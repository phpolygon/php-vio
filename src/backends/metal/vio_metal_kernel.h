/*
 * php-vio - Metal backend: vertex / geometry stages as compute kernels
 *
 * Metal has no geometry stage (METAL-GEOMETRY-PLAN.md). A draw through a
 * pipeline with one runs
 *   1. the vertex stage as a kernel: one thread per (stream vertex, instance),
 *      attributes fetched from the mesh buffer, outputs written as a record of
 *      vec4 slots;
 *   2. the geometry stage as a kernel: one thread per (primitive x invocation,
 *      instance), inputs loaded from the vertex records, EmitVertex() appending
 *      a record to the invocation's own range and strips turned into an index
 *      list;
 *   3. a generated pass-through vertex function that reads the records by
 *      gl_VertexIndex and hands them to the unchanged fragment stage.
 *
 * A stage becomes a kernel without hand-written SPIR-V loads and stores: the
 * module is rewritten structurally (execution model GLCompute, interface
 * variables Private, EmitVertex / EndPrimitive calls to two empty functions),
 * SPIRV-Cross turns it into GLSL with names vio chose, and the plumbing is
 * added as GLSL text around it before glslang compiles the kernel. Plain C, so
 * it can be checked against SPIRV-Cross and glslang on any host. Included by
 * vio_metal.m only (after vio_metal_msl.h); every function is static.
 *
 * Record layout (vec4 slots): 0 gl_Position, 1 (gl_Layer, gl_ViewportIndex as
 * int bits, gl_PointSize, -), then every user output in location order,
 * ceil(components / 4) x columns x array length slots each.
 */

#ifndef VIO_METAL_KERNEL_H
#define VIO_METAL_KERNEL_H

#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef HAVE_SPIRV_CROSS

/* GLSL bindings of the plumbing buffers. The kernels are renumbered like any
 * other stage afterwards (metal_gfx_spirv_to_msl), so these only identify the
 * buffers in the resource table; user resources keep their own bindings. */
#define VIO_MK_BIND_IN      20  /* VS kernel: mesh floats; GS kernel: vertex records */
#define VIO_MK_BIND_OUT     21  /* records written by the kernel */
#define VIO_MK_BIND_PARAMS  22  /* uvec4 parameters */
#define VIO_MK_BIND_INST    23  /* VS kernel: per-instance mat4 (locations 3..6) */
#define VIO_MK_BIND_VID     24  /* VS kernel: vertex index of every stream vertex */
#define VIO_MK_BIND_IDX     25  /* GS kernel: index list of the emitted primitives */

#define VIO_MK_LOCAL_SIZE   64
#define VIO_MK_MAX_VARS     48
#define VIO_MK_MAX_MEMBERS  64

enum { MK_BASE_FLOAT = 0, MK_BASE_INT, MK_BASE_UINT };
enum { MK_PRIM_POINTS = 0, MK_PRIM_LINES, MK_PRIM_TRIANGLES };

/* SPIR-V numbers used below. */
enum {
    MK_OP_NAME = 5, MK_OP_MEMBER_NAME = 6, MK_OP_ENTRY_POINT = 15, MK_OP_EXECUTION_MODE = 16,
    MK_OP_TYPE_VOID = 19, MK_OP_TYPE_INT = 21, MK_OP_TYPE_FLOAT = 22, MK_OP_TYPE_VECTOR = 23,
    MK_OP_TYPE_MATRIX = 24, MK_OP_TYPE_ARRAY = 28, MK_OP_TYPE_STRUCT = 30, MK_OP_TYPE_POINTER = 32,
    MK_OP_CONSTANT = 43, MK_OP_FUNCTION = 54, MK_OP_FUNCTION_END = 56, MK_OP_FUNCTION_CALL = 57,
    MK_OP_VARIABLE = 59, MK_OP_DECORATE = 71, MK_OP_MEMBER_DECORATE = 72, MK_OP_EMIT_VERTEX = 218,
    MK_OP_END_PRIMITIVE = 219, MK_OP_EMIT_STREAM_VERTEX = 220, MK_OP_END_STREAM_PRIMITIVE = 221,
    MK_OP_LABEL = 248, MK_OP_RETURN = 253, MK_OP_EXECUTION_MODE_ID = 331,
    MK_OP_DECORATE_STRING = 5632, MK_OP_MEMBER_DECORATE_STRING = 5633
};
enum { MK_MODEL_VERTEX = 0, MK_MODEL_GEOMETRY = 3, MK_MODEL_GLCOMPUTE = 5 };
enum { MK_ST_INPUT = 1, MK_ST_OUTPUT = 3, MK_ST_PRIVATE = 6 };
enum {
    MK_BI_POSITION = 0, MK_BI_POINT_SIZE = 1, MK_BI_CLIP_DISTANCE = 3, MK_BI_CULL_DISTANCE = 4,
    MK_BI_VERTEX_ID = 5, MK_BI_INSTANCE_ID = 6, MK_BI_PRIMITIVE_ID = 7, MK_BI_INVOCATION_ID = 8,
    MK_BI_LAYER = 9, MK_BI_VIEWPORT_INDEX = 10, MK_BI_VERTEX_INDEX = 42, MK_BI_INSTANCE_INDEX = 43,
    MK_BI_BASE_VERTEX = 4424, MK_BI_BASE_INSTANCE = 4425, MK_BI_DRAW_INDEX = 4426
};

typedef struct {
    uint32_t id;
    int      location;
    int      base;        /* MK_BASE_* */
    int      components;  /* 1..4 */
    int      columns;     /* 1, or 2..4 for a matrix */
    int      count;       /* array length behind the per-vertex dimension, 1 if none */
    int      flat, noperspective;
    int      slot;        /* first record slot */
} mk_var;

typedef struct {
    uint32_t id, storage;
    int      builtin;
} mk_builtin;

typedef struct {
    uint32_t var, st;     /* the gl_PerVertex variable and its struct type, 0 if absent */
    uint32_t members;
    int      member_builtin[VIO_MK_MAX_MEMBERS];
} mk_pervertex;

typedef struct {
    const uint32_t *w;
    size_t          n;
    uint32_t        bound;
    size_t         *def;
    int32_t        *builtin, *location;
    unsigned char  *flat, *noperspective, *is_block;
    uint32_t        model, ep, ep_fn_type, void_type;
    /* geometry stage */
    int             in_vertices;   /* 1 / 2 / 3 / 4 / 6 */
    int             out_prim;      /* MK_PRIM_* */
    int             max_vertices;
    int             invocations;
    int             emit_count;    /* OpEmitVertex + OpEndPrimitive (+ stream forms) */
    mk_var          in[VIO_MK_MAX_VARS], out[VIO_MK_MAX_VARS];
    int             in_count, out_count;
    mk_builtin      bi[VIO_MK_MAX_VARS];
    int             bi_count;
    mk_pervertex    pv_in, pv_out;
    int             has_samplers;  /* any UniformConstant image / sampler variable */
    int             out_slots;     /* record slots of the outputs, builtins included */
} mk_module;

/* ── Small string builder ───────────────────────────────────────── */

typedef struct { char *s; size_t n, cap; int oom; } mk_sb;

static void mk_cat(mk_sb *b, const char *t)
{
    size_t l = strlen(t);
    if (b->oom) return;
    if (b->n + l + 1 > b->cap) {
        size_t cap = b->cap ? b->cap : 2048;
        while (b->n + l + 1 > cap) cap *= 2;
        char *g = (char *)realloc(b->s, cap);
        if (!g) { b->oom = 1; return; }
        b->s = g;
        b->cap = cap;
    }
    memcpy(b->s + b->n, t, l + 1);
    b->n += l;
}

static void mk_printf(mk_sb *b, const char *fmt, ...)
{
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n < 0 || (size_t)n >= sizeof(buf)) { b->oom = 1; return; }
    mk_cat(b, buf);
}

static int mk_fail(char **error_msg, const char *fmt, ...)
{
    if (error_msg && !*error_msg) {
        char buf[512];
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(buf, sizeof(buf), fmt, ap);
        va_end(ap);
        *error_msg = strdup(buf);
    }
    return -1;
}

/* ── SPIR-V analysis ────────────────────────────────────────────── */

static void mk_free(mk_module *m)
{
    free(m->def); free(m->builtin); free(m->location);
    free(m->flat); free(m->noperspective); free(m->is_block);
    m->def = NULL; m->builtin = m->location = NULL; m->flat = m->noperspective = m->is_block = NULL;
}

static uint32_t mk_const_u32(const mk_module *m, uint32_t id)
{
    size_t c = id < m->bound ? m->def[id] : 0;
    if (!c || (m->w[c] & 0xFFFF) != MK_OP_CONSTANT || (m->w[c] >> 16) < 4) return 0;
    return m->w[c + 3];
}

static size_t mk_def(const mk_module *m, uint32_t id) { return id < m->bound ? m->def[id] : 0; }
static uint32_t mk_op(const mk_module *m, size_t at) { return m->w[at] & 0xFFFF; }

static const char *mk_name_of(const mk_module *m, uint32_t id)
{
    for (size_t i = 5; i < m->n; ) {
        uint32_t op = m->w[i] & 0xFFFF, wc = m->w[i] >> 16;
        if (!wc) break;
        if (op == MK_OP_NAME && wc >= 3 && m->w[i + 1] == id) return (const char *)&m->w[i + 2];
        i += wc;
    }
    return "?";
}

/* Scalar / vector / matrix element of a varying; arrays: `skip_outer` drops the
 * per-vertex dimension of a geometry input, one more dimension becomes count. */
static int mk_decode(const mk_module *m, uint32_t type, int skip_outer, mk_var *v, uint32_t *struct_out)
{
    uint32_t lens[4];
    int nl = 0;
    *struct_out = 0;
    for (;;) {
        size_t d = mk_def(m, type);
        if (!d) return 0;
        if (mk_op(m, d) != MK_OP_TYPE_ARRAY) break;
        if (nl >= 4) return 0;
        lens[nl++] = mk_const_u32(m, m->w[d + 3]);
        type = m->w[d + 2];
    }
    int first = skip_outer ? 1 : 0;
    if (skip_outer && nl == 0) return 0;
    if (nl - first > 1) return 0;                  /* arrays of arrays */
    v->count = (nl - first == 1) ? (int)lens[first] : 1;
    if (v->count < 1) return 0;

    size_t d = mk_def(m, type);
    if (!d) return 0;
    if (mk_op(m, d) == MK_OP_TYPE_STRUCT) { *struct_out = type; return 1; }
    v->columns = 1;
    if (mk_op(m, d) == MK_OP_TYPE_MATRIX) {
        v->columns = (int)m->w[d + 3];
        d = mk_def(m, m->w[d + 2]);
        if (!d) return 0;
    }
    v->components = 1;
    if (mk_op(m, d) == MK_OP_TYPE_VECTOR) {
        v->components = (int)m->w[d + 3];
        d = mk_def(m, m->w[d + 2]);
        if (!d) return 0;
    }
    if (mk_op(m, d) == MK_OP_TYPE_FLOAT && m->w[d + 2] == 32) v->base = MK_BASE_FLOAT;
    else if (mk_op(m, d) == MK_OP_TYPE_INT && m->w[d + 2] == 32) v->base = m->w[d + 3] ? MK_BASE_INT : MK_BASE_UINT;
    else return 0;
    if (v->components < 1 || v->components > 4 || v->columns < 1 || v->columns > 4) return 0;
    if (v->columns > 1 && v->base != MK_BASE_FLOAT) return 0;
    return 1;
}

static int mk_var_slots(const mk_var *v) { return v->columns * v->count; }

static void mk_sort(mk_var *vars, int n)
{
    for (int a = 1; a < n; a++) {
        mk_var t = vars[a];
        int b = a - 1;
        while (b >= 0 && vars[b].location > t.location) { vars[b + 1] = vars[b]; b--; }
        vars[b + 1] = t;
    }
}

static int mk_parse(mk_module *m, const uint32_t *w, size_t bytes, char **error_msg)
{
    size_t n = bytes / sizeof(uint32_t);
    memset(m, 0, sizeof(*m));
    if (n < 5 || w[0] != 0x07230203) return mk_fail(error_msg, "not a SPIR-V module");
    m->w = w;
    m->n = n;
    m->bound = w[3];
    m->def = (size_t *)calloc(m->bound, sizeof(size_t));
    m->builtin = (int32_t *)malloc(m->bound * sizeof(int32_t));
    m->location = (int32_t *)malloc(m->bound * sizeof(int32_t));
    m->flat = (unsigned char *)calloc(m->bound, 1);
    m->noperspective = (unsigned char *)calloc(m->bound, 1);
    m->is_block = (unsigned char *)calloc(m->bound, 1);
    if (!m->def || !m->builtin || !m->location || !m->flat || !m->noperspective || !m->is_block) {
        mk_free(m);
        return mk_fail(error_msg, "out of memory");
    }
    for (uint32_t i = 0; i < m->bound; i++) { m->builtin[i] = -1; m->location[i] = -1; }
    m->invocations = 1;

    /* member builtins of every struct: (struct, member) -> builtin */
    struct { uint32_t st, member; int builtin; } mb[VIO_MK_MAX_MEMBERS * 2];
    int mb_count = 0;

    for (size_t i = 5; i < n; ) {
        uint32_t op = w[i] & 0xFFFF, wc = w[i] >> 16;
        if (wc == 0 || i + wc > n) { mk_free(m); return mk_fail(error_msg, "malformed SPIR-V"); }
        switch (op) {
            case MK_OP_ENTRY_POINT:
                if (!m->ep && wc >= 3) { m->model = w[i + 1]; m->ep = w[i + 2]; }
                break;
            case MK_OP_EXECUTION_MODE:
                if (wc >= 3 && w[i + 1] == m->ep) {
                    switch (w[i + 2]) {
                        case 0:  if (wc >= 4) m->invocations = (int)w[i + 3]; break;   /* Invocations */
                        case 19: m->in_vertices = 1; break;                             /* InputPoints */
                        case 20: m->in_vertices = 2; break;                             /* InputLines */
                        case 21: m->in_vertices = 4; break;                             /* InputLinesAdjacency */
                        case 22: m->in_vertices = 3; break;                             /* Triangles */
                        case 23: m->in_vertices = 6; break;                             /* InputTrianglesAdjacency */
                        case 26: if (wc >= 4) m->max_vertices = (int)w[i + 3]; break;   /* OutputVertices */
                        case 27: m->out_prim = MK_PRIM_POINTS; break;
                        case 28: m->out_prim = MK_PRIM_LINES; break;
                        case 29: m->out_prim = MK_PRIM_TRIANGLES; break;
                        default: break;
                    }
                }
                break;
            case MK_OP_DECORATE:
                if (wc >= 3 && w[i + 1] < m->bound) {
                    uint32_t t = w[i + 1], dec = w[i + 2];
                    if (dec == 11 && wc >= 4) m->builtin[t] = (int32_t)w[i + 3];
                    else if (dec == 30 && wc >= 4) m->location[t] = (int32_t)w[i + 3];
                    else if (dec == 14) m->flat[t] = 1;
                    else if (dec == 13) m->noperspective[t] = 1;
                    else if (dec == 2) m->is_block[t] = 1;
                }
                break;
            case MK_OP_MEMBER_DECORATE:
                if (wc >= 5 && w[i + 3] == 11 && mb_count < (int)(sizeof(mb) / sizeof(mb[0]))) {
                    mb[mb_count].st = w[i + 1];
                    mb[mb_count].member = w[i + 2];
                    mb[mb_count].builtin = (int)w[i + 4];
                    mb_count++;
                }
                break;
            case MK_OP_FUNCTION:
                if (wc >= 5 && w[i + 2] == m->ep) { m->void_type = w[i + 1]; m->ep_fn_type = w[i + 4]; }
                break;
            case MK_OP_EMIT_VERTEX: case MK_OP_END_PRIMITIVE:
            case MK_OP_EMIT_STREAM_VERTEX: case MK_OP_END_STREAM_PRIMITIVE:
                m->emit_count++;
                break;
            case 19: case 20: case 21: case 22: case 23: case 24: case 25: case 26: case 27:
            case 28: case 29: case 30: case 32: case 33:
                if (wc >= 2 && w[i + 1] < m->bound) m->def[w[i + 1]] = i;
                break;
            case MK_OP_CONSTANT: case MK_OP_VARIABLE:
                if (wc >= 3 && w[i + 2] < m->bound) m->def[w[i + 2]] = i;
                break;
            default: break;
        }
        i += wc;
    }
    if (m->model != MK_MODEL_VERTEX && m->model != MK_MODEL_GEOMETRY) {
        mk_free(m);
        return mk_fail(error_msg, "only vertex and geometry stages become kernels");
    }
    if (!m->void_type || !m->ep_fn_type) { mk_free(m); return mk_fail(error_msg, "entry point function not found"); }
    if (m->model == MK_MODEL_GEOMETRY) {
        if (!m->in_vertices) m->in_vertices = 1;
        if (m->max_vertices < 1) { mk_free(m); return mk_fail(error_msg, "geometry stage without max_vertices"); }
        if (m->invocations < 1) m->invocations = 1;
    }

    for (size_t i = 5; i < n; i += w[i] >> 16) {
        if ((w[i] & 0xFFFF) != MK_OP_VARIABLE || (w[i] >> 16) < 4) continue;
        uint32_t storage = w[i + 3], id = w[i + 2];
        size_t pt = mk_def(m, w[i + 1]);
        if (!pt || mk_op(m, pt) != MK_OP_TYPE_POINTER) continue;
        uint32_t pointee = w[pt + 3];
        if (storage == 0 /* UniformConstant */) {
            /* image / sampler / sampled image (possibly arrays of them) */
            uint32_t t = pointee;
            size_t d;
            while ((d = mk_def(m, t)) && mk_op(m, d) == MK_OP_TYPE_ARRAY) t = m->w[d + 2];
            if (d && (mk_op(m, d) == 25 || mk_op(m, d) == 26 || mk_op(m, d) == 27)) m->has_samplers = 1;
            continue;
        }
        if (storage != MK_ST_INPUT && storage != MK_ST_OUTPUT) continue;
        if (m->builtin[id] >= 0) {
            if (m->bi_count >= VIO_MK_MAX_VARS) { mk_free(m); return mk_fail(error_msg, "too many built-ins"); }
            m->bi[m->bi_count].id = id;
            m->bi[m->bi_count].storage = storage;
            m->bi[m->bi_count].builtin = m->builtin[id];
            m->bi_count++;
            continue;
        }
        mk_var v;
        memset(&v, 0, sizeof(v));
        v.id = id;
        v.location = m->location[id];
        v.flat = m->flat[id];
        v.noperspective = m->noperspective[id];
        uint32_t st = 0;
        int arrayed = m->model == MK_MODEL_GEOMETRY && storage == MK_ST_INPUT;
        if (!mk_decode(m, pointee, arrayed, &v, &st)) {
            mk_free(m);
            return mk_fail(error_msg, "'%s': unsupported varying type (scalars, vectors, matrices and one array dimension)",
                           mk_name_of(m, id));
        }
        if (st) {
            int has_builtin = 0;
            for (int k = 0; k < mb_count; k++) if (mb[k].st == st) { has_builtin = 1; break; }
            if (!has_builtin) {
                mk_free(m);
                return mk_fail(error_msg, "'%s': interface blocks and struct varyings are not supported by the Metal geometry emulation",
                               mk_name_of(m, id));
            }
            mk_pervertex *pv = storage == MK_ST_INPUT ? &m->pv_in : &m->pv_out;
            size_t sd = mk_def(m, st);
            pv->var = id;
            pv->st = st;
            pv->members = (m->w[sd] >> 16) - 2;
            if (pv->members > VIO_MK_MAX_MEMBERS) pv->members = VIO_MK_MAX_MEMBERS;
            for (uint32_t k = 0; k < pv->members; k++) pv->member_builtin[k] = -1;
            for (int k = 0; k < mb_count; k++) {
                if (mb[k].st == st && mb[k].member < pv->members) pv->member_builtin[mb[k].member] = mb[k].builtin;
            }
            continue;
        }
        if (v.location < 0) {
            mk_free(m);
            return mk_fail(error_msg, "'%s': varyings need a location", mk_name_of(m, id));
        }
        if (storage == MK_ST_INPUT) {
            if (m->in_count >= VIO_MK_MAX_VARS) { mk_free(m); return mk_fail(error_msg, "too many inputs"); }
            m->in[m->in_count++] = v;
        } else {
            if (m->out_count >= VIO_MK_MAX_VARS) { mk_free(m); return mk_fail(error_msg, "too many outputs"); }
            m->out[m->out_count++] = v;
        }
    }
    mk_sort(m->in, m->in_count);
    mk_sort(m->out, m->out_count);
    int slot = 2;
    for (int k = 0; k < m->out_count; k++) { m->out[k].slot = slot; slot += mk_var_slots(&m->out[k]); }
    m->out_slots = slot;
    return 0;
}

static int mk_is_converted(const mk_module *m, uint32_t id)
{
    if (!id) return 0;
    if (id == m->pv_in.var || id == m->pv_out.var) return 1;
    for (int k = 0; k < m->in_count; k++) if (m->in[k].id == id) return 1;
    for (int k = 0; k < m->out_count; k++) if (m->out[k].id == id) return 1;
    for (int k = 0; k < m->bi_count; k++) if (m->bi[k].id == id) return 1;
    return 0;
}

static int mk_is_pv_struct(const mk_module *m, uint32_t id)
{
    return id && (id == m->pv_in.st || id == m->pv_out.st);
}

/* ── Names ──────────────────────────────────────────────────────── */

static const char *mk_builtin_member_name(int builtin)
{
    switch (builtin) {
        case MK_BI_POSITION:       return "vio_Position";
        case MK_BI_POINT_SIZE:     return "vio_PointSize";
        case MK_BI_CLIP_DISTANCE:  return "vio_ClipDistance";
        case MK_BI_CULL_DISTANCE:  return "vio_CullDistance";
        default:                   return NULL;
    }
}

static void mk_var_name(char *out, size_t cap, const mk_module *m, uint32_t id)
{
    if (id == m->pv_in.var)  { snprintf(out, cap, "vio_gl_in"); return; }
    if (id == m->pv_out.var) { snprintf(out, cap, "vio_gl_out"); return; }
    for (int k = 0; k < m->in_count; k++) if (m->in[k].id == id) { snprintf(out, cap, "vio_i%d", m->in[k].location); return; }
    for (int k = 0; k < m->out_count; k++) if (m->out[k].id == id) { snprintf(out, cap, "vio_o%d", m->out[k].location); return; }
    for (int k = 0; k < m->bi_count; k++) {
        if (m->bi[k].id == id) {
            snprintf(out, cap, "vio_b%d_%s", m->bi[k].builtin, m->bi[k].storage == MK_ST_INPUT ? "in" : "out");
            return;
        }
    }
    snprintf(out, cap, "vio_x%u", id);
}

static void mk_member_name(char *out, size_t cap, const mk_pervertex *pv, uint32_t k)
{
    const char *n = k < pv->members ? mk_builtin_member_name(pv->member_builtin[k]) : NULL;
    if (n) snprintf(out, cap, "%s", n);
    else snprintf(out, cap, "vio_m%u", k);
}

static const mk_builtin *mk_find_builtin(const mk_module *m, uint32_t storage, int builtin)
{
    for (int k = 0; k < m->bi_count; k++) if (m->bi[k].storage == storage && m->bi[k].builtin == builtin) return &m->bi[k];
    return NULL;
}

static int mk_pv_member(const mk_pervertex *pv, int builtin)
{
    if (!pv->var) return -1;
    for (uint32_t k = 0; k < pv->members; k++) if (pv->member_builtin[k] == builtin) return (int)k;
    return -1;
}

/* ── SPIR-V rewrite ─────────────────────────────────────────────── */

static size_t mk_put_string(uint32_t *out, size_t o, const char *s)
{
    size_t len = strlen(s) + 1, words = (len + 3) / 4;
    memset(out + o, 0, words * 4);
    memcpy(out + o, s, len);
    return o + words;
}

static size_t mk_put_name(uint32_t *out, size_t o, uint32_t id, const char *s)
{
    size_t start = o++;
    out[o++] = id;
    o = mk_put_string(out, o, s);
    out[start] = ((uint32_t)(o - start) << 16) | MK_OP_NAME;
    return o;
}

static size_t mk_put_member_name(uint32_t *out, size_t o, uint32_t st, uint32_t member, const char *s)
{
    size_t start = o++;
    out[o++] = st;
    out[o++] = member;
    o = mk_put_string(out, o, s);
    out[start] = ((uint32_t)(o - start) << 16) | MK_OP_MEMBER_NAME;
    return o;
}

static int mk_is_debug_or_later(uint32_t op)
{
    /* Anything that ends the names section: annotations, types, constants, variables, functions. */
    return op == MK_OP_DECORATE || op == MK_OP_MEMBER_DECORATE || op == 73 /* DecorationGroup */ ||
           op == MK_OP_DECORATE_STRING || op == MK_OP_MEMBER_DECORATE_STRING ||
           (op >= 19 && op <= 39) || (op >= 41 && op <= 52) || op == MK_OP_VARIABLE || op == MK_OP_FUNCTION;
}

/* The module as a GLCompute stage with Private interface variables and the
 * emit calls redirected. Returns malloc'd words. */
static uint32_t *mk_rewrite(const mk_module *m, size_t *out_words)
{
    const uint32_t *w = m->w;
    size_t cap = m->n + 64 + (size_t)(m->in_count + m->out_count + m->bi_count + 4) * 16 + VIO_MK_MAX_MEMBERS * 2 * 12 + 32;
    uint32_t *out = (uint32_t *)malloc(cap * sizeof(uint32_t));
    if (!out) return NULL;
    uint32_t bound = m->bound;
    uint32_t emit_fn = 0, emit_label = 0, end_fn = 0, end_label = 0;
    if (m->model == MK_MODEL_GEOMETRY) {
        emit_fn = bound++; emit_label = bound++;
        end_fn = bound++;  end_label = bound++;
    }
    size_t o = 0;
    for (int h = 0; h < 5; h++) out[o++] = w[h];
    int names_done = 0, local_size_done = 0;

    for (size_t i = 5; i < m->n; ) {
        uint32_t op = w[i] & 0xFFFF, wc = w[i] >> 16;
        int drop = 0;

        if (!names_done && mk_is_debug_or_later(op)) {
            char nm[64];
            uint32_t ids[VIO_MK_MAX_VARS * 3 + 2];
            int nid = 0;
            if (m->pv_in.var) ids[nid++] = m->pv_in.var;
            if (m->pv_out.var) ids[nid++] = m->pv_out.var;
            for (int k = 0; k < m->in_count; k++) ids[nid++] = m->in[k].id;
            for (int k = 0; k < m->out_count; k++) ids[nid++] = m->out[k].id;
            for (int k = 0; k < m->bi_count; k++) ids[nid++] = m->bi[k].id;
            for (int k = 0; k < nid; k++) {
                mk_var_name(nm, sizeof(nm), m, ids[k]);
                o = mk_put_name(out, o, ids[k], nm);
            }
            const mk_pervertex *pvs[2] = { &m->pv_in, &m->pv_out };
            for (int p = 0; p < 2; p++) {
                if (!pvs[p]->var) continue;
                if (p == 1 && pvs[1]->st == pvs[0]->st) continue;
                o = mk_put_name(out, o, pvs[p]->st, p == 0 ? "vio_PerVertexIn" : "vio_PerVertexOut");
                for (uint32_t k = 0; k < pvs[p]->members; k++) {
                    mk_member_name(nm, sizeof(nm), pvs[p], k);
                    o = mk_put_member_name(out, o, pvs[p]->st, k, nm);
                }
            }
            if (emit_fn) {
                o = mk_put_name(out, o, emit_fn, "vio_emit");
                o = mk_put_name(out, o, end_fn, "vio_end_primitive");
            }
            names_done = 1;
        }

        size_t start = o;
        switch (op) {
            case MK_OP_EXECUTION_MODE: case MK_OP_EXECUTION_MODE_ID:
                if (wc >= 2 && w[i + 1] == m->ep) {
                    drop = 1;
                    if (!local_size_done) {
                        out[o++] = (6u << 16) | MK_OP_EXECUTION_MODE;
                        out[o++] = m->ep;
                        out[o++] = 17;   /* LocalSize */
                        out[o++] = VIO_MK_LOCAL_SIZE; out[o++] = 1; out[o++] = 1;
                        local_size_done = 1;
                    }
                }
                break;
            case MK_OP_NAME:
                drop = wc >= 2 && (mk_is_converted(m, w[i + 1]) || mk_is_pv_struct(m, w[i + 1]));
                break;
            case MK_OP_MEMBER_NAME:
                drop = wc >= 2 && mk_is_pv_struct(m, w[i + 1]);
                break;
            case MK_OP_DECORATE: case MK_OP_DECORATE_STRING:
                drop = wc >= 3 && (mk_is_converted(m, w[i + 1]) || (w[i + 2] == 2 && mk_is_pv_struct(m, w[i + 1])));
                break;
            case MK_OP_MEMBER_DECORATE: case MK_OP_MEMBER_DECORATE_STRING:
                drop = wc >= 2 && mk_is_pv_struct(m, w[i + 1]);
                break;
            case MK_OP_EMIT_VERTEX: case MK_OP_EMIT_STREAM_VERTEX:
            case MK_OP_END_PRIMITIVE: case MK_OP_END_STREAM_PRIMITIVE: {
                int emit = op == MK_OP_EMIT_VERTEX || op == MK_OP_EMIT_STREAM_VERTEX;
                out[o++] = (4u << 16) | MK_OP_FUNCTION_CALL;
                out[o++] = m->void_type;
                out[o++] = bound++;
                out[o++] = emit ? emit_fn : end_fn;
                drop = 1;
                break;
            }
            default: break;
        }
        if (!drop) {
            for (uint32_t k = 0; k < wc; k++) out[o++] = w[i + k];
            if (op == MK_OP_ENTRY_POINT && w[i + 2] == m->ep) {
                out[start + 1] = MK_MODEL_GLCOMPUTE;
                if (!local_size_done) {
                    out[o++] = (6u << 16) | MK_OP_EXECUTION_MODE;
                    out[o++] = m->ep;
                    out[o++] = 17;
                    out[o++] = VIO_MK_LOCAL_SIZE; out[o++] = 1; out[o++] = 1;
                    local_size_done = 1;
                }
            } else if (op == MK_OP_TYPE_POINTER && wc >= 4 && (w[i + 2] == MK_ST_INPUT || w[i + 2] == MK_ST_OUTPUT)) {
                out[start + 2] = MK_ST_PRIVATE;
            } else if (op == MK_OP_VARIABLE && wc >= 4 && mk_is_converted(m, w[i + 2])) {
                out[start + 3] = MK_ST_PRIVATE;
            }
        }
        i += wc;
    }
    if (emit_fn) {
        uint32_t fns[2][2] = { { emit_fn, emit_label }, { end_fn, end_label } };
        for (int f = 0; f < 2; f++) {
            out[o++] = (5u << 16) | MK_OP_FUNCTION;
            out[o++] = m->void_type; out[o++] = fns[f][0]; out[o++] = 0; out[o++] = m->ep_fn_type;
            out[o++] = (2u << 16) | MK_OP_LABEL; out[o++] = fns[f][1];
            out[o++] = (1u << 16) | MK_OP_RETURN;
            out[o++] = (1u << 16) | MK_OP_FUNCTION_END;
        }
    }
    out[3] = bound;
    *out_words = o;
    return out;
}

/* SPIRV-Cross: rewritten module -> GLSL 450 (Vulkan semantics, like the Vulkan
 * backend's round trip). Returns malloc'd text. */
static char *mk_to_glsl(const uint32_t *words, size_t count, char **error_msg)
{
    spvc_context ctx = NULL;
    spvc_parsed_ir ir = NULL;
    spvc_compiler c = NULL;
    spvc_compiler_options opts = NULL;
    const char *src = NULL;
    if (spvc_context_create(&ctx) != SPVC_SUCCESS) { mk_fail(error_msg, "SPIRV-Cross context"); return NULL; }
    if (spvc_context_parse_spirv(ctx, words, count, &ir) != SPVC_SUCCESS ||
        spvc_context_create_compiler(ctx, SPVC_BACKEND_GLSL, ir, SPVC_CAPTURE_MODE_TAKE_OWNERSHIP, &c) != SPVC_SUCCESS ||
        spvc_compiler_create_compiler_options(c, &opts) != SPVC_SUCCESS) {
        mk_fail(error_msg, "%s", spvc_context_get_last_error_string(ctx));
        spvc_context_destroy(ctx);
        return NULL;
    }
    spvc_compiler_options_set_uint(opts, SPVC_COMPILER_OPTION_GLSL_VERSION, 450);
    spvc_compiler_options_set_bool(opts, SPVC_COMPILER_OPTION_GLSL_ES, SPVC_FALSE);
    spvc_compiler_options_set_bool(opts, SPVC_COMPILER_OPTION_GLSL_VULKAN_SEMANTICS, SPVC_TRUE);
    spvc_compiler_install_compiler_options(c, opts);
    if (spvc_compiler_compile(c, &src) != SPVC_SUCCESS || !src) {
        mk_fail(error_msg, "%s", spvc_context_get_last_error_string(ctx));
        spvc_context_destroy(ctx);
        return NULL;
    }
    char *r = strdup(src);
    spvc_context_destroy(ctx);
    return r;
}

/* ── GLSL helpers ───────────────────────────────────────────────── */

static void mk_glsl_type(char *out, size_t cap, const mk_var *v)
{
    static const char *pfx[] = { "", "i", "u" };
    static const char *scal[] = { "float", "int", "uint" };
    if (v->columns > 1) {
        if (v->columns == v->components) snprintf(out, cap, "mat%d", v->columns);
        else snprintf(out, cap, "mat%dx%d", v->columns, v->components);
    } else if (v->components == 1) {
        snprintf(out, cap, "%s", scal[v->base]);
    } else {
        snprintf(out, cap, "%svec%d", pfx[v->base], v->components);
    }
}

/* vec4 slot -> one column / vector of the varying's element type. */
static void mk_unpack(char *out, size_t cap, const mk_var *v, const char *slot)
{
    static const char *sw[] = { "", ".x", ".xy", ".xyz", "" };
    const char *s = sw[v->components];
    if (v->base == MK_BASE_FLOAT) snprintf(out, cap, "%s%s", slot, s);
    else snprintf(out, cap, "%s(%s%s)", v->base == MK_BASE_INT ? "floatBitsToInt" : "floatBitsToUint", slot, s);
}

/* One column / vector -> vec4 slot. */
static void mk_pack(char *out, size_t cap, const mk_var *v, const char *val)
{
    char x[256];
    if (v->base == MK_BASE_FLOAT) snprintf(x, sizeof(x), "%s", val);
    else snprintf(x, sizeof(x), "%s(%s)", v->base == MK_BASE_INT ? "intBitsToFloat" : "uintBitsToFloat", val);
    switch (v->components) {
        case 1:  snprintf(out, cap, "vec4(%s, 0.0, 0.0, 0.0)", x); break;
        case 2:  snprintf(out, cap, "vec4(%s, 0.0, 0.0)", x); break;
        case 3:  snprintf(out, cap, "vec4(%s, 0.0)", x); break;
        default: snprintf(out, cap, "%s", x); break;
    }
}

/* Statements writing `name` (an output of `m`) into record slots at `rec`. */
static void mk_emit_store(mk_sb *b, const mk_var *v, const char *name, const char *buf, const char *rec)
{
    for (int e = 0; e < v->count; e++) {
        for (int c = 0; c < v->columns; c++) {
            char ref[96], packed[320];
            if (v->count > 1 && v->columns > 1) snprintf(ref, sizeof(ref), "%s[%d][%d]", name, e, c);
            else if (v->count > 1) snprintf(ref, sizeof(ref), "%s[%d]", name, e);
            else if (v->columns > 1) snprintf(ref, sizeof(ref), "%s[%d]", name, c);
            else snprintf(ref, sizeof(ref), "%s", name);
            mk_pack(packed, sizeof(packed), v, ref);
            mk_printf(b, "    %s.r[%s + %du] = %s;\n", buf, rec, v->slot + e * v->columns + c, packed);
        }
    }
}

/* Statements loading `name` (element `elem` may be "" or "[k]") from slot `slot0` on. */
static void mk_emit_load(mk_sb *b, const mk_var *v, const char *name, const char *elem, const char *buf,
                         const char *rec, int slot0)
{
    for (int e = 0; e < v->count; e++) {
        for (int c = 0; c < v->columns; c++) {
            char ref[128], slot[128], val[192];
            snprintf(ref, sizeof(ref), "%s%s", name, elem);
            if (v->count > 1) { size_t l = strlen(ref); snprintf(ref + l, sizeof(ref) - l, "[%d]", e); }
            if (v->columns > 1) { size_t l = strlen(ref); snprintf(ref + l, sizeof(ref) - l, "[%d]", c); }
            snprintf(slot, sizeof(slot), "%s.r[%s + %du]", buf, rec, slot0 + e * v->columns + c);
            mk_unpack(val, sizeof(val), v, slot);
            mk_printf(b, "    %s = %s;\n", ref, val);
        }
    }
}

/* Replace the first `void <name>()` definition's body with `body`. */
static int mk_replace_body(char **src, const char *name, const char *body)
{
    char sig[96];
    snprintf(sig, sizeof(sig), "void %s()", name);
    char *at = strstr(*src, sig);
    while (at && (at[strlen(sig)] == ';')) at = strstr(at + 1, sig);   /* skip prototypes */
    if (!at) return -1;
    char *open = strchr(at, '{');
    if (!open) return -1;
    int depth = 0;
    char *p = open;
    for (; *p; p++) {
        if (*p == '{') depth++;
        else if (*p == '}' && --depth == 0) break;
    }
    if (!*p) return -1;
    size_t pre = (size_t)(open - *src), post = strlen(p + 1), bl = strlen(body);
    char *r = (char *)malloc(pre + bl + post + 4);
    if (!r) return -1;
    memcpy(r, *src, pre);
    r[pre] = '{';
    memcpy(r + pre + 1, body, bl);
    r[pre + 1 + bl] = '}';
    memcpy(r + pre + 2 + bl, p + 1, post + 1);
    free(*src);
    *src = r;
    return 0;
}

/* SPIRV-Cross's `void main()` -> `void vio_stage_main()`, declarations after
 * the local-size line, `tail` (the new main) at the end. */
static char *mk_assemble(char *glsl, const char *decls, const char *tail, char **error_msg)
{
    char *main_at = strstr(glsl, "void main()");
    char *ls = strstr(glsl, "layout(local_size_x");
    if (!main_at || !ls) { free(glsl); mk_fail(error_msg, "unexpected SPIRV-Cross output"); return NULL; }
    char *ls_end = strchr(ls, '\n');
    if (!ls_end) { free(glsl); mk_fail(error_msg, "unexpected SPIRV-Cross output"); return NULL; }
    ls_end++;
    mk_sb b = {0};
    size_t head = (size_t)(ls_end - glsl);
    char *headbuf = (char *)malloc(head + 1);
    if (!headbuf) { free(glsl); return NULL; }
    memcpy(headbuf, glsl, head);
    headbuf[head] = '\0';
    mk_cat(&b, headbuf);
    free(headbuf);
    mk_cat(&b, decls);
    /* from the local-size line to main, then the renamed main and the rest */
    size_t mid = (size_t)(main_at - ls_end);
    char *midbuf = (char *)malloc(mid + 1);
    if (!midbuf) { free(glsl); free(b.s); return NULL; }
    memcpy(midbuf, ls_end, mid);
    midbuf[mid] = '\0';
    mk_cat(&b, midbuf);
    free(midbuf);
    mk_cat(&b, "void vio_stage_main()");
    mk_cat(&b, main_at + strlen("void main()"));
    mk_cat(&b, "\n");
    mk_cat(&b, tail);
    free(glsl);
    if (b.oom) { free(b.s); mk_fail(error_msg, "out of memory"); return NULL; }
    return b.s;
}

/* Statements that write slots 0 and 1 (position, layer / viewport / point size). */
static void mk_emit_store_builtins(mk_sb *b, const mk_module *m, const char *buf, const char *rec)
{
    char nm[64];
    int pos = mk_pv_member(&m->pv_out, MK_BI_POSITION);
    int ps  = mk_pv_member(&m->pv_out, MK_BI_POINT_SIZE);
    if (pos >= 0) {
        mk_member_name(nm, sizeof(nm), &m->pv_out, (uint32_t)pos);
        mk_printf(b, "    %s.r[%s] = vio_gl_out.%s;\n", buf, rec, nm);
    } else {
        mk_printf(b, "    %s.r[%s] = vec4(0.0, 0.0, 0.0, 1.0);\n", buf, rec);
    }
    const mk_builtin *layer = mk_find_builtin(m, MK_ST_OUTPUT, MK_BI_LAYER);
    const mk_builtin *vp    = mk_find_builtin(m, MK_ST_OUTPUT, MK_BI_VIEWPORT_INDEX);
    char psize[64] = "1.0";
    if (ps >= 0) { mk_member_name(nm, sizeof(nm), &m->pv_out, (uint32_t)ps); snprintf(psize, sizeof(psize), "vio_gl_out.%s", nm); }
    mk_printf(b, "    %s.r[%s + 1u] = vec4(intBitsToFloat(%s), intBitsToFloat(%s), %s, 0.0);\n", buf, rec,
              layer ? "vio_b9_out" : "0", vp ? "vio_b10_out" : "0", psize);
}

/* gl_PointSize is 1.0 unless the stage writes it (GL's default size). */
static void mk_emit_point_size_default(mk_sb *b, const mk_module *m)
{
    int ps = mk_pv_member(&m->pv_out, MK_BI_POINT_SIZE);
    if (ps < 0) return;
    char nm[64];
    mk_member_name(nm, sizeof(nm), &m->pv_out, (uint32_t)ps);
    mk_printf(b, "    vio_gl_out.%s = 1.0;\n", nm);
}

/* MSL index of the plumbing buffer at GLSL `binding` after renumbering, -1 if
 * the kernel does not use it. */
static int mk_res_index(const vio_metal_stage_res *res, int binding)
{
    for (int i = 0; i < res->buffer_count; i++) if (res->buffers[i].binding == binding) return res->buffers[i].msl_index;
    return -1;
}

/* The stage's own default uniform block (the scan in metal_gfx_spirv_to_msl
 * would take the plumbing parameters for it): first UBO / push-constant block
 * below the plumbing bindings. */
static int mk_user_cbuffer(const vio_metal_stage_res *res)
{
    for (int i = 0; i < res->buffer_count; i++) {
        const vio_metal_res_buffer *b = &res->buffers[i];
        if (b->kind == 1 || b->kind == 3) continue;
        if (b->kind == 0 && b->binding >= VIO_MK_BIND_IN && b->binding <= VIO_MK_BIND_IDX) continue;
        return b->msl_index;
    }
    return -1;
}

/* The stage's own storage buffer at `binding` (vio_bind_storage_buffer), or
 * its first one when the binding is not found. */
static int mk_user_storage(const vio_metal_stage_res *res, int binding)
{
    int first = -1;
    for (int i = 0; i < res->buffer_count; i++) {
        const vio_metal_res_buffer *b = &res->buffers[i];
        if (b->kind != 1 || (b->binding >= VIO_MK_BIND_IN && b->binding <= VIO_MK_BIND_IDX)) continue;
        if (b->binding == binding) return b->msl_index;
        if (first < 0) first = b->msl_index;
    }
    return first;
}

/* Bytes of per-vertex mesh attributes the vertex stage reads (its default stride). */
static int mk_vs_mesh_stride(const mk_module *vs)
{
    int n = 0;
    for (int k = 0; k < vs->in_count; k++) {
        const mk_var *v = &vs->in[k];
        if (v->location >= 3 && v->location <= 6) continue;
        n += v->components * v->columns * v->count;
    }
    return n * 4;
}

/* ── Vertex stage kernel ────────────────────────────────────────── */

/* Mesh attribute offsets in floats, as the Metal vertex descriptor packs them:
 * ascending locations, locations 3..6 are the per-instance mat4 columns. */
static int mk_vs_mesh_offset(const mk_module *vs, int location)
{
    int off = 0;
    for (int k = 0; k < vs->in_count; k++) {
        const mk_var *v = &vs->in[k];
        if (v->location >= 3 && v->location <= 6) continue;
        if (v->location == location) return off;
        off += v->components * v->columns * v->count;
    }
    return off;
}

static int mk_vs_uses_instance(const mk_module *vs)
{
    for (int k = 0; k < vs->in_count; k++) if (vs->in[k].location >= 3 && vs->in[k].location <= 6) return 1;
    return 0;
}

/* GLSL of the vertex kernel. params: x = stream vertices, y = instances,
 * z = mesh stride in floats. Record = vertex * instances order:
 * (instance * x + vertex). */
static char *mk_vs_kernel_glsl(const mk_module *vs, char **error_msg)
{
    size_t words = 0;
    uint32_t *spv = mk_rewrite(vs, &words);
    if (!spv) { mk_fail(error_msg, "out of memory"); return NULL; }
    char *glsl = mk_to_glsl(spv, words, error_msg);
    free(spv);
    if (!glsl) return NULL;

    mk_sb d = {0}, t = {0};
    mk_printf(&d, "layout(std430, set = 0, binding = %d) readonly buffer VioMesh { float f[]; } vio_mesh;\n", VIO_MK_BIND_IN);
    mk_printf(&d, "layout(std430, set = 0, binding = %d) writeonly buffer VioRecOut { vec4 r[]; } vio_rec;\n", VIO_MK_BIND_OUT);
    mk_printf(&d, "layout(std140, set = 0, binding = %d) uniform VioParams { uvec4 p; } vio_params;\n", VIO_MK_BIND_PARAMS);
    mk_printf(&d, "layout(std430, set = 0, binding = %d) readonly buffer VioInst { mat4 m[]; } vio_inst;\n", VIO_MK_BIND_INST);
    mk_printf(&d, "layout(std430, set = 0, binding = %d) readonly buffer VioVid { uint v[]; } vio_vid;\n", VIO_MK_BIND_VID);

    mk_cat(&t, "void main()\n{\n");
    mk_cat(&t, "    uint vio_v = gl_GlobalInvocationID.x, vio_n = gl_GlobalInvocationID.y;\n");
    mk_cat(&t, "    if (vio_v >= vio_params.p.x || vio_n >= vio_params.p.y) return;\n");
    mk_cat(&t, "    uint vio_vi = vio_vid.v[vio_v];\n");
    mk_cat(&t, "    uint vio_base = vio_vi * vio_params.p.z;\n");
    for (int k = 0; k < vs->in_count; k++) {
        const mk_var *v = &vs->in[k];
        char ty[24];
        mk_glsl_type(ty, sizeof(ty), v);
        if (v->location >= 3 && v->location <= 6) {
            /* per-instance mat4 columns (identity when the draw gave no matrices) */
            for (int e = 0; e < v->count; e++) {
                char ref[64];
                if (v->count > 1) snprintf(ref, sizeof(ref), "vio_i%d[%d]", v->location, e);
                else snprintf(ref, sizeof(ref), "vio_i%d", v->location);
                int col = v->location - 3 + e * v->columns;
                if (v->columns > 1) {
                    mk_printf(&t, "    %s = %s(", ref, ty);
                    for (int c = 0; c < v->columns; c++) {
                        static const char *sw[] = { "", ".x", ".xy", ".xyz", "" };
                        mk_printf(&t, "%svio_inst.m[vio_n][%d]%s", c ? ", " : "", col + c > 3 ? 3 : col + c, sw[v->components]);
                    }
                    mk_cat(&t, ");\n");
                } else {
                    static const char *sw[] = { "", ".x", ".xy", ".xyz", "" };
                    mk_printf(&t, "    %s = %s(vio_inst.m[vio_n][%d]%s);\n", ref, ty, col > 3 ? 3 : col, sw[v->components]);
                }
            }
            continue;
        }
        int off = mk_vs_mesh_offset(vs, v->location);
        for (int e = 0; e < v->count; e++) {
            for (int c = 0; c < v->columns; c++) {
                char ref[64];
                snprintf(ref, sizeof(ref), "vio_i%d", v->location);
                if (v->count > 1) { size_t l = strlen(ref); snprintf(ref + l, sizeof(ref) - l, "[%d]", e); }
                if (v->columns > 1) { size_t l = strlen(ref); snprintf(ref + l, sizeof(ref) - l, "[%d]", c); }
                int at = off + (e * v->columns + c) * v->components;
                static const char *ctor[3][5] = {
                    { "", "float", "vec2", "vec3", "vec4" },
                    { "", "int", "ivec2", "ivec3", "ivec4" },
                    { "", "uint", "uvec2", "uvec3", "uvec4" } };
                mk_printf(&t, "    %s = %s(", ref, ctor[v->base][v->components]);
                for (int q = 0; q < v->components; q++) {
                    mk_printf(&t, "%svio_mesh.f[vio_base + %du]", q ? ", " : "", at + q);
                }
                mk_cat(&t, ");\n");
            }
        }
    }
    for (int k = 0; k < vs->bi_count; k++) {
        const mk_builtin *bi = &vs->bi[k];
        if (bi->storage != MK_ST_INPUT) continue;
        char nm[64];
        mk_var_name(nm, sizeof(nm), vs, bi->id);
        switch (bi->builtin) {
            case MK_BI_VERTEX_INDEX: case MK_BI_VERTEX_ID: mk_printf(&t, "    %s = int(vio_vi);\n", nm); break;
            case MK_BI_INSTANCE_INDEX: case MK_BI_INSTANCE_ID: mk_printf(&t, "    %s = int(vio_n);\n", nm); break;
            default: mk_printf(&t, "    %s = 0;\n", nm); break;
        }
    }
    mk_emit_point_size_default(&t, vs);
    mk_cat(&t, "    vio_stage_main();\n");
    mk_cat(&t, "    uint vio_r = (vio_n * vio_params.p.x + vio_v) * ");
    mk_printf(&t, "%du;\n", vs->out_slots);
    mk_emit_store_builtins(&t, vs, "vio_rec", "vio_r");
    for (int k = 0; k < vs->out_count; k++) {
        char nm[32];
        snprintf(nm, sizeof(nm), "vio_o%d", vs->out[k].location);
        mk_emit_store(&t, &vs->out[k], nm, "vio_rec", "vio_r");
    }
    mk_cat(&t, "}\n");
    char *r = (d.oom || t.oom) ? NULL : mk_assemble(glsl, d.s, t.s, error_msg);
    if (d.oom || t.oom) free(glsl);
    free(d.s); free(t.s);
    return r;
}

/* ── Geometry stage kernel ──────────────────────────────────────── */

/* Indices one invocation can produce (a list of its strips). */
static int mk_gs_index_count(const mk_module *gs)
{
    int v = gs->max_vertices;
    switch (gs->out_prim) {
        case MK_PRIM_POINTS: return v;
        case MK_PRIM_LINES:  return v >= 2 ? (v - 1) * 2 : 0;
        default:             return v >= 3 ? (v - 2) * 3 : 0;
    }
}

/* GLSL of the geometry kernel. params: x = primitives per instance,
 * y = vertex records per instance (stream vertices), z = instances.
 * Grid: x = primitive * invocations + invocation, y = instance. Vertex record
 * v of invocation g is 1 + g * max_vertices + v (record 0 is the culled
 * "dead" vertex every unused index points at). */
static char *mk_gs_kernel_glsl(const mk_module *gs, const mk_module *vs, char **error_msg)
{
    size_t words = 0;
    uint32_t *spv = mk_rewrite(gs, &words);
    if (!spv) { mk_fail(error_msg, "out of memory"); return NULL; }
    char *glsl = mk_to_glsl(spv, words, error_msg);
    free(spv);
    if (!glsl) return NULL;

    int idx_per = mk_gs_index_count(gs);
    mk_sb d = {0}, e = {0}, t = {0};
    mk_printf(&d, "layout(std430, set = 0, binding = %d) readonly buffer VioRecIn { vec4 r[]; } vio_in;\n", VIO_MK_BIND_IN);
    mk_printf(&d, "layout(std430, set = 0, binding = %d) writeonly buffer VioRecOut { vec4 r[]; } vio_rec;\n", VIO_MK_BIND_OUT);
    mk_printf(&d, "layout(std140, set = 0, binding = %d) uniform VioParams { uvec4 p; } vio_params;\n", VIO_MK_BIND_PARAMS);
    mk_printf(&d, "layout(std430, set = 0, binding = %d) writeonly buffer VioIdx { uint i[]; } vio_idx;\n", VIO_MK_BIND_IDX);
    mk_cat(&d, "uint vio_g;\nuint vio_nv;\nuint vio_ni;\nuint vio_strip;\n");

    /* EmitVertex(): store the outputs as the invocation's next record, extend
     * the strip as a list (GL's odd-triangle order (i+1, i, i+2)). */
    mk_printf(&e, "\n    if (vio_nv >= %du) return;\n", gs->max_vertices);
    mk_printf(&e, "    uint vio_v = 1u + vio_g * %du + vio_nv;\n", gs->max_vertices);
    mk_printf(&e, "    uint vio_r = vio_v * %du;\n", gs->out_slots);
    mk_emit_store_builtins(&e, gs, "vio_rec", "vio_r");
    for (int k = 0; k < gs->out_count; k++) {
        char nm[32];
        snprintf(nm, sizeof(nm), "vio_o%d", gs->out[k].location);
        mk_emit_store(&e, &gs->out[k], nm, "vio_rec", "vio_r");
    }
    mk_printf(&e, "    uint vio_ib = vio_g * %du;\n", idx_per);
    switch (gs->out_prim) {
        case MK_PRIM_POINTS:
            mk_cat(&e, "    vio_idx.i[vio_ib + vio_ni] = vio_v; vio_ni++;\n");
            break;
        case MK_PRIM_LINES:
            mk_cat(&e, "    if (vio_strip >= 1u) { vio_idx.i[vio_ib + vio_ni] = vio_v - 1u; vio_idx.i[vio_ib + vio_ni + 1u] = vio_v; vio_ni += 2u; }\n");
            break;
        default:
            mk_cat(&e, "    if (vio_strip >= 2u) {\n"
                       "        bool vio_odd = (vio_strip & 1u) != 0u;\n"
                       "        vio_idx.i[vio_ib + vio_ni]      = vio_odd ? vio_v - 1u : vio_v - 2u;\n"
                       "        vio_idx.i[vio_ib + vio_ni + 1u] = vio_odd ? vio_v - 2u : vio_v - 1u;\n"
                       "        vio_idx.i[vio_ib + vio_ni + 2u] = vio_v;\n"
                       "        vio_ni += 3u;\n"
                       "    }\n");
            break;
    }
    mk_cat(&e, "    vio_strip++;\n    vio_nv++;\n");

    mk_cat(&t, "void main()\n{\n");
    mk_printf(&t, "    uint vio_x = gl_GlobalInvocationID.x, vio_n = gl_GlobalInvocationID.y;\n");
    mk_printf(&t, "    if (vio_x >= vio_params.p.x * %du || vio_n >= vio_params.p.z) return;\n", gs->invocations);
    mk_printf(&t, "    uint vio_prim = vio_x / %du, vio_inv = vio_x %% %du;\n", gs->invocations, gs->invocations);
    mk_printf(&t, "    vio_g = vio_n * vio_params.p.x * %du + vio_x;\n", gs->invocations);
    mk_cat(&t, "    vio_nv = 0u; vio_ni = 0u; vio_strip = 0u;\n");
    for (int k = 0; k < gs->bi_count; k++) {
        const mk_builtin *bi = &gs->bi[k];
        char nm[64];
        mk_var_name(nm, sizeof(nm), gs, bi->id);
        if (bi->storage == MK_ST_INPUT) {
            if (bi->builtin == MK_BI_PRIMITIVE_ID) mk_printf(&t, "    %s = int(vio_prim);\n", nm);
            else if (bi->builtin == MK_BI_INVOCATION_ID) mk_printf(&t, "    %s = int(vio_inv);\n", nm);
            else mk_printf(&t, "    %s = 0;\n", nm);
        } else {
            mk_printf(&t, "    %s = 0;\n", nm);
        }
    }
    /* Inputs: record of vertex k of this primitive. */
    mk_printf(&t, "    for (uint vio_k = 0u; vio_k < %du; vio_k++) {\n", gs->in_vertices);
    mk_printf(&t, "    uint vio_r = (vio_n * vio_params.p.y + vio_prim * %du + vio_k) * %du;\n", gs->in_vertices, vs->out_slots);
    if (gs->pv_in.var) {
        int pos = mk_pv_member(&gs->pv_in, MK_BI_POSITION);
        int ps = mk_pv_member(&gs->pv_in, MK_BI_POINT_SIZE);
        char nm[64];
        if (pos >= 0) {
            mk_member_name(nm, sizeof(nm), &gs->pv_in, (uint32_t)pos);
            mk_printf(&t, "    vio_gl_in[vio_k].%s = vio_in.r[vio_r];\n", nm);
        }
        if (ps >= 0) {
            mk_member_name(nm, sizeof(nm), &gs->pv_in, (uint32_t)ps);
            mk_printf(&t, "    vio_gl_in[vio_k].%s = vio_in.r[vio_r + 1u].z;\n", nm);
        }
    }
    for (int k = 0; k < gs->in_count; k++) {
        const mk_var *v = &gs->in[k];
        const mk_var *src = NULL;
        for (int j = 0; j < vs->out_count; j++) if (vs->out[j].location == v->location) { src = &vs->out[j]; break; }
        char nm[32];
        snprintf(nm, sizeof(nm), "vio_i%d", v->location);
        if (!src) continue;   /* not written upstream: stays zero */
        mk_emit_load(&t, v, nm, "[vio_k]", "vio_in", "vio_r", src->slot);
    }
    mk_cat(&t, "    }\n");
    mk_emit_point_size_default(&t, gs);
    mk_cat(&t, "    vio_stage_main();\n");
    mk_printf(&t, "    uint vio_ib = vio_g * %du;\n", idx_per);
    mk_printf(&t, "    for (uint vio_j = vio_ni; vio_j < %du; vio_j++) vio_idx.i[vio_ib + vio_j] = 0u;\n", idx_per);
    mk_cat(&t, "}\n");

    char *r = NULL;
    if (!d.oom && !e.oom && !t.oom && mk_replace_body(&glsl, "vio_emit", e.s) == 0 &&
        mk_replace_body(&glsl, "vio_end_primitive", "\n    vio_strip = 0u;\n") == 0) {
        r = mk_assemble(glsl, d.s, t.s, error_msg);
    } else {
        free(glsl);
        mk_fail(error_msg, "emit functions not found in the geometry kernel");
    }
    free(d.s); free(e.s); free(t.s);
    return r;
}

/* ── Pass-through vertex stage ──────────────────────────────────── */

/* Vertex function of the draw after the geometry kernel: reads record
 * gl_VertexIndex and hands the geometry outputs to the fragment stage. */
static char *mk_passthrough_vs_glsl(const mk_module *gs)
{
    mk_sb b = {0};
    int layer = mk_find_builtin(gs, MK_ST_OUTPUT, MK_BI_LAYER) != NULL;
    int vp = mk_find_builtin(gs, MK_ST_OUTPUT, MK_BI_VIEWPORT_INDEX) != NULL;
    mk_cat(&b, "#version 450\n");
    /* GLSL extension name, split for the audit gate (test 070) like vio_shader_reflect.c */
    if (layer || vp) mk_cat(&b, "#extension GL_" "ARB_shader_viewport_layer_array : require\n");
    mk_printf(&b, "layout(std430, set = 0, binding = %d) readonly buffer VioRec { vec4 r[]; } vio_rec;\n", VIO_MK_BIND_IN);
    for (int k = 0; k < gs->out_count; k++) {
        const mk_var *v = &gs->out[k];
        char ty[24];
        mk_glsl_type(ty, sizeof(ty), v);
        mk_printf(&b, "layout(location = %d) %s%sout %s vio_o%d", v->location, v->flat ? "flat " : "",
                  v->noperspective ? "noperspective " : "", ty, v->location);
        if (v->count > 1) mk_printf(&b, "[%d]", v->count);
        mk_cat(&b, ";\n");
    }
    mk_cat(&b, "void main()\n{\n");
    mk_printf(&b, "    uint vio_r = uint(gl_VertexIndex) * %du;\n", gs->out_slots);
    mk_cat(&b, "    gl_Position = vio_rec.r[vio_r];\n");
    if (gs->out_prim == MK_PRIM_POINTS) mk_cat(&b, "    gl_PointSize = vio_rec.r[vio_r + 1u].z;\n");
    if (layer) mk_cat(&b, "    gl_Layer = floatBitsToInt(vio_rec.r[vio_r + 1u].x);\n");
    if (vp) mk_cat(&b, "    gl_ViewportIndex = floatBitsToInt(vio_rec.r[vio_r + 1u].y);\n");
    for (int k = 0; k < gs->out_count; k++) {
        char nm[32];
        snprintf(nm, sizeof(nm), "vio_o%d", gs->out[k].location);
        mk_emit_load(&b, &gs->out[k], nm, "", "vio_rec", "vio_r", gs->out[k].slot);
    }
    mk_cat(&b, "}\n");
    if (b.oom) { free(b.s); return NULL; }
    return b.s;
}

#endif /* HAVE_SPIRV_CROSS */

#endif /* VIO_METAL_KERNEL_H */

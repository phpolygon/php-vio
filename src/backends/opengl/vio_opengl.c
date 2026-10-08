/*
 * php-vio - OpenGL Core Backend implementation
 *
 * Floor target: GL 3.3 Core / GLSL 330. The window system tries a context
 * ladder (see vio_window.c) and we use whatever it negotiated — the runtime
 * GLSL version is what we ask SPIRV-Cross to emit, so on a 4.1 context we
 * still get 410, on 3.3 we get 330, and the shader sources work either way.
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "php.h"

#ifdef HAVE_OPENGL

#include <glad/glad.h>
#include "../../vio_shader_cache.h"

#include "vio_opengl.h"
#include "../../shaders/default_shaders.h"
#include "../../vio_window.h"
#include "../../vio_mesh.h"
#include "../../vio_render_target.h"
#include "../../vio_texfmt.h"
#include "../../vio_cubemap.h"
#include "../../vio_font.h"
#include "../../vio_pipeline.h"
#include "../../vio_texture.h"
#include "../../vio_buffer.h"
#include "../../vio_shader.h"
#include "../../vio_shader_compiler.h"
#include "../../vio_shader_reflect.h"

vio_opengl_state vio_gl = {0};

/* Monotonic id of the GL context vio_gl currently describes. Every GL name a
 * vio object owns is stamped with the generation it was created under and the
 * destructors compare it: a PHP object can outlive its context (freed during
 * GC after vio_destroy, or only once a NEW context is current), and a GL
 * name of a dead context is either invalid or — worse — already handed out
 * again by the next context, so glDelete* would silently kill a live object
 * (the second OpenGL context in one process rendered black). Lives outside
 * vio_gl because opengl_init() memsets that struct per context. */
static unsigned int gl_context_generation = 0;

unsigned int vio_opengl_context_generation(void)
{
    return gl_context_generation;
}

/* True when the names stamped with `gen` belong to the live context. */
static int gl_owned_by_live_context(unsigned int gen)
{
    return vio_gl.initialized && gen == gl_context_generation;
}

/* ── Uniform-name resolution for SPIR-V-path shaders ──────────────────
 *
 * vio_spirv_to_glsl() asks SPIRV-Cross to emit uniform buffers as plain
 * uniforms (GLSL 4.1 on macOS has no usable UBO binding story), and glslang
 * puts loose `uniform mat4 u_mvp;` declarations of a #version 450 source into
 * gl_DefaultUniformBlock. Both end up as ONE struct-typed uniform in the
 * transpiled GLSL — `uniform Matrices _19;` — so the names GL knows are
 * `_19.uProjection`, while PHP calls vio_set_uniform('uProjection', …).
 * glGetUniformLocation("uProjection") returned -1 and the value was silently
 * dropped: every uniform of every non-raw shader was dead on OpenGL.
 *
 * Resolve an exact match first, then fall back to the `<struct>.name` (or
 * `<struct>.name[0]` for arrays, whose location covers the whole array) suffix
 * among the program's active uniforms. Lookups are cached per (program, name);
 * entries are dropped when their program is deleted, because GL reuses program
 * ids. */
#define GL_ULOC_CACHE_SIZE 1024
#define GL_ULOC_NAME_MAX   56

/* A name can resolve to more than one location when several stages of the
 * SPIR-V path declare the same uniform: each stage's flattened block becomes
 * its own struct-typed uniform (`_19.u_mvp` in the VS, `_27.u_mvp` in the GS),
 * and GL treats them as independent. vio_set_uniform must then write all of
 * them, so an entry carries the primary location plus up to
 * GL_ULOC_ALT_MAX further matches (-1 = none). */
#define GL_ULOC_ALT_MAX 2

typedef struct {
    GLuint program;   /* 0 = empty slot */
    GLint  loc;
    GLint  alt[GL_ULOC_ALT_MAX];
    char   name[GL_ULOC_NAME_MAX];
} gl_uloc_entry;

static gl_uloc_entry gl_uloc_cache[GL_ULOC_CACHE_SIZE];

static unsigned gl_uloc_hash(GLuint program, const char *name)
{
    unsigned h = 5381u ^ (program * 2654435761u);
    for (; *name; name++) h = h * 33u + (unsigned char)*name;
    return h;
}

static void gl_shadow_forget_program(GLuint program);

static void gl_uloc_forget_program(GLuint program)
{
    if (!program) return;
    for (int i = 0; i < GL_ULOC_CACHE_SIZE; i++) {
        if (gl_uloc_cache[i].program == program) gl_uloc_cache[i].program = 0;
    }
    gl_shadow_forget_program(program);
}

/* Copy `name` into `out`, rewriting every "[<digits>]" to "[0]" — the form in
 * which glGetActiveUniform() reports array elements. */
static size_t gl_uloc_normalize(const char *name, char *out, size_t cap)
{
    size_t o = 0;
    for (const char *p = name; *p && o + 4 < cap; p++) {
        if (*p == '[') {
            const char *q = p + 1;
            while (*q >= '0' && *q <= '9') q++;
            if (*q == ']' && q > p + 1) { out[o++] = '['; out[o++] = '0'; out[o++] = ']'; p = q; continue; }
        }
        out[o++] = *p;
    }
    out[o] = '\0';
    return o;
}

/* Collect every `<prefix>.<name>` match among the active uniforms into out[]
 * (at most max entries). Returns the number found. */
static int gl_uloc_suffix_scan(GLuint program, const char *name, size_t len, GLint *out, int max)
{
    char norm[256];
    size_t nlen = gl_uloc_normalize(name, norm, sizeof(norm));
    GLint count = 0;
    glGetProgramiv(program, GL_ACTIVE_UNIFORMS, &count);
    char buf[256], full[512];
    int found = 0;
    for (GLint i = 0; i < count && found < max; i++) {
        GLsizei n = 0; GLint size = 0; GLenum type = 0;
        glGetActiveUniform(program, (GLuint)i, (GLsizei)sizeof(buf), &n, &size, &type, buf);
        /* active name = "<prefix>.<norm>" ? */
        if (n <= (GLsizei)nlen + 1 || buf[n - nlen - 1] != '.' || strcmp(buf + n - nlen, norm) != 0) continue;
        size_t plen = (size_t)n - nlen - 1;
        if (plen + 1 + len + 1 > sizeof(full)) continue;
        memcpy(full, buf, plen);
        full[plen] = '.';
        memcpy(full + plen + 1, name, len + 1);
        GLint loc = glGetUniformLocation(program, full);   /* exact element (or element 0 for a bare array name) */
        if (loc >= 0) out[found++] = loc;
    }
    return found;
}
/* Resolve `name` to its primary location (return value) and, for uniforms
 * declared in several stages, the further locations in alt[GL_ULOC_ALT_MAX]
 * (-1 padded). alt may be NULL. */
static GLint gl_uniform_location_all(GLuint program, const char *name, GLint *alt)
{
    size_t len = strlen(name);
    int cacheable = len < GL_ULOC_NAME_MAX;
    unsigned idx = gl_uloc_hash(program, name) % GL_ULOC_CACHE_SIZE;
    gl_uloc_entry *slot = NULL;
    if (cacheable) {
        for (int probe = 0; probe < 8; probe++) {
            gl_uloc_entry *e = &gl_uloc_cache[(idx + probe) % GL_ULOC_CACHE_SIZE];
            if (e->program == program && strcmp(e->name, name) == 0) {
                if (alt) memcpy(alt, e->alt, sizeof(e->alt));
                return e->loc;
            }
            if (e->program == 0 && !slot) slot = e;
        }
        if (!slot) slot = &gl_uloc_cache[idx];   /* window full: evict */
    }

    GLint found[1 + GL_ULOC_ALT_MAX];
    for (int i = 0; i < 1 + GL_ULOC_ALT_MAX; i++) found[i] = -1;
    GLint loc = glGetUniformLocation(program, name);
    if (loc < 0) {
        int n = gl_uloc_suffix_scan(program, name, len, found, 1 + GL_ULOC_ALT_MAX);
        if (n > 0) loc = found[0];
        for (int i = n; i < 1 + GL_ULOC_ALT_MAX; i++) found[i] = -1;
    }

    if (slot) {
        slot->program = program;
        slot->loc = loc;
        memcpy(slot->alt, found + 1, sizeof(slot->alt));
        memcpy(slot->name, name, len + 1);
    }
    if (alt) memcpy(alt, found + 1, sizeof(GLint) * GL_ULOC_ALT_MAX);
    return loc;
}
static GLint gl_uniform_location(GLuint program, const char *name)
{
    return gl_uniform_location_all(program, name, NULL);
}

/* ── Shader compilation helpers ───────────────────────────────────── */

static unsigned int compile_shader_stage(const char *source, GLenum type)
{
    unsigned int shader = glCreateShader(type);
    glShaderSource(shader, 1, &source, NULL);
    glCompileShader(shader);

    int success;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &success);
    if (!success) {
        char log[512];
        glGetShaderInfoLog(shader, sizeof(log), NULL, log);
        php_error_docref(NULL, E_WARNING, "OpenGL shader compilation failed: %s", log);
        glDeleteShader(shader);
        return 0;
    }
    return shader;
}

/* Program-binary cache (GAP-PHASE5 Block 4, GL >= 4.1 glGetProgramBinary). The
 * binary is driver-specific, so the key also hashes GL_VENDOR / GL_RENDERER /
 * GL_VERSION; a stale or rejected binary just falls through to a normal link.
 * File layout: 4 bytes binaryFormat + the blob. */
static uint64_t opengl_program_cache_key(const char *const sources[5])
{
    /* Every stage is part of the key, and an absent stage hashes differently
     * from an empty one: the same VS + FS with and without a geometry stage
     * are distinct programs. */
    uint64_t key = vio_shader_cache_hash("gl-program", sources[0], strlen(sources[0]));
    for (int i = 1; i < 5; i++) {
        key = vio_shader_cache_hash_more(key, sources[i] ? "|" : "#", 1);
        if (sources[i]) key = vio_shader_cache_hash_more(key, sources[i], strlen(sources[i]));
    }
    const char *vendor = (const char *)glGetString(GL_VENDOR);
    const char *renderer = (const char *)glGetString(GL_RENDERER);
    const char *version = (const char *)glGetString(GL_VERSION);
    if (vendor)   key = vio_shader_cache_hash_more(key, vendor, strlen(vendor));
    if (renderer) key = vio_shader_cache_hash_more(key, renderer, strlen(renderer));
    if (version)  key = vio_shader_cache_hash_more(key, version, strlen(version));
    return key;
}

static unsigned int opengl_program_from_cache(uint64_t key)
{
    size_t len = 0;
    unsigned char *data = vio_shader_cache_load(key, "glpb", &len);
    if (!data) return 0;
    unsigned int program = 0;
    if (len > 4) {
        GLenum format = 0;
        memcpy(&format, data, 4);
        program = glCreateProgram();
        glProgramBinary(program, format, data + 4, (GLsizei)(len - 4));
        int ok = 0;
        glGetProgramiv(program, GL_LINK_STATUS, &ok);
        if (!ok) { glDeleteProgram(program); program = 0; }
    }
    free(data);
    return program;
}

static void opengl_program_to_cache(uint64_t key, unsigned int program)
{
    int len = 0;
    glGetProgramiv(program, GL_PROGRAM_BINARY_LENGTH, &len);
    if (len <= 0) return;
    unsigned char *buf = malloc((size_t)len + 4);
    if (!buf) return;
    GLenum format = 0;
    GLsizei written = 0;
    glGetProgramBinary(program, len, &written, &format, buf + 4);
    if (written > 0) {
        memcpy(buf, &format, 4);
        vio_shader_cache_store(key, "glpb", buf, (size_t)written + 4);
    }
    free(buf);
}

unsigned int vio_opengl_compile_shader_source(const char *vert_src, const char *frag_src)
{
    return vio_opengl_compile_program(vert_src, frag_src, NULL, NULL, NULL);
}

unsigned int vio_opengl_compile_program(const char *vert_src, const char *frag_src,
                                        const char *geom_src, const char *tesc_src,
                                        const char *tese_src)
{
    const char *sources[5] = { vert_src, frag_src, geom_src, tesc_src, tese_src };
    const GLenum types[5]  = { GL_VERTEX_SHADER, GL_FRAGMENT_SHADER, GL_GEOMETRY_SHADER,
                               GL_TESS_CONTROL_SHADER, GL_TESS_EVALUATION_SHADER };
    unsigned int stages[5] = { 0, 0, 0, 0, 0 };
    int n = 0;

    if (!vert_src || !frag_src) return 0;
    int use_cache = vio_shader_cache_dir() != NULL && GLAD_GL_VERSION_4_1;
    uint64_t key = 0;
    if (use_cache) {
        key = opengl_program_cache_key(sources);
        unsigned int cached = opengl_program_from_cache(key);
        if (cached) return cached;
    }

    for (int i = 0; i < 5; i++) {
        if (!sources[i]) continue;
        stages[i] = compile_shader_stage(sources[i], types[i]);
        if (!stages[i]) {
            for (int j = 0; j < i; j++) if (stages[j]) glDeleteShader(stages[j]);
            return 0;
        }
        n++;
    }
    if (!stages[0] || !stages[1]) {
        for (int j = 0; j < 5; j++) if (stages[j]) glDeleteShader(stages[j]);
        return 0;
    }

    unsigned int program = glCreateProgram();
    for (int i = 0; i < 5; i++) if (stages[i]) glAttachShader(program, stages[i]);
    if (use_cache) glProgramParameteri(program, GL_PROGRAM_BINARY_RETRIEVABLE_HINT, GL_TRUE);
    glLinkProgram(program);

    int success;
    glGetProgramiv(program, GL_LINK_STATUS, &success);
    if (!success) {
        char log[512];
        glGetProgramInfoLog(program, sizeof(log), NULL, log);
        php_error_docref(NULL, E_WARNING, "OpenGL shader link failed: %s", log);
        glDeleteProgram(program);
        program = 0;
    } else if (use_cache) {
        opengl_program_to_cache(key, program);
    }

    for (int i = 0; i < 5; i++) if (stages[i]) glDeleteShader(stages[i]);
    (void)n;
    return program;
}

void vio_opengl_delete_program(unsigned int program)
{
    if (program) {
        gl_uloc_forget_program(program);
        glDeleteProgram(program);
    }
}

/* ── Backend vtable implementation ────────────────────────────────── */

static int opengl_init(vio_config *cfg)
{
    (void)cfg;
    /* GLFW window hints for OpenGL must be set before window creation.
     * We handle this by re-creating the window with OpenGL hints
     * in create_surface. init() just resets state. */
    memset(&vio_gl, 0, sizeof(vio_gl));
    vio_gl.clear_r = 0.1f;
    vio_gl.clear_g = 0.1f;
    vio_gl.clear_b = 0.1f;
    vio_gl.clear_a = 1.0f;
    vio_gl.draw_topology = VIO_TRIANGLES;
    vio_gl.patch_vertices = 3;
    return 0;
}

static GLenum gl_topology_mode(vio_topology t);

/* Primitive mode for the mesh draw calls: the bound pipeline's topology
 * (GL_TRIANGLES until a pipeline says otherwise). */
static GLenum gl_draw_mode(void)
{
    return gl_topology_mode((vio_topology)vio_gl.draw_topology);
}

static GLenum gl_topology_mode(vio_topology t)
{
    switch (t) {
        case VIO_TRIANGLE_STRIP: return GL_TRIANGLE_STRIP;
        case VIO_TRIANGLE_FAN:   return GL_TRIANGLE_FAN;
        case VIO_LINES:          return GL_LINES;
        case VIO_LINE_STRIP:     return GL_LINE_STRIP;
        case VIO_POINTS:         return GL_POINTS;
        case VIO_LINES_ADJACENCY:          return GL_LINES_ADJACENCY;
        case VIO_LINE_STRIP_ADJACENCY:     return GL_LINE_STRIP_ADJACENCY;
        case VIO_TRIANGLES_ADJACENCY:      return GL_TRIANGLES_ADJACENCY;
        case VIO_TRIANGLE_STRIP_ADJACENCY: return GL_TRIANGLE_STRIP_ADJACENCY;
        case VIO_PATCHES:        return GL_PATCHES;
        case VIO_TRIANGLES:
        default:                 return GL_TRIANGLES;
    }
}

/* vio_gpu_info: GL_RENDERER of the live context; GL has no portable query for
 * dedicated video memory, so that stays 0. */
static void opengl_gpu_info(const char **name, uint64_t *vram_bytes)
{
    static char gpu_name[256];
    (void)vram_bytes;
    if (!vio_gl.initialized) return;
    const char *renderer = (const char *)glGetString(GL_RENDERER);
    if (!renderer) return;
    snprintf(gpu_name, sizeof(gpu_name), "%s", renderer);
    *name = gpu_name;
}

static int opengl_supports_feature(vio_feature feature);

/* vio_backend_info (A4): GL has no PCI vendor id and no device type query, so
 * both come from GL_VENDOR / GL_RENDERER; software rasterizers (llvmpipe,
 * softpipe, Microsoft's GDI renderer) report their vendor as Mesa / Microsoft. */
static int opengl_describe(vio_backend_description *out)
{
    static char api[32], device[256], driver[256], core[24];
    if (!vio_gl.initialized || !out) return -1;
    const char *vendor = (const char *)glGetString(GL_VENDOR);
    const char *renderer = (const char *)glGetString(GL_RENDERER);
    const char *version = (const char *)glGetString(GL_VERSION);
    if (!vendor) vendor = "";
    if (!renderer) renderer = "";
    snprintf(api, sizeof(api), "OpenGL %d.%d", vio_gl.gl_major, vio_gl.gl_minor);
    snprintf(core, sizeof(core), "core_%d_%d", vio_gl.gl_major, vio_gl.gl_minor);
    snprintf(device, sizeof(device), "%s", renderer[0] ? renderer : "OpenGL");
    snprintf(driver, sizeof(driver), "%s", version ? version : "");
    int software = strstr(renderer, "llvmpipe") || strstr(renderer, "softpipe")
                || strstr(renderer, "GDI Generic") || strstr(renderer, "SwiftShader");
    uint32_t id = 0;
    if (strstr(renderer, "GDI Generic") || strstr(vendor, "Microsoft")) id = 0x1414;
    else if (software || strstr(vendor, "Mesa") || strstr(vendor, "VMware")) id = 0x10005;
    else if (strstr(vendor, "NVIDIA")) id = 0x10DE;
    else if (strstr(vendor, "ATI") || strstr(vendor, "AMD")) id = 0x1002;
    else if (strstr(vendor, "Intel")) id = 0x8086;
    else if (strstr(vendor, "Apple")) id = 0x106B;
    else if (strstr(vendor, "ARM")) id = 0x13B5;
    else if (strstr(vendor, "Qualcomm")) id = 0x5143;
    out->api = api;
    out->device = device;
    out->shading_language = "GLSL";
    out->shading_language_version = vio_gl.glsl_version / 10;
    out->shading_language_max = vio_gl.glsl_version / 10;
    out->family_count = 0;
    out->families[out->family_count++] = core;
    out->cap_count = 0;
    vio_describe_feature_caps(out, opengl_supports_feature);
    out->vendor_id = id;
    out->driver = driver;
    out->device_type = software ? "software" : NULL;
    out->vram_bytes = 0;
    return 0;
}

/* vio_adapters (A6): GL cannot list adapters, it only knows the one its live
 * context runs on. */
static int opengl_enumerate_adapters(vio_adapter_info *out, int max)
{
    vio_backend_description d;
    if (max < 1 || !vio_gl.initialized) return 0;
    memset(&d, 0, sizeof(d));
    if (opengl_describe(&d) != 0) return 0;
    memset(out, 0, sizeof(*out));
    snprintf(out->name, sizeof(out->name), "%s", d.device);
    snprintf(out->driver, sizeof(out->driver), "%s", d.driver ? d.driver : "");
    out->vendor_id = d.vendor_id;
    out->device_type = d.device_type;
    for (int f = 0; f < 64; f++)
        if (opengl_supports_feature((vio_feature)f)) out->features |= VIO_FEATURE_BIT(f);
    return 1;
}

static void opengl_shutdown(void)
{
    if (vio_gl.default_shader_program) {
        glDeleteProgram(vio_gl.default_shader_program);
        vio_gl.default_shader_program = 0;
    }
    if (vio_gl.default_shader_pos_only) {
        glDeleteProgram(vio_gl.default_shader_pos_only);
        vio_gl.default_shader_pos_only = 0;
    }
    if (vio_gl.extensions) {
        for (int i = 0; i < vio_gl.extension_count; i++) {
            free(vio_gl.extensions[i]);
        }
        free(vio_gl.extensions);
        vio_gl.extensions = NULL;
        vio_gl.extension_count = 0;
    }
    if (vio_gl.renderer) { free(vio_gl.renderer); vio_gl.renderer = NULL; }
    if (vio_gl.vendor)   { free(vio_gl.vendor);   vio_gl.vendor   = NULL; }
    memset(gl_uloc_cache, 0, sizeof(gl_uloc_cache));   /* program ids die with the context */
    vio_gl.initialized = 0;
}

static void *opengl_create_surface(vio_config *cfg)
{
    (void)cfg;
    /* Surface creation is handled by the window system for OpenGL */
    return NULL;
}

static void opengl_destroy_surface(void *surface)
{
    (void)surface;
}

static void opengl_resize(int width, int height)
{
    glViewport(0, 0, width, height);
}

/* GL keeps pipeline state in the VioPipeline object and applies it when the
 * pipeline is bound (php_vio.c), so there is no backend handle. */
static void *opengl_create_pipeline(vio_pipeline_desc *desc)
{
    (void)desc;
    return NULL;
}

static void opengl_destroy_pipeline(void *pipeline)
{
    (void)pipeline;
}

static void opengl_bind_pipeline(void *pipeline)
{
    (void)pipeline;
}

/* ── Compute primitive: backend storage-buffer + pipeline structs ─────
 *
 * A storage buffer is a GL SSBO (GL_SHADER_STORAGE_BUFFER). The PHP layer's
 * vio_storage_buffer() routes through create_buffer(VIO_BUFFER_STORAGE), so a
 * STORAGE desc produces one of these handles; every other buffer type stays on
 * the mesh/uniform paths and create_buffer returns NULL as before. */
typedef struct _vio_opengl_compute_buffer {
    GLuint  ssbo;
    unsigned int gl_generation;
    size_t  size;     /* bytes */
    int     stride;   /* element stride (informational; raw float access in GL) */
} vio_opengl_compute_buffer;

#define VIO_GL_COMPUTE_MAX_BINDINGS 8

typedef struct _vio_opengl_compute_binding {
    vio_opengl_compute_buffer *buffer;
    int slot;     /* GLSL SSBO binding point */
    int access;   /* VIO_COMPUTE_READ (0) / VIO_COMPUTE_WRITE (1) */
} vio_opengl_compute_binding;

typedef struct _vio_opengl_compute_pipeline {
    GLuint program;
    unsigned int gl_generation;
    GLuint ubo;              /* params UBO (lazily (re)created in set_uniforms) */
    GLsizeiptr ubo_capacity; /* current UBO byte capacity */
    int    params_binding;   /* reflected UBO binding point (GLSL binding = 2) */
    vio_opengl_compute_binding bindings[VIO_GL_COMPUTE_MAX_BINDINGS];
    int    binding_count;
    /* Storage images: GL image unit == GLSL binding (glBindImageTexture). */
    struct { GLuint texture; GLboolean layered; int slot; int access; } images[VIO_GL_COMPUTE_MAX_BINDINGS];
    int    image_count;
} vio_opengl_compute_pipeline;

/* Drain glGetError() to stderr (bring-up aid). Loops because GL queues errors. */
static void opengl_drain_gl_errors(const char *where)
{
    GLenum e;
    while ((e = glGetError()) != GL_NO_ERROR) {
        fprintf(stderr, "[OpenGL GL error @ %s] 0x%04x\n", where, (unsigned)e);
        fflush(stderr);
    }
}

static void *opengl_create_buffer(vio_buffer_desc *desc)
{
    if (!desc || desc->type != VIO_BUFFER_STORAGE) {
        return NULL; /* non-storage buffers handled by the mesh/uniform systems */
    }
    if (!vio_gl.initialized || desc->size == 0) return NULL;

    vio_opengl_compute_buffer *buf = calloc(1, sizeof(vio_opengl_compute_buffer));
    if (!buf) return NULL;
    buf->size   = desc->size;
    buf->stride = desc->stride;

    glGenBuffers(1, &buf->ssbo);
    buf->gl_generation = gl_context_generation;
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, buf->ssbo);
    /* Input buffers carry `data` (read-once) -> STATIC_DRAW; output buffers are
     * sized-and-zeroed (NULL data) and read back -> DYNAMIC_DRAW. Either way the
     * store is initialised: when data is NULL we zero-fill so an output buffer
     * that is never written keeps a deterministic 0 (not garbage). */
    GLenum usage = desc->data ? GL_STATIC_DRAW : GL_DYNAMIC_DRAW;
    if (desc->data) {
        glBufferData(GL_SHADER_STORAGE_BUFFER, (GLsizeiptr)desc->size, desc->data, usage);
    } else {
        void *zeros = calloc(1, desc->size);
        glBufferData(GL_SHADER_STORAGE_BUFFER, (GLsizeiptr)desc->size, zeros, usage);
        if (zeros) free(zeros);
    }
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);

    if (!buf->ssbo) { free(buf); return NULL; }
    return buf;
}

/* create_buffer only makes storage buffers (SSBOs) on OpenGL, so that is what
 * this writes. It used to be a stub: vio_update_buffer did nothing on OpenGL. */
static void opengl_update_buffer(void *buffer, const void *data, size_t size, size_t offset)
{
    vio_opengl_compute_buffer *buf = (vio_opengl_compute_buffer *)buffer;
    if (!buf || !data || size == 0 || !buf->ssbo || !vio_gl.initialized) return;
    if (buf->gl_generation != gl_context_generation || offset >= buf->size) return;
    if (size > buf->size - offset) size = buf->size - offset;
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, buf->ssbo);
    glBufferSubData(GL_SHADER_STORAGE_BUFFER, (GLintptr)offset, (GLsizeiptr)size, data);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
}

static void opengl_destroy_buffer(void *buffer)
{
    (void)buffer;
}

/* GL textures are created by the VioTexture paths that own the GL name; the
 * generic slot is not used on this backend. */
static void *opengl_create_texture(vio_texture_desc *desc)
{
    (void)desc;
    return NULL;
}

static void opengl_destroy_texture(void *texture)
{
    (void)texture;
}

static void *opengl_compile_shader(vio_shader_desc *desc)
{
    if (!desc || !desc->vertex_data || !desc->fragment_data) {
        return NULL;
    }
    unsigned int program = vio_opengl_compile_shader_source(
        (const char *)desc->vertex_data,
        (const char *)desc->fragment_data
    );
    /* Store as void* - caller must manage lifetime */
    return (void *)(uintptr_t)program;
}

static void opengl_destroy_shader(void *shader)
{
    unsigned int program = (unsigned int)(uintptr_t)shader;
    if (program) {
        gl_uloc_forget_program(program);
        glDeleteProgram(program);
    }
}

static int opengl_has_timer_query(void)
{
    return vio_gl.initialized && GLAD_GL_VERSION_3_3;
}

static void opengl_begin_frame(void)
{
    glClearColor(vio_gl.clear_r, vio_gl.clear_g, vio_gl.clear_b, vio_gl.clear_a);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
    vio_gl.in_frame = 1;

    /* GPU timestamps (GAP-PHASE5 Block 3): names belong to the current context
     * generation; harvest the slot about to be reused, then stamp the start. */
    if (opengl_has_timer_query()) {
        if (vio_gl.ts_generation != gl_context_generation || vio_gl.ts_query[0][0] == 0) {
            glGenQueries(3 * VIO_GPU_TS_PER_FRAME, &vio_gl.ts_query[0][0]);
            for (int i = 0; i < 3; i++) { vio_gl.ts_pending[i] = 0; vio_gl.ts_marks[i].count = 0; }
            vio_gl.last_gpu_ms = -1.0;
            vio_gl.ts_result_valid = 0;
            vio_gl.ts_generation = gl_context_generation;
        }
        int slot = vio_gl.ts_slot;
        if (vio_gl.ts_pending[slot]) {
            /* GL_QUERY_RESULT waits for the three-frame-old slot (like the D3D12 /
             * Vulkan fence wait); skipping unfinished frames left the values
             * stale whenever nothing throttles the CPU (headless, vsync off). */
            {
                uint64_t ticks[VIO_GPU_TS_PER_FRAME];
                int n = 2 + vio_gl.ts_marks[slot].count;
                for (int i = 0; i < n; i++) {
                    GLuint64 v = 0;
                    glGetQueryObjectui64v(vio_gl.ts_query[slot][i], GL_QUERY_RESULT, &v);
                    ticks[i] = (uint64_t)v;
                }
                /* >=: a frame shorter than the timer resolution reads 0 ms; skipping
                 * it kept the previous frame's sections (test 172, unmarked frames) */
                if (ticks[1] >= ticks[0]) {
                    vio_gl.last_gpu_ms = (double)(ticks[1] - ticks[0]) / 1.0e6;
                    vio_gpu_mark_resolve(&vio_gl.ts_result, &vio_gl.ts_marks[slot], ticks, 1.0e-6);
                    vio_gl.ts_result_valid = 1;
                }
            }
            vio_gl.ts_pending[slot] = 0;
        }
        vio_gl.ts_marks[slot].count = 0;
        glQueryCounter(vio_gl.ts_query[slot][0], GL_TIMESTAMP);
    }
}

static void opengl_end_frame(void)
{
    if (opengl_has_timer_query() && vio_gl.ts_query[0][0] != 0) {
        int slot = vio_gl.ts_slot;
        glQueryCounter(vio_gl.ts_query[slot][1], GL_TIMESTAMP);
        vio_gl.ts_pending[slot] = 1;
        vio_gl.ts_slot = (slot + 1) % 3;
    }
    /* Flush any pending GL commands */
    glFlush();
    vio_gl.in_frame = 0;
}

static double opengl_gpu_frame_time(void)
{
    return opengl_has_timer_query() && vio_gl.ts_query[0][0] != 0 ? vio_gl.last_gpu_ms : -1.0;
}

static int opengl_gpu_mark(const char *name)
{
    if (!vio_gl.in_frame || !opengl_has_timer_query() || vio_gl.ts_query[0][0] == 0
        || vio_gl.ts_generation != gl_context_generation) return 0;
    int slot = vio_gl.ts_slot;
    int i = vio_gpu_mark_push(&vio_gl.ts_marks[slot], name);
    if (i < 0) return 0;
    glQueryCounter(vio_gl.ts_query[slot][2 + i], GL_TIMESTAMP);
    return 1;
}

static const vio_gpu_mark_result *opengl_gpu_marks(void)
{
    return opengl_has_timer_query() && vio_gl.ts_result_valid ? &vio_gl.ts_result : NULL;
}

static void opengl_draw(vio_draw_cmd *cmd)
{
    (void)cmd;
}

static void opengl_draw_indexed(vio_draw_indexed_cmd *cmd)
{
    (void)cmd;
}

static void opengl_present(void)
{
    /* Swap is handled by the window system (vio_end -> vio_window_swap_buffers) */
}

static void opengl_gpu_flush(void)
{
    glFinish();
}

static void opengl_clear(float r, float g, float b, float a)
{
    vio_gl.clear_r = r;
    vio_gl.clear_g = g;
    vio_gl.clear_b = b;
    vio_gl.clear_a = a;

    /* Eager inside a frame (D3D11 / Metal semantics): clears whatever
     * framebuffer is bound right now — the swapchain / headless FBO or a bound
     * render target. Before vio_begin the colour is latched for begin_frame.
     * Previously the in-frame call was a no-op, which left render targets that
     * were bound and "cleared" mid-frame with undefined depth. */
    if (vio_gl.initialized && vio_gl.in_frame) {
        glClearColor(r, g, b, a);
        glDepthMask(GL_TRUE);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
    }
}

/* ── Compute primitive: pipeline / bind / uniforms / dispatch / readback ──
 *
 * GLSL compute source -> SPIR-V (glslang) -> GLSL >=430 (spirv-cross, UBO/SSBO
 * blocks preserved) -> GL_COMPUTE_SHADER program. Buffers bound via
 * glBindBufferBase: SSBOs in the GL_SHADER_STORAGE_BUFFER binding namespace (so
 * binding 0 and 1 don't collide), the params UBO in the GL_UNIFORM_BUFFER
 * namespace at its reflected binding. SSBO/UBO blocks keep explicit `binding=N`
 * qualifiers from the source, so no glShaderStorageBlockBinding/glUniformBlock-
 * Binding fixup is needed; we apply it defensively only if reflection disagrees. */

static void *opengl_create_compute_pipeline(vio_shader_desc *desc)
{
    if (!vio_gl.initialized || !vio_gl.caps.has_compute_shader) {
        php_error_docref(NULL, E_WARNING, "OpenGL: compute requires GL >= 4.3");
        return NULL;
    }

    const char *src = (const char *)desc->fragment_data;
    if (!src && desc->vertex_data) src = (const char *)desc->vertex_data;
    if (!src) {
        php_error_docref(NULL, E_WARNING, "OpenGL: compute pipeline missing source");
        return NULL;
    }
    size_t src_size = desc->fragment_size ? desc->fragment_size : desc->vertex_size;

    char *err = NULL;
    uint32_t *spirv = NULL;
    size_t spirv_size = 0;
    int free_spirv = 0;

    int is_spirv = (src_size >= 4 && *(const uint32_t *)src == 0x07230203);
    if (is_spirv) {
        spirv = (uint32_t *)src;
        spirv_size = src_size;
    } else {
        spirv = vio_compile_glsl_compute_to_spirv(src, &spirv_size, &err);
        if (!spirv) {
            php_error_docref(NULL, E_WARNING, "OpenGL: CS GLSL->SPIR-V failed: %s", err ? err : "unknown");
            if (err) free(err);
            return NULL;
        }
        free_spirv = 1;
    }

    /* Reflect the params UBO binding (GLSL binding = 2 in the canonical shader).
     * Defaults to 0 if reflection is unavailable, matching the bind fallback. */
    int params_binding = 0;
    {
        vio_reflect_result refl;
        char *rerr = NULL;
        if (vio_spirv_reflect(spirv, spirv_size, &refl, &rerr) == 0) {
            if (refl.ubo_count > 0) {
                params_binding = (int)refl.ubos[0].binding;
                for (int i = 1; i < refl.ubo_count; i++) {
                    if ((int)refl.ubos[i].binding < params_binding)
                        params_binding = (int)refl.ubos[i].binding;
                }
            }
            vio_reflect_free(&refl);
        } else {
            if (rerr) free(rerr);
        }
    }

    /* Emit GLSL >= the active context version (>=430). spirv-cross keeps the
     * std140 UBO + std430 SSBO blocks with explicit bindings. */
    int glsl_version = vio_gl.glsl_version >= 430 ? vio_gl.glsl_version : 430;
    char *glsl = vio_spirv_to_glsl_compute(spirv, spirv_size, glsl_version, &err);
    if (free_spirv) free(spirv);
    if (!glsl && !is_spirv && vio_gl.caps.has_subgroup) {
        /* SPIRV-Cross refuses subgroup operations outside Vulkan semantics (shuffle,
         * min / max, quad); a driver with GL_KHR_shader_subgroup takes the caller's
         * GLSL as it is. */
        glsl = strdup(src);
        if (err) { free(err); err = NULL; }
    }
    if (!glsl) {
        php_error_docref(NULL, E_WARNING, "OpenGL: CS SPIR-V->GLSL failed: %s", err ? err : "unknown");
        if (err) free(err);
        return NULL;
    }

    GLuint shader = glCreateShader(GL_COMPUTE_SHADER);
    const char *gsrc = glsl;
    glShaderSource(shader, 1, &gsrc, NULL);
    glCompileShader(shader);
    GLint ok = 0;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[1024];
        glGetShaderInfoLog(shader, sizeof(log), NULL, log);
        php_error_docref(NULL, E_WARNING, "OpenGL: compute shader compile failed: %s", log);
        if (getenv("VIO_DUMP_CS_GLSL")) {
            fprintf(stderr, "==== failed compute GLSL ====\n%s\n==== end ====\n", glsl);
        }
        free(glsl);
        glDeleteShader(shader);
        return NULL;
    }
    free(glsl);

    GLuint program = glCreateProgram();
    glAttachShader(program, shader);
    glLinkProgram(program);
    glDeleteShader(shader); /* flagged for deletion; freed when detached at link */
    glGetProgramiv(program, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[1024];
        glGetProgramInfoLog(program, sizeof(log), NULL, log);
        php_error_docref(NULL, E_WARNING, "OpenGL: compute program link failed: %s", log);
        glDeleteProgram(program);
        return NULL;
    }

    vio_opengl_compute_pipeline *cp = calloc(1, sizeof(vio_opengl_compute_pipeline));
    if (!cp) { glDeleteProgram(program); return NULL; }
    cp->program = program;
    cp->gl_generation = gl_context_generation;
    cp->params_binding = params_binding;

    /* Defensive: if the UBO block lost its explicit binding (older spirv-cross),
     * pin it to params_binding so glBindBufferBase(GL_UNIFORM_BUFFER, ...) lands. */
    GLuint blk = glGetUniformBlockIndex(program, "Params");
    if (blk != GL_INVALID_INDEX) {
        glUniformBlockBinding(program, blk, (GLuint)params_binding);
    }

    return cp;
}

static void opengl_destroy_compute_pipeline(void *pipeline_ptr)
{
    vio_opengl_compute_pipeline *cp = (vio_opengl_compute_pipeline *)pipeline_ptr;
    if (!cp) return;
    if (gl_owned_by_live_context(cp->gl_generation)) {
        if (cp->ubo) glDeleteBuffers(1, &cp->ubo);
        if (cp->program) glDeleteProgram(cp->program);
    }
    free(cp);
}

static void opengl_compute_bind_buffer(void *pipeline_ptr, void *backend_buffer,
                                       int slot, int access, int element_count, int stride)
{
    (void)element_count; (void)stride; /* raw float SSBO access — view metadata unused in GL */
    vio_opengl_compute_pipeline *cp = (vio_opengl_compute_pipeline *)pipeline_ptr;
    vio_opengl_compute_buffer *buf = (vio_opengl_compute_buffer *)backend_buffer;
    if (!cp || !buf) return;
    /* One buffer per slot: rebinding a slot replaces its binding (the list used
     * to only grow and dropped every bind past the table size). */
    for (int i = 0; i < cp->binding_count; i++) {
        if (cp->bindings[i].slot == slot) { cp->bindings[i].buffer = buf; cp->bindings[i].access = access; return; }
    }
    if (cp->binding_count >= VIO_GL_COMPUTE_MAX_BINDINGS) return;

    vio_opengl_compute_binding *b = &cp->bindings[cp->binding_count++];
    b->buffer = buf;
    b->slot   = slot;
    b->access = access;
}

static void opengl_compute_bind_image(void *pipeline_ptr, void *tex_obj, int slot, int access)
{
    vio_opengl_compute_pipeline *cp = (vio_opengl_compute_pipeline *)pipeline_ptr;
    vio_texture_object *t = (vio_texture_object *)tex_obj;
    if (!cp || !t || !t->texture_id) return;
    for (int i = 0; i < cp->image_count; i++) {
        if (cp->images[i].slot == slot) {
            cp->images[i].texture = t->texture_id; cp->images[i].layered = t->is_3d ? GL_TRUE : GL_FALSE;
            cp->images[i].access = access; return;
        }
    }
    if (cp->image_count >= VIO_GL_COMPUTE_MAX_BINDINGS) return;
    cp->images[cp->image_count].texture = t->texture_id;
    cp->images[cp->image_count].layered = t->is_3d ? GL_TRUE : GL_FALSE;
    cp->images[cp->image_count].slot    = slot;
    cp->images[cp->image_count].access  = access;
    cp->image_count++;
}

static void opengl_compute_set_uniforms(void *pipeline_ptr, const void *data, int size)
{
    vio_opengl_compute_pipeline *cp = (vio_opengl_compute_pipeline *)pipeline_ptr;
    if (!cp || !data || size <= 0 || !vio_gl.initialized) return;

    if (!cp->ubo) {
        glGenBuffers(1, &cp->ubo);
        cp->ubo_capacity = 0;
    }
    glBindBuffer(GL_UNIFORM_BUFFER, cp->ubo);
    if (cp->ubo_capacity < (GLsizeiptr)size) {
        glBufferData(GL_UNIFORM_BUFFER, (GLsizeiptr)size, data, GL_DYNAMIC_DRAW);
        cp->ubo_capacity = (GLsizeiptr)size;
    } else {
        glBufferSubData(GL_UNIFORM_BUFFER, 0, (GLsizeiptr)size, data);
    }
    glBindBuffer(GL_UNIFORM_BUFFER, 0);
}

/* GL dispatches are already asynchronous on the in-order GL queue and the
 * glMemoryBarrier after each dispatch orders them against later draws and
 * readbacks, so there is nothing to wait for explicitly. */
static void opengl_compute_wait(void)
{
}

static void opengl_dispatch_compute(vio_compute_cmd *cmd)
{
    if (!cmd || !vio_gl.initialized) return;
    vio_opengl_compute_pipeline *cp = (vio_opengl_compute_pipeline *)cmd->pipeline;
    if (!cp || !cp->program) {
        php_error_docref(NULL, E_WARNING, "OpenGL: dispatch_compute with invalid pipeline");
        return;
    }

    glUseProgram(cp->program);

    /* SSBOs: GL_SHADER_STORAGE_BUFFER binding namespace, slot == GLSL binding. */
    for (int i = 0; i < cp->binding_count; i++) {
        vio_opengl_compute_binding *b = &cp->bindings[i];
        if (!b->buffer || !b->buffer->ssbo) continue;
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, (GLuint)b->slot, b->buffer->ssbo);
    }

    /* Params UBO: separate GL_UNIFORM_BUFFER namespace, at its reflected binding. */
    if (cp->ubo) {
        glBindBufferBase(GL_UNIFORM_BUFFER, (GLuint)cp->params_binding, cp->ubo);
    }

    /* Storage images: image unit == GLSL binding. The textures were created
     * with a sized RGBA8 internal format (required for image load/store). */
    for (int i = 0; i < cp->image_count; i++) {
        if (!cp->images[i].texture) continue;
        glBindImageTexture((GLuint)cp->images[i].slot, cp->images[i].texture, 0,
                           cp->images[i].layered, 0,
                           cp->images[i].access == 1 ? GL_WRITE_ONLY : GL_READ_ONLY, GL_RGBA8);
    }

    GLuint gx = cmd->group_count_x > 0 ? (GLuint)cmd->group_count_x : 1;
    GLuint gy = cmd->group_count_y > 0 ? (GLuint)cmd->group_count_y : 1;
    GLuint gz = cmd->group_count_z > 0 ? (GLuint)cmd->group_count_z : 1;
    glDispatchCompute(gx, gy, gz);

    /* Make SSBO writes visible to subsequent glGetBufferSubData / glMapBufferRange
     * readback, and image writes to later texture fetches. Missing barrier
     * bits yield stale data. */
    glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT | GL_BUFFER_UPDATE_BARRIER_BIT |
                    GL_SHADER_IMAGE_ACCESS_BARRIER_BIT | GL_TEXTURE_FETCH_BARRIER_BIT);
    for (int i = 0; i < cp->image_count; i++) {
        glBindImageTexture((GLuint)cp->images[i].slot, 0, 0, GL_FALSE, 0, GL_READ_ONLY, GL_RGBA8);
    }

    /* Unbind so the SSBOs/UBO aren't left bound to these points. */
    for (int i = 0; i < cp->binding_count; i++) {
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, (GLuint)cp->bindings[i].slot, 0);
    }
    if (cp->ubo) glBindBufferBase(GL_UNIFORM_BUFFER, (GLuint)cp->params_binding, 0);
    glUseProgram(0);

    if (vio_gl.caps.has_debug_output || getenv("VIO_GL_COMPUTE_DEBUG")) {
        opengl_drain_gl_errors("dispatch_compute");
    }
}

static size_t opengl_read_buffer(void *backend_buffer, void *out, size_t size)
{
    vio_opengl_compute_buffer *buf = (vio_opengl_compute_buffer *)backend_buffer;
    if (!buf || !buf->ssbo || !out || size == 0 || !vio_gl.initialized) return 0;

    size_t n = size < buf->size ? size : buf->size;
    /* Draws that wrote it through a fragment storage block (A15). */
    if (glMemoryBarrier) glMemoryBarrier(GL_BUFFER_UPDATE_BARRIER_BIT | GL_SHADER_STORAGE_BARRIER_BIT);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, buf->ssbo);
    /* glGetBufferSubData blocks until prior GPU writes (made visible by the
     * dispatch's glMemoryBarrier) complete, then copies into out. */
    glGetBufferSubData(GL_SHADER_STORAGE_BUFFER, 0, (GLsizeiptr)n, out);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);

    if (getenv("VIO_GL_COMPUTE_DEBUG")) opengl_drain_gl_errors("read_buffer");
    return n;
}

static void opengl_set_viewport(int x, int y, int width, int height)
{
    if (!vio_gl.initialized) return;
    glViewport((GLint)x, (GLint)y, (GLsizei)width, (GLsizei)height);   /* sets every indexed viewport */
}

/* GL 4.1 / ARB_viewport_array: indexed viewports; the 3D path does not
 * scissor, so no scissor array is needed. */
static int opengl_set_viewports(const int *rects, int count)
{
    if (!vio_gl.initialized || !glViewportIndexedf) return -1;
    for (int i = 0; i < count; i++) {
        glViewportIndexedf((GLuint)i, (GLfloat)rects[i * 4], (GLfloat)rects[i * 4 + 1],
                           (GLfloat)rects[i * 4 + 2], (GLfloat)rects[i * 4 + 3]);
    }
    return 0;
}

/* True when the active context is at least the given GL version. The window
 * system negotiates a 4.6 → 3.3 ladder and writes the result into vio_gl.
 * Both fields are 0 until vio_opengl_setup_context() has run, in which case
 * every check returns 0 — that's correct: no context, no capabilities. */
static inline int gl_ge(int major, int minor)
{
    return (vio_gl.gl_major > major) ||
           (vio_gl.gl_major == major && vio_gl.gl_minor >= minor);
}

/* ── Object destructors (vio_*_object → glDelete* of owned handles) ── */

static void opengl_destroy_buffer_obj(void *buf_obj)
{
    vio_buffer_object *buf = (vio_buffer_object *)buf_obj;
    if (buf->buffer_id) {
        if (gl_owned_by_live_context(buf->gl_generation)) glDeleteBuffers(1, &buf->buffer_id);
        buf->buffer_id = 0;
    }
    /* STORAGE buffers (compute) live behind backend_buffer as a GL SSBO wrapper. */
    if (buf->backend_buffer) {
        vio_opengl_compute_buffer *cb = (vio_opengl_compute_buffer *)buf->backend_buffer;
        if (cb->ssbo && gl_owned_by_live_context(cb->gl_generation)) glDeleteBuffers(1, &cb->ssbo);
        free(cb);
        buf->backend_buffer = NULL;
    }
}

static void opengl_destroy_texture_obj(void *tex_obj)
{
    vio_texture_object *tex = (vio_texture_object *)tex_obj;
    /* `borrowed` textures (e.g. render-target color/depth alias) are owned
     * by another object and must not be deleted here. */
    if (tex->texture_id && !tex->borrowed) {
        if (gl_owned_by_live_context(tex->gl_generation)) glDeleteTextures(1, &tex->texture_id);
        tex->texture_id = 0;
    }
}

static void opengl_destroy_shader_obj(void *shader_obj)
{
    vio_shader_object *sh = (vio_shader_object *)shader_obj;
    if (sh->program) {
        gl_uloc_forget_program(sh->program);
        if (gl_owned_by_live_context(sh->gl_generation)) glDeleteProgram(sh->program);
        sh->program = 0;
    }
}

static void opengl_destroy_mesh(void *mesh_ptr)
{
    vio_mesh_object *mesh = (vio_mesh_object *)mesh_ptr;
    int live = gl_owned_by_live_context(mesh->gl_generation);
    if (mesh->ebo) { if (live) glDeleteBuffers(1, &mesh->ebo); mesh->ebo = 0; }
    if (mesh->vbo) { if (live) glDeleteBuffers(1, &mesh->vbo); mesh->vbo = 0; }
    if (mesh->vao) { if (live) glDeleteVertexArrays(1, &mesh->vao); mesh->vao = 0; }
}

static void opengl_destroy_cubemap(void *cm_ptr)
{
    vio_cubemap_object *cm = (vio_cubemap_object *)cm_ptr;
    if (cm->borrowed) {
        /* vio_render_target_cubemap wrapper: the RT owns the texture name. */
        cm->texture_id = 0;
        return;
    }
    if (cm->texture_id) {
        if (gl_owned_by_live_context(cm->gl_generation)) glDeleteTextures(1, &cm->texture_id);
        cm->texture_id = 0;
    }
}

static void opengl_destroy_font_atlas(void *font_ptr)
{
    vio_font_object *font = (vio_font_object *)font_ptr;
    /* A VioFont (PHP object) can outlive the GL context it uploaded its atlas
     * into: PHP frees fonts during GC, which for a create/destroy-per-frame
     * consumer (e.g. VRT capture) runs AFTER vio_destroy has torn the context
     * down. Destroying the context already freed every texture it owned, so
     * calling glDeleteTextures now would either run with no current context
     * (crash) or against a later context whose texture-id space has been reused
     * (silent corruption) — the accumulating fault behind "Premature end of PHP
     * process" after ~40 render cycles. Only touch the GL object while the
     * context that created it is live; otherwise just clear the stale id. */
    if (font->atlas_texture && gl_owned_by_live_context(font->gl_generation) && glDeleteTextures) {
        glDeleteTextures(1, &font->atlas_texture);
    }
    font->atlas_texture = 0;
}

/* GL storage triple for a vio_pixel_format colour attachment. */
static void opengl_color_format(int fmt, GLint *internal, GLenum *base, GLenum *type)
{
    switch (fmt) {
        case VIO_FORMAT_RGBA16F:    *internal = GL_RGBA16F;        *base = GL_RGBA; *type = GL_FLOAT; break;
        case VIO_FORMAT_RGBA32F:    *internal = GL_RGBA32F;        *base = GL_RGBA; *type = GL_FLOAT; break;
        case VIO_FORMAT_R11G11B10F: *internal = GL_R11F_G11F_B10F; *base = GL_RGB;  *type = GL_FLOAT; break;
        case VIO_FORMAT_RG16F:      *internal = GL_RG16F;          *base = GL_RG;   *type = GL_FLOAT; break;
        case VIO_FORMAT_R16F:       *internal = GL_R16F;           *base = GL_RED;  *type = GL_FLOAT; break;
        case VIO_FORMAT_R32F:       *internal = GL_R32F;           *base = GL_RED;  *type = GL_FLOAT; break;
        case VIO_FORMAT_R8:         *internal = GL_R8;             *base = GL_RED;  *type = GL_UNSIGNED_BYTE; break;
        case VIO_FORMAT_RGBA8:
        default:                    *internal = GL_RGBA8;          *base = GL_RGBA; *type = GL_UNSIGNED_BYTE; break;
    }
}

static void opengl_destroy_render_target(void *rt_ptr)
{
    vio_render_target_object *rt = (vio_render_target_object *)rt_ptr;
    /* Only act on RTs created by OpenGL — guarding by backend_type avoids
     * mangling a struct that happens to share the slot with a D3D RT in
     * cross-backend tests. */
    if (rt->backend_type != VIO_RT_BACKEND_OPENGL) return;
    if (vio_gl.current_bound_rt == rt) vio_gl.current_bound_rt = NULL;
    int live = gl_owned_by_live_context(rt->gl_generation);
    if (rt->gl_msaa_fbo) {
        if (live) glDeleteFramebuffers(1, &rt->gl_msaa_fbo);
        rt->gl_msaa_fbo = 0;
    }
    if (rt->gl_msaa_color_rb) { if (live) glDeleteRenderbuffers(1, &rt->gl_msaa_color_rb); rt->gl_msaa_color_rb = 0; }
    for (int i = 1; i < 4; i++)
        if (rt->gl_msaa_color_rbs[i]) { if (live) glDeleteRenderbuffers(1, &rt->gl_msaa_color_rbs[i]); rt->gl_msaa_color_rbs[i] = 0; }
    if (rt->gl_msaa_depth_rb) { if (live) glDeleteRenderbuffers(1, &rt->gl_msaa_depth_rb); rt->gl_msaa_depth_rb = 0; }
    if (rt->gl_msaa_color_arr) { if (live) glDeleteTextures(1, &rt->gl_msaa_color_arr); rt->gl_msaa_color_arr = 0; }
    if (rt->gl_msaa_depth_tex) { if (live) glDeleteTextures(1, &rt->gl_msaa_depth_tex); rt->gl_msaa_depth_tex = 0; }
    if (rt->gl_msaa_depth_arr) { if (live) glDeleteTextures(1, &rt->gl_msaa_depth_arr); rt->gl_msaa_depth_arr = 0; }
    if (rt->fbo) {
        if (live) glDeleteFramebuffers(1, &rt->fbo);
        rt->fbo = 0;
    }
    if (rt->color_texture) {
        if (live) glDeleteTextures(1, &rt->color_texture);
        rt->color_texture = 0;
        rt->color_textures[0] = 0;
    }
    /* MRT attachments 1..n (index 0 is the scalar above). */
    for (int i = 1; i < rt->attachment_count && i < VIO_MAX_COLOR_ATTACHMENTS; i++) {
        if (rt->color_textures[i]) {
            if (live) glDeleteTextures(1, &rt->color_textures[i]);
            rt->color_textures[i] = 0;
        }
    }
    if (rt->depth_texture) {
        if (live) glDeleteTextures(1, &rt->depth_texture);
        rt->depth_texture = 0;
    }
}

/* Attach one layer (cube face or array layer) at `level` of a layered target to
 * the target's FBO, which must be bound as GL_FRAMEBUFFER. Colour and depth
 * carry the same layer structure; depth only exists at level 0, so smaller
 * levels render without depth (the Metal contract). */
/* Multiview (see gl_mv_prepare): the target whose attachments are multiview
 * attachments right now. Any re-attach below replaces them, so it resets this. */
static void *gl_mv_rt = NULL;
static int   gl_mv_views = 0;

static void opengl_rt_attach_layer(vio_render_target_object *rt, int layer, int level)
{
    if (gl_mv_rt == rt) { gl_mv_rt = NULL; gl_mv_views = 0; }
    if (layer < 0) {
        /* VIO_RT_ALL_LAYERS: layered attachments (every face / layer); gl_Layer
         * in the geometry or vertex stage selects the destination. */
        if (!rt->depth_only) glFramebufferTexture(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, rt->color_texture, level);
        glFramebufferTexture(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, level == 0 ? rt->depth_texture : 0, 0);
        return;
    }
    if (rt->is_cube) {
        if (!rt->depth_only) {
            glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                                   GL_TEXTURE_CUBE_MAP_POSITIVE_X + layer, rt->color_texture, level);
        }
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_TEXTURE_CUBE_MAP_POSITIVE_X + layer,
                               level == 0 ? rt->depth_texture : 0, 0);
    } else {
        if (!rt->depth_only) glFramebufferTextureLayer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, rt->color_texture, level, layer);
        glFramebufferTextureLayer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, level == 0 ? rt->depth_texture : 0, 0, layer);
    }
}

/* Cube / array MSAA (A24): the MSAA FBO renders into one layer of a multisample
 * array (or every layer for VIO_RT_ALL_LAYERS); each face keeps its own samples,
 * so re-binding a face keeps its colour and depth. */
static void gl_msaa_attach_layer(GLenum target, vio_render_target_object *rt, int layer)
{
    if (layer < 0) {
        glFramebufferTexture(target, GL_COLOR_ATTACHMENT0, rt->gl_msaa_color_arr, 0);
        glFramebufferTexture(target, GL_DEPTH_STENCIL_ATTACHMENT, rt->gl_msaa_depth_arr, 0);
    } else {
        glFramebufferTextureLayer(target, GL_COLOR_ATTACHMENT0, rt->gl_msaa_color_arr, 0, layer);
        glFramebufferTextureLayer(target, GL_DEPTH_STENCIL_ATTACHMENT, rt->gl_msaa_depth_arr, 0, layer);
    }
}

static void gl_create_layered_msaa(vio_render_target_object *rt, int width, int height, int hdr)
{
    int layers = vio_rt_layer_count(rt);
    GLint max_samples = 1, max_color = 1, max_depth = 1;
    if (rt->samples <= 1 || !GLAD_GL_VERSION_3_2) { rt->samples = 1; return; }
    glGetIntegerv(GL_MAX_SAMPLES, &max_samples);
    glGetIntegerv(GL_MAX_COLOR_TEXTURE_SAMPLES, &max_color);
    glGetIntegerv(GL_MAX_DEPTH_TEXTURE_SAMPLES, &max_depth);
    int samples = rt->samples > 8 ? 8 : rt->samples;
    if (samples > max_samples) samples = max_samples;
    if (samples > max_color) samples = max_color;
    if (samples > max_depth) samples = max_depth;
    GLint internal; GLenum base, type;
    opengl_color_format(rt->attachment_count > 0 ? rt->formats[0] : (hdr ? VIO_FORMAT_RGBA16F : VIO_FORMAT_RGBA8), &internal, &base, &type);
    while (samples > 1) {
        glGenTextures(1, &rt->gl_msaa_color_arr);
        glBindTexture(GL_TEXTURE_2D_MULTISAMPLE_ARRAY, rt->gl_msaa_color_arr);
        glTexImage3DMultisample(GL_TEXTURE_2D_MULTISAMPLE_ARRAY, samples, internal, width, height, layers, GL_TRUE);
        glGenTextures(1, &rt->gl_msaa_depth_arr);
        glBindTexture(GL_TEXTURE_2D_MULTISAMPLE_ARRAY, rt->gl_msaa_depth_arr);
        glTexImage3DMultisample(GL_TEXTURE_2D_MULTISAMPLE_ARRAY, samples, GL_DEPTH24_STENCIL8, width, height, layers, GL_TRUE);
        glBindTexture(GL_TEXTURE_2D_MULTISAMPLE_ARRAY, 0);
        glGenFramebuffers(1, &rt->gl_msaa_fbo);
        glBindFramebuffer(GL_FRAMEBUFFER, rt->gl_msaa_fbo);
        gl_msaa_attach_layer(GL_FRAMEBUFFER, rt, 0);
        if (glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE) {
            glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
            glDepthMask(GL_TRUE);
            glClearDepth(1.0);
            for (int l = 0; l < layers; l++) {
                gl_msaa_attach_layer(GL_FRAMEBUFFER, rt, l);
                glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
            }
            gl_msaa_attach_layer(GL_FRAMEBUFFER, rt, 0);
            rt->gl_msaa_layer = 0;
            break;
        }
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glDeleteFramebuffers(1, &rt->gl_msaa_fbo);
        glDeleteTextures(1, &rt->gl_msaa_color_arr);
        glDeleteTextures(1, &rt->gl_msaa_depth_arr);
        rt->gl_msaa_fbo = rt->gl_msaa_color_arr = rt->gl_msaa_depth_arr = 0;
        samples >>= 1;
    }
    glBindFramebuffer(GL_FRAMEBUFFER, rt->fbo);
    rt->samples = rt->gl_msaa_fbo ? samples : 1;
}

/* Cube ('cube' => true) and array ('layers' => N) targets: colour (optional,
 * cube with a mip chain when requested) and DEPTH24_STENCIL8 depth with the
 * same layer structure - a depth cube / depth array, so a later layered bind
 * can attach all layers at once and depth_only targets sample as
 * samplerCube / sampler2DArray. +X / layer 0 is attached initially. */
static int opengl_create_layered_render_target(vio_render_target_object *rt, int width, int height, int hdr, int depth_only)
{
    GLenum target = rt->is_cube ? GL_TEXTURE_CUBE_MAP : GL_TEXTURE_2D_ARRAY;
    int layers = vio_rt_layer_count(rt);
    if (rt->is_cube) height = width;
    if (rt->mip_levels < 1) rt->mip_levels = 1;

    if (!depth_only) {
        /* Mip storage is allocated up front so bind_render_target_face can
         * target level > 0 before any glGenerateMipmap. */
        GLint internal; GLenum base, type;
        opengl_color_format(rt->attachment_count > 0 ? rt->formats[0] : (hdr ? VIO_FORMAT_RGBA16F : VIO_FORMAT_RGBA8),
                            &internal, &base, &type);
        glGenTextures(1, &rt->color_texture);
        rt->color_textures[0] = rt->color_texture;
        glBindTexture(target, rt->color_texture);
        for (int level = 0; level < rt->mip_levels; level++) {
            int w = width >> level, h = height >> level;
            if (w < 1) w = 1;
            if (h < 1) h = 1;
            if (rt->is_cube) {
                for (int f = 0; f < 6; f++) {
                    glTexImage2D(GL_TEXTURE_CUBE_MAP_POSITIVE_X + f, level, internal, w, w, 0, base, type, NULL);
                }
            } else {
                glTexImage3D(GL_TEXTURE_2D_ARRAY, level, internal, w, h, layers, 0, base, type, NULL);
            }
        }
        glTexParameteri(target, GL_TEXTURE_MIN_FILTER, rt->mip_levels > 1 ? GL_LINEAR_MIPMAP_LINEAR : GL_LINEAR);
        glTexParameteri(target, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(target, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(target, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glTexParameteri(target, GL_TEXTURE_WRAP_R, GL_CLAMP_TO_EDGE);
        glTexParameteri(target, GL_TEXTURE_MAX_LEVEL, rt->mip_levels - 1);
        glBindTexture(target, 0);
    }

    glGenTextures(1, &rt->depth_texture);
    glBindTexture(target, rt->depth_texture);
    if (rt->is_cube) {
        for (int f = 0; f < 6; f++) {
            glTexImage2D(GL_TEXTURE_CUBE_MAP_POSITIVE_X + f, 0, GL_DEPTH24_STENCIL8, width, width, 0,
                         GL_DEPTH_STENCIL, GL_UNSIGNED_INT_24_8, NULL);
        }
    } else {
        glTexImage3D(GL_TEXTURE_2D_ARRAY, 0, GL_DEPTH24_STENCIL8, width, height, layers, 0,
                     GL_DEPTH_STENCIL, GL_UNSIGNED_INT_24_8, NULL);
    }
    glTexParameteri(target, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(target, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(target, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(target, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexParameteri(target, GL_TEXTURE_WRAP_R, GL_CLAMP_TO_EDGE);
    glTexParameteri(target, GL_TEXTURE_MAX_LEVEL, 0);
    glBindTexture(target, 0);

    if (depth_only) {
        glDrawBuffer(GL_NONE);
        glReadBuffer(GL_NONE);
    }
    opengl_rt_attach_layer(rt, 0, 0);
    GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    if (status == GL_FRAMEBUFFER_COMPLETE) {
        /* Defined initial contents: every layer cleared, depth at 1.0. */
        glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
        glDepthMask(GL_TRUE);
        glClearDepth(1.0);
        for (int l = 0; l < layers; l++) {
            opengl_rt_attach_layer(rt, l, 0);
            glClear((depth_only ? 0 : GL_COLOR_BUFFER_BIT) | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
        }
        opengl_rt_attach_layer(rt, 0, 0);
    }
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    if (status != GL_FRAMEBUFFER_COMPLETE) {
        php_error_docref(NULL, E_WARNING, "%s render target FBO is not complete (status: 0x%04x)",
                         rt->is_cube ? "Cube" : "Array", status);
        return -1;
    }
    rt->bound_face = 0;
    rt->bound_level = 0;
    if (!depth_only) gl_create_layered_msaa(rt, width, height, hdr);
    else rt->samples = 1;
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    rt->backend_type = VIO_RT_BACKEND_OPENGL;
    return 0;
}

static int opengl_create_render_target(void *rt_ptr, int width, int height, int hdr, int depth_only)
{
    vio_render_target_object *rt = (vio_render_target_object *)rt_ptr;
    if (!vio_gl.initialized) return -1;

    glGenFramebuffers(1, &rt->fbo);
    rt->gl_generation = gl_context_generation;
    glBindFramebuffer(GL_FRAMEBUFFER, rt->fbo);

    if (rt->is_cube || rt->layers > 1) {
        return opengl_create_layered_render_target(rt, width, height, hdr, depth_only);
    }

    /* Depth texture (always created — shadow-map use-case needs it as SRV).
     * DEPTH24_STENCIL8 so the stencil test (VIO_FEATURE_STENCIL) has its 8 bits;
     * sampling through sampler2DShadow / glReadPixels(GL_DEPTH_COMPONENT) still
     * reads the depth plane (GL_DEPTH_STENCIL_TEXTURE_MODE defaults to depth). */
    glGenTextures(1, &rt->depth_texture);
    glBindTexture(GL_TEXTURE_2D, rt->depth_texture);
    /* depth_only + 'mipmaps' (A26): every level, levels > 0 filled by vio_generate_mipmaps. */
    for (int l = 0; l < (depth_only && rt->mip_levels > 1 ? rt->mip_levels : 1); l++) {
        int lw = width >> l, lh = height >> l;
        glTexImage2D(GL_TEXTURE_2D, l, GL_DEPTH24_STENCIL8, lw > 0 ? lw : 1, lh > 0 ? lh : 1,
            0, GL_DEPTH_STENCIL, GL_UNSIGNED_INT_24_8, NULL);
    }
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, depth_only && rt->mip_levels > 1 ? rt->mip_levels - 1 : 0);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_BORDER);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_BORDER);
    float border_color[] = {1.0f, 1.0f, 1.0f, 1.0f};
    glTexParameterfv(GL_TEXTURE_2D, GL_TEXTURE_BORDER_COLOR, border_color);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_TEXTURE_2D,
        rt->depth_texture, 0);

    if (depth_only) {
        glDrawBuffer(GL_NONE);
        glReadBuffer(GL_NONE);
    } else {
        /* One colour texture per attachment (MRT: 'attachments' => [...]).
         * The legacy single target arrives as attachment_count == 1 with
         * formats[0] derived from 'hdr'. */
        int n = rt->attachment_count > 0 ? rt->attachment_count : 1;
        if (n > VIO_MAX_COLOR_ATTACHMENTS) n = VIO_MAX_COLOR_ATTACHMENTS;
        GLenum draw_bufs[VIO_MAX_COLOR_ATTACHMENTS];
        for (int i = 0; i < n; i++) {
            int fmt = rt->attachment_count > 0 ? rt->formats[i] : (hdr ? VIO_FORMAT_RGBA16F : VIO_FORMAT_RGBA8);
            GLint internal; GLenum base, type;
            opengl_color_format(fmt, &internal, &base, &type);
            GLuint tex = 0;
            glGenTextures(1, &tex);
            glBindTexture(GL_TEXTURE_2D, tex);
            glTexImage2D(GL_TEXTURE_2D, 0, internal, width, height, 0, base, type, NULL);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
            glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0 + i, GL_TEXTURE_2D, tex, 0);
            rt->color_textures[i] = tex;
            draw_bufs[i] = GL_COLOR_ATTACHMENT0 + (GLenum)i;
        }
        rt->color_texture = rt->color_textures[0];
        rt->attachment_count = n;
        /* FBO state: which colour attachments fragment outputs 0..n-1 write. */
        glDrawBuffers(n, draw_bufs);
    }

    GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    if (status == GL_FRAMEBUFFER_COMPLETE) {
        /* Defined initial contents (depth 1.0, colour 0) so a target that is
         * bound and drawn into without an explicit clear still depth-tests. */
        glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
        glDepthMask(GL_TRUE);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
    }
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glBindTexture(GL_TEXTURE_2D, 0);

    if (status != GL_FRAMEBUFFER_COMPLETE) {
        php_error_docref(NULL, E_WARNING,
            "Render target FBO is not complete (status: 0x%04x)", status);
        return -1;
    }

    /* MSAA (every colour attachment, A24): a second FBO with multisample
     * renderbuffers is what gets drawn into; unbind / readback resolve it into
     * the texture FBO above with glBlitFramebuffer. Before GAP-PLAN Phase 3
     * rt->samples was ignored here while VIO_FEATURE_RENDER_TARGET_MSAA
     * reported 1. */
    rt->samples = rt->samples > 1 ? rt->samples : 1;
    if (rt->samples > 1 && !depth_only) {
        int n_att = rt->attachment_count > 0 ? rt->attachment_count : 1;
        GLint max_samples = 1;
        glGetIntegerv(GL_MAX_SAMPLES, &max_samples);
        int samples = rt->samples > 8 ? 8 : rt->samples;
        if (samples > max_samples) samples = max_samples;
        while (samples > 1) {
            GLint internal; GLenum base, type;
            opengl_color_format(rt->attachment_count > 0 ? rt->formats[0] : (hdr ? VIO_FORMAT_RGBA16F : VIO_FORMAT_RGBA8),
                                &internal, &base, &type);
            glGenFramebuffers(1, &rt->gl_msaa_fbo);
            glGenRenderbuffers(1, &rt->gl_msaa_color_rb);
            for (int i = 1; i < n_att; i++) glGenRenderbuffers(1, &rt->gl_msaa_color_rbs[i]);
            glGenRenderbuffers(1, &rt->gl_msaa_depth_rb);
            glBindRenderbuffer(GL_RENDERBUFFER, rt->gl_msaa_color_rb);
            glRenderbufferStorageMultisample(GL_RENDERBUFFER, samples, internal, width, height);
            glBindRenderbuffer(GL_RENDERBUFFER, rt->gl_msaa_depth_rb);
            glRenderbufferStorageMultisample(GL_RENDERBUFFER, samples, GL_DEPTH24_STENCIL8, width, height);
            glBindRenderbuffer(GL_RENDERBUFFER, 0);
            glBindFramebuffer(GL_FRAMEBUFFER, rt->gl_msaa_fbo);
            glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, rt->gl_msaa_color_rb);
            GLenum ms_bufs[VIO_MAX_COLOR_ATTACHMENTS] = { GL_COLOR_ATTACHMENT0 };
            for (int i = 1; i < n_att; i++) {
                GLint ai; GLenum ab, at;
                opengl_color_format(rt->formats[i], &ai, &ab, &at);
                glBindRenderbuffer(GL_RENDERBUFFER, rt->gl_msaa_color_rbs[i]);
                glRenderbufferStorageMultisample(GL_RENDERBUFFER, samples, ai, width, height);
                glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0 + (GLenum)i, GL_RENDERBUFFER, rt->gl_msaa_color_rbs[i]);
                ms_bufs[i] = GL_COLOR_ATTACHMENT0 + (GLenum)i;
            }
            glBindRenderbuffer(GL_RENDERBUFFER, 0);
            glDrawBuffers(n_att, ms_bufs);
            glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, rt->gl_msaa_depth_rb);
            GLenum ms_status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
            if (ms_status == GL_FRAMEBUFFER_COMPLETE) {
                glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
                glDepthMask(GL_TRUE);
                glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
                glBindFramebuffer(GL_FRAMEBUFFER, 0);
                break;
            }
            /* This sample count is not renderable here: drop to the next tier. */
            glBindFramebuffer(GL_FRAMEBUFFER, 0);
            glDeleteFramebuffers(1, &rt->gl_msaa_fbo);
            glDeleteRenderbuffers(1, &rt->gl_msaa_color_rb);
            glDeleteRenderbuffers(1, &rt->gl_msaa_depth_rb);
            for (int i = 1; i < n_att; i++) { glDeleteRenderbuffers(1, &rt->gl_msaa_color_rbs[i]); rt->gl_msaa_color_rbs[i] = 0; }
            rt->gl_msaa_fbo = rt->gl_msaa_color_rb = rt->gl_msaa_depth_rb = 0;
            samples >>= 1;
        }
        rt->samples = rt->gl_msaa_fbo ? samples : 1;
    } else if (rt->samples > 1 && depth_only && rt->mip_levels <= 1 && GLAD_GL_VERSION_3_2) {
        /* depth_only MSAA (A24): a multisample depth texture drawn into, resolved
         * into depth_texture by a shader (max / min of the samples). */
        GLint max_depth = 1;
        glGetIntegerv(GL_MAX_DEPTH_TEXTURE_SAMPLES, &max_depth);
        int samples = rt->samples > 8 ? 8 : rt->samples;
        if (samples > max_depth) samples = max_depth;
        while (samples > 1) {
            glGenTextures(1, &rt->gl_msaa_depth_tex);
            glBindTexture(GL_TEXTURE_2D_MULTISAMPLE, rt->gl_msaa_depth_tex);
            glTexImage2DMultisample(GL_TEXTURE_2D_MULTISAMPLE, samples, GL_DEPTH24_STENCIL8, width, height, GL_TRUE);
            glBindTexture(GL_TEXTURE_2D_MULTISAMPLE, 0);
            glGenFramebuffers(1, &rt->gl_msaa_fbo);
            glBindFramebuffer(GL_FRAMEBUFFER, rt->gl_msaa_fbo);
            glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_TEXTURE_2D_MULTISAMPLE, rt->gl_msaa_depth_tex, 0);
            glDrawBuffer(GL_NONE);
            glReadBuffer(GL_NONE);
            if (glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE) {
                glDepthMask(GL_TRUE);
                glClearDepth(1.0);
                glClear(GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
                glBindFramebuffer(GL_FRAMEBUFFER, 0);
                break;
            }
            glBindFramebuffer(GL_FRAMEBUFFER, 0);
            glDeleteFramebuffers(1, &rt->gl_msaa_fbo);
            glDeleteTextures(1, &rt->gl_msaa_depth_tex);
            rt->gl_msaa_fbo = rt->gl_msaa_depth_tex = 0;
            samples >>= 1;
        }
        rt->samples = rt->gl_msaa_fbo ? samples : 1;
    } else {
        rt->samples = 1;
    }

    rt->backend_type = VIO_RT_BACKEND_OPENGL;
    return 0;
}

static int gl_resolve_depth_msaa(vio_render_target_object *rt);

/* Resolve a multisampled target into its texture FBO (no-op otherwise). */
static void opengl_rt_resolve_msaa(vio_render_target_object *rt)
{
    if (!rt || !rt->gl_msaa_fbo || !rt->gl_msaa_dirty) return;
    if (rt->gl_msaa_depth_tex) {   /* depth_only MSAA (A24) */
        gl_resolve_depth_msaa(rt);
        rt->gl_msaa_dirty = 0;
        return;
    }
    GLint prev_read = 0, prev_draw = 0;
    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &prev_read);
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &prev_draw);
    if (rt->gl_msaa_color_arr) {
        /* Cube / array: the layer(s) the MSAA FBO rendered into, face by face. */
        int layers = vio_rt_layer_count(rt);
        int first = rt->gl_msaa_layer < 0 ? 0 : rt->gl_msaa_layer;
        int last = rt->gl_msaa_layer < 0 ? layers - 1 : rt->gl_msaa_layer;
        for (int l = first; l <= last; l++) {
            glBindFramebuffer(GL_FRAMEBUFFER, rt->fbo);
            opengl_rt_attach_layer(rt, l, 0);
            glBindFramebuffer(GL_READ_FRAMEBUFFER, rt->gl_msaa_fbo);
            gl_msaa_attach_layer(GL_READ_FRAMEBUFFER, rt, l);
            glBlitFramebuffer(0, 0, rt->width, rt->height, 0, 0, rt->width, rt->height,
                              GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT, GL_NEAREST);
        }
        glBindFramebuffer(GL_FRAMEBUFFER, rt->gl_msaa_fbo);
        gl_msaa_attach_layer(GL_FRAMEBUFFER, rt, rt->gl_msaa_layer);
        glBindFramebuffer(GL_READ_FRAMEBUFFER, (GLuint)prev_read);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, (GLuint)prev_draw);
        rt->gl_msaa_dirty = 0;
        return;
    }
    glBindFramebuffer(GL_READ_FRAMEBUFFER, rt->gl_msaa_fbo);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, rt->fbo);
    /* One blit per attachment (read / draw buffer i), depth with the first. */
    int n = rt->attachment_count > 0 ? rt->attachment_count : 1;
    GLenum all[VIO_MAX_COLOR_ATTACHMENTS];
    for (int i = 0; i < n; i++) {
        GLenum buf = GL_COLOR_ATTACHMENT0 + (GLenum)i;
        all[i] = buf;
        glReadBuffer(buf);
        glDrawBuffers(1, &buf);
        glBlitFramebuffer(0, 0, rt->width, rt->height, 0, 0, rt->width, rt->height,
                          GL_COLOR_BUFFER_BIT | (i == 0 ? GL_DEPTH_BUFFER_BIT : 0), GL_NEAREST);
    }
    glDrawBuffers(n, all);
    glReadBuffer(GL_COLOR_ATTACHMENT0);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, (GLuint)prev_read);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, (GLuint)prev_draw);
    rt->gl_msaa_dirty = 0;
}

static void opengl_bind_render_target(void *rt_ptr)
{
    vio_render_target_object *rt = (vio_render_target_object *)rt_ptr;
    if (rt->backend_type != VIO_RT_BACKEND_OPENGL || !vio_gl.initialized) return;
    /* A previously bound multisampled target is resolved when the binding
     * moves away from it (bind-to-bind chains never see an unbind). */
    if (vio_gl.current_bound_rt && vio_gl.current_bound_rt != rt) {
        opengl_rt_resolve_msaa((vio_render_target_object *)vio_gl.current_bound_rt);
    }
    vio_gl.current_bound_rt = rt;
    if (rt->gl_msaa_fbo) {
        if (rt->gl_msaa_color_arr && rt->gl_msaa_layer != 0) opengl_rt_resolve_msaa(rt);
        glBindFramebuffer(GL_FRAMEBUFFER, rt->gl_msaa_fbo);
        if (rt->gl_msaa_color_arr) {
            gl_msaa_attach_layer(GL_FRAMEBUFFER, rt, 0);
            rt->gl_msaa_layer = 0;
            rt->bound_face = 0;
            rt->bound_level = 0;
        }
        rt->gl_msaa_dirty = 1;
        glViewport(0, 0, rt->width, rt->height);
        return;
    }
    glBindFramebuffer(GL_FRAMEBUFFER, rt->fbo);
    if (rt->is_cube || rt->layers > 1) {
        /* Plain bind of a cube / array RT targets +X / layer 0 at level 0. */
        opengl_rt_attach_layer(rt, 0, 0);
        rt->bound_face = 0;
        rt->bound_level = 0;
    }
    glViewport(0, 0, rt->width, rt->height);
}

static int opengl_bind_render_target_face(void *rt_ptr, int face, int level)
{
    vio_render_target_object *rt = (vio_render_target_object *)rt_ptr;
    if (!rt || rt->backend_type != VIO_RT_BACKEND_OPENGL || !vio_gl.initialized) return -1;
    if ((!rt->is_cube && rt->layers <= 1) || level < 0 || level >= rt->mip_levels) return -1;
    if (face == VIO_RT_ALL_LAYERS) {
        if (level != 0 || !GLAD_GL_VERSION_3_2) return -1;
    } else if (face < 0 || face >= vio_rt_layer_count(rt)) {
        return -1;
    }
    if (vio_gl.current_bound_rt && vio_gl.current_bound_rt != rt) {
        opengl_rt_resolve_msaa((vio_render_target_object *)vio_gl.current_bound_rt);
    }
    vio_gl.current_bound_rt = rt;
    /* Leaving one face of a multisampled cube / array resolves it. */
    int ms_face = face == VIO_RT_ALL_LAYERS ? -1 : face;
    if (rt->gl_msaa_color_arr && rt->gl_msaa_dirty && (level != 0 || rt->gl_msaa_layer != ms_face)) opengl_rt_resolve_msaa(rt);
    if (rt->gl_msaa_color_arr && level == 0) {
        glBindFramebuffer(GL_FRAMEBUFFER, rt->gl_msaa_fbo);
        gl_msaa_attach_layer(GL_FRAMEBUFFER, rt, ms_face);
        rt->gl_msaa_layer = ms_face;
        rt->gl_msaa_dirty = 1;
        glViewport(0, 0, rt->width, rt->height);
        rt->bound_face = face;
        rt->bound_level = 0;
        return 0;
    }
    glBindFramebuffer(GL_FRAMEBUFFER, rt->fbo);
    opengl_rt_attach_layer(rt, face, level);
    int w = rt->width >> level, h = rt->height >> level;
    if (w < 1) w = 1;
    if (h < 1) h = 1;
    glViewport(0, 0, w, h);
    rt->bound_face = face;
    rt->bound_level = level;
    return 0;
}

static int opengl_render_target_cubemap(void *rt_ptr, void *cm_obj)
{
    vio_render_target_object *rt = (vio_render_target_object *)rt_ptr;
    vio_cubemap_object *cm = (vio_cubemap_object *)cm_obj;
    if (!rt || !cm || !rt->is_cube) return -1;
    /* depth_only cube: the depth cubemap (samplerCube .r / samplerCubeShadow). */
    unsigned int id = rt->depth_only ? rt->depth_texture : rt->color_texture;
    if (!id) return -1;
    cm->texture_id   = id;   /* borrowed — RT owns the GL name */
    cm->mipmaps      = rt->mip_levels > 1;
    cm->borrowed     = 1;
    cm->resolution   = rt->width;
    cm->backend_type = 1;
    return 0;
}

static int opengl_read_render_target(void *rt_ptr, int face, int attachment, void *out_rgba)
{
    vio_render_target_object *rt = (vio_render_target_object *)rt_ptr;
    if (!rt || rt->backend_type != VIO_RT_BACKEND_OPENGL || !vio_gl.initialized || !rt->fbo) return -1;
    if (attachment < 0 || attachment >= (rt->attachment_count > 0 ? rt->attachment_count : 1)) return -1;
    int w = rt->width, h = rt->height;
    unsigned char *out = (unsigned char *)out_rgba;

    opengl_rt_resolve_msaa(rt);   /* the texture FBO holds the resolved image */
    GLint prev_fbo = 0;
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prev_fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, rt->fbo);
    if (!rt->depth_only) glReadBuffer(GL_COLOR_ATTACHMENT0 + (GLenum)attachment);
    int layered = rt->is_cube || rt->layers > 1;
    if (layered) {
        int f = face >= 0 ? face : (rt->bound_face >= 0 ? rt->bound_face : 0);
        opengl_rt_attach_layer(rt, f, 0);
    }

    if (rt->depth_only) {
        float *depth = (float *)emalloc((size_t)w * h * sizeof(float));
        glReadPixels(0, 0, w, h, GL_DEPTH_COMPONENT, GL_FLOAT, depth);
        for (int y = 0; y < h; y++) {
            const float *row = depth + (size_t)(h - 1 - y) * w;   /* flip to top-down */
            unsigned char *dst = out + (size_t)y * w * 4;
            for (int x = 0; x < w; x++) {
                float d = row[x]; if (d < 0.0f) d = 0.0f; if (d > 1.0f) d = 1.0f;
                unsigned char g = (unsigned char)(d * 255.0f + 0.5f);
                dst[x*4+0] = dst[x*4+1] = dst[x*4+2] = g; dst[x*4+3] = 255;
            }
        }
        efree(depth);
    } else {
        /* GL clamps + quantises float formats to UNSIGNED_BYTE for us; the
         * missing channels of R/RG formats read back as 0 (G/B) and 1 (A). */
        glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, out);
        int stride = w * 4;
        unsigned char *tmp = (unsigned char *)emalloc(stride);
        for (int y = 0; y < h / 2; y++) {
            unsigned char *top = out + y * stride, *bot = out + (h - 1 - y) * stride;
            memcpy(tmp, top, stride); memcpy(top, bot, stride); memcpy(bot, tmp, stride);
        }
        efree(tmp);
    }

    if (layered) {
        /* Restore the attachment the RT had bound before the read. */
        opengl_rt_attach_layer(rt, rt->bound_face >= 0 ? rt->bound_face : 0, rt->bound_level);
    }
    if (!rt->depth_only) glReadBuffer(GL_COLOR_ATTACHMENT0);
    glBindFramebuffer(GL_FRAMEBUFFER, (GLuint)prev_fbo);
    return 0;
}

static int opengl_update_texture(void *tex_obj, const void *pixels, int x, int y, int w, int h)
{
    vio_texture_object *t = (vio_texture_object *)tex_obj;
    if (!vio_gl.initialized || !t || !t->texture_id || t->is_3d) return -1;
    glBindTexture(GL_TEXTURE_2D, t->texture_id);
    /* upload_texture_2d stores data row 0 at GL row 0 (no flip), and the 2D /
     * 3D paths sample it that way — so a sub-region upload is a plain
     * glTexSubImage2D at the caller's (x, y). */
    GLenum fmt = t->channels == 1 ? GL_RED : GL_RGBA;
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexSubImage2D(GL_TEXTURE_2D, 0, x, y, w, h, fmt, GL_UNSIGNED_BYTE, pixels);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    glBindTexture(GL_TEXTURE_2D, 0);
    return 0;
}

/* Depth mip chain (A26): each level is the max / min of the 2x2 texels below,
 * written as gl_FragDepth by a full-screen triangle; the depth texture's base
 * level is pinned to the source level so sampling never sees the target. Odd
 * sizes fold the extra column / row into the last texel. */
static const char *gl_depth_reduce_vs =
    "#version 330 core\n"
    "void main() { vec2 p = vec2((gl_VertexID << 1) & 2, gl_VertexID & 2); gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0); }\n";
static const char *gl_depth_reduce_fs =
    "#version 330 core\n"
    "uniform sampler2D u_src; uniform int u_mode; uniform ivec2 u_src_size; uniform ivec2 u_dst_size;\n"
    "void main() {\n"
    "    ivec2 o = ivec2(gl_FragCoord.xy);\n"
    "    ivec2 n = ivec2((o.x == u_dst_size.x - 1 && (u_src_size.x & 1) == 1 && u_src_size.x > 1) ? 3 : 2,\n"
    "                    (o.y == u_dst_size.y - 1 && (u_src_size.y & 1) == 1 && u_src_size.y > 1) ? 3 : 2);\n"
    "    float d = u_mode == 0 ? 0.0 : 1.0;\n"
    "    for (int y = 0; y < 3; y++) for (int x = 0; x < 3; x++) {\n"
    "        if (x >= n.x || y >= n.y) continue;\n"
    "        float s = texelFetch(u_src, min(o * 2 + ivec2(x, y), u_src_size - 1), 0).r;\n"
    "        d = u_mode == 0 ? max(d, s) : min(d, s);\n"
    "    }\n"
    "    gl_FragDepth = d;\n"
    "}\n";
static GLuint gl_depth_reduce_prog = 0, gl_depth_reduce_vao = 0;
static unsigned int gl_depth_reduce_gen = 0;

static int gl_generate_depth_mips(vio_render_target_object *rt)
{
    if (!rt->depth_texture || rt->mip_levels < 2) return -1;
    if (!gl_depth_reduce_prog || gl_depth_reduce_gen != gl_context_generation) {
        gl_depth_reduce_prog = vio_opengl_compile_shader_source(gl_depth_reduce_vs, gl_depth_reduce_fs);
        glGenVertexArrays(1, &gl_depth_reduce_vao);
        gl_depth_reduce_gen = gl_context_generation;
        if (!gl_depth_reduce_prog) return -1;
    }
    GLint prev_fbo = 0, prev_prog = 0, prev_vao = 0, prev_active = 0, prev_tex = 0, prev_func = GL_LESS, vp[4];
    GLboolean prev_mask = GL_TRUE;
    GLboolean prev_test = glIsEnabled(GL_DEPTH_TEST), prev_scissor = glIsEnabled(GL_SCISSOR_TEST), prev_cull = glIsEnabled(GL_CULL_FACE);
    GLboolean prev_stencil = glIsEnabled(GL_STENCIL_TEST);
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prev_fbo);
    glGetIntegerv(GL_CURRENT_PROGRAM, &prev_prog);
    glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &prev_vao);
    glGetIntegerv(GL_ACTIVE_TEXTURE, &prev_active);
    glGetIntegerv(GL_VIEWPORT, vp);
    glGetIntegerv(GL_DEPTH_FUNC, &prev_func);
    glGetBooleanv(GL_DEPTH_WRITEMASK, &prev_mask);
    glActiveTexture(GL_TEXTURE0);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &prev_tex);

    GLuint fbo = 0;
    glGenFramebuffers(1, &fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glDrawBuffer(GL_NONE);
    glReadBuffer(GL_NONE);
    glUseProgram(gl_depth_reduce_prog);
    glBindVertexArray(gl_depth_reduce_vao);
    glBindTexture(GL_TEXTURE_2D, rt->depth_texture);
    glUniform1i(glGetUniformLocation(gl_depth_reduce_prog, "u_src"), 0);
    glUniform1i(glGetUniformLocation(gl_depth_reduce_prog, "u_mode"), rt->depth_reduction == VIO_DEPTH_REDUCE_MIN ? 1 : 0);
    GLint loc_src = glGetUniformLocation(gl_depth_reduce_prog, "u_src_size");
    GLint loc_dst = glGetUniformLocation(gl_depth_reduce_prog, "u_dst_size");
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_ALWAYS);
    glDepthMask(GL_TRUE);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_CULL_FACE);
    glDisable(GL_STENCIL_TEST);
    int ok = 1;
    for (int l = 1; l < rt->mip_levels && ok; l++) {
        int sw = rt->width >> (l - 1), sh = rt->height >> (l - 1);
        int dw = rt->width >> l, dh = rt->height >> l;
        if (sw < 1) sw = 1;
        if (sh < 1) sh = 1;
        if (dw < 1) dw = 1;
        if (dh < 1) dh = 1;
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_BASE_LEVEL, l - 1);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, l - 1);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_TEXTURE_2D, rt->depth_texture, l);
        if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) { ok = 0; break; }
        glViewport(0, 0, dw, dh);
        glUniform2i(loc_src, sw, sh);
        glUniform2i(loc_dst, dw, dh);
        glDrawArrays(GL_TRIANGLES, 0, 3);
    }
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_BASE_LEVEL, 0);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, rt->mip_levels - 1);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST_MIPMAP_NEAREST);
    glBindFramebuffer(GL_FRAMEBUFFER, (GLuint)prev_fbo);
    glDeleteFramebuffers(1, &fbo);
    glBindTexture(GL_TEXTURE_2D, (GLuint)prev_tex);
    glActiveTexture((GLenum)prev_active);
    glUseProgram((GLuint)prev_prog);
    glBindVertexArray((GLuint)prev_vao);
    glViewport(vp[0], vp[1], vp[2], vp[3]);
    glDepthFunc((GLenum)prev_func);
    glDepthMask(prev_mask);
    if (!prev_test) glDisable(GL_DEPTH_TEST);
    if (prev_scissor) glEnable(GL_SCISSOR_TEST);
    if (prev_cull) glEnable(GL_CULL_FACE);
    if (prev_stencil) glEnable(GL_STENCIL_TEST);
    return ok ? 0 : -1;
}

/* depth_only MSAA (A24): the samples of each texel reduced (max / min, the
 * target's depth_reduction) into the single-sample depth texture by a
 * full-screen triangle writing gl_FragDepth. */
static const char *gl_depth_resolve_fs =
    "#version 330 core\n"
    "uniform sampler2DMS u_src; uniform int u_mode; uniform int u_samples;\n"
    "void main() {\n"
    "    ivec2 p = ivec2(gl_FragCoord.xy);\n"
    "    float d = u_mode == 0 ? 0.0 : 1.0;\n"
    "    for (int s = 0; s < u_samples; s++) { float v = texelFetch(u_src, p, s).r; d = u_mode == 0 ? max(d, v) : min(d, v); }\n"
    "    gl_FragDepth = d;\n"
    "}\n";
static GLuint gl_depth_resolve_prog = 0;
static unsigned int gl_depth_resolve_gen = 0;

static int gl_resolve_depth_msaa(vio_render_target_object *rt)
{
    if (!rt->gl_msaa_depth_tex || !rt->depth_texture) return -1;
    if (!gl_depth_reduce_vao || gl_depth_reduce_gen != gl_context_generation) {
        glGenVertexArrays(1, &gl_depth_reduce_vao);
        gl_depth_reduce_prog = 0;
        gl_depth_reduce_gen = gl_context_generation;
    }
    if (!gl_depth_resolve_prog || gl_depth_resolve_gen != gl_context_generation) {
        gl_depth_resolve_prog = vio_opengl_compile_shader_source(gl_depth_reduce_vs, gl_depth_resolve_fs);
        gl_depth_resolve_gen = gl_context_generation;
        if (!gl_depth_resolve_prog) return -1;
    }
    GLint prev_fbo = 0, prev_prog = 0, prev_vao = 0, prev_active = 0, prev_tex = 0, prev_func = GL_LESS, vp[4];
    GLboolean prev_mask = GL_TRUE;
    GLboolean prev_test = glIsEnabled(GL_DEPTH_TEST), prev_scissor = glIsEnabled(GL_SCISSOR_TEST), prev_cull = glIsEnabled(GL_CULL_FACE);
    GLboolean prev_stencil = glIsEnabled(GL_STENCIL_TEST);
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prev_fbo);
    glGetIntegerv(GL_CURRENT_PROGRAM, &prev_prog);
    glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &prev_vao);
    glGetIntegerv(GL_ACTIVE_TEXTURE, &prev_active);
    glGetIntegerv(GL_VIEWPORT, vp);
    glGetIntegerv(GL_DEPTH_FUNC, &prev_func);
    glGetBooleanv(GL_DEPTH_WRITEMASK, &prev_mask);
    glActiveTexture(GL_TEXTURE0);
    glGetIntegerv(GL_TEXTURE_BINDING_2D_MULTISAMPLE, &prev_tex);

    glBindFramebuffer(GL_FRAMEBUFFER, rt->fbo);
    glUseProgram(gl_depth_resolve_prog);
    glBindVertexArray(gl_depth_reduce_vao);
    glBindTexture(GL_TEXTURE_2D_MULTISAMPLE, rt->gl_msaa_depth_tex);
    glUniform1i(glGetUniformLocation(gl_depth_resolve_prog, "u_src"), 0);
    glUniform1i(glGetUniformLocation(gl_depth_resolve_prog, "u_mode"), rt->depth_reduction == VIO_DEPTH_REDUCE_MIN ? 1 : 0);
    glUniform1i(glGetUniformLocation(gl_depth_resolve_prog, "u_samples"), rt->samples);
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_ALWAYS);
    glDepthMask(GL_TRUE);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_CULL_FACE);
    glDisable(GL_STENCIL_TEST);
    glViewport(0, 0, rt->width, rt->height);
    glDrawArrays(GL_TRIANGLES, 0, 3);

    glBindTexture(GL_TEXTURE_2D_MULTISAMPLE, (GLuint)prev_tex);
    glActiveTexture((GLenum)prev_active);
    glBindFramebuffer(GL_FRAMEBUFFER, (GLuint)prev_fbo);
    glUseProgram((GLuint)prev_prog);
    glBindVertexArray((GLuint)prev_vao);
    glViewport(vp[0], vp[1], vp[2], vp[3]);
    glDepthFunc((GLenum)prev_func);
    glDepthMask(prev_mask);
    if (!prev_test) glDisable(GL_DEPTH_TEST);
    if (prev_scissor) glEnable(GL_SCISSOR_TEST);
    if (prev_cull) glEnable(GL_CULL_FACE);
    if (prev_stencil) glEnable(GL_STENCIL_TEST);
    return 0;
}
static int opengl_generate_mipmaps(void *obj, int kind)
{
    if (!obj || !vio_gl.initialized) return -1;
    GLenum target = GL_TEXTURE_2D;
    GLuint id = 0;
    switch (kind) {
        case 0: {
            vio_render_target_object *rt = (vio_render_target_object *)obj;
            if (rt->backend_type == VIO_RT_BACKEND_OPENGL && rt->depth_only) return gl_generate_depth_mips(rt);
            if (rt->backend_type != VIO_RT_BACKEND_OPENGL || !rt->color_texture) return -1;
            target = rt->is_cube ? GL_TEXTURE_CUBE_MAP : GL_TEXTURE_2D;
            id = rt->color_texture;
            break;
        }
        case 1: {
            vio_texture_object *t = (vio_texture_object *)obj;
            if (!t->texture_id) return -1;
            target = t->is_3d ? GL_TEXTURE_3D : (t->is_array ? GL_TEXTURE_2D_ARRAY : GL_TEXTURE_2D);
            id = t->texture_id;
            break;
        }
        case 2: {
            vio_cubemap_object *cm = (vio_cubemap_object *)obj;
            if (!cm->texture_id) return -1;
            target = GL_TEXTURE_CUBE_MAP;
            id = cm->texture_id;
            break;
        }
        default: return -1;
    }
    glBindTexture(target, id);
    glGenerateMipmap(target);
    glTexParameteri(target, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
    glBindTexture(target, 0);
    return 0;
}

static void opengl_unbind_render_target(unsigned int default_fbo, int width, int height)
{
    if (!vio_gl.initialized) return;
    if (vio_gl.current_bound_rt) {
        opengl_rt_resolve_msaa((vio_render_target_object *)vio_gl.current_bound_rt);
        vio_gl.current_bound_rt = NULL;
    }
    glBindFramebuffer(GL_FRAMEBUFFER, default_fbo);
    if (width > 0 && height > 0) {
        glViewport(0, 0, width, height);
    }
}

static void opengl_set_uniform(const char *name, const void *data, int count, int type)
{
    if (!vio_gl.initialized) return;

    /* glUniform* operates on the currently bound program; glGetUniformLocation
     * needs an explicit program ID. Pull it from GL state — set by the most
     * recent bind_pipeline_state -> glUseProgram. */
    GLint program = 0;
    glGetIntegerv(GL_CURRENT_PROGRAM, &program);
    if (program <= 0) return;

    GLint alt[GL_ULOC_ALT_MAX];
    GLint loc = gl_uniform_location_all((GLuint)program, name, alt);
    if (loc < 0) return;  /* silently drop unknown uniforms — matches old behavior */

    /* Write the primary location and every per-stage duplicate (a uniform
     * declared in both the vertex and a geometry / tessellation stage). */
    for (int k = -1; k < GL_ULOC_ALT_MAX; k++) {
        GLint l = (k < 0) ? loc : alt[k];
        if (l < 0) continue;
        switch (type) {
            case VIO_UNIFORM_INT:
                glUniform1iv(l, count > 0 ? count : 1, (const GLint *)data);
                break;
            case VIO_UNIFORM_FLOAT:
                glUniform1fv(l, count > 0 ? count : 1, (const GLfloat *)data);
                break;
            case VIO_UNIFORM_VEC2:
                glUniform2fv(l, count, (const GLfloat *)data);
                break;
            case VIO_UNIFORM_VEC3:
                glUniform3fv(l, count, (const GLfloat *)data);
                break;
            case VIO_UNIFORM_VEC4:
                glUniform4fv(l, count, (const GLfloat *)data);
                break;
            case VIO_UNIFORM_MAT3:
                glUniformMatrix3fv(l, count, GL_FALSE, (const GLfloat *)data);
                break;
            case VIO_UNIFORM_MAT4:
                glUniformMatrix4fv(l, count, GL_FALSE, (const GLfloat *)data);
                break;
        }
    }
}

static int opengl_create_uniform_buffer(void *buf_obj, int size, const void *initial_data, int binding)
{
    vio_buffer_object *buf = (vio_buffer_object *)buf_obj;
    if (!vio_gl.initialized) return -1;

    glGenBuffers(1, &buf->buffer_id);
    buf->gl_generation = gl_context_generation;
    glBindBuffer(GL_UNIFORM_BUFFER, buf->buffer_id);
    glBufferData(GL_UNIFORM_BUFFER, (GLsizeiptr)size, initial_data, GL_DYNAMIC_DRAW);
    glBindBufferBase(GL_UNIFORM_BUFFER, (GLuint)binding, buf->buffer_id);
    glBindBuffer(GL_UNIFORM_BUFFER, 0);
    return 0;
}

static void opengl_update_uniform_buffer(void *buf_obj, const void *data, int size, int offset)
{
    vio_buffer_object *buf = (vio_buffer_object *)buf_obj;
    if (!buf || !buf->buffer_id) return;
    glBindBuffer(GL_UNIFORM_BUFFER, buf->buffer_id);
    glBufferSubData(GL_UNIFORM_BUFFER, (GLintptr)offset, (GLsizeiptr)size, data);
    glBindBuffer(GL_UNIFORM_BUFFER, 0);
}

static void opengl_bind_uniform_buffer(void *buf_obj, int binding)
{
    vio_buffer_object *buf = (vio_buffer_object *)buf_obj;
    if (!buf || !buf->buffer_id || !vio_gl.initialized) return;
    glBindBufferBase(GL_UNIFORM_BUFFER, (GLuint)binding, buf->buffer_id);
}

static void opengl_bind_texture_id(unsigned int texture_id, int slot)
{
    if (!vio_gl.initialized) return;
    glActiveTexture(GL_TEXTURE0 + (GLenum)slot);
    glBindTexture(GL_TEXTURE_2D, texture_id);
}

static void opengl_bind_cubemap_id(unsigned int texture_id, int slot)
{
    if (!vio_gl.initialized) return;
    glActiveTexture(GL_TEXTURE0 + (GLenum)slot);
    glBindTexture(GL_TEXTURE_CUBE_MAP, texture_id);
}

static void opengl_bind_texture_3d_id(unsigned int texture_id, int slot)
{
    if (!vio_gl.initialized) return;
    glActiveTexture(GL_TEXTURE0 + (GLenum)slot);
    glBindTexture(GL_TEXTURE_3D, texture_id);
}

static void opengl_flush_draw_state(void)
{
    if (!vio_gl.initialized) return;
    glBindVertexArray(0);
    glUseProgram(0);
}

static int opengl_upload_cubemap(void *cm_obj, int width, int height, const void *face_rgba[6])
{
    vio_cubemap_object *cm = (vio_cubemap_object *)cm_obj;
    if (!vio_gl.initialized) return -1;

    glGenTextures(1, &cm->texture_id);
    cm->gl_generation = gl_context_generation;
    glBindTexture(GL_TEXTURE_CUBE_MAP, cm->texture_id);

    for (int i = 0; i < 6; i++) {
        glTexImage2D(GL_TEXTURE_CUBE_MAP_POSITIVE_X + i, 0, GL_RGBA,
                     width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, face_rgba[i]);
    }

    if (cm->mipmaps) {
        glGenerateMipmap(GL_TEXTURE_CUBE_MAP);
        glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
    } else {
        glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    }
    glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_WRAP_R, GL_CLAMP_TO_EDGE);

    glBindTexture(GL_TEXTURE_CUBE_MAP, 0);
    return 0;
}

static int opengl_upload_font_atlas(void *font_obj, int width, int height,
                                    const unsigned char *r8_data, int swizzle_red_to_alpha)
{
    vio_font_object *font = (vio_font_object *)font_obj;
    if (!vio_gl.initialized) return -1;

    glGenTextures(1, &font->atlas_texture);
    font->gl_generation = gl_context_generation;
    glBindTexture(GL_TEXTURE_2D, font->atlas_texture);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RED, width, height,
                 0, GL_RED, GL_UNSIGNED_BYTE, r8_data);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    if (swizzle_red_to_alpha) {
        /* Sample .a returns the R8 coverage — the shader expects alpha. */
        GLint swizzle[] = {GL_ONE, GL_ONE, GL_ONE, GL_RED};
        glTexParameteriv(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_RGBA, swizzle);
    }
    glBindTexture(GL_TEXTURE_2D, 0);
    return 0;
}

/* Glyph atlas filled on demand (A33): the atlas is GL_RED, row 0 at GL row 0. */
static int opengl_update_font_atlas(void *font_obj, const unsigned char *r8, int x, int y, int w, int h)
{
    vio_font_object *font = (vio_font_object *)font_obj;
    if (!vio_gl.initialized || !font || !font->atlas_texture || !r8) return -1;
    glBindTexture(GL_TEXTURE_2D, font->atlas_texture);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexSubImage2D(GL_TEXTURE_2D, 0, x, y, w, h, GL_RED, GL_UNSIGNED_BYTE, r8);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    glBindTexture(GL_TEXTURE_2D, 0);
    return 0;
}

/* ── Comparison sampling (sampler*Shadow) ────────────────────────────
 *
 * A depth texture sampled through sampler2DShadow / sampler2DArrayShadow /
 * samplerCubeShadow needs GL_TEXTURE_COMPARE_MODE, and without it the result
 * is undefined. The texture itself must stay without compare mode, because
 * the same depth target is also read manually (texture(sampler2D, uv).r).
 * So vio keeps the texture as it is and binds a sampler object with compare
 * mode to every unit a shadow sampler of the CURRENT program reads, for the
 * duration of one draw - the D3D model (comparison sampler chosen by the
 * shader's sampler type): LEQUAL, linear (hardware PCF), clamp to an opaque
 * white border. The units are released after the draw so the 2D batch and
 * later draws sample them normally. */
#define GL_SHADOW_CACHE_SIZE 64
#define GL_SHADOW_MAX_LOCS   16

typedef struct {
    GLuint program;                    /* 0 = empty slot */
    int    count;                      /* shadow sampler locations (array elements expanded) */
    GLint  locs[GL_SHADOW_MAX_LOCS];
} gl_shadow_entry;

static gl_shadow_entry gl_shadow_cache[GL_SHADOW_CACHE_SIZE];
static GLuint   gl_cmp_sampler;
static unsigned gl_cmp_sampler_gen;

static void gl_shadow_forget_program(GLuint program)
{
    for (int i = 0; i < GL_SHADOW_CACHE_SIZE; i++) {
        if (gl_shadow_cache[i].program == program) gl_shadow_cache[i].program = 0;
    }
}

static int gl_is_shadow_sampler_type(GLenum type)
{
    switch (type) {
        case GL_SAMPLER_1D_SHADOW:
        case GL_SAMPLER_2D_SHADOW:
        case GL_SAMPLER_1D_ARRAY_SHADOW:
        case GL_SAMPLER_2D_ARRAY_SHADOW:
        case GL_SAMPLER_2D_RECT_SHADOW:
        case GL_SAMPLER_CUBE_SHADOW:
        case GL_SAMPLER_CUBE_MAP_ARRAY_SHADOW:
            return 1;
        default:
            return 0;
    }
}

/* The program's shadow-sampler locations, collected once per program. */
static const gl_shadow_entry *gl_shadow_entry_for(GLuint program)
{
    unsigned slot = (program * 2654435761u) % GL_SHADOW_CACHE_SIZE;
    gl_shadow_entry *e = &gl_shadow_cache[slot];
    if (e->program == program) return e;
    e->program = program;
    e->count = 0;
    GLint active = 0;
    glGetProgramiv(program, GL_ACTIVE_UNIFORMS, &active);
    for (GLint u = 0; u < active && e->count < GL_SHADOW_MAX_LOCS; u++) {
        char name[128];
        GLint size = 0;
        GLenum type = 0;
        glGetActiveUniform(program, (GLuint)u, sizeof(name), NULL, &size, &type, name);
        if (!gl_is_shadow_sampler_type(type)) continue;
        /* Array elements may not have consecutive locations: query each one. */
        char *bracket = strchr(name, '[');
        if (bracket) *bracket = '\0';
        for (GLint k = 0; k < size && e->count < GL_SHADOW_MAX_LOCS; k++) {
            char elem[160];
            if (size > 1) snprintf(elem, sizeof(elem), "%s[%d]", name, (int)k);
            else snprintf(elem, sizeof(elem), "%s", name);
            GLint loc = glGetUniformLocation(program, elem);
            if (loc >= 0) e->locs[e->count++] = loc;
        }
    }
    return e;
}

/* Bind the comparison sampler to every unit the current program's shadow
 * samplers read; returns the bound units as a bit mask for gl_shadow_end(). */
/* ── Multiview (GL_OVR_multiview2) ────────────────────────────────────
 * A program compiled with 'view_count' draws every view into one layer of the
 * target bound with VIO_RT_ALL_LAYERS: its attachments become multiview
 * attachments (views 0..N-1) for the draw, and a plain program gets the layered
 * attachments back. */
#define GL_MV_MAX_PROGRAMS 64
static struct { GLuint program; int views; unsigned int gen; } gl_mv_programs[GL_MV_MAX_PROGRAMS];

void vio_opengl_set_program_views(unsigned int program, int views)
{
    if (!program) return;
    int slot = -1;
    for (int i = 0; i < GL_MV_MAX_PROGRAMS; i++) {
        if (gl_mv_programs[i].program == program && gl_mv_programs[i].gen == gl_context_generation) { slot = i; break; }
        if (slot < 0 && (gl_mv_programs[i].program == 0 || gl_mv_programs[i].gen != gl_context_generation)) slot = i;
    }
    if (slot < 0) slot = (int)(program % GL_MV_MAX_PROGRAMS);
    gl_mv_programs[slot].program = views > 1 ? program : 0;
    gl_mv_programs[slot].views = views;
    gl_mv_programs[slot].gen = gl_context_generation;
}

/* Views come from GL_OVR_multiview2 unless forced to instancing. */
static int gl_mv_native(void)
{
    return vio_gl.caps.has_multiview && !vio_gl.caps.multiview_emulate;
}

static int gl_mv_program_views(void)
{
    GLint program = 0;
    glGetIntegerv(GL_CURRENT_PROGRAM, &program);
    for (int i = 0; i < GL_MV_MAX_PROGRAMS && program > 0; i++) {
        if (gl_mv_programs[i].program == (GLuint)program && gl_mv_programs[i].gen == gl_context_generation) return gl_mv_programs[i].views;
    }
    return 0;
}

/* Multiview by instancing (A10): instances per user instance of this draw. */
static int gl_mv_instances(void)
{
    if (gl_mv_native()) return 1;
    int v = gl_mv_program_views();
    return v > 1 ? v : 1;
}

static void gl_mv_prepare(void)
{
    if (!gl_mv_native()) return;
    GLint program = 0;
    glGetIntegerv(GL_CURRENT_PROGRAM, &program);
    int views = 0;
    for (int i = 0; i < GL_MV_MAX_PROGRAMS && program > 0; i++) {
        if (gl_mv_programs[i].program == (GLuint)program && gl_mv_programs[i].gen == gl_context_generation) { views = gl_mv_programs[i].views; break; }
    }
    vio_render_target_object *rt = (vio_render_target_object *)vio_gl.current_bound_rt;
    if (gl_mv_rt && (gl_mv_rt != rt || views != gl_mv_views)) {
        /* Back to the layered attachments of the previous multiview target. */
        vio_render_target_object *old = (vio_render_target_object *)gl_mv_rt;
        if (old == rt && rt->bound_face == VIO_RT_ALL_LAYERS) {
            glBindFramebuffer(GL_FRAMEBUFFER, rt->fbo);
            opengl_rt_attach_layer(rt, -1, 0);
        }
        gl_mv_rt = NULL;
        gl_mv_views = 0;
    }
    if (views > 1 && rt && rt->bound_face == VIO_RT_ALL_LAYERS && rt->layers >= views && !gl_mv_rt) {
        glBindFramebuffer(GL_FRAMEBUFFER, rt->fbo);
        if (!rt->depth_only) glFramebufferTextureMultiviewOVR(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, rt->color_texture, 0, 0, views);
        glFramebufferTextureMultiviewOVR(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, rt->depth_texture, 0, 0, views);
        gl_mv_rt = rt;
        gl_mv_views = views;
    }
}

/* gl_ClipDistance (OPEN-ITEMS-PLAN A29): the planes the bound pipeline's last
 * geometry stage writes are enabled around each of its draws only, so the 2D
 * batch and vio's own passes never clip against stale distances. */
static int gl_clip_active;

static void gl_clip_begin(GLint program)
{
    if (program <= 0 || (GLuint)program != vio_gl.clip_program || vio_gl.clip_count <= 0) return;
    for (int i = 0; i < vio_gl.clip_count; i++) glEnable(GL_CLIP_DISTANCE0 + i);
    gl_clip_active = vio_gl.clip_count;
}

static void gl_clip_end(void)
{
    for (int i = 0; i < gl_clip_active; i++) glDisable(GL_CLIP_DISTANCE0 + i);
    gl_clip_active = 0;
}

/* Fragment storage buffers (A15): SSBO binding points, re-bound before every
 * draw (compute dispatches use the same binding points). */
static GLuint gl_fs_storage[VIO_MAX_FRAGMENT_STORAGE];
static unsigned int gl_fs_storage_gen;
static int gl_fs_storage_used;

static int opengl_bind_fragment_storage(void *backend_buffer, int binding)
{
    if (binding < 0 || binding >= VIO_MAX_FRAGMENT_STORAGE) return -1;
    vio_opengl_compute_buffer *buf = (vio_opengl_compute_buffer *)backend_buffer;
    if (gl_fs_storage_gen != gl_context_generation) { memset(gl_fs_storage, 0, sizeof(gl_fs_storage)); gl_fs_storage_gen = gl_context_generation; }
    gl_fs_storage[binding] = buf ? buf->ssbo : 0;
    /* Unbinding releases the binding point now: a shader that still declares
     * the block must not keep writing into the old buffer. */
    if (!buf && vio_gl.initialized) glBindBufferBase(GL_SHADER_STORAGE_BUFFER, (GLuint)binding, 0);
    return 0;
}

static void gl_fs_storage_apply(void)
{
    if (gl_fs_storage_gen != gl_context_generation) return;
    for (int i = 0; i < VIO_MAX_FRAGMENT_STORAGE; i++) {
        if (!gl_fs_storage[i]) continue;
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, (GLuint)i, gl_fs_storage[i]);
        gl_fs_storage_used = 1;
    }
}

static unsigned gl_shadow_begin(void)
{
    gl_fs_storage_apply();
    GLint program = 0;    glGetIntegerv(GL_CURRENT_PROGRAM, &program);
    gl_clip_begin(program);
    if (!glBindSampler || !glGenSamplers) return 0;
    if (program <= 0) return 0;
    const gl_shadow_entry *e = gl_shadow_entry_for((GLuint)program);
    if (e->count == 0) return 0;
    if (!gl_cmp_sampler || gl_cmp_sampler_gen != gl_context_generation) {
        /* Sampler objects belong to a context: a new context gets its own. */
        glGenSamplers(1, &gl_cmp_sampler);
        gl_cmp_sampler_gen = gl_context_generation;
        static const float white[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
        glSamplerParameteri(gl_cmp_sampler, GL_TEXTURE_COMPARE_MODE, GL_COMPARE_REF_TO_TEXTURE);
        glSamplerParameteri(gl_cmp_sampler, GL_TEXTURE_COMPARE_FUNC, GL_LEQUAL);
        glSamplerParameteri(gl_cmp_sampler, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glSamplerParameteri(gl_cmp_sampler, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glSamplerParameteri(gl_cmp_sampler, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_BORDER);
        glSamplerParameteri(gl_cmp_sampler, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_BORDER);
        glSamplerParameteri(gl_cmp_sampler, GL_TEXTURE_WRAP_R, GL_CLAMP_TO_BORDER);
        glSamplerParameterfv(gl_cmp_sampler, GL_TEXTURE_BORDER_COLOR, white);
    }
    unsigned mask = 0;
    for (int i = 0; i < e->count; i++) {
        GLint unit = 0;
        glGetUniformiv((GLuint)program, e->locs[i], &unit);
        if (unit < 0 || unit >= 32 || (mask & (1u << unit))) continue;
        glBindSampler((GLuint)unit, gl_cmp_sampler);
        mask |= 1u << unit;
    }
    return mask;
}

static void gl_shadow_end(unsigned mask)
{
    gl_clip_end();
    for (GLuint unit = 0; mask; unit++, mask >>= 1) {
        if (mask & 1u) glBindSampler(unit, 0);
    }
}

static void opengl_draw_mesh(void *mesh_obj)
{
    vio_mesh_object *mesh = (vio_mesh_object *)mesh_obj;
    if (!vio_gl.initialized) return;

    /* If no pipeline bound a program, fall back to the built-in default
     * (color-aware or pos-only depending on the mesh's vertex layout). */
    GLint current_program = 0;
    glGetIntegerv(GL_CURRENT_PROGRAM, &current_program);
    int used_default = 0;
    if (current_program <= 0) {
        GLuint fallback = mesh->has_colors
            ? vio_gl.default_shader_program
            : vio_gl.default_shader_pos_only;
        glUseProgram(fallback);
        used_default = 1;
    }

    gl_mv_prepare();
    unsigned shadow_units = gl_shadow_begin();
    glBindVertexArray(mesh->vao);
    int mv = gl_mv_instances();
    if (mv > 1) {
        if (mesh->index_count > 0)
            glDrawElementsInstanced(gl_draw_mode(), mesh->index_count, mesh->index_bytes == 2 ? GL_UNSIGNED_SHORT : GL_UNSIGNED_INT, 0, mv);
        else
            glDrawArraysInstanced(gl_draw_mode(), 0, mesh->vertex_count, mv);
    } else if (mesh->index_count > 0) {
        glDrawElements(gl_draw_mode(), mesh->index_count, mesh->index_bytes == 2 ? GL_UNSIGNED_SHORT : GL_UNSIGNED_INT, 0);
    } else {
        glDrawArrays(gl_draw_mode(), 0, mesh->vertex_count);
    }
    glBindVertexArray(0);
    gl_shadow_end(shadow_units);

    if (used_default) {
        glUseProgram(0);
    }
}

static void opengl_draw_mesh_instanced(void *mesh_obj,
                                       const float *matrices, int instance_count)
{
    vio_mesh_object *mesh = (vio_mesh_object *)mesh_obj;
    if (!vio_gl.initialized || instance_count <= 0) return;

    /* Transient instance-VBO populated with the column-major mat4 stream. */
    GLuint instance_vbo = 0;
    glGenBuffers(1, &instance_vbo);
    glBindBuffer(GL_ARRAY_BUFFER, instance_vbo);
    glBufferData(GL_ARRAY_BUFFER,
                 (GLsizeiptr)((size_t)instance_count * 16 * sizeof(float)),
                 matrices, GL_STREAM_DRAW);

    /* Bind mesh VAO and wire the per-instance matrix attributes at locations
     * 3..6 (one vec4 per column). Divisor=1 advances them per instance; with
     * multiview by instancing every `mv` instances (one per view). */
    int mv = gl_mv_instances();
    glBindVertexArray(mesh->vao);
    for (int col = 0; col < 4; col++) {
        GLuint loc = (GLuint)(3 + col);
        glEnableVertexAttribArray(loc);
        glVertexAttribPointer(loc, 4, GL_FLOAT, GL_FALSE,
                              sizeof(float) * 16,
                              (void *)(uintptr_t)(sizeof(float) * 4 * col));
        glVertexAttribDivisor(loc, (GLuint)mv);
    }

    gl_mv_prepare();
    unsigned shadow_units = gl_shadow_begin();
    if (mesh->index_count > 0) {
        glDrawElementsInstanced(gl_draw_mode(), mesh->index_count,
                                mesh->index_bytes == 2 ? GL_UNSIGNED_SHORT : GL_UNSIGNED_INT, 0, (GLsizei)instance_count * mv);
    } else {
        glDrawArraysInstanced(gl_draw_mode(), 0, mesh->vertex_count,
                              (GLsizei)instance_count * mv);
    }
    gl_shadow_end(shadow_units);

    for (int col = 0; col < 4; col++) {
        glVertexAttribDivisor((GLuint)(3 + col), 0);
        glDisableVertexAttribArray((GLuint)(3 + col));
    }
    glBindVertexArray(0);
    glDeleteBuffers(1, &instance_vbo);
}

/* ── Graphics-stage storage buffers (Path B: readback-free instancing) ──── */

/* Bind a storage buffer (SSBO) to the graphics pipeline so the vertex shader
 * can read it via gl_InstanceIndex. binding is the GLSL `layout(std430,
 * binding=N)` point. access/element_count/stride are unused on GL — an SSBO is
 * bound whole and the shader's std430 layout defines interpretation. The
 * binding persists until re-bound, so it survives across the following draw. */
static void opengl_bind_storage_buffer(void *backend_buffer, int binding, int access,
                                       int element_count, int stride)
{
    (void)access; (void)element_count; (void)stride;
    if (!vio_gl.initialized || !backend_buffer) return;
    vio_opengl_compute_buffer *buf = (vio_opengl_compute_buffer *)backend_buffer;
    /* Make prior compute writes to this SSBO visible to the vertex-stage read
     * that the following draw issues (compute -> graphics RAW hazard). */
    glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, binding, buf->ssbo);
}

/* Instanced draw with per-instance data pulled from the bound SSBO (no instance
 * VBO, no divisor attributes). The vertex shader indexes the buffer via
 * gl_InstanceIndex. Mirrors opengl_draw_mesh_instanced minus the instance
 * attribute wiring. */
static void opengl_draw_instanced_from_storage(void *mesh_obj, int instance_count)
{
    vio_mesh_object *mesh = (vio_mesh_object *)mesh_obj;
    if (!vio_gl.initialized || instance_count <= 0) return;

    gl_mv_prepare();
    unsigned shadow_units = gl_shadow_begin();
    glBindVertexArray(mesh->vao);
    int mv = gl_mv_instances();
    if (mesh->index_count > 0) {
        glDrawElementsInstanced(gl_draw_mode(), mesh->index_count,
                                mesh->index_bytes == 2 ? GL_UNSIGNED_SHORT : GL_UNSIGNED_INT, 0, (GLsizei)instance_count * mv);
    } else {
        glDrawArraysInstanced(gl_draw_mode(), 0, mesh->vertex_count,
                              (GLsizei)instance_count * mv);
    }
    glBindVertexArray(0);
    gl_shadow_end(shadow_units);
}

/* Indirect draw (GAP-PHASE5 Block 8, GL >= 4.0): the SSBO doubles as the
 * GL_DRAW_INDIRECT_BUFFER; one glDraw*Indirect per record (multi-draw needs 4.3,
 * the loop keeps 4.0 contexts covered). */
static void opengl_draw_indirect(void *mesh_obj, void *args_buffer, int max_draws, size_t offset)
{
    vio_mesh_object *mesh = (vio_mesh_object *)mesh_obj;
    vio_opengl_compute_buffer *args = (vio_opengl_compute_buffer *)args_buffer;
    if (!vio_gl.initialized || !GLAD_GL_VERSION_4_0 || !mesh || !args || !args->ssbo || max_draws <= 0) return;
    gl_mv_prepare();
    unsigned shadow_units = gl_shadow_begin();
    glBindVertexArray(mesh->vao);
    int mv = gl_mv_instances();
    if (mv > 1) {
        /* Multiview by instancing: every record's instances x views, its first
         * instance scaled alike (gl_InstanceIndex / views). The records are read
         * back - a stall, but only on this emulated path. */
        size_t stride = mesh->index_count > 0 ? 20 : 16;
        GLuint rec[5];
        glBindBuffer(GL_COPY_READ_BUFFER, args->ssbo);
        for (int i = 0; i < max_draws; i++) {
            memset(rec, 0, sizeof(rec));
            glGetBufferSubData(GL_COPY_READ_BUFFER, (GLintptr)(offset + (size_t)i * stride), (GLsizeiptr)stride, rec);
            if (mesh->index_count > 0) {
                GLenum type = mesh->index_bytes == 2 ? GL_UNSIGNED_SHORT : GL_UNSIGNED_INT;
                const void *first = (const void *)(uintptr_t)((size_t)rec[2] * (size_t)mesh->index_bytes);
                if (GLAD_GL_VERSION_4_2)
                    glDrawElementsInstancedBaseVertexBaseInstance(gl_draw_mode(), (GLsizei)rec[0], type, first,
                        (GLsizei)(rec[1] * (GLuint)mv), (GLint)rec[3], rec[4] * (GLuint)mv);
                else
                    glDrawElementsInstancedBaseVertex(gl_draw_mode(), (GLsizei)rec[0], type, first, (GLsizei)(rec[1] * (GLuint)mv), (GLint)rec[3]);
            } else if (GLAD_GL_VERSION_4_2) {
                glDrawArraysInstancedBaseInstance(gl_draw_mode(), (GLint)rec[2], (GLsizei)rec[0], (GLsizei)(rec[1] * (GLuint)mv), rec[3] * (GLuint)mv);
            } else {
                glDrawArraysInstanced(gl_draw_mode(), (GLint)rec[2], (GLsizei)rec[0], (GLsizei)(rec[1] * (GLuint)mv));
            }
        }
        glBindBuffer(GL_COPY_READ_BUFFER, 0);
        glBindVertexArray(0);
        gl_shadow_end(shadow_units);
        return;
    }
    glBindBuffer(GL_DRAW_INDIRECT_BUFFER, args->ssbo);
    if (mesh->index_count > 0) {
        GLenum type = mesh->index_bytes == 2 ? GL_UNSIGNED_SHORT : GL_UNSIGNED_INT;
        for (int i = 0; i < max_draws; i++) {
            glDrawElementsIndirect(gl_draw_mode(), type, (const void *)(uintptr_t)(offset + (size_t)i * 20));
        }
    } else {
        for (int i = 0; i < max_draws; i++) {
            glDrawArraysIndirect(gl_draw_mode(), (const void *)(uintptr_t)(offset + (size_t)i * 16));
        }
    }
    glBindBuffer(GL_DRAW_INDIRECT_BUFFER, 0);
    glBindVertexArray(0);
    gl_shadow_end(shadow_units);
}

static int gl_has_ext(const char *name);   /* defined with the caps setup below */

/* ── Texture arrays / block compression / explicit mip chains (GAP-PHASE5 Block 9) ── */

/* S3TC is an extension, not core: the vendored core-profile glad declares no enums
 * for it, so static Linux / macOS builds failed with "undeclared" (Windows picked
 * them up from another header). Values from EXT_texture_compression_s3tc. */
#ifndef GL_COMPRESSED_RGBA_S3TC_DXT1_EXT
#define GL_COMPRESSED_RGBA_S3TC_DXT1_EXT 0x83F1
#endif
#ifndef GL_COMPRESSED_RGBA_S3TC_DXT5_EXT
#define GL_COMPRESSED_RGBA_S3TC_DXT5_EXT 0x83F3
#endif

static GLenum opengl_texfmt_internal(int fmt)
{
    switch (fmt) {
        case VIO_FORMAT_BC1: return GL_COMPRESSED_RGBA_S3TC_DXT1_EXT;
        case VIO_FORMAT_BC3: return GL_COMPRESSED_RGBA_S3TC_DXT5_EXT;
        case VIO_FORMAT_BC4: return GL_COMPRESSED_RED_RGTC1;
        case VIO_FORMAT_BC5: return GL_COMPRESSED_RG_RGTC2;
        case VIO_FORMAT_BC7: return GL_COMPRESSED_RGBA_BPTC_UNORM;
        case VIO_FORMAT_ASTC_4x4: return 0x93B0;   /* GL_COMPRESSED_RGBA_ASTC_4x4_KHR */
        case VIO_FORMAT_ASTC_5x5: return 0x93B2;
        case VIO_FORMAT_ASTC_6x6: return 0x93B4;
        case VIO_FORMAT_ASTC_8x8: return 0x93B7;
        case VIO_FORMAT_R8:  return GL_R8;
        default:             return GL_RGBA8;
    }
}

/* S3TC (BC1/BC3) is an extension every desktop driver ships; RGTC (BC4/BC5) is
 * core since 3.0; BPTC (BC7) is core since 4.2. */
static int opengl_has_texfmt(int fmt)
{
    switch (fmt) {
        case VIO_FORMAT_BC1: case VIO_FORMAT_BC3: return gl_has_ext("GL_EXT_texture_compression_s3tc");
        case VIO_FORMAT_BC7: return gl_ge(4, 2) || gl_has_ext("GL_ARB_texture_compression_bptc");
        case VIO_FORMAT_ASTC_4x4: case VIO_FORMAT_ASTC_5x5: case VIO_FORMAT_ASTC_6x6: case VIO_FORMAT_ASTC_8x8:
            return gl_has_ext("GL_KHR_texture_compression_astc_ldr");
        default: return 1;
    }
}

/* Level-major data, layers consecutive inside a level (see vio_texture_desc).
 * GL_TEXTURE_2D_ARRAY for layers > 1, else GL_TEXTURE_2D; glCompressedTexImage
 * for BC formats. An explicit chain sets MAX_LEVEL; a single uncompressed level
 * with `mipmaps` is completed with glGenerateMipmap. */
static int opengl_upload_texture_ex(void *tex_obj, vio_texture_desc *desc)
{
    vio_texture_object *tex = (vio_texture_object *)tex_obj;
    if (!vio_gl.initialized || !desc || !desc->data) return -1;
    int compressed = vio_texfmt_is_compressed(desc->format);
    if (!opengl_has_texfmt(desc->format)) return -1;
    int layers = desc->layers > 1 ? desc->layers : 1;
    int levels = desc->mip_levels > 1 ? desc->mip_levels : 1;
    GLenum target = layers > 1 ? GL_TEXTURE_2D_ARRAY : GL_TEXTURE_2D;
    GLenum internal = opengl_texfmt_internal(desc->format);
    GLenum ext_fmt = desc->format == VIO_FORMAT_R8 ? GL_RED : GL_RGBA;
    int mip = levels > 1 || (desc->mipmaps && !compressed);

    glGenTextures(1, &tex->texture_id);
    tex->gl_generation = gl_context_generation;
    glBindTexture(target, tex->texture_id);

    GLint gl_wrap;
    switch (desc->wrap) {
        case VIO_WRAP_CLAMP:  gl_wrap = GL_CLAMP_TO_EDGE; break;
        case VIO_WRAP_MIRROR: gl_wrap = GL_MIRRORED_REPEAT; break;
        default:              gl_wrap = GL_REPEAT; break;
    }
    glTexParameteri(target, GL_TEXTURE_WRAP_S, gl_wrap);
    glTexParameteri(target, GL_TEXTURE_WRAP_T, gl_wrap);
    int nearest = desc->filter == VIO_FILTER_NEAREST;
    glTexParameteri(target, GL_TEXTURE_MAG_FILTER, nearest ? GL_NEAREST : GL_LINEAR);
    glTexParameteri(target, GL_TEXTURE_MIN_FILTER,
        nearest ? (mip ? GL_NEAREST_MIPMAP_NEAREST : GL_NEAREST) : (mip ? GL_LINEAR_MIPMAP_LINEAR : GL_LINEAR));
    if (desc->anisotropy > 1 && !nearest &&
        (gl_ge(4, 6) || gl_has_ext("GL_ARB_texture_filter_anisotropic") || gl_has_ext("GL_EXT_texture_filter_anisotropic"))) {
        GLfloat max_aniso = 1.0f;
        glGetFloatv(GL_MAX_TEXTURE_MAX_ANISOTROPY, &max_aniso);
        GLfloat want = (GLfloat)(desc->anisotropy > 16 ? 16 : desc->anisotropy);
        glTexParameterf(target, GL_TEXTURE_MAX_ANISOTROPY, want < max_aniso ? want : max_aniso);
    }

    if (!compressed) glPixelStorei(GL_UNPACK_ALIGNMENT, 1);   /* tightly packed rows (R8, odd widths) */
    const uint8_t *p = (const uint8_t *)desc->data;
    int lw = desc->width, lh = desc->height;
    for (int l = 0; l < levels; l++) {
        size_t image = vio_texfmt_image_size(desc->format, lw, lh);
        size_t total = image * (size_t)layers;
        if (layers > 1) {
            if (compressed) glCompressedTexImage3D(target, l, internal, lw, lh, layers, 0, (GLsizei)total, p);
            else            glTexImage3D(target, l, internal, lw, lh, layers, 0, ext_fmt, GL_UNSIGNED_BYTE, p);
        } else {
            if (compressed) glCompressedTexImage2D(target, l, internal, lw, lh, 0, (GLsizei)image, p);
            else            glTexImage2D(target, l, internal, lw, lh, 0, ext_fmt, GL_UNSIGNED_BYTE, p);
        }
        p += total;
        lw = lw > 1 ? lw / 2 : 1;
        lh = lh > 1 ? lh / 2 : 1;
    }
    if (!compressed) glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    if (levels > 1)  glTexParameteri(target, GL_TEXTURE_MAX_LEVEL, levels - 1);
    else if (mip)    glGenerateMipmap(target);
    glBindTexture(target, 0);
    return glGetError() == GL_NO_ERROR ? 0 : -1;
}

static void opengl_bind_texture_array_id(unsigned int texture_id, int slot)
{
    if (!vio_gl.initialized) return;
    glActiveTexture(GL_TEXTURE0 + (GLenum)slot);
    glBindTexture(GL_TEXTURE_2D_ARRAY, texture_id);
}

static int opengl_upload_texture_2d(void *tex_obj,
                                    const void *pixels, int width, int height, int channels,
                                    int filter, int wrap, int mipmaps)
{
    (void)channels;  /* always uploaded as RGBA — stbi already expanded */
    vio_texture_object *tex = (vio_texture_object *)tex_obj;
    if (!vio_gl.initialized) return -1;

    glGenTextures(1, &tex->texture_id);
    tex->gl_generation = gl_context_generation;
    glBindTexture(GL_TEXTURE_2D, tex->texture_id);

    GLint gl_wrap;
    switch (wrap) {
        case VIO_WRAP_CLAMP:  gl_wrap = GL_CLAMP_TO_EDGE; break;
        case VIO_WRAP_MIRROR: gl_wrap = GL_MIRRORED_REPEAT; break;
        default:              gl_wrap = GL_REPEAT; break;
    }
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, gl_wrap);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, gl_wrap);

    GLint gl_filter = (filter == VIO_FILTER_NEAREST) ? GL_NEAREST : GL_LINEAR;
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, gl_filter);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, gl_filter);

    /* vio_texture(['anisotropy' => N]): core in GL 4.6, otherwise the
     * ubiquitous ARB/EXT extension (same token value). Clamped to the driver
     * maximum; silently off when neither is exported (tex->anisotropy is read
     * from the object so the vtable signature stays untouched). */
    if (tex->anisotropy > 1 && filter != VIO_FILTER_NEAREST &&
        (gl_ge(4, 6) || gl_has_ext("GL_ARB_texture_filter_anisotropic") ||
         gl_has_ext("GL_EXT_texture_filter_anisotropic"))) {
        GLfloat max_aniso = 1.0f;
        glGetFloatv(GL_MAX_TEXTURE_MAX_ANISOTROPY, &max_aniso);
        GLfloat want = (GLfloat)(tex->anisotropy > 16 ? 16 : tex->anisotropy);
        glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MAX_ANISOTROPY, want < max_aniso ? want : max_aniso);
    }

    /* Sized internal format: image load/store (storage images) rejects the
     * unsized GL_RGBA; RGBA8 is what every backend stores anyway. */
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels);

    if (mipmaps) {
        glGenerateMipmap(GL_TEXTURE_2D);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER,
            (filter == VIO_FILTER_NEAREST) ? GL_NEAREST_MIPMAP_NEAREST
                                           : GL_LINEAR_MIPMAP_LINEAR);
    }

    glBindTexture(GL_TEXTURE_2D, 0);
    return 0;
}

/* Upload an RGBA8 volume texture (Fieldtracing SDF). GL_TEXTURE_3D is core
 * since GL 1.2, so this needs no extension guard on the 3.3+ floor. No mipmaps:
 * the SDF trace samples a single LOD (coarse cones can be added later via an
 * explicit mip parameter). */
static int opengl_upload_texture_3d(void *tex_obj,
                                    const void *pixels, int width, int height, int depth,
                                    int channels, int filter, int wrap)
{
    (void)channels;  /* always RGBA8 */
    vio_texture_object *tex = (vio_texture_object *)tex_obj;
    if (!vio_gl.initialized) return -1;

    glGenTextures(1, &tex->texture_id);
    tex->gl_generation = gl_context_generation;
    glBindTexture(GL_TEXTURE_3D, tex->texture_id);

    GLint gl_wrap;
    switch (wrap) {
        case VIO_WRAP_CLAMP:  gl_wrap = GL_CLAMP_TO_EDGE; break;
        case VIO_WRAP_MIRROR: gl_wrap = GL_MIRRORED_REPEAT; break;
        default:              gl_wrap = GL_REPEAT; break;
    }
    glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_WRAP_S, gl_wrap);
    glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_WRAP_T, gl_wrap);
    glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_WRAP_R, gl_wrap);

    GLint gl_filter = (filter == VIO_FILTER_NEAREST) ? GL_NEAREST : GL_LINEAR;
    glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_MIN_FILTER, gl_filter);
    glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_MAG_FILTER, gl_filter);

    glTexImage3D(GL_TEXTURE_3D, 0, GL_RGBA8, width, height, depth, 0,
                 GL_RGBA, GL_UNSIGNED_BYTE, pixels);

    glBindTexture(GL_TEXTURE_3D, 0);
    return 0;
}

static int opengl_create_mesh(void *mesh_obj,
                              const void *vertex_data, int vertex_data_size,
                              int stride,
                              const vio_mesh_attrib *layout, int layout_count,
                              const void *indices, int index_count, int index_bytes)
{
    vio_mesh_object *mesh = (vio_mesh_object *)mesh_obj;
    if (!vio_gl.initialized) return -1;

    glGenVertexArrays(1, &mesh->vao);
    mesh->gl_generation = gl_context_generation;
    glGenBuffers(1, &mesh->vbo);

    glBindVertexArray(mesh->vao);
    glBindBuffer(GL_ARRAY_BUFFER, mesh->vbo);
    glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)vertex_data_size, vertex_data, GL_STATIC_DRAW);

    for (int i = 0; i < layout_count; i++) {
        glVertexAttribPointer((GLuint)layout[i].location,
                              (GLint)layout[i].components,
                              GL_FLOAT, GL_FALSE,
                              (GLsizei)stride,
                              (void *)(intptr_t)layout[i].offset);
        glEnableVertexAttribArray((GLuint)layout[i].location);
    }

    if (indices && index_count > 0) {
        glGenBuffers(1, &mesh->ebo);
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, mesh->ebo);
        glBufferData(GL_ELEMENT_ARRAY_BUFFER,
                     (GLsizeiptr)((index_bytes == 2 ? 2 : 4) * (size_t)index_count),
                     indices, GL_STATIC_DRAW);
    }

    glBindVertexArray(0);
    return 0;
}

/* Apply one vio blend mode either globally (buf < 0) or to one draw buffer
 * (GL 4.0 glBlendFunci family; used by pipelines with per-attachment state). */
static void opengl_apply_blend(int buf, int blend)
{
    GLenum eq = GL_FUNC_ADD, sRGB = GL_ONE, dRGB = GL_ZERO, sA = GL_ONE, dA = GL_ZERO;
    int enable = 1;
    switch (blend) {
        case VIO_BLEND_ALPHA:         sRGB = GL_SRC_ALPHA; dRGB = GL_ONE_MINUS_SRC_ALPHA; sA = GL_ONE; dA = GL_ONE_MINUS_SRC_ALPHA; break;
        case VIO_BLEND_ADDITIVE:      sRGB = GL_SRC_ALPHA; dRGB = GL_ONE; sA = GL_ONE; dA = GL_ONE; break;
        case VIO_BLEND_PREMULTIPLIED: sRGB = GL_ONE; dRGB = GL_ONE_MINUS_SRC_ALPHA; sA = GL_ONE; dA = GL_ONE_MINUS_SRC_ALPHA; break;
        case VIO_BLEND_MULTIPLY:      sRGB = GL_DST_COLOR; dRGB = GL_ZERO; sA = GL_DST_ALPHA; dA = GL_ZERO; break;
        case VIO_BLEND_SCREEN:        sRGB = GL_ONE; dRGB = GL_ONE_MINUS_SRC_COLOR; sA = GL_ONE; dA = GL_ONE_MINUS_SRC_ALPHA; break;
        case VIO_BLEND_MIN:           eq = GL_MIN; sRGB = dRGB = sA = dA = GL_ONE; break;
        case VIO_BLEND_MAX:           eq = GL_MAX; sRGB = dRGB = sA = dA = GL_ONE; break;
        default:                      enable = 0; break;
    }
    if (buf < 0) {
        glBlendEquation(eq);
        if (!enable) { glDisable(GL_BLEND); return; }
        glEnable(GL_BLEND);
        glBlendFuncSeparate(sRGB, dRGB, sA, dA);
        return;
    }
    glBlendEquationi((GLuint)buf, eq);
    if (!enable) { glDisablei(GL_BLEND, (GLuint)buf); return; }
    glEnablei(GL_BLEND, (GLuint)buf);
    glBlendFuncSeparatei((GLuint)buf, sRGB, dRGB, sA, dA);
}

static GLenum opengl_compare_func(int f)
{
    switch (f) {
        case VIO_CMP_NEVER:    return GL_NEVER;
        case VIO_CMP_LESS:     return GL_LESS;
        case VIO_CMP_EQUAL:    return GL_EQUAL;
        case VIO_CMP_LEQUAL:   return GL_LEQUAL;
        case VIO_CMP_GREATER:  return GL_GREATER;
        case VIO_CMP_NOTEQUAL: return GL_NOTEQUAL;
        case VIO_CMP_GEQUAL:   return GL_GEQUAL;
        default:               return GL_ALWAYS;
    }
}

static GLenum opengl_stencil_op(int op)
{
    switch (op) {
        case VIO_STENCIL_ZERO:      return GL_ZERO;
        case VIO_STENCIL_REPLACE:   return GL_REPLACE;
        case VIO_STENCIL_INCR:      return GL_INCR;
        case VIO_STENCIL_DECR:      return GL_DECR;
        case VIO_STENCIL_INVERT:    return GL_INVERT;
        case VIO_STENCIL_INCR_WRAP: return GL_INCR_WRAP;
        case VIO_STENCIL_DECR_WRAP: return GL_DECR_WRAP;
        default:                    return GL_KEEP;
    }
}

static void opengl_bind_pipeline_state(void *pipe_ptr)
{
    vio_pipeline_object *pipe = (vio_pipeline_object *)pipe_ptr;
    if (!vio_gl.initialized || !pipe) return;

    glUseProgram(pipe->shader_program);

    /* Clip planes of this pipeline, applied per draw (gl_clip_begin). */
    {
        vio_shader_object *sh = (vio_shader_object *)pipe->shader_ref;
        if (sh && !sh->clip_known) {
            const uint32_t *spv = sh->vert_spirv;
            size_t sz = sh->vert_spirv_size;
            if (sh->stage_spirv[VIO_EXTRA_STAGE_INDEX(VIO_STAGE_GEOMETRY)]) {
                spv = sh->stage_spirv[VIO_EXTRA_STAGE_INDEX(VIO_STAGE_GEOMETRY)];
                sz = sh->stage_spirv_size[VIO_EXTRA_STAGE_INDEX(VIO_STAGE_GEOMETRY)];
            } else if (sh->stage_spirv[VIO_EXTRA_STAGE_INDEX(VIO_STAGE_TESS_EVAL)]) {
                spv = sh->stage_spirv[VIO_EXTRA_STAGE_INDEX(VIO_STAGE_TESS_EVAL)];
                sz = sh->stage_spirv_size[VIO_EXTRA_STAGE_INDEX(VIO_STAGE_TESS_EVAL)];
            }
            int nclip = spv ? vio_spirv_output_clip_distances(spv, sz) : 0;
            sh->clip_distances = nclip > 8 ? 8 : nclip;
            sh->clip_known = 1;
        }
        vio_gl.clip_program = pipe->shader_program;
        vio_gl.clip_count = sh ? sh->clip_distances : 0;
    }

    /* Primitive mode for the following draws. A shader with a tessellation
     * control stage only accepts patches, whatever 'topology' says. */
    {
        vio_shader_object *sh = (vio_shader_object *)pipe->shader_ref;
        vio_topology topo = pipe->topology;
        if (sh && sh->has_tessellation) topo = VIO_PATCHES;
        vio_gl.draw_topology = (int)topo;
        vio_gl.patch_vertices = pipe->patch_vertices > 0 ? pipe->patch_vertices : 3;
        if (topo == VIO_PATCHES && vio_gl.caps.has_tessellation && glPatchParameteri) {
            glPatchParameteri(GL_PATCH_VERTICES, vio_gl.patch_vertices);
        }
    }

    if (pipe->cull_mode == VIO_CULL_NONE) {
        glDisable(GL_CULL_FACE);
    } else {
        glEnable(GL_CULL_FACE);
        glCullFace(pipe->cull_mode == VIO_CULL_BACK ? GL_BACK : GL_FRONT);
    }

    if (pipe->depth_test) {
        glEnable(GL_DEPTH_TEST);
        glDepthFunc(pipe->depth_func == VIO_DEPTH_LEQUAL ? GL_LEQUAL : GL_LESS);
    } else {
        glDisable(GL_DEPTH_TEST);
    }
    glDepthMask(pipe->depth_write ? GL_TRUE : GL_FALSE);
    /* Stencil (VIO_FEATURE_STENCIL): one function / op set for both faces. A
     * pipeline without 'stencil' disables the test and restores the full write
     * mask so vio_clear can reset the plane. */
    if (pipe->stencil_enable) {
        glEnable(GL_STENCIL_TEST);
        glStencilFunc(opengl_compare_func(pipe->stencil_func), pipe->stencil_ref, (GLuint)pipe->stencil_read_mask);
        glStencilOp(opengl_stencil_op(pipe->stencil_fail_op), opengl_stencil_op(pipe->stencil_depth_fail_op),
                    opengl_stencil_op(pipe->stencil_pass_op));
        glStencilMask((GLuint)pipe->stencil_write_mask);
    } else {
        glDisable(GL_STENCIL_TEST);
        glStencilMask(0xFFu);
    }
    if (pipe->depth_bias != 0.0f || pipe->slope_scaled_depth_bias != 0.0f) {
        glEnable(GL_POLYGON_OFFSET_FILL);
        glPolygonOffset(pipe->slope_scaled_depth_bias, pipe->depth_bias);
    } else {
        glDisable(GL_POLYGON_OFFSET_FILL);
    }

    /* Blend + colour write mask: one global state, or - 'attachment_blend' /
     * 'attachment_color_mask' - per draw buffer (GL 4.0 indexed state). A later
     * pipeline without the per-attachment arrays resets every buffer again through
     * the non-indexed calls, so no state leaks between pipelines. */
    if (pipe->per_attachment && GLAD_GL_VERSION_4_0) {
        for (int ai = 0; ai < VIO_MAX_COLOR_ATTACHMENTS; ai++) {
            int cm = pipe->attachment_mask[ai];
            glColorMaski((GLuint)ai, (cm & VIO_COLOR_R) ? GL_TRUE : GL_FALSE, (cm & VIO_COLOR_G) ? GL_TRUE : GL_FALSE,
                         (cm & VIO_COLOR_B) ? GL_TRUE : GL_FALSE, (cm & VIO_COLOR_A) ? GL_TRUE : GL_FALSE);
            opengl_apply_blend(ai, pipe->attachment_blend[ai]);
        }
    } else {
        glColorMask((pipe->color_mask & VIO_COLOR_R) ? GL_TRUE : GL_FALSE,
                    (pipe->color_mask & VIO_COLOR_G) ? GL_TRUE : GL_FALSE,
                    (pipe->color_mask & VIO_COLOR_B) ? GL_TRUE : GL_FALSE,
                    (pipe->color_mask & VIO_COLOR_A) ? GL_TRUE : GL_FALSE);
        opengl_apply_blend(-1, (int)pipe->blend);
    }
}

/* The headless surface, when it is multisampled: draws land in gl_headless.fbo
 * and are resolved into gl_headless.resolve_fbo before anyone reads them. A
 * window gets its samples from GLFW (GLFW_SAMPLES); without this an offscreen
 * run drew aliased edges where the same frame on screen was smooth. */
static struct {
    GLuint fbo;          /* multisampled draw target, 0 when single-sampled */
    GLuint resolve_fbo;  /* single-sampled copy the reads come from */
    GLuint color_rb;
    GLuint depth_rb;
    GLuint resolve_rb;
    int    width;
    int    height;
} gl_headless;

/* Samples the driver will actually give us, at most 8 and a power of two. */
static int opengl_headless_samples(int requested)
{
    if (requested < 2) return 1;
    GLint max_samples = 1;
    glGetIntegerv(GL_MAX_SAMPLES, &max_samples);
    int samples = requested > 8 ? 8 : requested;
    if (samples > (int)max_samples) samples = (int)max_samples;
    return samples > 1 ? samples : 1;
}

/* A multisampled headless surface plus the single-sampled buffer its frames
 * resolve into. Returns the draw FBO, or 0 when the driver will not give us
 * one at any sample count the caller asked for. */
static unsigned int opengl_setup_headless_msaa(int width, int height, int samples)
{
    while (samples > 1) {
        GLuint fbo = 0, color_rb = 0, depth_rb = 0;
        glGenFramebuffers(1, &fbo);
        glBindFramebuffer(GL_FRAMEBUFFER, fbo);

        glGenRenderbuffers(1, &color_rb);
        glBindRenderbuffer(GL_RENDERBUFFER, color_rb);
        glRenderbufferStorageMultisample(GL_RENDERBUFFER, samples, GL_RGBA8, width, height);
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, color_rb);

        glGenRenderbuffers(1, &depth_rb);
        glBindRenderbuffer(GL_RENDERBUFFER, depth_rb);
        glRenderbufferStorageMultisample(GL_RENDERBUFFER, samples, GL_DEPTH24_STENCIL8, width, height);
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, depth_rb);

        GLuint resolve_fbo = 0, resolve_rb = 0;
        if (glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE) {
            glGenFramebuffers(1, &resolve_fbo);
            glBindFramebuffer(GL_FRAMEBUFFER, resolve_fbo);
            glGenRenderbuffers(1, &resolve_rb);
            glBindRenderbuffer(GL_RENDERBUFFER, resolve_rb);
            glRenderbufferStorage(GL_RENDERBUFFER, GL_RGBA8, width, height);
            glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, resolve_rb);

            if (glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE) {
                gl_headless.fbo = fbo;
                gl_headless.resolve_fbo = resolve_fbo;
                gl_headless.color_rb = color_rb;
                gl_headless.depth_rb = depth_rb;
                gl_headless.resolve_rb = resolve_rb;
                gl_headless.width = width;
                gl_headless.height = height;
                /* Leave the draw target bound, as the single-sampled path does. */
                glBindFramebuffer(GL_FRAMEBUFFER, fbo);
                return fbo;
            }
        }

        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        if (resolve_rb) glDeleteRenderbuffers(1, &resolve_rb);
        if (resolve_fbo) glDeleteFramebuffers(1, &resolve_fbo);
        glDeleteRenderbuffers(1, &color_rb);
        glDeleteRenderbuffers(1, &depth_rb);
        glDeleteFramebuffers(1, &fbo);
        /* Some drivers refuse a count they advertise; try the next one down. */
        samples >>= 1;
    }
    return 0;
}

static unsigned int opengl_setup_headless(int width, int height, int samples)
{
    if (!vio_gl.initialized) return 0;

    samples = opengl_headless_samples(samples);
    /* One multisampled headless surface at a time: the resolve pair is kept
     * here, not on the FBO, and a second context would take the first one's
     * buffers with it when it goes. A second surface stays single-sampled. */
    if (samples > 1 && !gl_headless.fbo) {
        unsigned int msaa = opengl_setup_headless_msaa(width, height, samples);
        if (msaa) return msaa;
        /* No multisampled FBO to be had: a single-sampled surface is still a
         * usable one, so fall through rather than fail the context. */
    }

    GLuint fbo = 0;
    glGenFramebuffers(1, &fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);

    GLuint color_rb = 0;
    glGenRenderbuffers(1, &color_rb);
    glBindRenderbuffer(GL_RENDERBUFFER, color_rb);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_RGBA8, width, height);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, color_rb);

    GLuint depth_rb = 0;
    glGenRenderbuffers(1, &depth_rb);
    glBindRenderbuffer(GL_RENDERBUFFER, depth_rb);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, width, height);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, depth_rb);

    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glDeleteRenderbuffers(1, &color_rb);
        glDeleteRenderbuffers(1, &depth_rb);
        glDeleteFramebuffers(1, &fbo);
        return 0;
    }
    /* Keep FBO bound — every subsequent draw goes here until a render-target
     * binds something else. Matches the previous in-line behaviour. */
    return fbo;
}

static void opengl_teardown_headless(unsigned int fbo)
{
    if (!fbo || !vio_gl.initialized) return;

    if (gl_headless.fbo && gl_headless.fbo == (GLuint)fbo) {
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glDeleteRenderbuffers(1, &gl_headless.color_rb);
        glDeleteRenderbuffers(1, &gl_headless.depth_rb);
        glDeleteRenderbuffers(1, &gl_headless.resolve_rb);
        glDeleteFramebuffers(1, &gl_headless.resolve_fbo);
        glDeleteFramebuffers(1, &gl_headless.fbo);
        memset(&gl_headless, 0, sizeof(gl_headless));
        return;
    }

    /* Re-discover the attached renderbuffers via the FBO so we don't have
     * to track them in vio_gl globals (which wouldn't survive a multi-context
     * setup gracefully). */
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    GLint color_rb_int = 0, depth_rb_int = 0;
    glGetFramebufferAttachmentParameteriv(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
        GL_FRAMEBUFFER_ATTACHMENT_OBJECT_NAME, &color_rb_int);
    glGetFramebufferAttachmentParameteriv(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT,
        GL_FRAMEBUFFER_ATTACHMENT_OBJECT_NAME, &depth_rb_int);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);

    if (color_rb_int) {
        GLuint rb = (GLuint)color_rb_int;
        glDeleteRenderbuffers(1, &rb);
    }
    if (depth_rb_int) {
        GLuint rb = (GLuint)depth_rb_int;
        glDeleteRenderbuffers(1, &rb);
    }
    glDeleteFramebuffers(1, &fbo);
}

static int opengl_read_pixels(unsigned int fbo, int width, int height, void *out_rgba)
{
    if (!vio_gl.initialized || width <= 0 || height <= 0) return -1;

    /* A multisampled headless surface cannot be read directly: resolve it
     * into its single-sampled twin and read that. */
    if (fbo && gl_headless.fbo && gl_headless.fbo == (GLuint)fbo) {
        glBindFramebuffer(GL_READ_FRAMEBUFFER, gl_headless.fbo);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, gl_headless.resolve_fbo);
        glBlitFramebuffer(0, 0, gl_headless.width, gl_headless.height,
                          0, 0, gl_headless.width, gl_headless.height,
                          GL_COLOR_BUFFER_BIT, GL_NEAREST);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, gl_headless.fbo);
        fbo = gl_headless.resolve_fbo;
    }

    if (fbo) {
        glBindFramebuffer(GL_READ_FRAMEBUFFER, fbo);
    }
    glReadPixels(0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, out_rgba);

    /* OpenGL returns bottom-up; flip in place so callers always see top-down. */
    int stride = width * 4;
    unsigned char *data = (unsigned char *)out_rgba;
    unsigned char *tmp = (unsigned char *)emalloc(stride);
    for (int y = 0; y < height / 2; y++) {
        unsigned char *top = data + y * stride;
        unsigned char *bot = data + (height - 1 - y) * stride;
        memcpy(tmp, top, stride);
        memcpy(top, bot, stride);
        memcpy(bot, tmp, stride);
    }
    efree(tmp);
    return 0;
}

/* Linear scan over the cached extension list. List is small (typically 200-400
 * entries) and queried a handful of times at context setup; not worth a hash. */
/* vio_feature_info / vio_shader: multiview by instancing without OVR_multiview2. */
static int opengl_multiview_via_instancing(void)
{
    return vio_gl.initialized && !gl_mv_native();
}

static const char *opengl_feature_emulation(vio_feature f)
{
    if (f == VIO_FEATURE_MULTIVIEW && opengl_multiview_via_instancing())
        return "instancing, one instance per view, gl_Layer from the vertex stage";
    return NULL;
}

static int gl_has_ext(const char *name)
{
    for (int i = 0; i < vio_gl.extension_count; i++) {
        if (vio_gl.extensions[i] && strcmp(vio_gl.extensions[i], name) == 0) {
            return 1;
        }
    }
    return 0;
}

static int opengl_supports_feature(vio_feature feature)
{
    switch (feature) {
        case VIO_FEATURE_COMPUTE:        return vio_gl.caps.has_compute_shader;
        case VIO_FEATURE_TESSELLATION:   return vio_gl.caps.has_tessellation;
        case VIO_FEATURE_GEOMETRY:       return gl_ge(3, 2);   /* core in 3.3+ */
        case VIO_FEATURE_GEOMETRY_INSTANCING: return gl_ge(4, 0) || gl_has_ext("GL_ARB_gpu_shader5");
        case VIO_FEATURE_3D_PIPELINE:    return 1;
        case VIO_FEATURE_RAYTRACING:     return 0;             /* not exposed via core GL */
        case VIO_FEATURE_READ_PIXELS:    return 1;
        case VIO_FEATURE_INSTANCED_DRAW: return 1;             /* core since 3.1 */
        case VIO_FEATURE_RENDER_TARGET:        return 1;
        case VIO_FEATURE_RENDER_TARGET_HDR:    return 1;       /* RGBA16F since 3.0 */
        case VIO_FEATURE_RENDER_TARGET_DEPTH:  return 1;
        case VIO_FEATURE_RENDER_TARGET_MSAA:   return 1;
        case VIO_FEATURE_STENCIL:        return 1;             /* DEPTH24_STENCIL8 attachments + glStencil* state */
        case VIO_FEATURE_GPU_TIMESTAMP:  return opengl_has_timer_query(); /* GL_TIMESTAMP queries, core 3.3 */
        case VIO_FEATURE_INDIRECT_DRAW:  return vio_gl.initialized && GLAD_GL_VERSION_4_0; /* glDraw*Indirect */
        case VIO_FEATURE_TEXTURE_ARRAY:  return vio_gl.initialized;                        /* GL_TEXTURE_2D_ARRAY, core 3.0 */
        case VIO_FEATURE_RENDER_TARGET_LAYERED: return vio_gl.initialized;                 /* glFramebufferTextureLayer + depth cubemaps, core 3.0 */
        case VIO_FEATURE_LAYERED_RENDER: return vio_gl.initialized && GLAD_GL_VERSION_3_2;  /* glFramebufferTexture (layered attachment) */
        case VIO_FEATURE_VERTEX_LAYER:   return vio_gl.initialized && gl_has_ext("GL_ARB_shader_viewport_layer_array");
        case VIO_FEATURE_MULTI_VIEWPORT: return vio_gl.initialized && glViewportIndexedf != NULL &&
                                                (gl_ge(4, 1) || gl_has_ext("GL_ARB_viewport_array"));
        case VIO_FEATURE_TEXTURE_COMPRESSION_ASTC: return vio_gl.initialized && opengl_has_texfmt(VIO_FORMAT_ASTC_4x4);
        case VIO_FEATURE_DEPTH_MIPMAPS: return vio_gl.initialized;   /* gl_generate_depth_mips (A26) */
        case VIO_FEATURE_TEXTURE_COMPRESSION_BC:                                          /* S3TC ext (BC1/BC3) + core RGTC; BC7 needs BPTC / 4.2 */
            return vio_gl.initialized && gl_has_ext("GL_EXT_texture_compression_s3tc");
        case VIO_FEATURE_CUBEMAP:        return 1;
        case VIO_FEATURE_DEPTH_BIAS:     return 1;
        case VIO_FEATURE_SCISSOR:        return 1;
        case VIO_FEATURE_TEXTURE_SWIZZLE: return vio_gl.caps.has_texture_swizzle;
        /* GL_KHR_shader_subgroup as the driver reports it. SPIRV-Cross' GLSL
         * backend rejects shuffle, min/max/and/or/xor and quad operations outside
         * Vulkan semantics; such shaders reach the driver as the caller's GLSL
         * text instead (vio_shader / compute fallback, OPEN-ITEMS-PLAN B4/B5). */
        case VIO_FEATURE_SUBGROUP:       return vio_gl.caps.has_subgroup;
        case VIO_FEATURE_SUBGROUP_QUAD:  return vio_gl.caps.has_subgroup_quad;
        case VIO_FEATURE_BARYCENTRICS:   return vio_gl.caps.has_barycentrics;
        case VIO_FEATURE_ATOMIC64:       return vio_gl.caps.has_atomic64;
        case VIO_FEATURE_SHADER_FLOAT16: return vio_gl.caps.has_float16;
        case VIO_FEATURE_BASE_VERTEX:    return vio_gl.caps.has_draw_parameters;
        case VIO_FEATURE_COMPUTE_DERIVATIVES: return vio_gl.caps.has_compute_derivatives;
        /* GL_OVR_multiview2, or views by instancing with gl_Layer from the vertex
         * stage (OPEN-ITEMS-PLAN A10). */
        case VIO_FEATURE_MULTIVIEW:      return vio_gl.caps.has_multiview || vio_gl.caps.has_vertex_layer;
        case VIO_FEATURE_NATIVE_2D_BATCH: return 1;
        case VIO_FEATURE_DEBUG_OUTPUT:   return vio_gl.caps.has_debug_output;
        case VIO_FEATURE_DSA:            return vio_gl.caps.has_dsa;
        case VIO_FEATURE_BUFFER_STORAGE: return vio_gl.caps.has_buffer_storage;
        case VIO_FEATURE_TEXTURE_STORAGE:return vio_gl.caps.has_texture_storage;
        case VIO_FEATURE_SEPARATE_SHADERS: return vio_gl.caps.has_separate_shaders;
        case VIO_FEATURE_TEXTURE_3D:     return 1;  /* glTexImage3D core since GL 1.2 */
        case VIO_FEATURE_RENDER_TARGET_CUBE: return 1; /* glFramebufferTexture2D(CUBE_MAP_POSITIVE_X + face) */
        case VIO_FEATURE_MIPMAP_GEN:     return 1;  /* glGenerateMipmap, core since 3.0 */
        /* SSBO read from the vertex stage needs GL 4.3 (SSBOs are core 4.3);
         * has_compute_shader tracks exactly that tier. GL < 4.3 -> 0, callers
         * stay on the readback path. */
        case VIO_FEATURE_VERTEX_STORAGE: return vio_gl.caps.has_compute_shader;
        case VIO_FEATURE_FRAGMENT_STORAGE: return vio_gl.caps.has_compute_shader;   /* GL 4.3: >= 8 fragment SSBOs */
        case VIO_FEATURE_SAMPLER_FEEDBACK_GLSL: return vio_gl.caps.has_compute_shader;
        case VIO_FEATURE_STORAGE_IMAGE:  return vio_gl.caps.has_compute_shader; /* image load/store is 4.2, compute 4.3 */
        case VIO_FEATURE_MRT:            return 1;  /* glDrawBuffers, core since 3.0 */
        default:                         return 0;
    }
}

static const vio_backend opengl_backend = {
    .name              = "opengl",
    .api_version       = VIO_BACKEND_API_VERSION,
    .init              = opengl_init,
    .shutdown          = opengl_shutdown,
    .create_surface    = opengl_create_surface,
    .destroy_surface   = opengl_destroy_surface,
    .resize            = opengl_resize,
    .create_pipeline   = opengl_create_pipeline,
    .destroy_pipeline  = opengl_destroy_pipeline,
    .bind_pipeline     = opengl_bind_pipeline,
    .create_buffer     = opengl_create_buffer,
    .update_buffer     = opengl_update_buffer,
    .destroy_buffer    = opengl_destroy_buffer,
    .create_texture    = opengl_create_texture,
    .destroy_texture   = opengl_destroy_texture,
    .compile_shader    = opengl_compile_shader,
    .destroy_shader    = opengl_destroy_shader,
    .begin_frame       = opengl_begin_frame,
    .end_frame         = opengl_end_frame,
    .draw              = opengl_draw,
    .draw_indexed      = opengl_draw_indexed,
    .present           = opengl_present,
    .clear             = opengl_clear,
    .gpu_flush         = opengl_gpu_flush,
    .dispatch_compute  = opengl_dispatch_compute,
    .create_compute_pipeline  = opengl_create_compute_pipeline,
    .destroy_compute_pipeline = opengl_destroy_compute_pipeline,
    .compute_bind_buffer      = opengl_compute_bind_buffer,
    .compute_bind_image       = opengl_compute_bind_image,
    .compute_wait             = opengl_compute_wait,
    .compute_set_uniforms     = opengl_compute_set_uniforms,
    .read_buffer              = opengl_read_buffer,
    .bind_storage_buffer          = opengl_bind_storage_buffer,
    .bind_fragment_storage        = opengl_bind_fragment_storage,
    .draw_instanced_from_storage  = opengl_draw_instanced_from_storage,
    .supports_feature  = opengl_supports_feature,
    .feature_emulation = opengl_feature_emulation,
    .multiview_via_instancing = opengl_multiview_via_instancing,
    .gpu_frame_time    = opengl_gpu_frame_time,
    .gpu_mark          = opengl_gpu_mark,
    .gpu_marks         = opengl_gpu_marks,
    .gpu_info          = opengl_gpu_info,
    .describe          = opengl_describe,
    .enumerate_adapters = opengl_enumerate_adapters,
    .draw_indirect     = opengl_draw_indirect,
    .set_viewport      = opengl_set_viewport,
    .set_viewports     = opengl_set_viewports,
    .set_uniform       = opengl_set_uniform,
    .destroy_buffer_obj    = opengl_destroy_buffer_obj,
    .destroy_texture_obj   = opengl_destroy_texture_obj,
    .destroy_shader_obj    = opengl_destroy_shader_obj,
    .destroy_mesh          = opengl_destroy_mesh,
    .destroy_cubemap       = opengl_destroy_cubemap,
    .destroy_font_atlas    = opengl_destroy_font_atlas,
    .destroy_render_target = opengl_destroy_render_target,
    .create_render_target  = opengl_create_render_target,
    .bind_render_target    = opengl_bind_render_target,
    .unbind_render_target  = opengl_unbind_render_target,
    .bind_render_target_face = opengl_bind_render_target_face,
    .render_target_cubemap   = opengl_render_target_cubemap,
    .read_render_target      = opengl_read_render_target,
    .update_texture          = opengl_update_texture,
    .generate_mipmaps        = opengl_generate_mipmaps,
    .read_pixels           = opengl_read_pixels,
    .setup_headless        = opengl_setup_headless,
    .teardown_headless     = opengl_teardown_headless,
    .bind_pipeline_state   = opengl_bind_pipeline_state,
    .create_mesh           = opengl_create_mesh,
    .upload_texture_2d     = opengl_upload_texture_2d,
    .upload_texture_3d     = opengl_upload_texture_3d,
    .bind_texture_3d_id    = opengl_bind_texture_3d_id,
    .bind_texture_array_id = opengl_bind_texture_array_id,
    .upload_texture_ex     = opengl_upload_texture_ex,
    .draw_mesh             = opengl_draw_mesh,
    .draw_mesh_instanced   = opengl_draw_mesh_instanced,
    .create_uniform_buffer = opengl_create_uniform_buffer,
    .update_uniform_buffer = opengl_update_uniform_buffer,
    .bind_uniform_buffer   = opengl_bind_uniform_buffer,
    .bind_texture_id       = opengl_bind_texture_id,
    .bind_cubemap_id       = opengl_bind_cubemap_id,
    .upload_font_atlas     = opengl_upload_font_atlas,
    .update_font_atlas     = opengl_update_font_atlas,
    .flush_draw_state      = opengl_flush_draw_state,
    .upload_cubemap        = opengl_upload_cubemap,
};

void vio_backend_opengl_register(void)
{
    vio_register_backend(&opengl_backend);
}

/* ── OpenGL context setup (called after window creation) ──────────── */

int vio_opengl_get_glsl_version(void)
{
    return vio_gl.glsl_version > 0 ? vio_gl.glsl_version : 330;
}

int vio_opengl_setup_context(void)
{
    /* The GL entry points of the context the platform made current */
    if (!vio_plat()->gl_get_proc_address || !gladLoadGLLoader((GLADloadproc)vio_plat()->gl_get_proc_address)) {
        php_error_docref(NULL, E_WARNING, "Failed to initialize GLAD");
        return -1;
    }

    /* Detect what we actually got. The window system negotiates the highest
     * available core context (4.6 → 3.3 ladder), so the numbers reflect the
     * GPU+driver cap, not what we asked for. */
    glGetIntegerv(GL_MAJOR_VERSION, &vio_gl.gl_major);
    glGetIntegerv(GL_MINOR_VERSION, &vio_gl.gl_minor);
    vio_gl.glsl_version = vio_gl.gl_major * 100 + vio_gl.gl_minor * 10;
    if (vio_gl.glsl_version < 330) {
        /* Shouldn't happen — the ladder floor is 3.3 — but stay safe. */
        vio_gl.glsl_version = 330;
    }

    /* Cache the extension list once. The legacy glGetString(GL_EXTENSIONS)
     * returns NULL on core 3.2+ — use the indexed API. We strdup each name
     * so the cache outlives whatever transient buffer the driver returned. */
    GLint num_ext = 0;
    glGetIntegerv(GL_NUM_EXTENSIONS, &num_ext);
    if (num_ext > 0) {
        vio_gl.extensions = (char **)calloc((size_t)num_ext, sizeof(char *));
        if (vio_gl.extensions) {
            for (GLint i = 0; i < num_ext; i++) {
                const GLubyte *ext = glGetStringi(GL_EXTENSIONS, (GLuint)i);
                if (ext) {
                    size_t len = strlen((const char *)ext);
                    vio_gl.extensions[i] = (char *)malloc(len + 1);
                    if (vio_gl.extensions[i]) {
                        memcpy(vio_gl.extensions[i], ext, len + 1);
                    }
                }
            }
            vio_gl.extension_count = num_ext;
        }
    }

    /* Cache renderer / vendor strings so vio_gl_info() doesn't have to call
     * back into GL from php_vio.c (the audit gate forbids that). */
    const GLubyte *r = glGetString(GL_RENDERER);
    const GLubyte *v = glGetString(GL_VENDOR);
    if (r) {
        size_t rlen = strlen((const char *)r);
        vio_gl.renderer = (char *)malloc(rlen + 1);
        if (vio_gl.renderer) memcpy(vio_gl.renderer, r, rlen + 1);
    }
    if (v) {
        size_t vlen = strlen((const char *)v);
        vio_gl.vendor = (char *)malloc(vlen + 1);
        if (vio_gl.vendor) memcpy(vio_gl.vendor, v, vlen + 1);
    }

    /* Fill the per-feature cache. Each capability is true when either the
     * core version covers it or the matching extension is present. */
    vio_gl.caps.has_compute_shader   = gl_ge(4, 3) || gl_has_ext("GL_ARB_compute_shader");
    vio_gl.caps.has_tessellation     = gl_ge(4, 0) || gl_has_ext("GL_ARB_tessellation_shader");
    vio_gl.caps.has_separate_shaders = gl_ge(4, 1) || gl_has_ext("GL_ARB_separate_shader_objects");
    vio_gl.caps.has_debug_output     = gl_ge(4, 3) || gl_has_ext("GL_KHR_debug");
    vio_gl.caps.has_dsa              = gl_ge(4, 5) || gl_has_ext("GL_ARB_direct_state_access");
    vio_gl.caps.has_buffer_storage   = gl_ge(4, 4) || gl_has_ext("GL_ARB_buffer_storage");
    vio_gl.caps.has_texture_storage  = gl_ge(4, 2) || gl_has_ext("GL_ARB_texture_storage");
    vio_gl.caps.has_texture_swizzle  = gl_ge(3, 3) || gl_has_ext("GL_ARB_texture_swizzle");
    vio_gl.caps.has_subgroup = 0;
    vio_gl.caps.has_subgroup_quad = 0;
    vio_gl.caps.has_barycentrics = gl_has_ext("GL_EXT_fragment_shader_barycentric");
    /* 0 on GL: SPIRV-Cross declares 64-bit atomics as GL_EXT_shader_atomic_int64,
     * which no GL driver exposes, and the one GL extension that has them
     * (GL_NV_shader_atomic_int64, NVIDIA only) lacks the uint64_t overloads of
     * atomicAdd / atomicExchange / atomicCompSwap (driver 2026-10, RTX 2080:
     * only the int64_t ones compile). */
    vio_gl.caps.has_atomic64 = 0;
    vio_gl.caps.has_float16 = gl_has_ext("GL_AMD_gpu_shader_half_float") || gl_has_ext("GL_NV_gpu_shader5");
    vio_gl.caps.has_draw_parameters = (gl_ge(4, 6) || gl_has_ext("GL_ARB_shader_draw_parameters"))
                                   && (gl_ge(4, 2) || gl_has_ext("GL_ARB_base_instance"));
    vio_gl.caps.has_compute_derivatives = vio_gl.caps.has_compute_shader && gl_has_ext("GL_NV_compute_shader_derivatives");
    vio_gl.caps.has_multiview = gl_ge(3, 2) && gl_has_ext("GL_OVR_multiview2") && glFramebufferTextureMultiviewOVR != NULL;
    vio_gl.caps.has_vertex_layer = gl_ge(3, 2) && gl_has_ext("GL_ARB_shader_viewport_layer_array");
    {
        /* Test knob: run multiview by instancing on a driver with OVR_multiview2. */
#ifdef _WIN32
        char ev[8];
        DWORD n = GetEnvironmentVariableA("VIO_GL_EMULATE_MULTIVIEW", ev, sizeof(ev));
        vio_gl.caps.multiview_emulate = n > 0 && n < sizeof(ev) && ev[0] == '1';
#else
        const char *ev = getenv("VIO_GL_EMULATE_MULTIVIEW");
        vio_gl.caps.multiview_emulate = ev && ev[0] == '1';
#endif
    }
    if (gl_has_ext("GL_KHR_shader_subgroup")) {
        GLint stages = 0, features = 0;
        glGetIntegerv(GL_SUBGROUP_SUPPORTED_STAGES_KHR, &stages);
        glGetIntegerv(GL_SUBGROUP_SUPPORTED_FEATURES_KHR, &features);
        vio_gl.caps.has_subgroup_quad = (stages & GL_FRAGMENT_SHADER_BIT) && (features & GL_SUBGROUP_FEATURE_QUAD_BIT_KHR);
    }
    if (vio_gl.caps.has_compute_shader && gl_has_ext("GL_KHR_shader_subgroup")) {
        /* The extension reports which stages and operation groups it covers. */
        GLint stages = 0, features = 0, size = 0;
        glGetIntegerv(GL_SUBGROUP_SUPPORTED_STAGES_KHR, &stages);
        glGetIntegerv(GL_SUBGROUP_SUPPORTED_FEATURES_KHR, &features);
        glGetIntegerv(GL_SUBGROUP_SIZE_KHR, &size);
        const GLint need_stages = GL_COMPUTE_SHADER_BIT | GL_FRAGMENT_SHADER_BIT;
        const GLint need_ops = GL_SUBGROUP_FEATURE_BASIC_BIT_KHR | GL_SUBGROUP_FEATURE_VOTE_BIT_KHR
            | GL_SUBGROUP_FEATURE_ARITHMETIC_BIT_KHR | GL_SUBGROUP_FEATURE_BALLOT_BIT_KHR
            | GL_SUBGROUP_FEATURE_SHUFFLE_BIT_KHR;
        vio_gl.caps.has_subgroup = (stages & need_stages) == need_stages && (features & need_ops) == need_ops && size > 1;
        vio_gl.caps.subgroup_stages = stages;
        vio_gl.caps.subgroup_features = features;
        vio_gl.caps.subgroup_size = size;
    }

    glEnable(GL_DEPTH_TEST);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    /* Compile default shaders */
    vio_gl.default_shader_program = vio_opengl_compile_shader_source(
        vio_default_vertex_shader, vio_default_fragment_shader);
    vio_gl.default_shader_pos_only = vio_opengl_compile_shader_source(
        vio_default_vertex_shader_pos_only, vio_default_fragment_shader);

    if (!vio_gl.default_shader_program || !vio_gl.default_shader_pos_only) {
        php_error_docref(NULL, E_WARNING, "Failed to compile default shaders");
        return -1;
    }

    gl_context_generation++;
    vio_gl.initialized = 1;
    return 0;
}

#endif /* HAVE_OPENGL */

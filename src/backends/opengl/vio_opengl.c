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

#ifdef HAVE_GLFW

#include <glad/glad.h>
#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>

#include "vio_opengl.h"
#include "../../shaders/default_shaders.h"
#include "../../vio_window.h"
#include "../../vio_mesh.h"
#include "../../vio_render_target.h"
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

typedef struct {
    GLuint program;   /* 0 = empty slot */
    GLint  loc;
    char   name[GL_ULOC_NAME_MAX];
} gl_uloc_entry;

static gl_uloc_entry gl_uloc_cache[GL_ULOC_CACHE_SIZE];

static unsigned gl_uloc_hash(GLuint program, const char *name)
{
    unsigned h = 5381u ^ (program * 2654435761u);
    for (; *name; name++) h = h * 33u + (unsigned char)*name;
    return h;
}

static void gl_uloc_forget_program(GLuint program)
{
    if (!program) return;
    for (int i = 0; i < GL_ULOC_CACHE_SIZE; i++) {
        if (gl_uloc_cache[i].program == program) gl_uloc_cache[i].program = 0;
    }
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

static GLint gl_uloc_suffix_scan(GLuint program, const char *name, size_t len)
{
    char norm[256];
    size_t nlen = gl_uloc_normalize(name, norm, sizeof(norm));
    GLint count = 0;
    glGetProgramiv(program, GL_ACTIVE_UNIFORMS, &count);
    char buf[256], full[512];
    for (GLint i = 0; i < count; i++) {
        GLsizei n = 0; GLint size = 0; GLenum type = 0;
        glGetActiveUniform(program, (GLuint)i, (GLsizei)sizeof(buf), &n, &size, &type, buf);
        /* active name = "<prefix>.<norm>" ? */
        if (n <= (GLsizei)nlen + 1 || buf[n - nlen - 1] != '.' || strcmp(buf + n - nlen, norm) != 0) continue;
        size_t plen = (size_t)n - nlen - 1;
        if (plen + 1 + len + 1 > sizeof(full)) return -1;
        memcpy(full, buf, plen);
        full[plen] = '.';
        memcpy(full + plen + 1, name, len + 1);
        return glGetUniformLocation(program, full);   /* exact element (or element 0 for a bare array name) */
    }
    return -1;
}
static GLint gl_uniform_location(GLuint program, const char *name)
{
    size_t len = strlen(name);
    int cacheable = len < GL_ULOC_NAME_MAX;
    unsigned idx = gl_uloc_hash(program, name) % GL_ULOC_CACHE_SIZE;
    gl_uloc_entry *slot = NULL;
    if (cacheable) {
        for (int probe = 0; probe < 8; probe++) {
            gl_uloc_entry *e = &gl_uloc_cache[(idx + probe) % GL_ULOC_CACHE_SIZE];
            if (e->program == program && strcmp(e->name, name) == 0) return e->loc;
            if (e->program == 0 && !slot) slot = e;
        }
        if (!slot) slot = &gl_uloc_cache[idx];   /* window full: evict */
    }

    GLint loc = glGetUniformLocation(program, name);
    if (loc < 0) loc = gl_uloc_suffix_scan(program, name, len);

    if (slot) {
        slot->program = program;
        slot->loc = loc;
        memcpy(slot->name, name, len + 1);
    }
    return loc;
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

unsigned int vio_opengl_compile_shader_source(const char *vert_src, const char *frag_src)
{
    unsigned int vert = compile_shader_stage(vert_src, GL_VERTEX_SHADER);
    if (!vert) return 0;

    unsigned int frag = compile_shader_stage(frag_src, GL_FRAGMENT_SHADER);
    if (!frag) {
        glDeleteShader(vert);
        return 0;
    }

    unsigned int program = glCreateProgram();
    glAttachShader(program, vert);
    glAttachShader(program, frag);
    glLinkProgram(program);

    int success;
    glGetProgramiv(program, GL_LINK_STATUS, &success);
    if (!success) {
        char log[512];
        glGetProgramInfoLog(program, sizeof(log), NULL, log);
        php_error_docref(NULL, E_WARNING, "OpenGL shader link failed: %s", log);
        glDeleteProgram(program);
        program = 0;
    }

    glDeleteShader(vert);
    glDeleteShader(frag);
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
    return 0;
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

static void *opengl_create_pipeline(vio_pipeline_desc *desc)
{
    (void)desc;
    return NULL; /* TODO: Phase 4 */
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

static void opengl_update_buffer(void *buffer, const void *data, size_t size)
{
    (void)buffer;
    (void)data;
    (void)size;
}

static void opengl_destroy_buffer(void *buffer)
{
    (void)buffer;
}

static void *opengl_create_texture(vio_texture_desc *desc)
{
    (void)desc;
    return NULL; /* TODO: Phase 4 */
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

static void opengl_begin_frame(void)
{
    glClearColor(vio_gl.clear_r, vio_gl.clear_g, vio_gl.clear_b, vio_gl.clear_a);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
    vio_gl.in_frame = 1;
}

static void opengl_end_frame(void)
{
    /* Flush any pending GL commands */
    glFlush();
    vio_gl.in_frame = 0;
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
    glViewport((GLint)x, (GLint)y, (GLsizei)width, (GLsizei)height);
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
    if (rt->gl_msaa_depth_rb) { if (live) glDeleteRenderbuffers(1, &rt->gl_msaa_depth_rb); rt->gl_msaa_depth_rb = 0; }
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

static int opengl_create_render_target(void *rt_ptr, int width, int height, int hdr, int depth_only)
{
    vio_render_target_object *rt = (vio_render_target_object *)rt_ptr;
    if (!vio_gl.initialized) return -1;

    glGenFramebuffers(1, &rt->fbo);
    rt->gl_generation = gl_context_generation;
    glBindFramebuffer(GL_FRAMEBUFFER, rt->fbo);

    if (rt->is_cube) {
        /* Cubemap colour attachment (+X face bound initially) with a shared 2D
         * depth buffer at face size. Mip storage is allocated up front so
         * bind_render_target_face can target level > 0 before any
         * glGenerateMipmap. */
        if (rt->mip_levels < 1) rt->mip_levels = 1;
        GLint cube_internal; GLenum cube_base, cube_type;
        opengl_color_format(rt->attachment_count > 0 ? rt->formats[0] : (hdr ? VIO_FORMAT_RGBA16F : VIO_FORMAT_RGBA8),
                            &cube_internal, &cube_base, &cube_type);
        glGenTextures(1, &rt->color_texture);
        rt->color_textures[0] = rt->color_texture;
        glBindTexture(GL_TEXTURE_CUBE_MAP, rt->color_texture);
        for (int level = 0; level < rt->mip_levels; level++) {
            int dim = width >> level; if (dim < 1) dim = 1;
            for (int f = 0; f < 6; f++) {
                glTexImage2D(GL_TEXTURE_CUBE_MAP_POSITIVE_X + f, level, cube_internal,
                             dim, dim, 0, cube_base, cube_type, NULL);
            }
        }
        glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MIN_FILTER,
                        rt->mip_levels > 1 ? GL_LINEAR_MIPMAP_LINEAR : GL_LINEAR);
        glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_WRAP_R, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MAX_LEVEL, rt->mip_levels - 1);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                               GL_TEXTURE_CUBE_MAP_POSITIVE_X, rt->color_texture, 0);

        glGenTextures(1, &rt->depth_texture);
        glBindTexture(GL_TEXTURE_2D, rt->depth_texture);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH24_STENCIL8, width, width,
                     0, GL_DEPTH_STENCIL, GL_UNSIGNED_INT_24_8, NULL);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_TEXTURE_2D, rt->depth_texture, 0);

        GLenum cube_status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
        if (cube_status == GL_FRAMEBUFFER_COMPLETE) {
            /* Defined initial contents: every face cleared, depth at 1.0. */
            glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
            glDepthMask(GL_TRUE);
            for (int f = 0; f < 6; f++) {
                glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                                       GL_TEXTURE_CUBE_MAP_POSITIVE_X + f, rt->color_texture, 0);
                glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
            }
            glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                                   GL_TEXTURE_CUBE_MAP_POSITIVE_X, rt->color_texture, 0);
        }
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glBindTexture(GL_TEXTURE_CUBE_MAP, 0);
        glBindTexture(GL_TEXTURE_2D, 0);
        if (cube_status != GL_FRAMEBUFFER_COMPLETE) {
            php_error_docref(NULL, E_WARNING,
                "Cube render target FBO is not complete (status: 0x%04x)", cube_status);
            return -1;
        }
        rt->backend_type = VIO_RT_BACKEND_OPENGL;
        return 0;
    }

    /* Depth texture (always created — shadow-map use-case needs it as SRV).
     * DEPTH24_STENCIL8 so the stencil test (VIO_FEATURE_STENCIL) has its 8 bits;
     * sampling through sampler2DShadow / glReadPixels(GL_DEPTH_COMPONENT) still
     * reads the depth plane (GL_DEPTH_STENCIL_TEXTURE_MODE defaults to depth). */
    glGenTextures(1, &rt->depth_texture);
    glBindTexture(GL_TEXTURE_2D, rt->depth_texture);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH24_STENCIL8, width, height,
        0, GL_DEPTH_STENCIL, GL_UNSIGNED_INT_24_8, NULL);
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

    /* MSAA (single colour attachment only): a second FBO with multisample
     * renderbuffers is what gets drawn into; unbind / readback resolve it into
     * the texture FBO above with glBlitFramebuffer. Before GAP-PLAN Phase 3
     * rt->samples was ignored here while VIO_FEATURE_RENDER_TARGET_MSAA
     * reported 1. */
    rt->samples = rt->samples > 1 ? rt->samples : 1;
    if (rt->samples > 1 && !depth_only && (rt->attachment_count <= 1)) {
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
            glGenRenderbuffers(1, &rt->gl_msaa_depth_rb);
            glBindRenderbuffer(GL_RENDERBUFFER, rt->gl_msaa_color_rb);
            glRenderbufferStorageMultisample(GL_RENDERBUFFER, samples, internal, width, height);
            glBindRenderbuffer(GL_RENDERBUFFER, rt->gl_msaa_depth_rb);
            glRenderbufferStorageMultisample(GL_RENDERBUFFER, samples, GL_DEPTH24_STENCIL8, width, height);
            glBindRenderbuffer(GL_RENDERBUFFER, 0);
            glBindFramebuffer(GL_FRAMEBUFFER, rt->gl_msaa_fbo);
            glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, rt->gl_msaa_color_rb);
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
            rt->gl_msaa_fbo = rt->gl_msaa_color_rb = rt->gl_msaa_depth_rb = 0;
            samples >>= 1;
        }
        rt->samples = rt->gl_msaa_fbo ? samples : 1;
    } else {
        rt->samples = 1;
    }

    rt->backend_type = VIO_RT_BACKEND_OPENGL;
    return 0;
}

/* Resolve a multisampled target into its texture FBO (no-op otherwise). */
static void opengl_rt_resolve_msaa(vio_render_target_object *rt)
{
    if (!rt || !rt->gl_msaa_fbo || !rt->gl_msaa_dirty) return;
    GLint prev_read = 0, prev_draw = 0;
    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &prev_read);
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &prev_draw);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, rt->gl_msaa_fbo);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, rt->fbo);
    glBlitFramebuffer(0, 0, rt->width, rt->height, 0, 0, rt->width, rt->height,
                      GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT, GL_NEAREST);
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
        glBindFramebuffer(GL_FRAMEBUFFER, rt->gl_msaa_fbo);
        rt->gl_msaa_dirty = 1;
        glViewport(0, 0, rt->width, rt->height);
        return;
    }
    glBindFramebuffer(GL_FRAMEBUFFER, rt->fbo);
    if (rt->is_cube) {
        /* Plain bind of a cube RT targets +X at level 0. */
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                               GL_TEXTURE_CUBE_MAP_POSITIVE_X, rt->color_texture, 0);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, rt->depth_texture, 0);
        rt->bound_face = 0;
        rt->bound_level = 0;
    }
    glViewport(0, 0, rt->width, rt->height);
}

static int opengl_bind_render_target_face(void *rt_ptr, int face, int level)
{
    vio_render_target_object *rt = (vio_render_target_object *)rt_ptr;
    if (!rt || rt->backend_type != VIO_RT_BACKEND_OPENGL || !vio_gl.initialized) return -1;
    if (!rt->is_cube || face < 0 || face > 5 || level < 0 || level >= rt->mip_levels) return -1;
    glBindFramebuffer(GL_FRAMEBUFFER, rt->fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                           GL_TEXTURE_CUBE_MAP_POSITIVE_X + face, rt->color_texture, level);
    /* The shared depth buffer matches level 0 only; detach it for smaller levels
     * (GL 3+ allows the mismatch but we mirror the Metal contract). */
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D,
                           level == 0 ? rt->depth_texture : 0, 0);
    int dim = rt->width >> level; if (dim < 1) dim = 1;
    glViewport(0, 0, dim, dim);
    rt->bound_face = face;
    rt->bound_level = level;
    return 0;
}

static int opengl_render_target_cubemap(void *rt_ptr, void *cm_obj)
{
    vio_render_target_object *rt = (vio_render_target_object *)rt_ptr;
    vio_cubemap_object *cm = (vio_cubemap_object *)cm_obj;
    if (!rt || !cm || !rt->is_cube || !rt->color_texture) return -1;
    cm->texture_id   = rt->color_texture;   /* borrowed — RT owns the GL name */
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
    if (rt->is_cube) {
        int f = face >= 0 ? face : (rt->bound_face >= 0 ? rt->bound_face : 0);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                               GL_TEXTURE_CUBE_MAP_POSITIVE_X + f, rt->color_texture, 0);
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

    if (rt->is_cube) {
        /* Restore the attachment the RT had bound before the read. */
        int bf = rt->bound_face >= 0 ? rt->bound_face : 0;
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                               GL_TEXTURE_CUBE_MAP_POSITIVE_X + bf, rt->color_texture, rt->bound_level);
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

static int opengl_generate_mipmaps(void *obj, int kind)
{
    if (!obj || !vio_gl.initialized) return -1;
    GLenum target = GL_TEXTURE_2D;
    GLuint id = 0;
    switch (kind) {
        case 0: {
            vio_render_target_object *rt = (vio_render_target_object *)obj;
            if (rt->backend_type != VIO_RT_BACKEND_OPENGL || !rt->color_texture) return -1;
            target = rt->is_cube ? GL_TEXTURE_CUBE_MAP : GL_TEXTURE_2D;
            id = rt->color_texture;
            break;
        }
        case 1: {
            vio_texture_object *t = (vio_texture_object *)obj;
            if (!t->texture_id) return -1;
            target = t->is_3d ? GL_TEXTURE_3D : GL_TEXTURE_2D;
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

    GLint loc = gl_uniform_location((GLuint)program, name);
    if (loc < 0) return;  /* silently drop unknown uniforms — matches old behavior */

    switch (type) {
        case VIO_UNIFORM_INT:
            glUniform1i(loc, *(const GLint *)data);
            break;
        case VIO_UNIFORM_FLOAT:
            glUniform1f(loc, *(const GLfloat *)data);
            break;
        case VIO_UNIFORM_VEC2:
            glUniform2fv(loc, count, (const GLfloat *)data);
            break;
        case VIO_UNIFORM_VEC3:
            glUniform3fv(loc, count, (const GLfloat *)data);
            break;
        case VIO_UNIFORM_VEC4:
            glUniform4fv(loc, count, (const GLfloat *)data);
            break;
        case VIO_UNIFORM_MAT3:
            glUniformMatrix3fv(loc, count, GL_FALSE, (const GLfloat *)data);
            break;
        case VIO_UNIFORM_MAT4:
            glUniformMatrix4fv(loc, count, GL_FALSE, (const GLfloat *)data);
            break;
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

    glBindVertexArray(mesh->vao);
    if (mesh->index_count > 0) {
        glDrawElements(GL_TRIANGLES, mesh->index_count, GL_UNSIGNED_INT, 0);
    } else {
        glDrawArrays(GL_TRIANGLES, 0, mesh->vertex_count);
    }
    glBindVertexArray(0);

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
     * 3..6 (one vec4 per column). Divisor=1 advances them per instance. */
    glBindVertexArray(mesh->vao);
    for (int col = 0; col < 4; col++) {
        GLuint loc = (GLuint)(3 + col);
        glEnableVertexAttribArray(loc);
        glVertexAttribPointer(loc, 4, GL_FLOAT, GL_FALSE,
                              sizeof(float) * 16,
                              (void *)(uintptr_t)(sizeof(float) * 4 * col));
        glVertexAttribDivisor(loc, 1);
    }

    if (mesh->index_count > 0) {
        glDrawElementsInstanced(GL_TRIANGLES, mesh->index_count,
                                GL_UNSIGNED_INT, 0, (GLsizei)instance_count);
    } else {
        glDrawArraysInstanced(GL_TRIANGLES, 0, mesh->vertex_count,
                              (GLsizei)instance_count);
    }

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

    glBindVertexArray(mesh->vao);
    if (mesh->index_count > 0) {
        glDrawElementsInstanced(GL_TRIANGLES, mesh->index_count,
                                GL_UNSIGNED_INT, 0, (GLsizei)instance_count);
    } else {
        glDrawArraysInstanced(GL_TRIANGLES, 0, mesh->vertex_count,
                              (GLsizei)instance_count);
    }
    glBindVertexArray(0);
}

static int gl_has_ext(const char *name);   /* defined with the caps setup below */

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
                              const unsigned int *indices, int index_count)
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
                     sizeof(unsigned int) * (size_t)index_count,
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

static unsigned int opengl_setup_headless(int width, int height)
{
    if (!vio_gl.initialized) return 0;

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
        case VIO_FEATURE_3D_PIPELINE:    return 1;
        case VIO_FEATURE_RAYTRACING:
        case VIO_FEATURE_MULTIVIEW:      return 0;             /* not exposed via core GL */
        case VIO_FEATURE_READ_PIXELS:    return 1;
        case VIO_FEATURE_INSTANCED_DRAW: return 1;             /* core since 3.1 */
        case VIO_FEATURE_RENDER_TARGET:        return 1;
        case VIO_FEATURE_RENDER_TARGET_HDR:    return 1;       /* RGBA16F since 3.0 */
        case VIO_FEATURE_RENDER_TARGET_DEPTH:  return 1;
        case VIO_FEATURE_RENDER_TARGET_MSAA:   return 1;
        case VIO_FEATURE_STENCIL:        return 1;             /* DEPTH24_STENCIL8 attachments + glStencil* state */
        case VIO_FEATURE_CUBEMAP:        return 1;
        case VIO_FEATURE_DEPTH_BIAS:     return 1;
        case VIO_FEATURE_SCISSOR:        return 1;
        case VIO_FEATURE_TEXTURE_SWIZZLE: return vio_gl.caps.has_texture_swizzle;
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
    .draw_instanced_from_storage  = opengl_draw_instanced_from_storage,
    .supports_feature  = opengl_supports_feature,
    .set_viewport      = opengl_set_viewport,
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
    .draw_mesh             = opengl_draw_mesh,
    .draw_mesh_instanced   = opengl_draw_mesh_instanced,
    .create_uniform_buffer = opengl_create_uniform_buffer,
    .update_uniform_buffer = opengl_update_uniform_buffer,
    .bind_uniform_buffer   = opengl_bind_uniform_buffer,
    .bind_texture_id       = opengl_bind_texture_id,
    .bind_cubemap_id       = opengl_bind_cubemap_id,
    .upload_font_atlas     = opengl_upload_font_atlas,
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
    if (!gladLoadGLLoader((GLADloadproc)glfwGetProcAddress)) {
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

#endif /* HAVE_GLFW */

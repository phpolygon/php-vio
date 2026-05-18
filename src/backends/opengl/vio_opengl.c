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

vio_opengl_state vio_gl = {0};

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

static void *opengl_create_buffer(vio_buffer_desc *desc)
{
    (void)desc;
    return NULL; /* Handled by mesh system */
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
        glDeleteProgram(program);
    }
}

static void opengl_begin_frame(void)
{
    glClearColor(vio_gl.clear_r, vio_gl.clear_g, vio_gl.clear_b, vio_gl.clear_a);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
}

static void opengl_end_frame(void)
{
    /* Flush any pending GL commands */
    glFlush();
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
}

static void opengl_dispatch_compute(vio_compute_cmd *cmd)
{
    (void)cmd;
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
        glDeleteBuffers(1, &buf->buffer_id);
        buf->buffer_id = 0;
    }
}

static void opengl_destroy_texture_obj(void *tex_obj)
{
    vio_texture_object *tex = (vio_texture_object *)tex_obj;
    /* `borrowed` textures (e.g. render-target color/depth alias) are owned
     * by another object and must not be deleted here. */
    if (tex->texture_id && !tex->borrowed) {
        glDeleteTextures(1, &tex->texture_id);
        tex->texture_id = 0;
    }
}

static void opengl_destroy_shader_obj(void *shader_obj)
{
    vio_shader_object *sh = (vio_shader_object *)shader_obj;
    if (sh->program) {
        glDeleteProgram(sh->program);
        sh->program = 0;
    }
}

static void opengl_destroy_mesh(void *mesh_ptr)
{
    vio_mesh_object *mesh = (vio_mesh_object *)mesh_ptr;
    if (mesh->ebo) { glDeleteBuffers(1, &mesh->ebo); mesh->ebo = 0; }
    if (mesh->vbo) { glDeleteBuffers(1, &mesh->vbo); mesh->vbo = 0; }
    if (mesh->vao) { glDeleteVertexArrays(1, &mesh->vao); mesh->vao = 0; }
}

static void opengl_destroy_cubemap(void *cm_ptr)
{
    vio_cubemap_object *cm = (vio_cubemap_object *)cm_ptr;
    if (cm->texture_id) {
        glDeleteTextures(1, &cm->texture_id);
        cm->texture_id = 0;
    }
}

static void opengl_destroy_font_atlas(void *font_ptr)
{
    vio_font_object *font = (vio_font_object *)font_ptr;
    if (font->atlas_texture && glDeleteTextures) {
        glDeleteTextures(1, &font->atlas_texture);
        font->atlas_texture = 0;
    }
}

static void opengl_destroy_render_target(void *rt_ptr)
{
    vio_render_target_object *rt = (vio_render_target_object *)rt_ptr;
    /* Only act on RTs created by OpenGL — guarding by backend_type avoids
     * mangling a struct that happens to share the slot with a D3D RT in
     * cross-backend tests. */
    if (rt->backend_type != VIO_RT_BACKEND_OPENGL) return;
    if (rt->fbo) {
        glDeleteFramebuffers(1, &rt->fbo);
        rt->fbo = 0;
    }
    if (rt->color_texture) {
        glDeleteTextures(1, &rt->color_texture);
        rt->color_texture = 0;
    }
    if (rt->depth_texture) {
        glDeleteTextures(1, &rt->depth_texture);
        rt->depth_texture = 0;
    }
}

static int opengl_create_render_target(void *rt_ptr, int width, int height, int hdr, int depth_only)
{
    vio_render_target_object *rt = (vio_render_target_object *)rt_ptr;
    if (!vio_gl.initialized) return -1;

    glGenFramebuffers(1, &rt->fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, rt->fbo);

    /* Depth texture (always created — shadow-map use-case needs it as SRV) */
    glGenTextures(1, &rt->depth_texture);
    glBindTexture(GL_TEXTURE_2D, rt->depth_texture);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT24, width, height,
        0, GL_DEPTH_COMPONENT, GL_FLOAT, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_BORDER);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_BORDER);
    float border_color[] = {1.0f, 1.0f, 1.0f, 1.0f};
    glTexParameterfv(GL_TEXTURE_2D, GL_TEXTURE_BORDER_COLOR, border_color);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D,
        rt->depth_texture, 0);

    if (depth_only) {
        glDrawBuffer(GL_NONE);
        glReadBuffer(GL_NONE);
    } else {
        glGenTextures(1, &rt->color_texture);
        glBindTexture(GL_TEXTURE_2D, rt->color_texture);
        glTexImage2D(GL_TEXTURE_2D, 0, hdr ? GL_RGBA16F : GL_RGBA8, width, height,
            0, GL_RGBA, hdr ? GL_FLOAT : GL_UNSIGNED_BYTE, NULL);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
            rt->color_texture, 0);
    }

    GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glBindTexture(GL_TEXTURE_2D, 0);

    if (status != GL_FRAMEBUFFER_COMPLETE) {
        php_error_docref(NULL, E_WARNING,
            "Render target FBO is not complete (status: 0x%04x)", status);
        return -1;
    }

    rt->backend_type = VIO_RT_BACKEND_OPENGL;
    return 0;
}

static void opengl_bind_render_target(void *rt_ptr)
{
    vio_render_target_object *rt = (vio_render_target_object *)rt_ptr;
    if (rt->backend_type != VIO_RT_BACKEND_OPENGL || !vio_gl.initialized) return;
    glBindFramebuffer(GL_FRAMEBUFFER, rt->fbo);
    glViewport(0, 0, rt->width, rt->height);
}

static void opengl_unbind_render_target(unsigned int default_fbo, int width, int height)
{
    if (!vio_gl.initialized) return;
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

    GLint loc = glGetUniformLocation((GLuint)program, name);
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
    glBindTexture(GL_TEXTURE_CUBE_MAP, cm->texture_id);

    for (int i = 0; i < 6; i++) {
        glTexImage2D(GL_TEXTURE_CUBE_MAP_POSITIVE_X + i, 0, GL_RGBA,
                     width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, face_rgba[i]);
    }

    glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
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

static int opengl_upload_texture_2d(void *tex_obj,
                                    const void *pixels, int width, int height, int channels,
                                    int filter, int wrap, int mipmaps)
{
    (void)channels;  /* always uploaded as RGBA — stbi already expanded */
    vio_texture_object *tex = (vio_texture_object *)tex_obj;
    if (!vio_gl.initialized) return -1;

    glGenTextures(1, &tex->texture_id);
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

    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels);

    if (mipmaps) {
        glGenerateMipmap(GL_TEXTURE_2D);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER,
            (filter == VIO_FILTER_NEAREST) ? GL_NEAREST_MIPMAP_NEAREST
                                           : GL_LINEAR_MIPMAP_LINEAR);
    }

    glBindTexture(GL_TEXTURE_2D, 0);
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

    if (pipe->depth_bias != 0.0f || pipe->slope_scaled_depth_bias != 0.0f) {
        glEnable(GL_POLYGON_OFFSET_FILL);
        glPolygonOffset(pipe->slope_scaled_depth_bias, pipe->depth_bias);
    } else {
        glDisable(GL_POLYGON_OFFSET_FILL);
    }

    switch (pipe->blend) {
        case VIO_BLEND_NONE:
            glDisable(GL_BLEND);
            break;
        case VIO_BLEND_ALPHA:
            glEnable(GL_BLEND);
            glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
            break;
        case VIO_BLEND_ADDITIVE:
            glEnable(GL_BLEND);
            glBlendFunc(GL_SRC_ALPHA, GL_ONE);
            break;
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
    .read_pixels           = opengl_read_pixels,
    .setup_headless        = opengl_setup_headless,
    .teardown_headless     = opengl_teardown_headless,
    .bind_pipeline_state   = opengl_bind_pipeline_state,
    .create_mesh           = opengl_create_mesh,
    .upload_texture_2d     = opengl_upload_texture_2d,
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

    vio_gl.initialized = 1;
    return 0;
}

#endif /* HAVE_GLFW */

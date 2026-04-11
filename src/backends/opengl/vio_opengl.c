/*
 * php-vio - OpenGL 4.1 Core Backend implementation
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

static int opengl_supports_feature(vio_feature feature)
{
    switch (feature) {
        case VIO_FEATURE_COMPUTE:
            return 0; /* GL 4.1 doesn't have compute shaders (4.3+) */
        case VIO_FEATURE_TESSELLATION:
            return 1; /* GL 4.0+ */
        case VIO_FEATURE_GEOMETRY:
            return 1; /* GL 3.2+ */
        default:
            return 0;
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
    .dispatch_compute  = opengl_dispatch_compute,
    .supports_feature  = opengl_supports_feature,
};

void vio_backend_opengl_register(void)
{
    vio_register_backend(&opengl_backend);
}

/* ── OpenGL context setup (called after window creation) ──────────── */

int vio_opengl_setup_context(void)
{
    if (!gladLoadGLLoader((GLADloadproc)glfwGetProcAddress)) {
        php_error_docref(NULL, E_WARNING, "Failed to initialize GLAD");
        return -1;
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

    vio_gl.initialized = 1;
    return 0;
}

#endif /* HAVE_GLFW */

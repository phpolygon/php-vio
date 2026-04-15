/*
 * php-vio - Null backend implementation
 * All operations are no-ops. Used for testing and headless mode.
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "vio_backend_null.h"
#include <string.h>

static int null_init(vio_config *cfg)
{
    (void)cfg;
    return 0; /* success */
}

static void null_shutdown(void)
{
}

static void *null_create_surface(vio_config *cfg)
{
    (void)cfg;
    return NULL;
}

static void null_destroy_surface(void *surface)
{
    (void)surface;
}

static void null_resize(int width, int height)
{
    (void)width;
    (void)height;
}

static void *null_create_pipeline(vio_pipeline_desc *desc)
{
    (void)desc;
    return NULL;
}

static void null_destroy_pipeline(void *pipeline)
{
    (void)pipeline;
}

static void null_bind_pipeline(void *pipeline)
{
    (void)pipeline;
}

static void *null_create_buffer(vio_buffer_desc *desc)
{
    (void)desc;
    return NULL;
}

static void null_update_buffer(void *buffer, const void *data, size_t size)
{
    (void)buffer;
    (void)data;
    (void)size;
}

static void null_destroy_buffer(void *buffer)
{
    (void)buffer;
}

static void *null_create_texture(vio_texture_desc *desc)
{
    (void)desc;
    return NULL;
}

static void null_destroy_texture(void *texture)
{
    (void)texture;
}

static void *null_compile_shader(vio_shader_desc *desc)
{
    (void)desc;
    return NULL;
}

static void null_destroy_shader(void *shader)
{
    (void)shader;
}

static void null_begin_frame(void)
{
}

static void null_end_frame(void)
{
}

static void null_draw(vio_draw_cmd *cmd)
{
    (void)cmd;
}

static void null_draw_indexed(vio_draw_indexed_cmd *cmd)
{
    (void)cmd;
}

static void null_present(void)
{
}

static void null_clear(float r, float g, float b, float a)
{
    (void)r;
    (void)g;
    (void)b;
    (void)a;
}

static void null_dispatch_compute(vio_compute_cmd *cmd)
{
    (void)cmd;
}

static int null_supports_feature(vio_feature feature)
{
    (void)feature;
    return 0;
}

static const vio_backend null_backend = {
    .name              = "null",
    .api_version       = VIO_BACKEND_API_VERSION,
    .init              = null_init,
    .shutdown          = null_shutdown,
    .create_surface    = null_create_surface,
    .destroy_surface   = null_destroy_surface,
    .resize            = null_resize,
    .create_pipeline   = null_create_pipeline,
    .destroy_pipeline  = null_destroy_pipeline,
    .bind_pipeline     = null_bind_pipeline,
    .create_buffer     = null_create_buffer,
    .update_buffer     = null_update_buffer,
    .destroy_buffer    = null_destroy_buffer,
    .create_texture    = null_create_texture,
    .destroy_texture   = null_destroy_texture,
    .compile_shader    = null_compile_shader,
    .destroy_shader    = null_destroy_shader,
    .begin_frame       = null_begin_frame,
    .end_frame         = null_end_frame,
    .draw              = null_draw,
    .draw_indexed      = null_draw_indexed,
    .present           = null_present,
    .clear             = null_clear,
    .gpu_flush         = NULL,
    .dispatch_compute  = null_dispatch_compute,
    .supports_feature  = null_supports_feature,
};

void vio_backend_null_register(void)
{
    vio_register_backend(&null_backend);
}

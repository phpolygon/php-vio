/*
 * php-vio - PHP Video Input Output
 * Backend vtable definition - included by backend extensions
 */

#ifndef VIO_BACKEND_H
#define VIO_BACKEND_H

#include "vio_types.h"

#define VIO_BACKEND_API_VERSION 1
#define VIO_MAX_BACKENDS 8

typedef struct _vio_backend {
    const char *name;   /* "opengl", "vulkan", "metal" */
    int api_version;    /* Must match VIO_BACKEND_API_VERSION */

    /* Lifecycle */
    int   (*init)(vio_config *cfg);
    void  (*shutdown)(void);

    /* Surface & Window */
    void *(*create_surface)(vio_config *cfg);
    void  (*destroy_surface)(void *surface);
    void  (*resize)(int width, int height);

    /* Pipeline */
    void *(*create_pipeline)(vio_pipeline_desc *desc);
    void  (*destroy_pipeline)(void *pipeline);
    void  (*bind_pipeline)(void *pipeline);

    /* Resources */
    void *(*create_buffer)(vio_buffer_desc *desc);
    void  (*update_buffer)(void *buffer, const void *data, size_t size);
    void  (*destroy_buffer)(void *buffer);

    void *(*create_texture)(vio_texture_desc *desc);
    void  (*destroy_texture)(void *texture);

    /* Shader */
    void *(*compile_shader)(vio_shader_desc *desc);
    void  (*destroy_shader)(void *shader);

    /* Object destructors — invoked from Zend free_object handlers. Each takes
     * the full vio_*_object pointer (cast to void *) so the backend can free
     * whichever fields it populated. All four are optional: NULL = no-op,
     * caller must guard. Backends without a meaningful destructor leave the
     * slot NULL rather than registering a stub. */
    void  (*destroy_mesh)(void *mesh);            /* vio_mesh_object * */
    void  (*destroy_render_target)(void *rt);     /* vio_render_target_object * */
    void  (*destroy_cubemap)(void *cm);           /* vio_cubemap_object * */
    void  (*destroy_font_atlas)(void *font);      /* vio_font_object * */

    /* Drawing */
    void  (*begin_frame)(void);
    void  (*end_frame)(void);
    void  (*draw)(vio_draw_cmd *cmd);
    void  (*draw_indexed)(vio_draw_indexed_cmd *cmd);
    void  (*present)(void);
    void  (*clear)(float r, float g, float b, float a);
    void  (*gpu_flush)(void);  /* Block until all GPU work is complete (like glFinish) */

    /* State binding */
    void  (*set_uniform)(const char *name, const void *data, int count, int type);
    void  (*bind_texture)(void *texture, int slot);
    void  (*set_viewport)(int x, int y, int width, int height);

    /* Compute (optional) */
    void  (*dispatch_compute)(vio_compute_cmd *cmd);

    /* Query */
    int   (*supports_feature)(vio_feature feature);
} vio_backend;

/*
 * Backend extensions call this in their MINIT to register themselves.
 * Returns 0 on success, -1 on failure (e.g., registry full, version mismatch).
 */
int vio_register_backend(const vio_backend *backend);

/*
 * Find a registered backend by name. Returns NULL if not found.
 */
const vio_backend *vio_find_backend(const char *name);

/*
 * Get the best available backend (Vulkan > Metal > OpenGL).
 * Returns NULL if no backends are registered.
 */
const vio_backend *vio_get_auto_backend(void);

/*
 * Get the number of registered backends.
 */
int vio_backend_count(void);

/*
 * Get the name of a registered backend by index. Returns NULL if out of range.
 */
const char *vio_get_backend_name(int index);

#endif /* VIO_BACKEND_H */

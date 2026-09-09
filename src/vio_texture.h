/*
 * php-vio - Texture management
 */

#ifndef VIO_TEXTURE_H
#define VIO_TEXTURE_H

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "php.h"
#include "../include/vio_types.h"

struct _vio_backend;

typedef struct _vio_texture_object {
    unsigned int texture_id;    /* GL texture ID */
    void        *backend_texture; /* Backend texture (D3D11/D3D12/Vulkan) */
    int          width;
    int          height;
    int          depth;         /* > 0 for 3D / volume textures (Fieldtracing SDF) */
    int          is_3d;         /* 1 => bind via the GL_TEXTURE_3D path */
    int          channels;
    vio_filter   filter;
    vio_wrap     wrap;
    int          anisotropy;    /* vio_texture(['anisotropy' => N]), 0/1 = off */
    int          valid;
    int          borrowed;      /* 1 if texture_id is owned by another object (e.g. render target) */
    int          storage;       /* 1 => created with 'storage' => true (compute image2D/3D target) */
    const struct _vio_backend *backend;
    unsigned int gl_generation;   /* OpenGL: context generation that owns the GL names (vio_opengl.c) */
    zend_object  std;
} vio_texture_object;

extern zend_class_entry *vio_texture_ce;

void vio_texture_register(void);

static inline vio_texture_object *vio_texture_from_obj(zend_object *obj) {
    return (vio_texture_object *)((char *)obj - XtOffsetOf(vio_texture_object, std));
}

#define Z_VIO_TEXTURE_P(zv) vio_texture_from_obj(Z_OBJ_P(zv))

#endif /* VIO_TEXTURE_H */

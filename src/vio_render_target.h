/*
 * php-vio - Render target (offscreen FBO) management
 */

#ifndef VIO_RENDER_TARGET_H
#define VIO_RENDER_TARGET_H

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "php.h"

typedef struct _vio_render_target_object {
    unsigned int fbo;
    unsigned int color_texture;
    unsigned int depth_texture;
    int          width;
    int          height;
    int          depth_only;
    int          valid;
    zend_object  std;
} vio_render_target_object;

extern zend_class_entry *vio_render_target_ce;

void vio_render_target_register(void);

static inline vio_render_target_object *vio_render_target_from_obj(zend_object *obj) {
    return (vio_render_target_object *)((char *)obj - XtOffsetOf(vio_render_target_object, std));
}

#define Z_VIO_RENDER_TARGET_P(zv) vio_render_target_from_obj(Z_OBJ_P(zv))

#endif /* VIO_RENDER_TARGET_H */

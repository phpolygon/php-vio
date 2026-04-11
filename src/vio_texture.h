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

typedef struct _vio_texture_object {
    unsigned int texture_id;    /* GL texture ID */
    int          width;
    int          height;
    int          channels;
    vio_filter   filter;
    vio_wrap     wrap;
    int          valid;
    int          borrowed;      /* 1 if texture_id is owned by another object (e.g. render target) */
    zend_object  std;
} vio_texture_object;

extern zend_class_entry *vio_texture_ce;

void vio_texture_register(void);

static inline vio_texture_object *vio_texture_from_obj(zend_object *obj) {
    return (vio_texture_object *)((char *)obj - XtOffsetOf(vio_texture_object, std));
}

#define Z_VIO_TEXTURE_P(zv) vio_texture_from_obj(Z_OBJ_P(zv))

#endif /* VIO_TEXTURE_H */

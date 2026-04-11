/*
 * php-vio - Cubemap texture management
 */

#ifndef VIO_CUBEMAP_H
#define VIO_CUBEMAP_H

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "php.h"

typedef struct _vio_cubemap_object {
    unsigned int texture_id;    /* GL cubemap texture ID */
    int          resolution;
    int          valid;
    zend_object  std;
} vio_cubemap_object;

extern zend_class_entry *vio_cubemap_ce;

void vio_cubemap_register(void);

static inline vio_cubemap_object *vio_cubemap_from_obj(zend_object *obj) {
    return (vio_cubemap_object *)((char *)obj - XtOffsetOf(vio_cubemap_object, std));
}

#define Z_VIO_CUBEMAP_P(zv) vio_cubemap_from_obj(Z_OBJ_P(zv))

#endif /* VIO_CUBEMAP_H */

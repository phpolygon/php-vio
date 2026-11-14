/*
 * php-vio - Persistent instance buffer (vio_instance_buffer)
 *
 * Packed mat4 instance data (64 bytes per instance) that lives on the GPU: created
 * once, updated in place, drawn many times - instead of copying the instance bytes
 * of every draw into the backend's per-frame ring again and again.
 */

#ifndef VIO_INSTANCE_BUFFER_H
#define VIO_INSTANCE_BUFFER_H

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "php.h"

/* sizeof(mat4): the per-instance record every instanced draw reads. */
#define VIO_INSTANCE_STRIDE 64

typedef struct _vio_instance_buffer_object {
    size_t         size;            /* bytes, a multiple of VIO_INSTANCE_STRIDE */
    /* Native GPU vertex buffer (create_buffer slot) on backends with
     * persistent_instance_buffers; NULL otherwise. */
    void          *backend_buffer;
    const void    *backend;         /* the vio_backend that owns backend_buffer / the shadow */
    /* Backends without a native path: the CPU copy a draw reads from. */
    unsigned char *shadow;
    zend_object    std;
} vio_instance_buffer_object;

extern zend_class_entry *vio_instance_buffer_ce;

void vio_instance_buffer_register(void);

static inline vio_instance_buffer_object *vio_instance_buffer_from_obj(zend_object *obj) {
    return (vio_instance_buffer_object *)((char *)obj - offsetof(vio_instance_buffer_object, std));
}

#define Z_VIO_INSTANCE_BUFFER_P(zv) vio_instance_buffer_from_obj(Z_OBJ_P(zv))

#endif /* VIO_INSTANCE_BUFFER_H */

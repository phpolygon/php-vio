/*
 * php-vio - Buffer management (uniform buffers)
 */

#ifndef VIO_BUFFER_H
#define VIO_BUFFER_H

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "php.h"
#include "../include/vio_types.h"

typedef struct _vio_buffer_object {
    unsigned int        buffer_id;     /* GL buffer ID */
    void               *backend_buffer; /* Backend buffer (D3D11/D3D12/Vulkan) */
    const void         *backend;       /* vio_backend pointer for update_buffer */
    vio_buffer_type     type;
    size_t              size;
    int                 binding;       /* UBO binding point */
    int                 valid;
    zend_object         std;
} vio_buffer_object;

extern zend_class_entry *vio_buffer_ce;

void vio_buffer_register(void);

static inline vio_buffer_object *vio_buffer_from_obj(zend_object *obj) {
    return (vio_buffer_object *)((char *)obj - XtOffsetOf(vio_buffer_object, std));
}

#define Z_VIO_BUFFER_P(zv) vio_buffer_from_obj(Z_OBJ_P(zv))

#endif /* VIO_BUFFER_H */

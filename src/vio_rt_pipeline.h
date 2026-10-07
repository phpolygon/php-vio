/*
 * php-vio - Ray tracing pipeline object (VIO_FEATURE_RAYTRACING)
 */

#ifndef VIO_RT_PIPELINE_H
#define VIO_RT_PIPELINE_H

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "php.h"
#include "../include/vio_types.h"

typedef struct _vio_rt_pipeline_object {
    void        *backend_pipeline;  /* create_rt_pipeline handle */
    const void  *backend;           /* vio_backend that built it, for the destroy slot */
    /* vio_rt_bind_buffer(): the storage buffers of the next vio_trace_rays,
     * one per binding; the objects are referenced while bound. */
    zend_object *buffers[VIO_RT_MAX_BUFFERS];
    int          bindings[VIO_RT_MAX_BUFFERS];
    int          buffer_count;
    int          valid;
    zend_object  std;
} vio_rt_pipeline_object;

extern zend_class_entry *vio_rt_pipeline_ce;

void vio_rt_pipeline_register(void);

static inline vio_rt_pipeline_object *vio_rt_pipeline_from_obj(zend_object *obj) {
    return (vio_rt_pipeline_object *)((char *)obj - XtOffsetOf(vio_rt_pipeline_object, std));
}

#define Z_VIO_RT_PIPELINE_P(zv) vio_rt_pipeline_from_obj(Z_OBJ_P(zv))

#endif /* VIO_RT_PIPELINE_H */

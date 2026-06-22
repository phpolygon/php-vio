/*
 * php-vio - Compute pipeline object (GPU compute primitive)
 */

#ifndef VIO_COMPUTE_PIPELINE_H
#define VIO_COMPUTE_PIPELINE_H

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "php.h"
#include "../include/vio_types.h"

typedef struct _vio_compute_pipeline_object {
    void        *backend_pipeline;  /* backend compute pipeline (PSO / program) */
    const void  *backend;           /* vio_backend pointer for destroy / dispatch */
    int          valid;
    zend_object  std;
} vio_compute_pipeline_object;

extern zend_class_entry *vio_compute_pipeline_ce;

void vio_compute_pipeline_register(void);

static inline vio_compute_pipeline_object *vio_compute_pipeline_from_obj(zend_object *obj) {
    return (vio_compute_pipeline_object *)((char *)obj - XtOffsetOf(vio_compute_pipeline_object, std));
}

#define Z_VIO_COMPUTE_PIPELINE_P(zv) vio_compute_pipeline_from_obj(Z_OBJ_P(zv))

#endif /* VIO_COMPUTE_PIPELINE_H */

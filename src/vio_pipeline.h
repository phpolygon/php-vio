/*
 * php-vio - Pipeline management
 */

#ifndef VIO_PIPELINE_H
#define VIO_PIPELINE_H

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "php.h"
#include "../include/vio_types.h"

typedef struct _vio_pipeline_object {
    unsigned int   shader_program;   /* GL program (copied, not owned) */
    vio_topology   topology;
    vio_cull_mode  cull_mode;
    int            depth_test;
    vio_blend_mode blend;
    int            valid;
    zend_object    std;
} vio_pipeline_object;

extern zend_class_entry *vio_pipeline_ce;

void vio_pipeline_register(void);

static inline vio_pipeline_object *vio_pipeline_from_obj(zend_object *obj) {
    return (vio_pipeline_object *)((char *)obj - XtOffsetOf(vio_pipeline_object, std));
}

#define Z_VIO_PIPELINE_P(zv) vio_pipeline_from_obj(Z_OBJ_P(zv))

#endif /* VIO_PIPELINE_H */

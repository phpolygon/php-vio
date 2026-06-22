/*
 * php-vio - Compute pipeline object implementation
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "vio_compute_pipeline.h"
#include "../include/vio_backend.h"

zend_class_entry *vio_compute_pipeline_ce = NULL;
static zend_object_handlers vio_compute_pipeline_handlers;

static zend_object *vio_compute_pipeline_create_object(zend_class_entry *ce)
{
    vio_compute_pipeline_object *p =
        zend_object_alloc(sizeof(vio_compute_pipeline_object), ce);

    p->backend_pipeline = NULL;
    p->backend          = NULL;
    p->valid            = 0;

    zend_object_std_init(&p->std, ce);
    object_properties_init(&p->std, ce);
    p->std.handlers = &vio_compute_pipeline_handlers;

    return &p->std;
}

static void vio_compute_pipeline_free_object(zend_object *obj)
{
    vio_compute_pipeline_object *p = vio_compute_pipeline_from_obj(obj);

    if (p->backend && p->backend_pipeline) {
        const vio_backend *be = (const vio_backend *)p->backend;
        if (be->destroy_compute_pipeline) {
            be->destroy_compute_pipeline(p->backend_pipeline);
        }
        p->backend_pipeline = NULL;
    }

    zend_object_std_dtor(&p->std);
}

void vio_compute_pipeline_register(void)
{
    zend_class_entry ce;

    INIT_CLASS_ENTRY(ce, "VioComputePipeline", NULL);
    vio_compute_pipeline_ce = zend_register_internal_class(&ce);
    vio_compute_pipeline_ce->ce_flags |=
        ZEND_ACC_FINAL | ZEND_ACC_NO_DYNAMIC_PROPERTIES | ZEND_ACC_NOT_SERIALIZABLE;
    vio_compute_pipeline_ce->create_object = vio_compute_pipeline_create_object;

    memcpy(&vio_compute_pipeline_handlers, &std_object_handlers, sizeof(zend_object_handlers));
    vio_compute_pipeline_handlers.offset    = XtOffsetOf(vio_compute_pipeline_object, std);
    vio_compute_pipeline_handlers.free_obj  = vio_compute_pipeline_free_object;
    vio_compute_pipeline_handlers.clone_obj = NULL;
}

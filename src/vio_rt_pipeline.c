/*
 * php-vio - Ray tracing pipeline object implementation
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "vio_rt_pipeline.h"
#include "../include/vio_backend.h"

zend_class_entry *vio_rt_pipeline_ce = NULL;
static zend_object_handlers vio_rt_pipeline_handlers;

static zend_object *vio_rt_pipeline_create_object(zend_class_entry *ce)
{
    vio_rt_pipeline_object *p = zend_object_alloc(sizeof(vio_rt_pipeline_object), ce);

    p->backend_pipeline = NULL;
    p->backend          = NULL;
    p->buffer_count     = 0;
    p->valid            = 0;

    zend_object_std_init(&p->std, ce);
    object_properties_init(&p->std, ce);
    p->std.handlers = &vio_rt_pipeline_handlers;

    return &p->std;
}

static void vio_rt_pipeline_free_object(zend_object *obj)
{
    vio_rt_pipeline_object *p = vio_rt_pipeline_from_obj(obj);

    for (int i = 0; i < p->buffer_count; i++) OBJ_RELEASE(p->buffers[i]);
    p->buffer_count = 0;
    if (p->backend && p->backend_pipeline) {
        const vio_backend *be = (const vio_backend *)p->backend;
        if (be->destroy_rt_pipeline) {
            be->destroy_rt_pipeline(p->backend_pipeline);
        }
        p->backend_pipeline = NULL;
    }

    zend_object_std_dtor(&p->std);
}

/* The bound buffers, for the cycle collector. */
static HashTable *vio_rt_pipeline_get_gc(zend_object *obj, zval **table, int *n)
{
    vio_rt_pipeline_object *p = vio_rt_pipeline_from_obj(obj);
    zend_get_gc_buffer *gc = zend_get_gc_buffer_create();
    for (int i = 0; i < p->buffer_count; i++) zend_get_gc_buffer_add_obj(gc, p->buffers[i]);
    zend_get_gc_buffer_use(gc, table, n);
    return zend_std_get_properties(obj);
}

void vio_rt_pipeline_register(void)
{
    zend_class_entry ce;

    INIT_CLASS_ENTRY(ce, "VioRtPipeline", NULL);
    vio_rt_pipeline_ce = zend_register_internal_class(&ce);
    vio_rt_pipeline_ce->ce_flags |=
        ZEND_ACC_FINAL | ZEND_ACC_NO_DYNAMIC_PROPERTIES | ZEND_ACC_NOT_SERIALIZABLE;
    vio_rt_pipeline_ce->create_object = vio_rt_pipeline_create_object;

    memcpy(&vio_rt_pipeline_handlers, &std_object_handlers, sizeof(zend_object_handlers));
    vio_rt_pipeline_handlers.offset    = XtOffsetOf(vio_rt_pipeline_object, std);
    vio_rt_pipeline_handlers.free_obj  = vio_rt_pipeline_free_object;
    vio_rt_pipeline_handlers.get_gc    = vio_rt_pipeline_get_gc;
    vio_rt_pipeline_handlers.clone_obj = NULL;
}

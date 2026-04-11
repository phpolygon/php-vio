/*
 * php-vio - Pipeline management implementation
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "vio_pipeline.h"

zend_class_entry *vio_pipeline_ce = NULL;
static zend_object_handlers vio_pipeline_handlers;

static zend_object *vio_pipeline_create_object(zend_class_entry *ce)
{
    vio_pipeline_object *pipe = zend_object_alloc(sizeof(vio_pipeline_object), ce);

    pipe->shader_program = 0;
    pipe->topology       = VIO_TRIANGLES;
    pipe->cull_mode      = VIO_CULL_NONE;
    pipe->depth_test     = 1;
    pipe->blend          = VIO_BLEND_ALPHA;
    pipe->valid          = 0;

    zend_object_std_init(&pipe->std, ce);
    object_properties_init(&pipe->std, ce);
    pipe->std.handlers = &vio_pipeline_handlers;

    return &pipe->std;
}

static void vio_pipeline_free_object(zend_object *obj)
{
    /* Pipeline does not own the shader program - VioShader does */
    zend_object_std_dtor(&obj[0]);
}

void vio_pipeline_register(void)
{
    zend_class_entry ce;

    INIT_CLASS_ENTRY(ce, "VioPipeline", NULL);
    vio_pipeline_ce = zend_register_internal_class(&ce);
    vio_pipeline_ce->ce_flags |= ZEND_ACC_FINAL | ZEND_ACC_NO_DYNAMIC_PROPERTIES | ZEND_ACC_NOT_SERIALIZABLE;
    vio_pipeline_ce->create_object = vio_pipeline_create_object;

    memcpy(&vio_pipeline_handlers, &std_object_handlers, sizeof(zend_object_handlers));
    vio_pipeline_handlers.offset   = XtOffsetOf(vio_pipeline_object, std);
    vio_pipeline_handlers.free_obj = vio_pipeline_free_object;
    vio_pipeline_handlers.clone_obj = NULL;
}

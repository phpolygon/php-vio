/*
 * php-vio - Persistent instance buffer object implementation
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "vio_instance_buffer.h"
#include "../include/vio_backend.h"

zend_class_entry *vio_instance_buffer_ce = NULL;
static zend_object_handlers vio_instance_buffer_handlers;

static zend_object *vio_instance_buffer_create_object(zend_class_entry *ce)
{
    vio_instance_buffer_object *b = zend_object_alloc(sizeof(vio_instance_buffer_object), ce);
    b->size           = 0;
    b->backend_buffer = NULL;
    b->backend        = NULL;
    b->shadow         = NULL;
    zend_object_std_init(&b->std, ce);
    object_properties_init(&b->std, ce);
    b->std.handlers = &vio_instance_buffer_handlers;
    return &b->std;
}

static void vio_instance_buffer_free_object(zend_object *obj)
{
    vio_instance_buffer_object *b = vio_instance_buffer_from_obj(obj);
    if (b->backend_buffer && b->backend) {
        const vio_backend *be = (const vio_backend *)b->backend;
        if (be->destroy_buffer) be->destroy_buffer(b->backend_buffer);
    }
    b->backend_buffer = NULL;
    if (b->shadow) { efree(b->shadow); b->shadow = NULL; }
    zend_object_std_dtor(&b->std);
}

void vio_instance_buffer_register(void)
{
    zend_class_entry ce;
    INIT_CLASS_ENTRY(ce, "VioInstanceBuffer", NULL);
    vio_instance_buffer_ce = zend_register_internal_class(&ce);
    vio_instance_buffer_ce->ce_flags |= ZEND_ACC_FINAL | ZEND_ACC_NO_DYNAMIC_PROPERTIES | ZEND_ACC_NOT_SERIALIZABLE;
    vio_instance_buffer_ce->create_object = vio_instance_buffer_create_object;

    memcpy(&vio_instance_buffer_handlers, &std_object_handlers, sizeof(zend_object_handlers));
    vio_instance_buffer_handlers.offset    = offsetof(vio_instance_buffer_object, std);
    vio_instance_buffer_handlers.free_obj  = vio_instance_buffer_free_object;
    vio_instance_buffer_handlers.clone_obj = NULL;
}

/*
 * php-vio - Acceleration structure object implementation
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "vio_acceleration_structure.h"
#include "../include/vio_backend.h"

zend_class_entry *vio_acceleration_structure_ce = NULL;
static zend_object_handlers vio_acceleration_structure_handlers;

static zend_object *vio_acceleration_structure_create_object(zend_class_entry *ce)
{
    vio_acceleration_structure_object *as =
        zend_object_alloc(sizeof(vio_acceleration_structure_object), ce);

    as->backend_as     = NULL;
    as->backend        = NULL;
    as->instance_count = 0;
    as->valid          = 0;

    zend_object_std_init(&as->std, ce);
    object_properties_init(&as->std, ce);
    as->std.handlers = &vio_acceleration_structure_handlers;

    return &as->std;
}

static void vio_acceleration_structure_free_object(zend_object *obj)
{
    vio_acceleration_structure_object *as = vio_acceleration_structure_from_obj(obj);

    if (as->backend && as->backend_as) {
        const vio_backend *be = (const vio_backend *)as->backend;
        if (be->destroy_acceleration_structure) {
            be->destroy_acceleration_structure(as->backend_as);
        }
        as->backend_as = NULL;
    }

    zend_object_std_dtor(&as->std);
}

void vio_acceleration_structure_register(void)
{
    zend_class_entry ce;

    INIT_CLASS_ENTRY(ce, "VioAccelerationStructure", NULL);
    vio_acceleration_structure_ce = zend_register_internal_class(&ce);
    vio_acceleration_structure_ce->ce_flags |=
        ZEND_ACC_FINAL | ZEND_ACC_NO_DYNAMIC_PROPERTIES | ZEND_ACC_NOT_SERIALIZABLE;
    vio_acceleration_structure_ce->create_object = vio_acceleration_structure_create_object;

    memcpy(&vio_acceleration_structure_handlers, &std_object_handlers, sizeof(zend_object_handlers));
    vio_acceleration_structure_handlers.offset    = XtOffsetOf(vio_acceleration_structure_object, std);
    vio_acceleration_structure_handlers.free_obj  = vio_acceleration_structure_free_object;
    vio_acceleration_structure_handlers.clone_obj = NULL;
}

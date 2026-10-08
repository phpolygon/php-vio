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
    as->meshes         = NULL;
    as->mesh_count     = 0;
    as->inst_geo       = NULL;

    zend_object_std_init(&as->std, ce);
    object_properties_init(&as->std, ce);
    as->std.handlers = &vio_acceleration_structure_handlers;

    return &as->std;
}

void vio_acceleration_structure_keep(vio_acceleration_structure_object *as, zend_object *const *meshes, int mesh_count,
                                     const int *inst_geo, int instance_count)
{
    zend_object **old = as->meshes;
    int old_count = as->mesh_count;
    as->meshes = mesh_count > 0 ? ecalloc((size_t)mesh_count, sizeof(zend_object *)) : NULL;
    for (int i = 0; i < mesh_count; i++) { as->meshes[i] = meshes[i]; GC_ADDREF(meshes[i]); }
    as->mesh_count = mesh_count;
    for (int i = 0; i < old_count; i++) OBJ_RELEASE(old[i]);
    if (old) efree(old);
    if (as->inst_geo) efree(as->inst_geo);
    as->inst_geo = instance_count > 0 ? ecalloc((size_t)instance_count, sizeof(int)) : NULL;
    for (int i = 0; i < instance_count; i++) as->inst_geo[i] = inst_geo[i];
    as->instance_count = instance_count;
}

/* The held meshes, for the cycle collector. */
static HashTable *vio_acceleration_structure_get_gc(zend_object *obj, zval **table, int *n)
{
    vio_acceleration_structure_object *as = vio_acceleration_structure_from_obj(obj);
    zend_get_gc_buffer *gc = zend_get_gc_buffer_create();
    for (int i = 0; i < as->mesh_count; i++) zend_get_gc_buffer_add_obj(gc, as->meshes[i]);
    zend_get_gc_buffer_use(gc, table, n);
    return zend_std_get_properties(obj);
}

static void vio_acceleration_structure_free_object(zend_object *obj)
{
    vio_acceleration_structure_object *as = vio_acceleration_structure_from_obj(obj);
    vio_acceleration_structure_keep(as, NULL, 0, NULL, 0);

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
    vio_acceleration_structure_handlers.get_gc    = vio_acceleration_structure_get_gc;
    vio_acceleration_structure_handlers.clone_obj = NULL;
}

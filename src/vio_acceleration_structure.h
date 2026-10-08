/*
 * php-vio - Acceleration structure object (inline ray tracing, VIO_FEATURE_RAY_QUERY)
 */

#ifndef VIO_ACCELERATION_STRUCTURE_H
#define VIO_ACCELERATION_STRUCTURE_H

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "php.h"
#include "../include/vio_types.h"

typedef struct _vio_acceleration_structure_object {
    void        *backend_as;   /* backend BLASes + TLAS (create_acceleration_structure) */
    const void  *backend;      /* vio_backend that built it, for the destroy slot */
    int          instance_count;
    int          valid;
    /* vio_acceleration_structure_update (A14): the meshes of the bottom levels
     * (held, so their identity stays) and each instance's geometry index. */
    zend_object **meshes;
    int          mesh_count;
    int         *inst_geo;
    zend_object  std;
} vio_acceleration_structure_object;

extern zend_class_entry *vio_acceleration_structure_ce;

void vio_acceleration_structure_register(void);
/* Replace the held meshes and instance geometries (meshes: zend_object of each VioMesh). */
void vio_acceleration_structure_keep(vio_acceleration_structure_object *as, zend_object *const *meshes, int mesh_count,
                                     const int *inst_geo, int instance_count);

static inline vio_acceleration_structure_object *vio_acceleration_structure_from_obj(zend_object *obj) {
    return (vio_acceleration_structure_object *)((char *)obj - XtOffsetOf(vio_acceleration_structure_object, std));
}

#define Z_VIO_ACCELERATION_STRUCTURE_P(zv) vio_acceleration_structure_from_obj(Z_OBJ_P(zv))

#endif /* VIO_ACCELERATION_STRUCTURE_H */

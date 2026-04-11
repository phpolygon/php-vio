/*
 * php-vio - Mesh management (VAO/VBO/EBO)
 */

#ifndef VIO_MESH_H
#define VIO_MESH_H

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "php.h"
#include "../include/vio_types.h"

typedef struct _vio_mesh_object {
    unsigned int vao;
    unsigned int vbo;
    unsigned int ebo;
    int          vertex_count;
    int          index_count;
    int          has_colors;      /* 1 if vertex data includes colors */
    int          stride;          /* bytes per vertex */
    zend_object  std;
} vio_mesh_object;

extern zend_class_entry *vio_mesh_ce;

void vio_mesh_register(void);

static inline vio_mesh_object *vio_mesh_from_obj(zend_object *obj) {
    return (vio_mesh_object *)((char *)obj - XtOffsetOf(vio_mesh_object, std));
}

#define Z_VIO_MESH_P(zv) vio_mesh_from_obj(Z_OBJ_P(zv))

#endif /* VIO_MESH_H */

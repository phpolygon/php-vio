/*
 * php-vio - Mesh management implementation
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "vio_mesh.h"

#ifdef HAVE_GLFW
#include <glad/glad.h>
#endif

zend_class_entry *vio_mesh_ce = NULL;
static zend_object_handlers vio_mesh_handlers;

static zend_object *vio_mesh_create_object(zend_class_entry *ce)
{
    vio_mesh_object *mesh = zend_object_alloc(sizeof(vio_mesh_object), ce);

    mesh->vao          = 0;
    mesh->vbo          = 0;
    mesh->ebo          = 0;
    mesh->vertex_count = 0;
    mesh->index_count  = 0;
    mesh->has_colors   = 0;
    mesh->stride       = 0;

    zend_object_std_init(&mesh->std, ce);
    object_properties_init(&mesh->std, ce);
    mesh->std.handlers = &vio_mesh_handlers;

    return &mesh->std;
}

static void vio_mesh_free_object(zend_object *obj)
{
    vio_mesh_object *mesh = vio_mesh_from_obj(obj);

#ifdef HAVE_GLFW
    if (mesh->ebo) {
        glDeleteBuffers(1, &mesh->ebo);
    }
    if (mesh->vbo) {
        glDeleteBuffers(1, &mesh->vbo);
    }
    if (mesh->vao) {
        glDeleteVertexArrays(1, &mesh->vao);
    }
#endif

    zend_object_std_dtor(&mesh->std);
}

void vio_mesh_register(void)
{
    zend_class_entry ce;

    INIT_CLASS_ENTRY(ce, "VioMesh", NULL);
    vio_mesh_ce = zend_register_internal_class(&ce);
    vio_mesh_ce->ce_flags |= ZEND_ACC_FINAL | ZEND_ACC_NO_DYNAMIC_PROPERTIES | ZEND_ACC_NOT_SERIALIZABLE;
    vio_mesh_ce->create_object = vio_mesh_create_object;

    memcpy(&vio_mesh_handlers, &std_object_handlers, sizeof(zend_object_handlers));
    vio_mesh_handlers.offset   = XtOffsetOf(vio_mesh_object, std);
    vio_mesh_handlers.free_obj = vio_mesh_free_object;
    vio_mesh_handlers.clone_obj = NULL;
}

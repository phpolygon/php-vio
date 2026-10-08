/*
 * php-vio - Work graph object implementation
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "vio_work_graph.h"
#include "../include/vio_backend.h"

zend_class_entry *vio_work_graph_ce = NULL;
static zend_object_handlers vio_work_graph_handlers;

static zend_object *vio_work_graph_create_object(zend_class_entry *ce)
{
    vio_work_graph_object *g = zend_object_alloc(sizeof(vio_work_graph_object), ce);

    g->backend_graph = NULL;
    g->backend       = NULL;
    g->record_size   = 0;
    for (int i = 0; i < VIO_WORK_GRAPH_MAX_BUFFERS; i++) g->buffers[i] = NULL;
    g->valid         = 0;

    zend_object_std_init(&g->std, ce);
    object_properties_init(&g->std, ce);
    g->std.handlers = &vio_work_graph_handlers;

    return &g->std;
}

static void vio_work_graph_free_object(zend_object *obj)
{
    vio_work_graph_object *g = vio_work_graph_from_obj(obj);

    if (g->backend && g->backend_graph) {
        const vio_backend *be = (const vio_backend *)g->backend;
        if (be->destroy_work_graph) {
            be->destroy_work_graph(g->backend_graph);
        }
        g->backend_graph = NULL;
    }
    /* Only after the graph is gone: it held the buffers' backend pointers. */
    for (int i = 0; i < VIO_WORK_GRAPH_MAX_BUFFERS; i++) {
        if (g->buffers[i]) {
            OBJ_RELEASE(g->buffers[i]);
            g->buffers[i] = NULL;
        }
    }

    zend_object_std_dtor(&g->std);
}

void vio_work_graph_register(void)
{
    zend_class_entry ce;

    INIT_CLASS_ENTRY(ce, "VioWorkGraph", NULL);
    vio_work_graph_ce = zend_register_internal_class(&ce);
    vio_work_graph_ce->ce_flags |=
        ZEND_ACC_FINAL | ZEND_ACC_NO_DYNAMIC_PROPERTIES | ZEND_ACC_NOT_SERIALIZABLE;
    vio_work_graph_ce->create_object = vio_work_graph_create_object;

    memcpy(&vio_work_graph_handlers, &std_object_handlers, sizeof(zend_object_handlers));
    vio_work_graph_handlers.offset    = XtOffsetOf(vio_work_graph_object, std);
    vio_work_graph_handlers.free_obj  = vio_work_graph_free_object;
    vio_work_graph_handlers.clone_obj = NULL;
}

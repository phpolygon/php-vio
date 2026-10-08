/*
 * php-vio - Work graph object (SM 6.8 node shaders, VIO_FEATURE_WORK_GRAPHS)
 */

#ifndef VIO_WORK_GRAPH_H
#define VIO_WORK_GRAPH_H

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "php.h"
#include "../include/vio_types.h"

/* Storage buffers a graph can bind: register(u0) .. register(u7), space 0. */
#define VIO_WORK_GRAPH_MAX_BUFFERS 8

typedef struct _vio_work_graph_object {
    void        *backend_graph;   /* backend state object + backing memory (create_work_graph) */
    const void  *backend;         /* vio_backend that built it, for the destroy slot */
    int          record_size;     /* bytes per entry-node record */
    zend_object *buffers[VIO_WORK_GRAPH_MAX_BUFFERS];   /* bound VioBuffers, referenced */
    int          valid;
    zend_object  std;
} vio_work_graph_object;

extern zend_class_entry *vio_work_graph_ce;

void vio_work_graph_register(void);

static inline vio_work_graph_object *vio_work_graph_from_obj(zend_object *obj) {
    return (vio_work_graph_object *)((char *)obj - XtOffsetOf(vio_work_graph_object, std));
}

#define Z_VIO_WORK_GRAPH_P(zv) vio_work_graph_from_obj(Z_OBJ_P(zv))

#endif /* VIO_WORK_GRAPH_H */

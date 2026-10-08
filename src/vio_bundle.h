/*
 * php-vio - Recorded draw sequence (vio_bundle, OPEN-ITEMS A38, BUNDLE-PLAN.md)
 */

#ifndef VIO_BUNDLE_H
#define VIO_BUNDLE_H

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "php.h"

typedef struct _vio_bundle_texture {
    int          slot;
    zend_object *texture;      /* VioTexture, held */
} vio_bundle_texture;

typedef struct _vio_bundle_uniform {
    zend_string *name;
    zval         value;        /* int | float | array, copied */
} vio_bundle_uniform;

/* One draw: the vio_submit_batch record, parsed once. */
typedef struct _vio_bundle_record {
    zend_object        *pipeline;   /* VioPipeline or NULL (keep the previous one), held */
    vio_bundle_texture *textures;
    int                 texture_count;
    vio_bundle_uniform *uniforms;
    int                 uniform_count;
    zend_object        *mesh;       /* VioMesh, held */
} vio_bundle_record;

typedef struct _vio_bundle_object {
    vio_bundle_record *records;
    int                count;
    /* A native recording (BUNDLE-PLAN phases 2-4): the backend's object and the
     * backend that made it, for its destroy slot. NULL = the generic replay. */
    void              *backend_bundle;
    const void        *backend;
    zend_object        std;
} vio_bundle_object;

extern zend_class_entry *vio_bundle_ce;

void vio_bundle_register(void);
/* Release every held object and the parsed records. */
void vio_bundle_clear(vio_bundle_object *b);

static inline vio_bundle_object *vio_bundle_from_obj(zend_object *obj) {
    return (vio_bundle_object *)((char *)obj - XtOffsetOf(vio_bundle_object, std));
}

#define Z_VIO_BUNDLE_P(zv) vio_bundle_from_obj(Z_OBJ_P(zv))

#endif /* VIO_BUNDLE_H */

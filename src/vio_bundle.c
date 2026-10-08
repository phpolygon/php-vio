/*
 * php-vio - Recorded draw sequence object implementation (OPEN-ITEMS A38)
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "vio_bundle.h"
#include "../include/vio_backend.h"

zend_class_entry *vio_bundle_ce = NULL;
static zend_object_handlers vio_bundle_handlers;

static zend_object *vio_bundle_create_object(zend_class_entry *ce)
{
    vio_bundle_object *b = zend_object_alloc(sizeof(vio_bundle_object), ce);
    b->records        = NULL;
    b->count          = 0;
    b->backend_bundle = NULL;
    b->backend        = NULL;
    zend_object_std_init(&b->std, ce);
    object_properties_init(&b->std, ce);
    b->std.handlers = &vio_bundle_handlers;
    return &b->std;
}

void vio_bundle_clear(vio_bundle_object *b)
{
    for (int i = 0; i < b->count; i++) {
        vio_bundle_record *r = &b->records[i];
        if (r->pipeline) OBJ_RELEASE(r->pipeline);
        if (r->mesh) OBJ_RELEASE(r->mesh);
        for (int t = 0; t < r->texture_count; t++) OBJ_RELEASE(r->textures[t].texture);
        if (r->textures) efree(r->textures);
        for (int u = 0; u < r->uniform_count; u++) {
            zend_string_release(r->uniforms[u].name);
            zval_ptr_dtor(&r->uniforms[u].value);
        }
        if (r->uniforms) efree(r->uniforms);
    }
    if (b->records) efree(b->records);
    b->records = NULL;
    b->count = 0;
}

/* The held pipelines, textures and meshes, for the cycle collector. */
static HashTable *vio_bundle_get_gc(zend_object *obj, zval **table, int *n)
{
    vio_bundle_object *b = vio_bundle_from_obj(obj);
    zend_get_gc_buffer *gc = zend_get_gc_buffer_create();
    for (int i = 0; i < b->count; i++) {
        vio_bundle_record *r = &b->records[i];
        if (r->pipeline) zend_get_gc_buffer_add_obj(gc, r->pipeline);
        if (r->mesh) zend_get_gc_buffer_add_obj(gc, r->mesh);
        for (int t = 0; t < r->texture_count; t++) zend_get_gc_buffer_add_obj(gc, r->textures[t].texture);
        for (int u = 0; u < r->uniform_count; u++) zend_get_gc_buffer_add_zval(gc, &r->uniforms[u].value);
    }
    zend_get_gc_buffer_use(gc, table, n);
    return zend_std_get_properties(obj);
}

static void vio_bundle_free_object(zend_object *obj)
{
    vio_bundle_object *b = vio_bundle_from_obj(obj);
    if (b->backend && b->backend_bundle) {
        const vio_backend *be = (const vio_backend *)b->backend;
        if (be->destroy_bundle) be->destroy_bundle(b->backend_bundle);
        b->backend_bundle = NULL;
    }
    vio_bundle_clear(b);
    zend_object_std_dtor(&b->std);
}

void vio_bundle_register(void)
{
    zend_class_entry ce;
    INIT_CLASS_ENTRY(ce, "VioBundle", NULL);
    vio_bundle_ce = zend_register_internal_class(&ce);
    vio_bundle_ce->ce_flags |= ZEND_ACC_FINAL | ZEND_ACC_NO_DYNAMIC_PROPERTIES | ZEND_ACC_NOT_SERIALIZABLE;
    vio_bundle_ce->create_object = vio_bundle_create_object;

    memcpy(&vio_bundle_handlers, &std_object_handlers, sizeof(zend_object_handlers));
    vio_bundle_handlers.offset    = XtOffsetOf(vio_bundle_object, std);
    vio_bundle_handlers.free_obj  = vio_bundle_free_object;
    vio_bundle_handlers.get_gc    = vio_bundle_get_gc;
    vio_bundle_handlers.clone_obj = NULL;
}

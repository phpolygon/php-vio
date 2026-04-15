/*
 * php-vio - Buffer management implementation
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "vio_buffer.h"

#ifdef HAVE_GLFW
#include <glad/glad.h>
#endif

zend_class_entry *vio_buffer_ce = NULL;
static zend_object_handlers vio_buffer_handlers;

static zend_object *vio_buffer_create_object(zend_class_entry *ce)
{
    vio_buffer_object *buf = zend_object_alloc(sizeof(vio_buffer_object), ce);

    buf->buffer_id      = 0;
    buf->backend_buffer = NULL;
    buf->backend        = NULL;
    buf->type      = VIO_BUFFER_UNIFORM;
    buf->size      = 0;
    buf->binding   = 0;
    buf->valid     = 0;

    zend_object_std_init(&buf->std, ce);
    object_properties_init(&buf->std, ce);
    buf->std.handlers = &vio_buffer_handlers;

    return &buf->std;
}

static void vio_buffer_free_object(zend_object *obj)
{
    vio_buffer_object *buf = vio_buffer_from_obj(obj);

#ifdef HAVE_GLFW
    if (buf->buffer_id) {
        glDeleteBuffers(1, &buf->buffer_id);
        buf->buffer_id = 0;
    }
#endif

    zend_object_std_dtor(&buf->std);
}

void vio_buffer_register(void)
{
    zend_class_entry ce;

    INIT_CLASS_ENTRY(ce, "VioBuffer", NULL);
    vio_buffer_ce = zend_register_internal_class(&ce);
    vio_buffer_ce->ce_flags |= ZEND_ACC_FINAL | ZEND_ACC_NO_DYNAMIC_PROPERTIES | ZEND_ACC_NOT_SERIALIZABLE;
    vio_buffer_ce->create_object = vio_buffer_create_object;

    memcpy(&vio_buffer_handlers, &std_object_handlers, sizeof(zend_object_handlers));
    vio_buffer_handlers.offset   = XtOffsetOf(vio_buffer_object, std);
    vio_buffer_handlers.free_obj = vio_buffer_free_object;
    vio_buffer_handlers.clone_obj = NULL;
}

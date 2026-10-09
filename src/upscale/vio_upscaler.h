/*
 * php-vio - VioUpscaler object (vio_upscaler_create, TEMPORAL-S3)
 */

#ifndef VIO_UPSCALER_H
#define VIO_UPSCALER_H

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "php.h"
#include "vio_upscale.h"

typedef struct _vio_upscaler_object {
    void        *handle;            /* the backend's upscaler (upscaler_create) */
    const void  *backend;           /* vio_backend that created it, for its destroy slot */
    vio_upscale_create_desc desc;
    int          valid;
    struct _vio_upscaler_object *live_prev, *live_next;   /* live list (vio_upscaler_sweep) */
    zend_object  std;
} vio_upscaler_object;

extern zend_class_entry *vio_upscaler_ce;

void vio_upscaler_register(void);

/* Track / untrack a created upscaler (the free handler and the sweep untrack). */
void vio_upscaler_track(vio_upscaler_object *u);
/* Destroy the upscaler now (GPU work referencing it is waited for by the backend). */
void vio_upscaler_release(vio_upscaler_object *u);
/* Destroy every live upscaler of `backend` - before its device goes away
 * (vio_destroy / the context free handler call this ahead of backend->shutdown). */
void vio_upscaler_sweep(const void *backend);
/* Live upscalers of `backend`. */
int  vio_upscaler_live_count(const void *backend);

static inline vio_upscaler_object *vio_upscaler_from_obj(zend_object *obj) {
    return (vio_upscaler_object *)((char *)obj - XtOffsetOf(vio_upscaler_object, std));
}

#define Z_VIO_UPSCALER_P(zv) vio_upscaler_from_obj(Z_OBJ_P(zv))

#endif /* VIO_UPSCALER_H */

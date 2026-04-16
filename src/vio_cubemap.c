/*
 * php-vio - Cubemap texture Zend object
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "vio_cubemap.h"

#ifdef HAVE_GLFW
#include <glad/glad.h>
#endif

#ifdef HAVE_D3D11
#define COBJMACROS
#include <d3d11.h>
#endif

#ifdef HAVE_D3D12
#ifndef COBJMACROS
#define COBJMACROS
#endif
#include <d3d12.h>
#endif

zend_class_entry *vio_cubemap_ce = NULL;
static zend_object_handlers vio_cubemap_handlers;

static zend_object *vio_cubemap_create_object(zend_class_entry *ce)
{
    vio_cubemap_object *cm = zend_object_alloc(sizeof(vio_cubemap_object), ce);

    cm->texture_id    = 0;
    cm->d3d11_texture = NULL;
    cm->d3d11_srv     = NULL;
    cm->d3d11_sampler = NULL;
    cm->d3d12_resource  = NULL;
    cm->d3d12_srv_gpu   = 0;
    cm->d3d12_srv_cpu   = 0;
    cm->backend_type  = 0;
    cm->resolution    = 0;
    cm->valid         = 0;

    zend_object_std_init(&cm->std, ce);
    object_properties_init(&cm->std, ce);
    cm->std.handlers = &vio_cubemap_handlers;

    return &cm->std;
}

static void vio_cubemap_free_object(zend_object *obj)
{
    vio_cubemap_object *cm = vio_cubemap_from_obj(obj);

#ifdef HAVE_GLFW
    if (cm->texture_id) {
        glDeleteTextures(1, &cm->texture_id);
        cm->texture_id = 0;
    }
#endif

#ifdef HAVE_D3D11
    if (cm->d3d11_sampler) {
        ID3D11SamplerState_Release((ID3D11SamplerState *)cm->d3d11_sampler);
        cm->d3d11_sampler = NULL;
    }
    if (cm->d3d11_srv) {
        ID3D11ShaderResourceView_Release((ID3D11ShaderResourceView *)cm->d3d11_srv);
        cm->d3d11_srv = NULL;
    }
    if (cm->d3d11_texture) {
        ID3D11Texture2D_Release((ID3D11Texture2D *)cm->d3d11_texture);
        cm->d3d11_texture = NULL;
    }
#endif

#ifdef HAVE_D3D12
    if (cm->d3d12_resource) {
        ID3D12Resource_Release((ID3D12Resource *)cm->d3d12_resource);
        cm->d3d12_resource = NULL;
    }
#endif

    zend_object_std_dtor(&cm->std);
}

void vio_cubemap_register(void)
{
    zend_class_entry ce;

    INIT_CLASS_ENTRY(ce, "VioCubemap", NULL);
    vio_cubemap_ce = zend_register_internal_class(&ce);
    vio_cubemap_ce->ce_flags |= ZEND_ACC_FINAL | ZEND_ACC_NO_DYNAMIC_PROPERTIES | ZEND_ACC_NOT_SERIALIZABLE;
    vio_cubemap_ce->create_object = vio_cubemap_create_object;

    memcpy(&vio_cubemap_handlers, &std_object_handlers, sizeof(zend_object_handlers));
    vio_cubemap_handlers.offset   = XtOffsetOf(vio_cubemap_object, std);
    vio_cubemap_handlers.free_obj = vio_cubemap_free_object;
    vio_cubemap_handlers.clone_obj = NULL;
}

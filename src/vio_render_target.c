/*
 * php-vio - Render target (offscreen FBO) Zend object
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "vio_render_target.h"

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

zend_class_entry *vio_render_target_ce = NULL;
static zend_object_handlers vio_render_target_handlers;

static zend_object *vio_render_target_create_object(zend_class_entry *ce)
{
    vio_render_target_object *rt = zend_object_alloc(sizeof(vio_render_target_object), ce);

    rt->fbo           = 0;
    rt->color_texture = 0;
    rt->depth_texture = 0;
    rt->d3d11_rtv       = NULL;
    rt->d3d11_dsv       = NULL;
    rt->d3d11_color_tex = NULL;
    rt->d3d11_depth_tex = NULL;
    rt->d3d11_depth_srv = NULL;
    rt->d3d12_color_resource = NULL;
    rt->d3d12_depth_resource = NULL;
    rt->d3d12_rtv_heap       = NULL;
    rt->d3d12_dsv_heap       = NULL;
    rt->width         = 0;
    rt->height        = 0;
    rt->depth_only    = 0;
    rt->valid         = 0;
    rt->backend_type  = VIO_RT_BACKEND_NONE;

    zend_object_std_init(&rt->std, ce);
    object_properties_init(&rt->std, ce);
    rt->std.handlers = &vio_render_target_handlers;

    return &rt->std;
}

static void vio_render_target_free_object(zend_object *obj)
{
    vio_render_target_object *rt = vio_render_target_from_obj(obj);

#ifdef HAVE_GLFW
    if (rt->backend_type == VIO_RT_BACKEND_OPENGL) {
        if (rt->fbo) {
            glDeleteFramebuffers(1, &rt->fbo);
            rt->fbo = 0;
        }
        if (rt->color_texture) {
            glDeleteTextures(1, &rt->color_texture);
            rt->color_texture = 0;
        }
        if (rt->depth_texture) {
            glDeleteTextures(1, &rt->depth_texture);
            rt->depth_texture = 0;
        }
    }
#endif

#ifdef HAVE_D3D11
    if (rt->backend_type == VIO_RT_BACKEND_D3D11) {
        if (rt->d3d11_depth_srv) {
            ID3D11ShaderResourceView_Release((ID3D11ShaderResourceView *)rt->d3d11_depth_srv);
            rt->d3d11_depth_srv = NULL;
        }
        if (rt->d3d11_rtv) {
            ID3D11RenderTargetView_Release((ID3D11RenderTargetView *)rt->d3d11_rtv);
            rt->d3d11_rtv = NULL;
        }
        if (rt->d3d11_dsv) {
            ID3D11DepthStencilView_Release((ID3D11DepthStencilView *)rt->d3d11_dsv);
            rt->d3d11_dsv = NULL;
        }
        if (rt->d3d11_color_tex) {
            ID3D11Texture2D_Release((ID3D11Texture2D *)rt->d3d11_color_tex);
            rt->d3d11_color_tex = NULL;
        }
        if (rt->d3d11_depth_tex) {
            ID3D11Texture2D_Release((ID3D11Texture2D *)rt->d3d11_depth_tex);
            rt->d3d11_depth_tex = NULL;
        }
    }
#endif

#ifdef HAVE_D3D12
    if (rt->backend_type == VIO_RT_BACKEND_D3D12) {
        if (rt->d3d12_color_resource) {
            ID3D12Resource_Release((ID3D12Resource *)rt->d3d12_color_resource);
            rt->d3d12_color_resource = NULL;
        }
        if (rt->d3d12_depth_resource) {
            ID3D12Resource_Release((ID3D12Resource *)rt->d3d12_depth_resource);
            rt->d3d12_depth_resource = NULL;
        }
        if (rt->d3d12_rtv_heap) {
            ID3D12DescriptorHeap_Release((ID3D12DescriptorHeap *)rt->d3d12_rtv_heap);
            rt->d3d12_rtv_heap = NULL;
        }
        if (rt->d3d12_dsv_heap) {
            ID3D12DescriptorHeap_Release((ID3D12DescriptorHeap *)rt->d3d12_dsv_heap);
            rt->d3d12_dsv_heap = NULL;
        }
    }
#endif

    zend_object_std_dtor(&rt->std);
}

void vio_render_target_register(void)
{
    zend_class_entry ce;

    INIT_CLASS_ENTRY(ce, "VioRenderTarget", NULL);
    vio_render_target_ce = zend_register_internal_class(&ce);
    vio_render_target_ce->ce_flags |= ZEND_ACC_FINAL | ZEND_ACC_NO_DYNAMIC_PROPERTIES | ZEND_ACC_NOT_SERIALIZABLE;
    vio_render_target_ce->create_object = vio_render_target_create_object;

    memcpy(&vio_render_target_handlers, &std_object_handlers, sizeof(zend_object_handlers));
    vio_render_target_handlers.offset   = XtOffsetOf(vio_render_target_object, std);
    vio_render_target_handlers.free_obj = vio_render_target_free_object;
    vio_render_target_handlers.clone_obj = NULL;
}

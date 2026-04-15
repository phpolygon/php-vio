/*
 * php-vio - Render target (offscreen FBO) management
 */

#ifndef VIO_RENDER_TARGET_H
#define VIO_RENDER_TARGET_H

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "php.h"

typedef struct _vio_render_target_object {
    /* OpenGL */
    unsigned int fbo;
    unsigned int color_texture;
    unsigned int depth_texture;

    /* D3D11 (opaque pointers — actual types are ID3D11* behind void*) */
    void        *d3d11_rtv;           /* ID3D11RenderTargetView* */
    void        *d3d11_dsv;           /* ID3D11DepthStencilView* */
    void        *d3d11_color_tex;     /* ID3D11Texture2D* */
    void        *d3d11_depth_tex;     /* ID3D11Texture2D* */
    void        *d3d11_depth_srv;     /* ID3D11ShaderResourceView* (for shadow sampling) */

    /* D3D12 (opaque pointers — actual types are ID3D12Resource* etc.) */
    void        *d3d12_color_resource;  /* ID3D12Resource* */
    void        *d3d12_depth_resource;  /* ID3D12Resource* */
    void        *d3d12_rtv_heap;        /* ID3D12DescriptorHeap* (1 RTV) */
    void        *d3d12_dsv_heap;        /* ID3D12DescriptorHeap* (1 DSV) */

    /* Common */
    int          width;
    int          height;
    int          depth_only;
    int          valid;
    int          backend_type;        /* 0=none, 1=opengl, 2=d3d11, 3=d3d12 */
    zend_object  std;
} vio_render_target_object;

#define VIO_RT_BACKEND_NONE   0
#define VIO_RT_BACKEND_OPENGL 1
#define VIO_RT_BACKEND_D3D11  2
#define VIO_RT_BACKEND_D3D12  3

extern zend_class_entry *vio_render_target_ce;

void vio_render_target_register(void);

static inline vio_render_target_object *vio_render_target_from_obj(zend_object *obj) {
    return (vio_render_target_object *)((char *)obj - XtOffsetOf(vio_render_target_object, std));
}

#define Z_VIO_RENDER_TARGET_P(zv) vio_render_target_from_obj(Z_OBJ_P(zv))

#endif /* VIO_RENDER_TARGET_H */

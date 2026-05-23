/*
 * php-vio - Render target (offscreen FBO) management
 */

#ifndef VIO_RENDER_TARGET_H
#define VIO_RENDER_TARGET_H

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "php.h"

struct _vio_backend;

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
    void        *d3d11_color_srv;    /* ID3D11ShaderResourceView* for color attachment */
    /* Cached vio_d3d11_texture wrapper used by vio_render_target_texture().
     * Built lazily on the first call and reused on every subsequent one so
     * each frame's offscreen-blit doesn't leak a vio_d3d11_texture struct
     * plus a fresh AddRef on the SRV plus a fresh CreateSamplerState (D3D11
     * caps the sampler pool at 4096, and AddRef without a matching Release
     * accumulates indefinitely). Cleared with the rest of the RT's D3D11
     * resources in the free handler. */
    void        *d3d11_color_backend_texture; /* vio_d3d11_texture* */
    void        *d3d11_depth_backend_texture; /* vio_d3d11_texture* */

    /* D3D12 (opaque pointers — actual types are ID3D12Resource* etc.) */
    void        *d3d12_color_resource;  /* ID3D12Resource* */
    void        *d3d12_depth_resource;  /* ID3D12Resource* */
    void        *d3d12_rtv_heap;        /* ID3D12DescriptorHeap* (1 RTV) */
    void        *d3d12_dsv_heap;        /* ID3D12DescriptorHeap* (1 DSV) */

    /* Metal (opaque pointers — actual types are id<MTLTexture> CFBridgeRetained).
     * Stored as opaque void * so the public header doesn't pull in Metal
     * headers. The Metal backend transitions ownership across this boundary
     * via CFBridgingRetain / CFBridgingRelease. */
    void        *metal_color_texture;   /* id<MTLTexture> (CFRetained) */
    void        *metal_depth_texture;   /* id<MTLTexture> (CFRetained) */

    /* Vulkan (opaque — VkImage/VkImageView/VkRenderPass/VkFramebuffer/VkSampler
     * handles + VmaAllocation, stored as void* so the public header stays free
     * of Vulkan headers; 64-bit only). */
    void        *vulkan_color_image;          /* VkImage */
    void        *vulkan_color_alloc;          /* VmaAllocation */
    void        *vulkan_color_view;           /* VkImageView */
    void        *vulkan_depth_image;          /* VkImage */
    void        *vulkan_depth_alloc;          /* VmaAllocation */
    void        *vulkan_depth_view;           /* VkImageView */
    void        *vulkan_render_pass;          /* VkRenderPass (compatible with the swapchain 2D pipelines) */
    void        *vulkan_framebuffer;          /* VkFramebuffer */
    void        *vulkan_sampler;              /* VkSampler for sampling the result */
    void        *vulkan_color_backend_texture;/* vio_vulkan_texture* cached for vio_render_target_texture() */

    /* Common */
    int          width;
    int          height;
    int          depth_only;
    int          valid;
    int          backend_type;        /* 0=none, 1=opengl, 2=d3d11, 3=d3d12, 4=metal, 5=vulkan */
    int          d3d12_depth_is_srv;  /* 1 if depth resource is in SRV state (needs barrier to DEPTH_WRITE) */
    int          d3d12_color_is_srv;  /* 1 if color resource is in SRV state (needs barrier to RENDER_TARGET) */
    /* D3D12 cached SRV for shadow map sampling (allocated once at RT creation) */
    uint64_t     d3d12_depth_srv_gpu; /* D3D12_GPU_DESCRIPTOR_HANDLE.ptr */
    uint64_t     d3d12_depth_srv_cpu; /* D3D12_CPU_DESCRIPTOR_HANDLE.ptr */
    uint64_t     d3d12_color_srv_gpu; /* D3D12_GPU_DESCRIPTOR_HANDLE.ptr */
    uint64_t     d3d12_color_srv_cpu; /* D3D12_CPU_DESCRIPTOR_HANDLE.ptr */
    /* Cached vio_d3d12_texture wrapper, same lifecycle as the D3D11 pair. */
    void        *d3d12_color_backend_texture; /* vio_d3d12_texture* */
    void        *d3d12_depth_backend_texture; /* vio_d3d12_texture* */

    const struct _vio_backend *backend;  /* Backend that owns the resources above */
    zend_object  std;
} vio_render_target_object;

#define VIO_RT_BACKEND_NONE   0
#define VIO_RT_BACKEND_OPENGL 1
#define VIO_RT_BACKEND_D3D11  2
#define VIO_RT_BACKEND_D3D12  3
#define VIO_RT_BACKEND_METAL  4
#define VIO_RT_BACKEND_VULKAN 5

extern zend_class_entry *vio_render_target_ce;

void vio_render_target_register(void);

static inline vio_render_target_object *vio_render_target_from_obj(zend_object *obj) {
    return (vio_render_target_object *)((char *)obj - XtOffsetOf(vio_render_target_object, std));
}

#define Z_VIO_RENDER_TARGET_P(zv) vio_render_target_from_obj(Z_OBJ_P(zv))

#endif /* VIO_RENDER_TARGET_H */

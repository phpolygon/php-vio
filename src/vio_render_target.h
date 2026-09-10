/*
 * php-vio - Render target (offscreen FBO) management
 */

#ifndef VIO_RENDER_TARGET_H
#define VIO_RENDER_TARGET_H

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "php.h"
#include <stdint.h>
#include "../include/vio_types.h"

struct _vio_backend;

typedef struct _vio_render_target_object {
    /* OpenGL */
    unsigned int fbo;
    unsigned int color_texture;
    unsigned int depth_texture;
    /* MSAA (samples > 1, GAP-PLAN Phase 3): draws go into gl_msaa_fbo
     * (multisample renderbuffers) and are resolved into fbo / color_texture by
     * glBlitFramebuffer on unbind / readback. */
    unsigned int gl_msaa_fbo;
    unsigned int gl_msaa_color_rb;
    unsigned int gl_msaa_depth_rb;
    int          gl_msaa_dirty;

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
    /* Cube targets: one RTV per (face, mip) — ID3D11RenderTargetView*[6 * mip_levels],
     * index face * mip_levels + level. d3d11_rtv is NULL for cube targets. */
    void        *d3d11_face_rtvs;
    /* MSAA (samples > 1): the multisampled colour texture rendered into;
     * d3d11_color_tex is then the single-sample RESOLVE texture the SRV reads. */
    void        *d3d11_msaa_color_tex;        /* ID3D11Texture2D* (multisampled) */
    void        *d3d11_msaa_depth_tex;        /* ID3D11Texture2D* (multisampled) */
    int          d3d11_msaa_dirty;            /* 1 => resolve needed before sampling / readback */

    /* D3D12 (opaque pointers — actual types are ID3D12Resource* etc.) */
    void        *d3d12_color_resource;  /* ID3D12Resource* */
    void        *d3d12_depth_resource;  /* ID3D12Resource* */
    void        *d3d12_rtv_heap;        /* ID3D12DescriptorHeap* (RTVs: attachments, then the resolve targets when MSAA) */
    void        *d3d12_dsv_heap;        /* ID3D12DescriptorHeap* (1 DSV) */
    /* MSAA (samples > 1, GAP-PHASE5 Block 1): the multisampled colour resources the
     * RTVs point at; d3d12_color_resource(s) are then the single-sample RESOLVE
     * targets the SRVs / readback read. The PSO picks its SampleDesc variant from
     * the bound target's count (vio_d3d12_pipeline.pso_ms). */
    void        *d3d12_msaa_color_resources[VIO_MAX_COLOR_ATTACHMENTS]; /* ID3D12Resource* */
    int          d3d12_msaa_dirty;      /* 1 => drawn into since the last resolve */

    /* Metal (opaque pointers — actual types are id<MTLTexture> CFBridgeRetained).
     * Stored as opaque void * so the public header doesn't pull in Metal
     * headers. The Metal backend transitions ownership across this boundary
     * via CFBridgingRetain / CFBridgingRelease. */
    void        *metal_color_texture;   /* id<MTLTexture> (CFRetained) */
    void        *metal_depth_texture;   /* id<MTLTexture> (CFRetained) */
    /* Cached vio_metal_texture wrappers handed out by vio_render_target_texture()
     * so a 3D shader can sample the RT (same cache-the-wrapper lifecycle as the
     * D3D11 pair; freed in metal_destroy_render_target). */
    void        *metal_color_backend_texture; /* vio_metal_texture* */
    void        *metal_depth_backend_texture; /* vio_metal_texture* */
    /* MSAA (samples > 1): the multisample attachments actually rendered into;
     * metal_color_texture is then the single-sample RESOLVE texture. */
    void        *metal_msaa_color_texture; /* id<MTLTexture> 2DMultisample (CFRetained) */
    void        *metal_msaa_depth_texture; /* id<MTLTexture> 2DMultisample (CFRetained) */

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

    /* Multiple render targets (VIO_FEATURE_MRT). attachment_count is 1 for a
     * classic target; formats[i] is the vio_pixel_format of colour attachment
     * i. The per-backend *_s arrays hold ALL attachments — index 0 duplicates
     * the legacy scalar field above it (color_texture, metal_color_texture,
     * d3d11_rtv, d3d12_color_resource, ...), so single-target code keeps
     * using the scalar and MRT code loops over the array. Destructors release
     * index 0 through the scalar and indices >= 1 through the array. */
    int          attachment_count;
    int          formats[VIO_MAX_COLOR_ATTACHMENTS];
    unsigned int color_textures[VIO_MAX_COLOR_ATTACHMENTS];              /* GL */
    void        *metal_color_textures[VIO_MAX_COLOR_ATTACHMENTS];        /* id<MTLTexture> */
    void        *metal_msaa_color_textures[VIO_MAX_COLOR_ATTACHMENTS];   /* id<MTLTexture> 2DMultisample */
    void        *metal_color_backend_textures[VIO_MAX_COLOR_ATTACHMENTS];/* vio_metal_texture* wrappers */
    void        *d3d11_rtvs[VIO_MAX_COLOR_ATTACHMENTS];                  /* ID3D11RenderTargetView* */
    void        *d3d11_color_texs[VIO_MAX_COLOR_ATTACHMENTS];            /* ID3D11Texture2D* */
    void        *d3d11_color_srvs[VIO_MAX_COLOR_ATTACHMENTS];            /* ID3D11ShaderResourceView* */
    void        *d3d11_color_backend_textures[VIO_MAX_COLOR_ATTACHMENTS];/* vio_d3d11_texture* */
    void        *d3d12_color_resources[VIO_MAX_COLOR_ATTACHMENTS];       /* ID3D12Resource* */
    uint64_t     d3d12_color_srv_gpus[VIO_MAX_COLOR_ATTACHMENTS];
    uint64_t     d3d12_color_srv_cpus[VIO_MAX_COLOR_ATTACHMENTS];
    void        *d3d12_color_backend_textures[VIO_MAX_COLOR_ATTACHMENTS];/* vio_d3d12_texture* */

    /* Common */
    int          width;
    int          height;
    int          depth_only;
    int          is_cube;             /* 1 => colour attachment is a cubemap (width == height == face size) */
    int          mip_levels;          /* 1, or floor(log2(size)) + 1 when created with 'mipmaps' */
    int          bound_face;          /* cube: face currently bound as colour attachment (-1 = none) */
    int          bound_level;         /* cube: mip level currently bound */
    int          samples;             /* requested by vio_render_target(); backends clamp to what
                                         they support and write the effective count back (1 = off) */
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
    unsigned int gl_generation;   /* OpenGL: context generation that owns the GL names (vio_opengl.c) */
    zend_object  std;
} vio_render_target_object;

#define VIO_RT_BACKEND_NONE   0
#define VIO_RT_BACKEND_OPENGL 1
#define VIO_RT_BACKEND_D3D11  2
#define VIO_RT_BACKEND_D3D12  3
#define VIO_RT_BACKEND_METAL  4
#define VIO_RT_BACKEND_VULKAN 5

/* Bytes per pixel of a vio_pixel_format as stored by the GPU backends
 * (RGBA8 = 4, RGBA16F = 8, RGBA32F = 16, R11G11B10F = 4, RG16F = 4, R16F = 2,
 * R32F = 4, R8 = 1). */
int  vio_rt_format_bpp(int format);

/* Convert w*h pixels of `format` (row pitch src_pitch bytes) into top-down
 * RGBA8. Floats are clamped to [0,1]; missing channels read 0 (G/B) and 1 (A).
 * bgra = 1 for 8-bit sources stored B,G,R,A (Metal's BGRA8Unorm targets). */
/* Expand packed R10G10B10A2 pixels to RGBA8 in place (count pixels of 4 bytes). */
void vio_rt_rgb10a2_to_rgba8_inplace(unsigned char *buf, size_t count);
void vio_rt_convert_to_rgba8(int format, int bgra, const void *src, size_t src_pitch,
                             int w, int h, unsigned char *out);

extern zend_class_entry *vio_render_target_ce;

void vio_render_target_register(void);

static inline vio_render_target_object *vio_render_target_from_obj(zend_object *obj) {
    return (vio_render_target_object *)((char *)obj - XtOffsetOf(vio_render_target_object, std));
}

#define Z_VIO_RENDER_TARGET_P(zv) vio_render_target_from_obj(Z_OBJ_P(zv))

#endif /* VIO_RENDER_TARGET_H */

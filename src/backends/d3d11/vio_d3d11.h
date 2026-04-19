/*
 * php-vio - Direct3D 11 Backend
 */

#ifndef VIO_D3D11_H
#define VIO_D3D11_H

#include "../../../include/vio_backend.h"

#ifdef HAVE_D3D11

#include <d3d11.h>
#include <dxgi1_2.h>
#include <d3dcompiler.h>

/* Compiled shader pair (vertex + pixel) */
typedef struct _vio_d3d11_shader {
    ID3DBlob *vs_blob;
    ID3DBlob *ps_blob;
    ID3D11VertexShader *vs;
    ID3D11PixelShader  *ps;
} vio_d3d11_shader;

/* Pipeline = input layout + state objects */
typedef struct _vio_d3d11_pipeline {
    ID3D11InputLayout       *input_layout;
    ID3D11RasterizerState   *rasterizer_state;
    ID3D11DepthStencilState *depth_stencil_state;
    ID3D11BlendState        *blend_state;
    ID3D11VertexShader      *vs;
    ID3D11PixelShader       *ps;
    D3D11_PRIMITIVE_TOPOLOGY topology;
    UINT                     vertex_stride;
} vio_d3d11_pipeline;

/* Buffer wrapper */
typedef struct _vio_d3d11_buffer {
    ID3D11Buffer *buffer;
    vio_buffer_type type;
    size_t size;
    int binding;
} vio_d3d11_buffer;

/* Texture wrapper */
typedef struct _vio_d3d11_texture {
    ID3D11Texture2D          *texture;
    ID3D11ShaderResourceView *srv;
    ID3D11SamplerState       *sampler;       /* regular sampler (Sample) */
    ID3D11SamplerState       *sampler_cmp;   /* comparison sampler (SampleCmp), NULL if n/a */
    int width;
    int height;
    int is_depth;                             /* 1 if this is a depth texture */
} vio_d3d11_texture;

/* Cached constant buffer for set_uniform (avoids per-call CreateBuffer) */
#define VIO_D3D11_CB_CACHE_SLOTS 8

typedef struct _vio_d3d11_cb_cache_entry {
    ID3D11Buffer *buffer;
    UINT          capacity;   /* allocated ByteWidth */
} vio_d3d11_cb_cache_entry;

/* Global D3D11 state */
typedef struct _vio_d3d11_state {
    /* Device & context */
    ID3D11Device           *device;
    ID3D11DeviceContext    *context;
    D3D_FEATURE_LEVEL       feature_level;

    /* DXGI */
    IDXGISwapChain1        *swapchain;
    IDXGIFactory2          *factory;

    /* Render targets */
    ID3D11RenderTargetView *rtv;
    ID3D11DepthStencilView *dsv;
    ID3D11Texture2D        *depth_buffer;

    /* Currently bound render target (NULL = backbuffer) */
    ID3D11RenderTargetView *current_rtv;
    ID3D11DepthStencilView *current_dsv;
    int current_rt_width;
    int current_rt_height;

    /* Identity instance buffer bound to input slot 1 for non-instanced draws.
     * SPIRV-Cross generates HLSL where `layout(location=3..6) vec4` attributes
     * are treated as per-instance data in slot 1. For non-instanced draws the
     * shader still reads from slot 1; without this fallback the reads return
     * undefined memory and the geometry renders at garbage positions
     * (typically off-screen or collapsed to the origin). */
    ID3D11Buffer *identity_instance_buf;

    /* Persistent readback staging for vio_read_pixels.
     * FLIP_DISCARD swapchains lose backbuffer content after Present(), so
     * end_frame() eagerly copies the rendered backbuffer into this staging
     * texture. vio_read_pixels maps from here instead of the live RTV. */
    ID3D11Texture2D *readback_staging;
    UINT             readback_w;
    UINT             readback_h;

    /* State */
    int   initialized;
    float clear_r, clear_g, clear_b, clear_a;
    int   width, height;
    int   vsync;

    /* Constant buffer cache for set_uniform (per-slot, reused across frames) */
    vio_d3d11_cb_cache_entry cb_cache[VIO_D3D11_CB_CACHE_SLOTS];

    /* Debug */
    ID3D11Debug *debug_interface;
    int          debug_enabled;

    /* Window reference */
    void *glfw_window;
} vio_d3d11_state;

extern vio_d3d11_state vio_d3d11;

/* Registration */
void vio_backend_d3d11_register(void);

/* Called after GLFW window creation to set up D3D11 */
int vio_d3d11_setup_context(void *glfw_window, vio_config *cfg);

#endif /* HAVE_D3D11 */
#endif /* VIO_D3D11_H */

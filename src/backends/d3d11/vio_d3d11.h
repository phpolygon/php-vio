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
    ID3D11SamplerState       *sampler;
    int width;
    int height;
} vio_d3d11_texture;

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

    /* State */
    int   initialized;
    float clear_r, clear_g, clear_b, clear_a;
    int   width, height;
    int   vsync;

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

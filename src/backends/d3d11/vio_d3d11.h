/*
 * php-vio - Direct3D 11 Backend
 */

#ifndef VIO_D3D11_H
#define VIO_D3D11_H

#include "../../../include/vio_backend.h"

#ifdef HAVE_D3D11

#include <d3d11.h>
#include <dxgi1_2.h>
/* dxgi1_5.h: IDXGIFactory5 + DXGI_FEATURE_PRESENT_ALLOW_TEARING, needed to
 * query variable-refresh / uncapped-present support. Ships in every Windows 10
 * SDK that also has d3d12.h, which config.w32 already requires. */
#include <dxgi1_5.h>
#include <d3dcompiler.h>

/* Present flag is a #define in dxgi.h; guard for very old SDKs. The swapchain
 * flag (DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING = 2048) is an enum member and comes
 * from the same header generation as dxgi1_5.h, so it needs no fallback. */
#ifndef DXGI_PRESENT_ALLOW_TEARING
#define DXGI_PRESENT_ALLOW_TEARING 0x00000200UL
#endif

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
    int stride;                  /* structured element stride (bytes); <=4 => raw */
    /* Compute readback: a STAGING (CPU-readable) copy of a UAV output buffer that
     * dispatch_compute fills (CopyResource) after the dispatch, so
     * vio_storage_buffer_read can Map+memcpy without re-running the GPU. Lazily
     * created on first dispatch that writes this buffer. */
    ID3D11Buffer *readback_staging;
    size_t        readback_size; /* allocated bytes of readback_staging */
} vio_d3d11_buffer;

/* Max storage-buffer bindings per compute pipeline (SRV t# + UAV u#). */
#define VIO_D3D11_COMPUTE_MAX_BINDINGS 8

/* One recorded storage-buffer binding on a compute pipeline. */
typedef struct _vio_d3d11_compute_binding {
    struct _vio_d3d11_buffer *buffer;  /* the bound storage buffer */
    int slot;                          /* shader register index (t# or u#) */
    int access;                        /* 0 = READ (SRV), 1 = WRITE (UAV) */
    int element_count;                 /* NumElements for the structured view */
    int stride;                        /* StructureByteStride (<=4 => raw) */
} vio_d3d11_compute_binding;

/* Compute pipeline = compute shader + recorded bindings + params constant buffer.
 *
 * Unlike D3D12 there is no root signature: in D3D11 you bind directly by register
 * slot (CSSetShaderResources / CSSetUnorderedAccessViews / CSSetConstantBuffers),
 * and the HLSL register number IS the GLSL binding number (spirv-cross maps
 * binding=N -> tN / uN / bN). The CBV register is read from SPIR-V reflection
 * (the Params UBO binding) exactly like the D3D12 path. */
typedef struct _vio_d3d11_compute_pipeline {
    ID3DBlob           *cs_blob;
    ID3D11ComputeShader *cs;
    vio_d3d11_compute_binding srvs[VIO_D3D11_COMPUTE_MAX_BINDINGS];
    int                 srv_count;
    vio_d3d11_compute_binding uavs[VIO_D3D11_COMPUTE_MAX_BINDINGS];
    int                 uav_count;
    /* Reflected register: the Params UBO's HLSL register (b#); -1 if no UBO. */
    int                 cbv_register;
    /* Params constant block — a USAGE_DEFAULT CB, updated via UpdateSubresource
     * in compute_set_uniforms and bound at cbv_register. */
    ID3D11Buffer       *params_buf;
    size_t              params_capacity; /* allocated bytes (16-aligned) */
} vio_d3d11_compute_pipeline;

/* Texture wrapper */
typedef struct _vio_d3d11_texture {
    ID3D11Texture2D          *texture;
    ID3D11Texture3D          *texture3d;     /* set instead of texture for volume textures */
    ID3D11ShaderResourceView *srv;
    ID3D11SamplerState       *sampler;       /* regular sampler (Sample) */
    ID3D11SamplerState       *sampler_cmp;   /* comparison sampler (SampleCmp), NULL if n/a */
    int width;
    int height;
    int depth;                                /* > 0 for 3D / volume textures */
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

    /* Tearing support (DXGI flag contract + VRR).
     *
     * NOT a throughput fix. Measured on a 144Hz display: without these flags a
     * windowed FLIP_DISCARD swapchain at SyncInterval=0 already presents at
     * thousands of fps. DWM does not block Present(0,0) — it simply drops frames
     * it never shows. Do not re-add a comment claiming otherwise.
     *
     * What the flags do buy:
     *   - VRR (G-Sync / FreeSync) does not engage on a flip-model swapchain
     *     without DXGI_PRESENT_ALLOW_TEARING; the display stays at a fixed rate.
     *   - Scan-out latency: a tear-allowed present hands the frame to the display
     *     immediately rather than at the next vblank boundary.
     *
     * Both halves are required and must agree:
     *   - DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING at creation AND on every
     *     ResizeBuffers (the flag set must match, or the resize strips it), and
     *   - DXGI_PRESENT_ALLOW_TEARING in the Present() flags argument.
     *
     * tearing_supported is the IDXGIFactory5 CheckFeatureSupport result, cached
     * once at init. swapchain_flags is the exact flag set the swapchain was
     * created with — ResizeBuffers MUST be given this same value. */
    int   tearing_supported;   /* DXGI_FEATURE_PRESENT_ALLOW_TEARING */
    UINT  swapchain_flags;     /* DXGI_SWAP_CHAIN_FLAG_* used at creation */
    int   present_failed_once; /* rate-limits the Present() failure warning */

    /* Render targets */
    ID3D11RenderTargetView *rtv;
    ID3D11DepthStencilView *dsv;
    ID3D11Texture2D        *depth_buffer;

    /* Currently bound render target (NULL = backbuffer) */
    ID3D11RenderTargetView *current_rtv;
    ID3D11DepthStencilView *current_dsv;
    int current_rt_width;
    int current_rt_height;

    /* vio_render_target_object* requested via vio_bind_render_target while
     * out of frame (before vio_begin — the warm-render "bind then begin"
     * order). D3D11 is immediate-mode, so the bind COULD be applied at once,
     * but d3d11_begin_frame() unconditionally resets current_rtv = rtv every
     * frame and would clobber a pre-begin bind. Deferring it lets vio_begin()
     * re-apply the bind AFTER begin_frame(), so the offscreen redirect
     * survives. NULL on a normal frame (strict no-op). Mirrors
     * vio_d3d12.pending_bound_rt. */
    void *pending_bound_rt;

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

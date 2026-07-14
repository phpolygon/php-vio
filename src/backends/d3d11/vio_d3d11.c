/*
 * php-vio - Direct3D 11 Backend implementation
 *
 * Uses D3D11 immediate-mode rendering with DXGI 1.2 swapchain.
 * Shaders: GLSL -> SPIR-V -> HLSL (SM 5.0) via SPIRV-Cross -> DXBC via D3DCompile.
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "php.h"

#ifdef HAVE_D3D11

#define COBJMACROS
#define INITGUID
#include <d3d11.h>
#include <dxgi1_2.h>
#include <d3dcompiler.h>

#ifdef HAVE_GLFW
#define GLFW_INCLUDE_NONE
#define GLFW_EXPOSE_NATIVE_WIN32
#include <GLFW/glfw3.h>
#include <GLFW/glfw3native.h>
#endif

#include "vio_d3d11.h"
#include "../vio_d3d_common.h"
#include "../../vio_shader_reflect.h"
#include <string.h>
#include <stdlib.h>

vio_d3d11_state vio_d3d11 = {0};

/* Currently bound pipeline (for vertex stride in draw calls) */
static vio_d3d11_pipeline *d3d11_current_pipeline = NULL;

/* Forward declarations for SPIRV-Cross HLSL transpilation (in vio_shader_reflect.c) */
extern char *vio_spirv_to_hlsl(const uint32_t *spirv, size_t spirv_size,
                                int shader_model, char **error_msg);
/* Forward declaration for GLSL -> SPIR-V (in vio_shader_compiler.c) */
extern uint32_t *vio_compile_glsl_to_spirv(const char *source, int stage,
                                            size_t *out_size, char **error_msg);
/* Forward declaration for GLSL compute -> SPIR-V (in vio_shader_compiler.c) */
extern uint32_t *vio_compile_glsl_compute_to_spirv(const char *source,
                                                   size_t *out_size, char **error_msg);

/* ── Helpers ──────────────────────────────────────────────────────── */
/* vio_format_to_dxgi, vio_format_byte_size, vio_usage_to_semantic from vio_d3d_common.h */

static D3D11_PRIMITIVE_TOPOLOGY vio_topology_to_d3d11(vio_topology t)
{
    switch (t) {
        case VIO_TRIANGLES:      return D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
        case VIO_TRIANGLE_STRIP: return D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP;
        case VIO_LINES:          return D3D11_PRIMITIVE_TOPOLOGY_LINELIST;
        case VIO_LINE_STRIP:     return D3D11_PRIMITIVE_TOPOLOGY_LINESTRIP;
        case VIO_POINTS:         return D3D11_PRIMITIVE_TOPOLOGY_POINTLIST;
        case VIO_TRIANGLE_FAN:   return D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST; /* no native fan */
        default:                 return D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
    }
}

/* ── Create render target & depth views ───────────────────────────── */

static int d3d11_create_views(void)
{
    HRESULT hr;
    ID3D11Texture2D *back_buffer = NULL;

    /* Get back buffer from swapchain */
    hr = IDXGISwapChain1_GetBuffer(vio_d3d11.swapchain, 0,
                                    &IID_ID3D11Texture2D, (void **)&back_buffer);
    if (FAILED(hr)) {
        php_error_docref(NULL, E_WARNING, "D3D11: Failed to get back buffer (0x%08lx)", hr);
        return -1;
    }

    hr = ID3D11Device_CreateRenderTargetView(vio_d3d11.device,
                                              (ID3D11Resource *)back_buffer,
                                              NULL, &vio_d3d11.rtv);
    ID3D11Texture2D_Release(back_buffer);
    if (FAILED(hr)) {
        php_error_docref(NULL, E_WARNING, "D3D11: Failed to create RTV (0x%08lx)", hr);
        return -1;
    }

    /* Create depth-stencil buffer */
    D3D11_TEXTURE2D_DESC depth_desc = {0};
    depth_desc.Width = vio_d3d11.width;
    depth_desc.Height = vio_d3d11.height;
    depth_desc.MipLevels = 1;
    depth_desc.ArraySize = 1;
    depth_desc.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
    depth_desc.SampleDesc.Count = 1;
    depth_desc.SampleDesc.Quality = 0;
    depth_desc.Usage = D3D11_USAGE_DEFAULT;
    depth_desc.BindFlags = D3D11_BIND_DEPTH_STENCIL;

    hr = ID3D11Device_CreateTexture2D(vio_d3d11.device, &depth_desc,
                                       NULL, &vio_d3d11.depth_buffer);
    if (FAILED(hr)) {
        php_error_docref(NULL, E_WARNING, "D3D11: Failed to create depth buffer (0x%08lx)", hr);
        return -1;
    }

    hr = ID3D11Device_CreateDepthStencilView(vio_d3d11.device,
                                              (ID3D11Resource *)vio_d3d11.depth_buffer,
                                              NULL, &vio_d3d11.dsv);
    if (FAILED(hr)) {
        php_error_docref(NULL, E_WARNING, "D3D11: Failed to create DSV (0x%08lx)", hr);
        return -1;
    }

    return 0;
}

static void d3d11_release_views(void)
{
    if (vio_d3d11.rtv) {
        ID3D11RenderTargetView_Release(vio_d3d11.rtv);
        vio_d3d11.rtv = NULL;
    }
    if (vio_d3d11.dsv) {
        ID3D11DepthStencilView_Release(vio_d3d11.dsv);
        vio_d3d11.dsv = NULL;
    }
    if (vio_d3d11.depth_buffer) {
        ID3D11Texture2D_Release(vio_d3d11.depth_buffer);
        vio_d3d11.depth_buffer = NULL;
    }
    /* Mirror + staging must be recreated at the new size after a swapchain resize.
     * Both are dropped together so they can never disagree on dimensions. */
    if (vio_d3d11.readback_mirror) {
        ID3D11Texture2D_Release(vio_d3d11.readback_mirror);
        vio_d3d11.readback_mirror = NULL;
    }
    if (vio_d3d11.readback_staging) {
        ID3D11Texture2D_Release(vio_d3d11.readback_staging);
        vio_d3d11.readback_staging = NULL;
    }
    vio_d3d11.readback_w = 0;
    vio_d3d11.readback_h = 0;
}

/* ── Lifecycle ────────────────────────────────────────────────────── */

static int d3d11_init(vio_config *cfg)
{
    HRESULT hr;
    UINT create_flags = 0;

    if (cfg->debug) {
        create_flags |= D3D11_CREATE_DEVICE_DEBUG;
        vio_d3d11.debug_enabled = 1;
    }

    D3D_FEATURE_LEVEL requested_levels[] = {
        D3D_FEATURE_LEVEL_11_1,
        D3D_FEATURE_LEVEL_11_0,
    };

    /* Use WARP if headless (software renderer, no GPU needed) */
    D3D_DRIVER_TYPE driver_type = cfg->headless
        ? D3D_DRIVER_TYPE_WARP
        : D3D_DRIVER_TYPE_HARDWARE;

    hr = D3D11CreateDevice(
        NULL,                           /* adapter (NULL = default) */
        driver_type,
        NULL,                           /* software module */
        create_flags,
        requested_levels, 2,
        D3D11_SDK_VERSION,
        &vio_d3d11.device,
        &vio_d3d11.feature_level,
        &vio_d3d11.context
    );

    if (FAILED(hr)) {
        php_error_docref(NULL, E_WARNING, "D3D11: Failed to create device (0x%08lx)", hr);
        return -1;
    }

    /* Debug interface */
    if (vio_d3d11.debug_enabled) {
        ID3D11Device_QueryInterface(vio_d3d11.device, &IID_ID3D11Debug,
                                     (void **)&vio_d3d11.debug_interface);
    }

    /* Create DXGI factory */
    hr = CreateDXGIFactory1(&IID_IDXGIFactory2, (void **)&vio_d3d11.factory);
    if (FAILED(hr)) {
        php_error_docref(NULL, E_WARNING, "D3D11: Failed to create DXGI factory (0x%08lx)", hr);
        return -1;
    }

    /* Probe DXGI_FEATURE_PRESENT_ALLOW_TEARING once. Requires IDXGIFactory5
     * (Windows 10 1511+); on older DXGI the QI simply fails and we keep the
     * vsync-throttled behaviour. Never assume the feature — a driver/OS without
     * it will fail Present() outright if the flag is passed. */
    {
        IDXGIFactory5 *factory5 = NULL;
        if (SUCCEEDED(IDXGIFactory2_QueryInterface(vio_d3d11.factory,
                                                   &IID_IDXGIFactory5,
                                                   (void **)&factory5))) {
            BOOL allow_tearing = FALSE;
            HRESULT thr = IDXGIFactory5_CheckFeatureSupport(
                factory5,
                DXGI_FEATURE_PRESENT_ALLOW_TEARING,
                &allow_tearing,
                sizeof(allow_tearing));
            /* CheckFeatureSupport can succeed and still leave the out-param
             * untouched on some drivers — require both. */
            vio_d3d11.tearing_supported = (SUCCEEDED(thr) && allow_tearing) ? 1 : 0;
            IDXGIFactory5_Release(factory5);
        }
    }

    /* Identity 4x4 matrix used as a dummy per-instance vertex buffer for
     * non-instanced draws. SPIRV-Cross'ed shaders reference vertex slot 1
     * for mat4 instance columns (locations 3..6); binding this buffer to
     * slot 1 makes the shader read identity for those attributes. */
    {
        const float identity[16] = {
            1.0f, 0.0f, 0.0f, 0.0f,
            0.0f, 1.0f, 0.0f, 0.0f,
            0.0f, 0.0f, 1.0f, 0.0f,
            0.0f, 0.0f, 0.0f, 1.0f,
        };
        D3D11_BUFFER_DESC ib_desc = {0};
        ib_desc.ByteWidth = sizeof(identity);
        ib_desc.Usage = D3D11_USAGE_IMMUTABLE;
        ib_desc.BindFlags = D3D11_BIND_VERTEX_BUFFER;
        D3D11_SUBRESOURCE_DATA ib_data = { .pSysMem = identity };
        ID3D11Device_CreateBuffer(vio_d3d11.device, &ib_desc, &ib_data,
                                   &vio_d3d11.identity_instance_buf);
    }

    vio_d3d11.vsync = cfg->vsync;
    vio_d3d11.initialized = 1;
    return 0;
}

static void d3d11_shutdown(void)
{
    if (!vio_d3d11.initialized) return;

    /* Ensure GPU is idle before releasing */
    if (vio_d3d11.context) {
        ID3D11DeviceContext_ClearState(vio_d3d11.context);
        ID3D11DeviceContext_Flush(vio_d3d11.context);
    }

    if (vio_d3d11.identity_instance_buf) {
        ID3D11Buffer_Release(vio_d3d11.identity_instance_buf);
        vio_d3d11.identity_instance_buf = NULL;
    }

    /* Release cached constant buffers */
    for (int i = 0; i < VIO_D3D11_CB_CACHE_SLOTS; i++) {
        if (vio_d3d11.cb_cache[i].buffer) {
            ID3D11Buffer_Release(vio_d3d11.cb_cache[i].buffer);
            vio_d3d11.cb_cache[i].buffer = NULL;
            vio_d3d11.cb_cache[i].capacity = 0;
        }
    }

    d3d11_release_views();

    if (vio_d3d11.swapchain) {
        IDXGISwapChain1_Release(vio_d3d11.swapchain);
        vio_d3d11.swapchain = NULL;
    }
    if (vio_d3d11.debug_interface) {
        ID3D11Debug_Release(vio_d3d11.debug_interface);
        vio_d3d11.debug_interface = NULL;
    }
    if (vio_d3d11.context) {
        ID3D11DeviceContext_Release(vio_d3d11.context);
        vio_d3d11.context = NULL;
    }
    if (vio_d3d11.factory) {
        IDXGIFactory2_Release(vio_d3d11.factory);
        vio_d3d11.factory = NULL;
    }
    if (vio_d3d11.device) {
        ID3D11Device_Release(vio_d3d11.device);
        vio_d3d11.device = NULL;
    }

    d3d11_current_pipeline = NULL;
    memset(&vio_d3d11, 0, sizeof(vio_d3d11));
}

/* ── Surface & Window ─────────────────────────────────────────────── */

static void *d3d11_create_surface(vio_config *cfg)
{
#ifdef HAVE_GLFW
    if (!vio_d3d11.glfw_window) {
        php_error_docref(NULL, E_WARNING, "D3D11: No GLFW window set");
        return NULL;
    }

    HWND hwnd = glfwGetWin32Window((GLFWwindow *)vio_d3d11.glfw_window);

    DXGI_SWAP_CHAIN_DESC1 sc_desc = {0};
    sc_desc.Width = cfg->width;
    sc_desc.Height = cfg->height;
    sc_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sc_desc.Stereo = FALSE;
    sc_desc.SampleDesc.Count = 1;
    sc_desc.SampleDesc.Quality = 0;
    sc_desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sc_desc.BufferCount = 2;
    sc_desc.Scaling = DXGI_SCALING_STRETCH;
    sc_desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    sc_desc.AlphaMode = DXGI_ALPHA_MODE_UNSPECIFIED;

    /* ALLOW_TEARING only when the factory reported support. Setting it blindly
     * makes CreateSwapChainForHwnd fail with DXGI_ERROR_INVALID_CALL on systems
     * that lack the feature. Cache the exact flag set: d3d11_resize() must pass
     * the identical value to ResizeBuffers or the resize fails / the swapchain
     * silently loses its tearing capability. */
    vio_d3d11.swapchain_flags = vio_d3d11.tearing_supported
        ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING
        : 0u;
    sc_desc.Flags = vio_d3d11.swapchain_flags;

    HRESULT hr = IDXGIFactory2_CreateSwapChainForHwnd(
        vio_d3d11.factory,
        (IUnknown *)vio_d3d11.device,
        hwnd,
        &sc_desc,
        NULL,  /* fullscreen desc */
        NULL,  /* restrict output */
        &vio_d3d11.swapchain
    );

    if (FAILED(hr)) {
        php_error_docref(NULL, E_WARNING, "D3D11: Failed to create swapchain (0x%08lx)", hr);
        return NULL;
    }

    /* Disable ALT+Enter fullscreen toggle. This is also a hard requirement for
     * ALLOW_TEARING: DXGI's automatic alt-enter handler would call
     * SetFullscreenState() behind our back, and a fullscreen-exclusive swapchain
     * must never be presented with DXGI_PRESENT_ALLOW_TEARING (INVALID_CALL).
     * With NO_ALT_ENTER set and no SetFullscreenState() call anywhere in this
     * backend, the swapchain is windowed for its entire lifetime — vio's
     * "fullscreen" (vio_set_fullscreen / vio_set_borderless in php_vio.c) is a
     * GLFW window/monitor change, which DXGI still sees as windowed. */
    IDXGIFactory2_MakeWindowAssociation(vio_d3d11.factory, hwnd, DXGI_MWA_NO_ALT_ENTER);

    vio_d3d11.width = cfg->width;
    vio_d3d11.height = cfg->height;

    if (d3d11_create_views() != 0) {
        return NULL;
    }

    return vio_d3d11.swapchain;
#else
    (void)cfg;
    php_error_docref(NULL, E_WARNING, "D3D11: Built without GLFW, cannot create surface");
    return NULL;
#endif
}

static void d3d11_destroy_surface(void *surface)
{
    (void)surface;
    d3d11_release_views();

    if (vio_d3d11.swapchain) {
        IDXGISwapChain1_Release(vio_d3d11.swapchain);
        vio_d3d11.swapchain = NULL;
    }
}

static void d3d11_resize(int width, int height)
{
    /* Skip no-op resizes, missing swapchain, and minimised (0x0) windows —
     * ResizeBuffers to a zero dimension fails. */
    if (!vio_d3d11.swapchain || width <= 0 || height <= 0 ||
        (width == vio_d3d11.width && height == vio_d3d11.height)) {
        return;
    }

    /* Must release all references to back buffer before resizing */
    ID3D11DeviceContext_OMSetRenderTargets(vio_d3d11.context, 0, NULL, NULL);
    d3d11_release_views();

    /* CRITICAL: the flag set passed here must be identical to the one the
     * swapchain was created with (vio_d3d11.swapchain_flags). ResizeBuffers does
     * not "keep" flags the way it keeps buffer count (0) and format (UNKNOWN) —
     * it REPLACES them. Passing 0 here on a swapchain created with
     * ALLOW_TEARING silently strips the tearing capability, and every subsequent
     * Present() with DXGI_PRESENT_ALLOW_TEARING then fails with
     * DXGI_ERROR_INVALID_CALL (frames stop reaching the screen after the first
     * window resize / fullscreen toggle). */
    HRESULT hr = IDXGISwapChain1_ResizeBuffers(vio_d3d11.swapchain,
                                                0,  /* keep buffer count */
                                                width, height,
                                                DXGI_FORMAT_UNKNOWN,  /* keep format */
                                                vio_d3d11.swapchain_flags);
    if (FAILED(hr)) {
        php_error_docref(NULL, E_WARNING, "D3D11: Failed to resize buffers (0x%08lx)", hr);
        return;
    }

    vio_d3d11.width = width;
    vio_d3d11.height = height;
    d3d11_create_views();
}

/* ── Pipeline ─────────────────────────────────────────────────────── */

static void *d3d11_create_pipeline(vio_pipeline_desc *desc)
{
    HRESULT hr;
    vio_d3d11_pipeline *pipeline = calloc(1, sizeof(vio_d3d11_pipeline));
    if (!pipeline) return NULL;

    vio_d3d11_shader *shader = (vio_d3d11_shader *)desc->shader;
    if (!shader) {
        free(pipeline);
        return NULL;
    }

    pipeline->vs = shader->vs;
    pipeline->ps = shader->ps;
    /* AddRef so pipeline survives shader destruction */
    if (pipeline->vs) ID3D11VertexShader_AddRef(pipeline->vs);
    if (pipeline->ps) ID3D11PixelShader_AddRef(pipeline->ps);
    pipeline->topology = vio_topology_to_d3d11(desc->topology);

    /* Input layout from vertex attributes.
     * Separate per-vertex (slot 0, locations 0-2) from per-instance (slot 1, locations 3-6). */
    if (desc->vertex_attrib_count > 0 && desc->vertex_layout) {
        /* Count per-vertex vs per-instance attributes */
        int per_vertex_count = 0;
        int per_instance_count = 0;
        for (int i = 0; i < desc->vertex_attrib_count; i++) {
            if (desc->vertex_layout[i].location >= 3 && desc->vertex_layout[i].location <= 6)
                per_instance_count++;
            else
                per_vertex_count++;
        }

        int total_elements = desc->vertex_attrib_count;
        D3D11_INPUT_ELEMENT_DESC *elements = calloc(total_elements,
                                                     sizeof(D3D11_INPUT_ELEMENT_DESC));
        UINT vertex_offset = 0;
        UINT instance_offset = 0;

        for (int i = 0; i < desc->vertex_attrib_count; i++) {
            int loc = desc->vertex_layout[i].location;
            elements[i].SemanticName = vio_usage_to_semantic(desc->vertex_layout[i].usage);
            elements[i].SemanticIndex = loc;
            elements[i].Format = vio_format_to_dxgi(desc->vertex_layout[i].format);

            if (loc >= 3 && loc <= 6) {
                /* Per-instance attribute (mat4 columns) — InputSlot 1 */
                elements[i].InputSlot = 1;
                elements[i].AlignedByteOffset = (loc - 3) * 16;  /* 4 floats per column */
                elements[i].InputSlotClass = D3D11_INPUT_PER_INSTANCE_DATA;
                elements[i].InstanceDataStepRate = 1;
            } else {
                /* Per-vertex attribute — InputSlot 0 */
                elements[i].InputSlot = 0;
                elements[i].AlignedByteOffset = vertex_offset;
                elements[i].InputSlotClass = D3D11_INPUT_PER_VERTEX_DATA;
                elements[i].InstanceDataStepRate = 0;
                vertex_offset += vio_format_byte_size(desc->vertex_layout[i].format);
            }
        }
        pipeline->vertex_stride = vertex_offset;

        hr = ID3D11Device_CreateInputLayout(vio_d3d11.device,
                                             elements, total_elements,
                                             ID3D10Blob_GetBufferPointer(shader->vs_blob),
                                             ID3D10Blob_GetBufferSize(shader->vs_blob),
                                             &pipeline->input_layout);
        free(elements);
        if (FAILED(hr)) {
            php_error_docref(NULL, E_WARNING, "D3D11: Failed to create input layout (0x%08lx)", hr);
            free(pipeline);
            return NULL;
        }
    }

    /* Rasterizer state */
    D3D11_RASTERIZER_DESC raster_desc = {0};
    raster_desc.FillMode = D3D11_FILL_SOLID;
    switch (desc->cull_mode) {
        case VIO_CULL_NONE:  raster_desc.CullMode = D3D11_CULL_NONE; break;
        case VIO_CULL_BACK:  raster_desc.CullMode = D3D11_CULL_BACK; break;
        case VIO_CULL_FRONT: raster_desc.CullMode = D3D11_CULL_FRONT; break;
    }
    raster_desc.FrontCounterClockwise = TRUE;  /* Match OpenGL/Vulkan winding */
    raster_desc.DepthClipEnable = TRUE;
    raster_desc.DepthBias = (INT)desc->depth_bias;
    raster_desc.SlopeScaledDepthBias = desc->slope_scaled_depth_bias;
    raster_desc.DepthBiasClamp = 0.0f;

    ID3D11Device_CreateRasterizerState(vio_d3d11.device, &raster_desc,
                                        &pipeline->rasterizer_state);

    /* Depth-stencil state */
    D3D11_DEPTH_STENCIL_DESC ds_desc = {0};
    ds_desc.DepthEnable = desc->depth_test ? TRUE : FALSE;
    ds_desc.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
    ds_desc.DepthFunc = (desc->depth_func == VIO_DEPTH_LEQUAL)
        ? D3D11_COMPARISON_LESS_EQUAL
        : D3D11_COMPARISON_LESS;

    ID3D11Device_CreateDepthStencilState(vio_d3d11.device, &ds_desc,
                                          &pipeline->depth_stencil_state);

    /* Blend state */
    D3D11_BLEND_DESC blend_desc = {0};
    blend_desc.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;

    if (desc->blend == VIO_BLEND_ALPHA) {
        blend_desc.RenderTarget[0].BlendEnable = TRUE;
        blend_desc.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA;
        blend_desc.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
        blend_desc.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
        blend_desc.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
        blend_desc.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
        blend_desc.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
    } else if (desc->blend == VIO_BLEND_ADDITIVE) {
        blend_desc.RenderTarget[0].BlendEnable = TRUE;
        blend_desc.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA;
        blend_desc.RenderTarget[0].DestBlend = D3D11_BLEND_ONE;
        blend_desc.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
        blend_desc.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
        blend_desc.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ONE;
        blend_desc.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
    }

    ID3D11Device_CreateBlendState(vio_d3d11.device, &blend_desc, &pipeline->blend_state);

    return pipeline;
}

static void d3d11_destroy_pipeline(void *pipeline_ptr)
{
    vio_d3d11_pipeline *p = (vio_d3d11_pipeline *)pipeline_ptr;
    if (!p) return;

    if (d3d11_current_pipeline == p) d3d11_current_pipeline = NULL;
    if (p->vs)                 ID3D11VertexShader_Release(p->vs);
    if (p->ps)                 ID3D11PixelShader_Release(p->ps);
    if (p->input_layout)       ID3D11InputLayout_Release(p->input_layout);
    if (p->rasterizer_state)   ID3D11RasterizerState_Release(p->rasterizer_state);
    if (p->depth_stencil_state) ID3D11DepthStencilState_Release(p->depth_stencil_state);
    if (p->blend_state)        ID3D11BlendState_Release(p->blend_state);
    free(p);
}

static void d3d11_bind_pipeline(void *pipeline_ptr)
{
    vio_d3d11_pipeline *p = (vio_d3d11_pipeline *)pipeline_ptr;
    if (!p) return;

    d3d11_current_pipeline = p;
    ID3D11DeviceContext_IASetInputLayout(vio_d3d11.context, p->input_layout);
    ID3D11DeviceContext_IASetPrimitiveTopology(vio_d3d11.context, p->topology);
    ID3D11DeviceContext_VSSetShader(vio_d3d11.context, p->vs, NULL, 0);
    ID3D11DeviceContext_PSSetShader(vio_d3d11.context, p->ps, NULL, 0);
    ID3D11DeviceContext_RSSetState(vio_d3d11.context, p->rasterizer_state);
    ID3D11DeviceContext_OMSetDepthStencilState(vio_d3d11.context, p->depth_stencil_state, 0);

    float blend_factor[4] = {0, 0, 0, 0};
    ID3D11DeviceContext_OMSetBlendState(vio_d3d11.context, p->blend_state, blend_factor, 0xFFFFFFFF);
}

/* ── Resources: Buffers ───────────────────────────────────────────── */

static void *d3d11_create_buffer(vio_buffer_desc *desc)
{
    vio_d3d11_buffer *buf = calloc(1, sizeof(vio_d3d11_buffer));
    if (!buf) return NULL;

    buf->type = desc->type;
    buf->size = desc->size;
    buf->binding = desc->binding;
    buf->stride = desc->stride;

    D3D11_BUFFER_DESC bd = {0};
    bd.ByteWidth = (UINT)desc->size;
    bd.Usage = D3D11_USAGE_DEFAULT;
    bd.CPUAccessFlags = 0;

    switch (desc->type) {
        case VIO_BUFFER_VERTEX:
            bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
            break;
        case VIO_BUFFER_INDEX:
            bd.BindFlags = D3D11_BIND_INDEX_BUFFER;
            break;
        case VIO_BUFFER_UNIFORM:
            bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
            bd.Usage = D3D11_USAGE_DYNAMIC;
            bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
            /* Constant buffers must be 16-byte aligned */
            bd.ByteWidth = (bd.ByteWidth + 15) & ~15;
            break;
        case VIO_BUFFER_STORAGE:
            /* Data-driven raw vs structured view, mirroring the D3D12 path.
             * spirv-cross transpiles GLSL `buffer { float x[]; }` to a (RW)Byte-
             * AddressBuffer — a RAW view — NOT a StructuredBuffer. So stride <= 4
             * (the default for the SDF baker) requests ALLOW_RAW_VIEWS and the
             * SRV/UAV are built as BUFFEREX/BUFFER raw views in dispatch_compute.
             * stride > 4 keeps the legacy structured view. Both bindable as SRV
             * (input) and UAV (output) so one path covers read+write buffers. */
            bd.BindFlags = D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_SHADER_RESOURCE;
            if (desc->stride > 4) {
                bd.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
                bd.StructureByteStride = (UINT)desc->stride;
            } else {
                /* Raw byte-address view: ByteWidth must be a multiple of 4. */
                bd.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS;
                bd.ByteWidth = (bd.ByteWidth + 3u) & ~3u;
                buf->size = bd.ByteWidth;
            }
            break;
    }

    D3D11_SUBRESOURCE_DATA init_data = {0};
    D3D11_SUBRESOURCE_DATA *init_ptr = NULL;
    if (desc->data) {
        init_data.pSysMem = desc->data;
        init_ptr = &init_data;
    }

    HRESULT hr = ID3D11Device_CreateBuffer(vio_d3d11.device, &bd, init_ptr, &buf->buffer);
    if (FAILED(hr)) {
        php_error_docref(NULL, E_WARNING, "D3D11: Failed to create buffer (0x%08lx)", hr);
        free(buf);
        return NULL;
    }

    return buf;
}

static void d3d11_update_buffer(void *buffer_ptr, const void *data, size_t size)
{
    vio_d3d11_buffer *buf = (vio_d3d11_buffer *)buffer_ptr;
    if (!buf || !buf->buffer || !data) return;

    if (buf->type == VIO_BUFFER_UNIFORM) {
        /* Dynamic buffers: Map/Unmap */
        D3D11_MAPPED_SUBRESOURCE mapped = {0};
        HRESULT hr = ID3D11DeviceContext_Map(vio_d3d11.context,
                                              (ID3D11Resource *)buf->buffer,
                                              0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
        if (SUCCEEDED(hr)) {
            memcpy(mapped.pData, data, size);
            ID3D11DeviceContext_Unmap(vio_d3d11.context,
                                      (ID3D11Resource *)buf->buffer, 0);
        }
    } else {
        /* Default buffers: UpdateSubresource */
        ID3D11DeviceContext_UpdateSubresource(vio_d3d11.context,
                                              (ID3D11Resource *)buf->buffer,
                                              0, NULL, data, 0, 0);
    }
}

static void d3d11_destroy_buffer(void *buffer_ptr)
{
    vio_d3d11_buffer *buf = (vio_d3d11_buffer *)buffer_ptr;
    if (!buf) return;

    if (buf->readback_staging) ID3D11Buffer_Release(buf->readback_staging);
    if (buf->buffer) ID3D11Buffer_Release(buf->buffer);
    free(buf);
}

/* ── Resources: Textures ──────────────────────────────────────────── */

static void *d3d11_create_texture(vio_texture_desc *desc)
{
    vio_d3d11_texture *tex = calloc(1, sizeof(vio_d3d11_texture));
    if (!tex) return NULL;

    tex->width = desc->width;
    tex->height = desc->height;

    D3D11_TEXTURE2D_DESC td = {0};
    td.Width = desc->width;
    td.Height = desc->height;
    td.MipLevels = desc->mipmaps ? 0 : 1;
    td.ArraySize = 1;
    td.Format = desc->single_channel ? DXGI_FORMAT_R8_UNORM : DXGI_FORMAT_R8G8B8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    if (desc->mipmaps) {
        td.BindFlags |= D3D11_BIND_RENDER_TARGET;
        td.MiscFlags = D3D11_RESOURCE_MISC_GENERATE_MIPS;
    }

    D3D11_SUBRESOURCE_DATA init_data = {0};
    D3D11_SUBRESOURCE_DATA *init_ptr = NULL;
    if (desc->data) {
        init_data.pSysMem = desc->data;
        init_data.SysMemPitch = desc->width * (desc->single_channel ? 1 : 4);
        init_ptr = desc->mipmaps ? NULL : &init_data; /* GenerateMips needs SRV first */
    }

    HRESULT hr = ID3D11Device_CreateTexture2D(vio_d3d11.device, &td,
                                               init_ptr, &tex->texture);
    if (FAILED(hr)) {
        free(tex);
        return NULL;
    }

    /* Shader resource view */
    D3D11_SHADER_RESOURCE_VIEW_DESC srv_desc = {0};
    srv_desc.Format = td.Format;
    srv_desc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    srv_desc.Texture2D.MipLevels = desc->mipmaps ? (UINT)-1 : 1;

    hr = ID3D11Device_CreateShaderResourceView(vio_d3d11.device,
                                                (ID3D11Resource *)tex->texture,
                                                &srv_desc, &tex->srv);
    if (FAILED(hr)) {
        ID3D11Texture2D_Release(tex->texture);
        free(tex);
        return NULL;
    }

    /* Upload data and generate mipmaps if needed */
    if (desc->mipmaps && desc->data) {
        ID3D11DeviceContext_UpdateSubresource(vio_d3d11.context,
                                              (ID3D11Resource *)tex->texture,
                                              0, NULL, desc->data,
                                              desc->width * 4, 0);
        ID3D11DeviceContext_GenerateMips(vio_d3d11.context, tex->srv);
    }

    /* Sampler */
    D3D11_SAMPLER_DESC sampler_desc = {0};
    sampler_desc.Filter = (desc->filter == VIO_FILTER_NEAREST)
        ? D3D11_FILTER_MIN_MAG_MIP_POINT
        : D3D11_FILTER_MIN_MAG_MIP_LINEAR;

    switch (desc->wrap) {
        case VIO_WRAP_REPEAT: sampler_desc.AddressU = sampler_desc.AddressV = sampler_desc.AddressW = D3D11_TEXTURE_ADDRESS_WRAP; break;
        case VIO_WRAP_CLAMP:  sampler_desc.AddressU = sampler_desc.AddressV = sampler_desc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP; break;
        case VIO_WRAP_MIRROR: sampler_desc.AddressU = sampler_desc.AddressV = sampler_desc.AddressW = D3D11_TEXTURE_ADDRESS_MIRROR; break;
    }
    sampler_desc.MaxAnisotropy = 1;
    sampler_desc.MaxLOD = D3D11_FLOAT32_MAX;

    ID3D11Device_CreateSamplerState(vio_d3d11.device, &sampler_desc, &tex->sampler);

    return tex;
}

/* Create a 3D / volume texture (Fieldtracing SDF). Mirrors d3d11_create_texture
 * but with a Texture3D resource + a TEXTURE3D SRV; the bind path (d3d11_bind_texture)
 * is unchanged because it binds tex->srv / tex->sampler, which are dimension-agnostic. */
static void *d3d11_create_texture_3d(vio_texture_desc *desc)
{
    if (desc->depth <= 0) return NULL;

    vio_d3d11_texture *tex = calloc(1, sizeof(vio_d3d11_texture));
    if (!tex) return NULL;

    tex->width = desc->width;
    tex->height = desc->height;
    tex->depth = desc->depth;

    D3D11_TEXTURE3D_DESC td = {0};
    td.Width = desc->width;
    td.Height = desc->height;
    td.Depth = desc->depth;
    td.MipLevels = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;

    D3D11_SUBRESOURCE_DATA init_data = {0};
    D3D11_SUBRESOURCE_DATA *init_ptr = NULL;
    if (desc->data) {
        init_data.pSysMem = desc->data;
        init_data.SysMemPitch = (UINT)desc->width * 4;                       /* row pitch */
        init_data.SysMemSlicePitch = (UINT)desc->width * (UINT)desc->height * 4; /* slice pitch */
        init_ptr = &init_data;
    }

    HRESULT hr = ID3D11Device_CreateTexture3D(vio_d3d11.device, &td, init_ptr, &tex->texture3d);
    if (FAILED(hr)) {
        free(tex);
        return NULL;
    }

    D3D11_SHADER_RESOURCE_VIEW_DESC srv_desc = {0};
    srv_desc.Format = td.Format;
    srv_desc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE3D;
    srv_desc.Texture3D.MipLevels = 1;

    hr = ID3D11Device_CreateShaderResourceView(vio_d3d11.device,
                                               (ID3D11Resource *)tex->texture3d,
                                               &srv_desc, &tex->srv);
    if (FAILED(hr)) {
        ID3D11Texture3D_Release(tex->texture3d);
        free(tex);
        return NULL;
    }

    D3D11_SAMPLER_DESC sampler_desc = {0};
    sampler_desc.Filter = (desc->filter == VIO_FILTER_NEAREST)
        ? D3D11_FILTER_MIN_MAG_MIP_POINT
        : D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    D3D11_TEXTURE_ADDRESS_MODE am;
    switch (desc->wrap) {
        case VIO_WRAP_REPEAT: am = D3D11_TEXTURE_ADDRESS_WRAP; break;
        case VIO_WRAP_MIRROR: am = D3D11_TEXTURE_ADDRESS_MIRROR; break;
        default:              am = D3D11_TEXTURE_ADDRESS_CLAMP; break;
    }
    sampler_desc.AddressU = sampler_desc.AddressV = sampler_desc.AddressW = am;
    sampler_desc.MaxAnisotropy = 1;
    sampler_desc.MaxLOD = D3D11_FLOAT32_MAX;
    ID3D11Device_CreateSamplerState(vio_d3d11.device, &sampler_desc, &tex->sampler);

    return tex;
}

static void d3d11_destroy_texture(void *texture_ptr)
{
    vio_d3d11_texture *tex = (vio_d3d11_texture *)texture_ptr;
    if (!tex) return;

    if (tex->sampler_cmp) ID3D11SamplerState_Release(tex->sampler_cmp);
    if (tex->sampler) ID3D11SamplerState_Release(tex->sampler);
    if (tex->srv) ID3D11ShaderResourceView_Release(tex->srv);
    if (tex->texture3d) ID3D11Texture3D_Release(tex->texture3d);
    if (tex->texture) ID3D11Texture2D_Release(tex->texture);
    free(tex);
}

/* Object destructors invoked from Zend free_object handlers; mirror the
 * destroy_mesh / destroy_cubemap slots in the vtable. Each takes the full
 * Zend object pointer so the backend can free whichever D3D11 fields it
 * populated. */

#include "../../vio_cubemap.h"
#include "../../vio_font.h"
#include "../../vio_render_target.h"

static void d3d11_destroy_cubemap(void *cm_ptr)
{
    vio_cubemap_object *cm = (vio_cubemap_object *)cm_ptr;
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
}

static void d3d11_destroy_font_atlas(void *font_ptr)
{
    vio_font_object *font = (vio_font_object *)font_ptr;
    if (font->atlas_backend_texture) {
        d3d11_destroy_texture(font->atlas_backend_texture);
        font->atlas_backend_texture = NULL;
    }
}

static void d3d11_destroy_render_target(void *rt_ptr)
{
    vio_render_target_object *rt = (vio_render_target_object *)rt_ptr;
    if (rt->backend_type != VIO_RT_BACKEND_D3D11) return;

    /* Drop cached backend-texture wrappers first. They borrow the RT's SRV
     * (no extra AddRef), so we only release the samplers + the
     * vio_d3d11_texture struct itself. The actual SRV release happens a
     * few lines below as part of the regular RT teardown. */
    for (int i = 0; i < 2; i++) {
        vio_d3d11_texture **slot = i == 0
            ? (vio_d3d11_texture **)&rt->d3d11_color_backend_texture
            : (vio_d3d11_texture **)&rt->d3d11_depth_backend_texture;
        vio_d3d11_texture *bt = *slot;
        if (bt) {
            if (bt->sampler)     ID3D11SamplerState_Release(bt->sampler);
            if (bt->sampler_cmp) ID3D11SamplerState_Release(bt->sampler_cmp);
            free(bt);
            *slot = NULL;
        }
    }
    if (rt->d3d11_depth_srv) {
        ID3D11ShaderResourceView_Release((ID3D11ShaderResourceView *)rt->d3d11_depth_srv);
        rt->d3d11_depth_srv = NULL;
    }
    if (rt->d3d11_color_srv) {
        ID3D11ShaderResourceView_Release((ID3D11ShaderResourceView *)rt->d3d11_color_srv);
        rt->d3d11_color_srv = NULL;
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

/* ── Shaders ──────────────────────────────────────────────────────── */

static void *d3d11_compile_shader(vio_shader_desc *desc)
{
    vio_d3d11_shader *shader = calloc(1, sizeof(vio_d3d11_shader));
    if (!shader) return NULL;

    HRESULT hr;
    const char *hlsl_vs = NULL;
    const char *hlsl_ps = NULL;
    char *allocated_vs = NULL;
    char *allocated_ps = NULL;

    if (desc->format == VIO_SHADER_GLSL || desc->format == VIO_SHADER_GLSL_RAW || desc->format == VIO_SHADER_AUTO) {
        /* GLSL/SPIR-V -> HLSL pipeline */
        char *err = NULL;
        uint32_t *vs_spirv = NULL;
        uint32_t *ps_spirv = NULL;
        size_t vs_spirv_size = 0, ps_spirv_size = 0;
        int free_vs_spirv = 0, free_ps_spirv = 0;

        /* Check if data is already SPIR-V (magic number 0x07230203) */
        int vs_is_spirv = (desc->vertex_size >= 4 &&
            *(const uint32_t *)desc->vertex_data == 0x07230203);
        int ps_is_spirv = (desc->fragment_size >= 4 &&
            *(const uint32_t *)desc->fragment_data == 0x07230203);

        if (vs_is_spirv) {
            vs_spirv = (uint32_t *)desc->vertex_data;
            vs_spirv_size = desc->vertex_size;
        } else {
            vs_spirv = vio_compile_glsl_to_spirv(
                (const char *)desc->vertex_data, 0, &vs_spirv_size, &err);
            if (!vs_spirv) {
                php_error_docref(NULL, E_WARNING, "D3D11: VS GLSL->SPIR-V failed: %s", err ? err : "unknown");
                if (err) free(err);
                free(shader);
                return NULL;
            }
            free_vs_spirv = 1;
        }

        if (ps_is_spirv) {
            ps_spirv = (uint32_t *)desc->fragment_data;
            ps_spirv_size = desc->fragment_size;
        } else {
            ps_spirv = vio_compile_glsl_to_spirv(
                (const char *)desc->fragment_data, 1, &ps_spirv_size, &err);
            if (!ps_spirv) {
                php_error_docref(NULL, E_WARNING, "D3D11: PS GLSL->SPIR-V failed: %s", err ? err : "unknown");
                if (err) free(err);
                if (free_vs_spirv) free(vs_spirv);
                free(shader);
                return NULL;
            }
            free_ps_spirv = 1;
        }

        /* SPIR-V -> HLSL (Shader Model 5.0) */
        allocated_vs = vio_spirv_to_hlsl(vs_spirv, vs_spirv_size, 50, &err);
        if (free_vs_spirv) free(vs_spirv);
        if (!allocated_vs) {
            php_error_docref(NULL, E_WARNING, "D3D11: VS SPIR-V->HLSL failed: %s", err ? err : "unknown");
            if (err) free(err);
            if (free_ps_spirv) free(ps_spirv);
            free(shader);
            return NULL;
        }

        allocated_ps = vio_spirv_to_hlsl(ps_spirv, ps_spirv_size, 50, &err);
        if (free_ps_spirv) free(ps_spirv);
        if (!allocated_ps) {
            php_error_docref(NULL, E_WARNING, "D3D11: PS SPIR-V->HLSL failed: %s", err ? err : "unknown");
            if (err) free(err);
            free(allocated_vs);
            free(shader);
            return NULL;
        }

        hlsl_vs = allocated_vs;
        hlsl_ps = allocated_ps;
    } else {
        /* Assume HLSL source passed directly */
        hlsl_vs = (const char *)desc->vertex_data;
        hlsl_ps = (const char *)desc->fragment_data;
    }

    /* Compile HLSL -> DXBC */
    UINT compile_flags = 0;
    if (vio_d3d11.debug_enabled) {
        compile_flags |= D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION;
    }

    ID3DBlob *error_blob = NULL;

    hr = D3DCompile(hlsl_vs, strlen(hlsl_vs), "vs_main", NULL, NULL,
                     "main", "vs_5_0", compile_flags, 0, &shader->vs_blob, &error_blob);
    if (FAILED(hr)) {
        php_error_docref(NULL, E_WARNING, "D3D11: VS compile failed: %s",
                          error_blob ? (char *)ID3D10Blob_GetBufferPointer(error_blob) : "unknown");
        if (error_blob) ID3D10Blob_Release(error_blob);
        goto fail;
    }

    hr = D3DCompile(hlsl_ps, strlen(hlsl_ps), "ps_main", NULL, NULL,
                     "main", "ps_5_0", compile_flags, 0, &shader->ps_blob, &error_blob);
    if (FAILED(hr)) {
        php_error_docref(NULL, E_WARNING, "D3D11: PS compile failed: %s",
                          error_blob ? (char *)ID3D10Blob_GetBufferPointer(error_blob) : "unknown");
        if (error_blob) ID3D10Blob_Release(error_blob);
        goto fail;
    }

    /* Create shader objects */
    hr = ID3D11Device_CreateVertexShader(vio_d3d11.device,
                                          ID3D10Blob_GetBufferPointer(shader->vs_blob),
                                          ID3D10Blob_GetBufferSize(shader->vs_blob),
                                          NULL, &shader->vs);
    if (FAILED(hr)) goto fail;

    hr = ID3D11Device_CreatePixelShader(vio_d3d11.device,
                                         ID3D10Blob_GetBufferPointer(shader->ps_blob),
                                         ID3D10Blob_GetBufferSize(shader->ps_blob),
                                         NULL, &shader->ps);
    if (FAILED(hr)) goto fail;

    if (allocated_vs) free(allocated_vs);
    if (allocated_ps) free(allocated_ps);
    return shader;

fail:
    if (allocated_vs) free(allocated_vs);
    if (allocated_ps) free(allocated_ps);
    if (shader->vs_blob) ID3D10Blob_Release(shader->vs_blob);
    if (shader->ps_blob) ID3D10Blob_Release(shader->ps_blob);
    if (shader->vs) ID3D11VertexShader_Release(shader->vs);
    if (shader->ps) ID3D11PixelShader_Release(shader->ps);
    free(shader);
    return NULL;
}

static void d3d11_destroy_shader(void *shader_ptr)
{
    vio_d3d11_shader *s = (vio_d3d11_shader *)shader_ptr;
    if (!s) return;

    if (s->vs) ID3D11VertexShader_Release(s->vs);
    if (s->ps) ID3D11PixelShader_Release(s->ps);
    if (s->vs_blob) ID3D10Blob_Release(s->vs_blob);
    if (s->ps_blob) ID3D10Blob_Release(s->ps_blob);
    free(s);
}

/* ── Drawing ──────────────────────────────────────────────────────── */

static void d3d11_begin_frame(void)
{
    /* Reset to backbuffer */
    vio_d3d11.current_rtv = vio_d3d11.rtv;
    vio_d3d11.current_dsv = vio_d3d11.dsv;
    vio_d3d11.current_rt_width = vio_d3d11.width;
    vio_d3d11.current_rt_height = vio_d3d11.height;

    /* Set render target and viewport */
    ID3D11DeviceContext_OMSetRenderTargets(vio_d3d11.context, 1,
                                           &vio_d3d11.rtv, vio_d3d11.dsv);

    D3D11_VIEWPORT vp = {0};
    vp.Width = (float)vio_d3d11.width;
    vp.Height = (float)vio_d3d11.height;
    vp.MinDepth = 0.0f;
    vp.MaxDepth = 1.0f;
    ID3D11DeviceContext_RSSetViewports(vio_d3d11.context, 1, &vp);
}

/* Mirror the current backbuffer into the GPU-LOCAL readback_mirror texture.
 * Must run at end of frame (before Present) so vio_read_pixels can still reach
 * the rendered content after FLIP_DISCARD invalidates the source.
 *
 * The mirror target is D3D11_USAGE_DEFAULT on purpose — see the readback_mirror
 * comment in vio_d3d11.h. Copying into a STAGING texture here (as this did
 * previously) pushes the entire backbuffer across PCIe every single frame, even
 * when nothing ever reads it: 2.29 ms/frame on a 3840x1080 backbuffer, scaling
 * linearly with resolution. The CPU-visible copy now happens only on demand, in
 * vio_d3d11_resolve_readback(). */
static void d3d11_mirror_backbuffer(void)
{
    if (!vio_d3d11.context || !vio_d3d11.current_rtv) return;

    ID3D11Resource *rtv_res = NULL;
    ID3D11RenderTargetView_GetResource(vio_d3d11.current_rtv, &rtv_res);
    if (!rtv_res) return;

    ID3D11Texture2D *bb_tex = NULL;
    ID3D11Resource_QueryInterface(rtv_res, &IID_ID3D11Texture2D, (void **)&bb_tex);
    ID3D11Resource_Release(rtv_res);
    if (!bb_tex) return;

    D3D11_TEXTURE2D_DESC bb_desc;
    ID3D11Texture2D_GetDesc(bb_tex, &bb_desc);

    /* Lazy-create or resize the mirror to match the current backbuffer. Any stale
     * staging texture is dropped at the same time so the two can never disagree
     * on size (resolve_readback recreates staging on demand). */
    if (!vio_d3d11.readback_mirror ||
        vio_d3d11.readback_w != bb_desc.Width ||
        vio_d3d11.readback_h != bb_desc.Height) {
        if (vio_d3d11.readback_mirror) {
            ID3D11Texture2D_Release(vio_d3d11.readback_mirror);
            vio_d3d11.readback_mirror = NULL;
        }
        if (vio_d3d11.readback_staging) {
            ID3D11Texture2D_Release(vio_d3d11.readback_staging);
            vio_d3d11.readback_staging = NULL;
        }

        D3D11_TEXTURE2D_DESC md = bb_desc;
        md.Usage = D3D11_USAGE_DEFAULT;
        /* Copy-only resource. SHADER_RESOURCE (rather than 0) keeps the debug
         * layer happy on every driver and costs nothing. */
        md.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        md.CPUAccessFlags = 0;
        md.MiscFlags = 0;
        if (FAILED(ID3D11Device_CreateTexture2D(vio_d3d11.device, &md, NULL,
                                                 &vio_d3d11.readback_mirror))) {
            ID3D11Texture2D_Release(bb_tex);
            return;
        }
        vio_d3d11.readback_w = bb_desc.Width;
        vio_d3d11.readback_h = bb_desc.Height;
    }

    /* VRAM -> VRAM. Does not touch the CPU and does not stall. */
    ID3D11DeviceContext_CopyResource(vio_d3d11.context,
                                     (ID3D11Resource *)vio_d3d11.readback_mirror,
                                     (ID3D11Resource *)bb_tex);
    ID3D11Texture2D_Release(bb_tex);
}

int vio_d3d11_resolve_readback(void)
{
    if (!vio_d3d11.context || !vio_d3d11.readback_mirror) return 0;

    D3D11_TEXTURE2D_DESC md;
    ID3D11Texture2D_GetDesc(vio_d3d11.readback_mirror, &md);

    if (!vio_d3d11.readback_staging) {
        D3D11_TEXTURE2D_DESC sd = md;
        sd.Usage = D3D11_USAGE_STAGING;
        sd.BindFlags = 0;
        sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        sd.MiscFlags = 0;
        if (FAILED(ID3D11Device_CreateTexture2D(vio_d3d11.device, &sd, NULL,
                                                 &vio_d3d11.readback_staging))) {
            php_error_docref(NULL, E_WARNING,
                "D3D11: failed to create readback staging texture (%ux%u)",
                md.Width, md.Height);
            return 0;
        }
    }

    /* The only PCIe-crossing copy, and only when someone actually reads. */
    ID3D11DeviceContext_CopyResource(vio_d3d11.context,
                                     (ID3D11Resource *)vio_d3d11.readback_staging,
                                     (ID3D11Resource *)vio_d3d11.readback_mirror);
    return 1;
}

static void d3d11_end_frame(void)
{
    /* Mirror the just-rendered backbuffer — before the caller's Present() /
     * swap rotates FLIP_DISCARD buffers. GPU-local; no CPU transfer. */
    d3d11_mirror_backbuffer();
}

/* Bind mesh vertex buffer to slot 0 and the identity instance buffer to
 * slot 1. The second slot is required by SPIRV-Cross'ed shaders that declare
 * `layout(location=3..6) vec4 a_instance_colN` — without a buffer at slot 1
 * the D3D11 runtime reads undefined memory and draws go invisible. */
static void d3d11_bind_vertex_slots(ID3D11Buffer *mesh_vb, UINT mesh_stride)
{
    ID3D11Buffer *buffers[2] = { mesh_vb, vio_d3d11.identity_instance_buf };
    UINT strides[2] = { mesh_stride, 64 }; /* identity is one 64-byte mat4 */
    UINT offsets[2] = { 0, 0 };
    UINT slot_count = vio_d3d11.identity_instance_buf ? 2 : 1;
    ID3D11DeviceContext_IASetVertexBuffers(vio_d3d11.context, 0, slot_count,
                                           buffers, strides, offsets);
}

static void d3d11_draw(vio_draw_cmd *cmd)
{
    if (!cmd) return;

    vio_d3d11_buffer *vb = (vio_d3d11_buffer *)cmd->vertex_buffer;
    if (vb) {
        UINT stride = cmd->vertex_stride > 0 ? (UINT)cmd->vertex_stride
                    : (d3d11_current_pipeline ? d3d11_current_pipeline->vertex_stride : 0);
        d3d11_bind_vertex_slots(vb->buffer, stride);
    }

    UINT instance_count = cmd->instance_count > 0 ? cmd->instance_count : 1;
    ID3D11DeviceContext_DrawInstanced(vio_d3d11.context,
                                      cmd->vertex_count,
                                      instance_count,
                                      cmd->first_vertex, 0);
}

static void d3d11_draw_indexed(vio_draw_indexed_cmd *cmd)
{
    if (!cmd) return;

    vio_d3d11_buffer *vb = (vio_d3d11_buffer *)cmd->vertex_buffer;
    vio_d3d11_buffer *ib = (vio_d3d11_buffer *)cmd->index_buffer;

    if (vb) {
        UINT stride = cmd->vertex_stride > 0 ? (UINT)cmd->vertex_stride
                    : (d3d11_current_pipeline ? d3d11_current_pipeline->vertex_stride : 0);
        d3d11_bind_vertex_slots(vb->buffer, stride);
    }

    if (ib) {
        ID3D11DeviceContext_IASetIndexBuffer(vio_d3d11.context, ib->buffer,
                                             DXGI_FORMAT_R32_UINT, 0);
    }

    UINT instance_count = cmd->instance_count > 0 ? cmd->instance_count : 1;
    ID3D11DeviceContext_DrawIndexedInstanced(vio_d3d11.context,
                                             cmd->index_count,
                                             instance_count,
                                             cmd->first_index,
                                             cmd->vertex_offset, 0);
}

static void d3d11_present(void)
{
    if (!vio_d3d11.swapchain) return;

    /* Present only when the backbuffer RTV is the bound color target. Any other
     * value of current_rtv means the frame's draws went somewhere other than
     * the swapchain — an offscreen color target (warm-render / render-to-texture)
     * or a depth-only pass (current_rtv == NULL) — and presenting would flip an
     * undrawn backbuffer to the screen (the visible "pre-warm" flash). vio_d3d11.rtv
     * is non-NULL whenever the swapchain exists, and d3d11_begin_frame re-syncs
     * current_rtv = rtv every frame, so normal frames always present.
     * vio_unbind_render_target restores current_rtv = rtv. Mirrors metal_present /
     * d3d12_present offscreen handling. */
    if (vio_d3d11.current_rtv != vio_d3d11.rtv) return;

    UINT sync_interval = vio_d3d11.vsync ? 1 : 0;

    /* DXGI_PRESENT_ALLOW_TEARING is legal ONLY when all of these hold:
     *   - the swapchain was created with DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING,
     *   - SyncInterval == 0 (with a non-zero interval DXGI fails the Present
     *     with DXGI_ERROR_INVALID_CALL — this is an API violation, not a hint),
     *   - the swapchain is windowed (guaranteed here: NO_ALT_ENTER is set and
     *     this backend never calls SetFullscreenState).
     * The flag is what lets VRR engage and hands the frame to scan-out without
     * waiting for a vblank boundary. It does NOT raise throughput — SyncInterval=0
     * already presents uncapped without it (measured). */
    UINT present_flags = 0;
    if (sync_interval == 0 &&
        (vio_d3d11.swapchain_flags & DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING)) {
        present_flags = DXGI_PRESENT_ALLOW_TEARING;
    }

    HRESULT hr = IDXGISwapChain1_Present(vio_d3d11.swapchain, sync_interval, present_flags);

    /* DXGI_STATUS_OCCLUDED is a success code (window hidden/minimised) — not an
     * error, the frame is simply dropped. Only genuine failures are reported,
     * and only once, so a device-removed loop cannot spam the PHP error log. */
    if (FAILED(hr) && !vio_d3d11.present_failed_once) {
        vio_d3d11.present_failed_once = 1;
        HRESULT removed = vio_d3d11.device
            ? ID3D11Device_GetDeviceRemovedReason(vio_d3d11.device)
            : S_OK;
        php_error_docref(NULL, E_WARNING,
            "D3D11: Present failed (0x%08lx) sync=%u flags=0x%08lx device_removed=0x%08lx",
            hr, sync_interval, (unsigned long)present_flags, removed);
    }
}

static void d3d11_clear(float r, float g, float b, float a)
{
    float color[4] = {r, g, b, a};
    if (vio_d3d11.current_rtv) {
        ID3D11DeviceContext_ClearRenderTargetView(vio_d3d11.context, vio_d3d11.current_rtv, color);
    }
    if (vio_d3d11.current_dsv) {
        ID3D11DeviceContext_ClearDepthStencilView(vio_d3d11.context, vio_d3d11.current_dsv,
                                                   D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL,
                                                   1.0f, 0);
    }
}

/* ── Compute ──────────────────────────────────────────────────────── */

/* Drain the D3D11 debug-layer info queue to stderr (debug builds only). Mirrors
 * d3d12_drain_info_queue: used during compute bring-up to prove ZERO debug-layer
 * ERROR/WARNING. Clears the queue after reading so each drain is incremental. */
static void d3d11_drain_info_queue(const char *where)
{
    if (!vio_d3d11.debug_enabled || !vio_d3d11.device) return;
    ID3D11InfoQueue *iq = NULL;
    if (FAILED(ID3D11Device_QueryInterface(vio_d3d11.device, &IID_ID3D11InfoQueue, (void **)&iq)) || !iq)
        return;
    UINT64 n = ID3D11InfoQueue_GetNumStoredMessages(iq);
    for (UINT64 i = 0; i < n; i++) {
        SIZE_T len = 0;
        if (FAILED(ID3D11InfoQueue_GetMessage(iq, i, NULL, &len)) || len == 0) continue;
        D3D11_MESSAGE *msg = (D3D11_MESSAGE *)malloc(len);
        if (!msg) continue;
        if (SUCCEEDED(ID3D11InfoQueue_GetMessage(iq, i, msg, &len))) {
            fprintf(stderr, "[D3D11 debug-layer @ %s] sev=%d id=%d: %.*s\n",
                    where, (int)msg->Severity, (int)msg->ID,
                    (int)msg->DescriptionByteLength, msg->pDescription);
            fflush(stderr);
        }
        free(msg);
    }
    ID3D11InfoQueue_ClearStoredMessages(iq);
    ID3D11InfoQueue_Release(iq);
}

/* Compile the GLSL compute source (or accept ready SPIR-V), transpile to HLSL
 * SM5.0 and create an ID3D11ComputeShader. Register mapping is DATA-DRIVEN from
 * SPIR-V reflection exactly like the D3D12 path: spirv-cross maps a GLSL
 * `binding = N` straight to the HLSL register NUMBER N within each register file
 * (UBO -> bN, readonly buffer -> tN, writeonly buffer -> uN). For D3D11 there is
 * no root signature — we bind directly by register slot — so SRV/UAV registers
 * equal their GLSL bindings and only the CBV register must be reflected. */
static void *d3d11_create_compute_pipeline(vio_shader_desc *desc)
{
    if (!vio_d3d11.device) return NULL;

    const char *src = (const char *)desc->fragment_data;
    if (!src && desc->vertex_data) src = (const char *)desc->vertex_data;
    if (!src) {
        php_error_docref(NULL, E_WARNING, "D3D11: compute pipeline missing source");
        return NULL;
    }
    size_t src_size = desc->fragment_size ? desc->fragment_size : desc->vertex_size;

    char *err = NULL;
    uint32_t *spirv = NULL;
    size_t spirv_size = 0;
    int free_spirv = 0;

    int is_spirv = (src_size >= 4 && *(const uint32_t *)src == 0x07230203);
    if (is_spirv) {
        spirv = (uint32_t *)src;
        spirv_size = src_size;
    } else {
        spirv = vio_compile_glsl_compute_to_spirv(src, &spirv_size, &err);
        if (!spirv) {
            php_error_docref(NULL, E_WARNING, "D3D11: CS GLSL->SPIR-V failed: %s", err ? err : "unknown");
            if (err) free(err);
            return NULL;
        }
        free_spirv = 1;
    }

    /* Reflect for the Params UBO register (the only one D3D11 can't derive from
     * the bind slot). SRV/UAV registers == their GLSL bindings == the slots the
     * PHP layer passes to compute_bind_buffer, so no table base is needed. */
    int cbv_register = -1;
    {
        vio_reflect_result refl;
        char *rerr = NULL;
        if (vio_spirv_reflect(spirv, spirv_size, &refl, &rerr) == 0) {
            if (refl.ubo_count > 0) {
                cbv_register = (int)refl.ubos[0].binding;
                for (int i = 1; i < refl.ubo_count; i++) {
                    if ((int)refl.ubos[i].binding < cbv_register)
                        cbv_register = (int)refl.ubos[i].binding;
                }
            }
            vio_reflect_free(&refl);
        } else {
            php_error_docref(NULL, E_WARNING, "D3D11: CS reflection failed (%s); CBV at b0",
                             rerr ? rerr : "unknown");
            if (rerr) free(rerr);
        }
    }

    char *hlsl = vio_spirv_to_hlsl(spirv, spirv_size, 50, &err);
    if (free_spirv) free(spirv);
    if (!hlsl) {
        php_error_docref(NULL, E_WARNING, "D3D11: CS SPIR-V->HLSL failed: %s", err ? err : "unknown");
        if (err) free(err);
        return NULL;
    }

    if (getenv("VIO_DUMP_CS_HLSL")) {
        fprintf(stderr, "==== D3D11 compute HLSL (CBV b%d) ====\n%s\n==== end ====\n",
                cbv_register, hlsl);
        fflush(stderr);
    }

    UINT compile_flags = 0;
    if (vio_d3d11.debug_enabled) {
        compile_flags |= D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION;
    }

    vio_d3d11_compute_pipeline *cp = calloc(1, sizeof(vio_d3d11_compute_pipeline));
    if (!cp) { free(hlsl); return NULL; }
    cp->cbv_register = cbv_register;

    ID3DBlob *error_blob = NULL;
    HRESULT hr = D3DCompile(hlsl, strlen(hlsl), "cs_main", NULL, NULL,
                            "main", "cs_5_0", compile_flags, 0, &cp->cs_blob, &error_blob);
    free(hlsl);
    if (FAILED(hr)) {
        php_error_docref(NULL, E_WARNING, "D3D11: CS compile failed: %s",
                         error_blob ? (char *)ID3D10Blob_GetBufferPointer(error_blob) : "unknown");
        if (error_blob) ID3D10Blob_Release(error_blob);
        free(cp);
        return NULL;
    }
    if (error_blob) ID3D10Blob_Release(error_blob);

    hr = ID3D11Device_CreateComputeShader(vio_d3d11.device,
            ID3D10Blob_GetBufferPointer(cp->cs_blob),
            ID3D10Blob_GetBufferSize(cp->cs_blob),
            NULL, &cp->cs);
    if (FAILED(hr)) {
        php_error_docref(NULL, E_WARNING, "D3D11: CreateComputeShader failed (0x%08lx)", hr);
        ID3D10Blob_Release(cp->cs_blob);
        free(cp);
        return NULL;
    }

    return cp;
}

static void d3d11_destroy_compute_pipeline(void *pipeline_ptr)
{
    vio_d3d11_compute_pipeline *cp = (vio_d3d11_compute_pipeline *)pipeline_ptr;
    if (!cp) return;
    /* All vio compute dispatches Flush()+wait via the staging Map (read_buffer),
     * but even an in-flight dispatch is safe: D3D11 defers resource destruction
     * until the GPU no longer references it. */
    if (cp->params_buf) ID3D11Buffer_Release(cp->params_buf);
    if (cp->cs) ID3D11ComputeShader_Release(cp->cs);
    if (cp->cs_blob) ID3D10Blob_Release(cp->cs_blob);
    free(cp);
}

static void d3d11_compute_bind_buffer(void *pipeline_ptr, void *backend_buffer,
                                      int slot, int access, int element_count, int stride)
{
    vio_d3d11_compute_pipeline *cp = (vio_d3d11_compute_pipeline *)pipeline_ptr;
    vio_d3d11_buffer *buf = (vio_d3d11_buffer *)backend_buffer;
    if (!cp || !buf) return;

    vio_d3d11_compute_binding b = {0};
    b.buffer = buf;
    b.slot = slot;
    b.access = access;
    b.element_count = element_count;
    b.stride = stride > 0 ? stride : (buf->stride > 0 ? buf->stride : 4);

    if (access == 1 /* VIO_COMPUTE_WRITE */) {
        if (cp->uav_count < VIO_D3D11_COMPUTE_MAX_BINDINGS) cp->uavs[cp->uav_count++] = b;
    } else {
        if (cp->srv_count < VIO_D3D11_COMPUTE_MAX_BINDINGS) cp->srvs[cp->srv_count++] = b;
    }
}

static void d3d11_compute_set_uniforms(void *pipeline_ptr, const void *data, int size)
{
    vio_d3d11_compute_pipeline *cp = (vio_d3d11_compute_pipeline *)pipeline_ptr;
    if (!cp || !data || size <= 0) return;

    /* Constant buffers must be a multiple of 16 bytes. A USAGE_DEFAULT CB updated
     * via UpdateSubresource (the whole-buffer write is well-defined for DEFAULT
     * buffers and avoids the MAP_WRITE_DISCARD partial-CB pitfall). */
    UINT aligned = ((UINT)size + 15u) & ~15u;

    if (!cp->params_buf || cp->params_capacity < aligned) {
        if (cp->params_buf) { ID3D11Buffer_Release(cp->params_buf); cp->params_buf = NULL; }
        D3D11_BUFFER_DESC bd = {0};
        bd.ByteWidth = aligned;
        bd.Usage = D3D11_USAGE_DEFAULT;
        bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        bd.CPUAccessFlags = 0;
        HRESULT hr = ID3D11Device_CreateBuffer(vio_d3d11.device, &bd, NULL, &cp->params_buf);
        if (FAILED(hr)) {
            php_error_docref(NULL, E_WARNING, "D3D11: compute params CB create failed (0x%08lx)", hr);
            cp->params_buf = NULL;
            return;
        }
        cp->params_capacity = aligned;
    }

    /* If the staged data is smaller than the aligned CB, pad into a temp so
     * UpdateSubresource (which writes the whole resource with no box) is fed a
     * full-width source — avoids reading past the caller's buffer. */
    if ((UINT)size == aligned) {
        ID3D11DeviceContext_UpdateSubresource(vio_d3d11.context,
            (ID3D11Resource *)cp->params_buf, 0, NULL, data, 0, 0);
    } else {
        unsigned char *tmp = calloc(1, aligned);
        if (!tmp) return;
        memcpy(tmp, data, (size_t)size);
        ID3D11DeviceContext_UpdateSubresource(vio_d3d11.context,
            (ID3D11Resource *)cp->params_buf, 0, NULL, tmp, 0, 0);
        free(tmp);
    }
}

/* Build an SRV for a bound storage buffer (raw BUFFEREX when stride<=4, else a
 * structured BUFFER view). Returns NULL on failure (caller skips the bind). */
static ID3D11ShaderResourceView *d3d11_compute_make_srv(vio_d3d11_compute_binding *b)
{
    if (!b->buffer || !b->buffer->buffer) return NULL;
    D3D11_SHADER_RESOURCE_VIEW_DESC sd = {0};
    int raw = (b->stride <= 4);
    if (raw) {
        sd.Format = DXGI_FORMAT_R32_TYPELESS;
        sd.ViewDimension = D3D11_SRV_DIMENSION_BUFFEREX;
        sd.BufferEx.FirstElement = 0;
        sd.BufferEx.NumElements = (UINT)(b->buffer->size / 4);
        sd.BufferEx.Flags = D3D11_BUFFEREX_SRV_FLAG_RAW;
    } else {
        sd.Format = DXGI_FORMAT_UNKNOWN;
        sd.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
        sd.Buffer.FirstElement = 0;
        sd.Buffer.NumElements = (UINT)b->element_count;
    }
    ID3D11ShaderResourceView *srv = NULL;
    HRESULT hr = ID3D11Device_CreateShaderResourceView(vio_d3d11.device,
        (ID3D11Resource *)b->buffer->buffer, &sd, &srv);
    if (FAILED(hr)) {
        php_error_docref(NULL, E_WARNING, "D3D11: compute SRV create failed (0x%08lx)", hr);
        return NULL;
    }
    return srv;
}

/* Build a UAV for a bound storage buffer (raw when stride<=4, else structured). */
static ID3D11UnorderedAccessView *d3d11_compute_make_uav(vio_d3d11_compute_binding *b)
{
    if (!b->buffer || !b->buffer->buffer) return NULL;
    D3D11_UNORDERED_ACCESS_VIEW_DESC ud = {0};
    ud.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
    ud.Buffer.FirstElement = 0;
    int raw = (b->stride <= 4);
    if (raw) {
        ud.Format = DXGI_FORMAT_R32_TYPELESS;
        ud.Buffer.NumElements = (UINT)(b->buffer->size / 4);
        ud.Buffer.Flags = D3D11_BUFFER_UAV_FLAG_RAW;
    } else {
        ud.Format = DXGI_FORMAT_UNKNOWN;
        ud.Buffer.NumElements = (UINT)b->element_count;
        ud.Buffer.Flags = 0;
    }
    ID3D11UnorderedAccessView *uav = NULL;
    HRESULT hr = ID3D11Device_CreateUnorderedAccessView(vio_d3d11.device,
        (ID3D11Resource *)b->buffer->buffer, &ud, &uav);
    if (FAILED(hr)) {
        php_error_docref(NULL, E_WARNING, "D3D11: compute UAV create failed (0x%08lx)", hr);
        return NULL;
    }
    return uav;
}

static void d3d11_dispatch_compute(vio_compute_cmd *cmd)
{
    if (!cmd) return;
    vio_d3d11_compute_pipeline *cp = (vio_d3d11_compute_pipeline *)cmd->pipeline;
    if (!cp || !cp->cs) {
        php_error_docref(NULL, E_WARNING, "D3D11: dispatch_compute with invalid pipeline");
        return;
    }

    ID3D11DeviceContext *ctx = vio_d3d11.context;

    /* Build SRV / UAV views for every recorded binding. Each is created at
     * dispatch time and released immediately after Dispatch so no view outlives
     * the call (the buffers themselves persist on the PHP-owned wrappers). */
    ID3D11ShaderResourceView  *srvs[VIO_D3D11_COMPUTE_MAX_BINDINGS] = {0};
    UINT                       srv_slots[VIO_D3D11_COMPUTE_MAX_BINDINGS] = {0};
    int                        srv_n = 0;
    ID3D11UnorderedAccessView *uavs[VIO_D3D11_COMPUTE_MAX_BINDINGS] = {0};
    UINT                       uav_slots[VIO_D3D11_COMPUTE_MAX_BINDINGS] = {0};
    int                        uav_n = 0;

    ID3D11DeviceContext_CSSetShader(ctx, cp->cs, NULL, 0);

    if (cp->params_buf && cp->cbv_register >= 0) {
        ID3D11Buffer *cbs[1] = { cp->params_buf };
        ID3D11DeviceContext_CSSetConstantBuffers(ctx, (UINT)cp->cbv_register, 1, cbs);
    }

    for (int i = 0; i < cp->srv_count; i++) {
        ID3D11ShaderResourceView *srv = d3d11_compute_make_srv(&cp->srvs[i]);
        if (!srv) continue;
        srvs[srv_n] = srv;
        srv_slots[srv_n] = (UINT)cp->srvs[i].slot;
        ID3D11DeviceContext_CSSetShaderResources(ctx, (UINT)cp->srvs[i].slot, 1, &srv);
        srv_n++;
    }

    for (int i = 0; i < cp->uav_count; i++) {
        ID3D11UnorderedAccessView *uav = d3d11_compute_make_uav(&cp->uavs[i]);
        if (!uav) continue;
        uavs[uav_n] = uav;
        uav_slots[uav_n] = (UINT)cp->uavs[i].slot;
        ID3D11DeviceContext_CSSetUnorderedAccessViews(ctx, (UINT)cp->uavs[i].slot, 1, &uav, NULL);
        uav_n++;
    }

    UINT gx = cmd->group_count_x > 0 ? (UINT)cmd->group_count_x : 1;
    UINT gy = cmd->group_count_y > 0 ? (UINT)cmd->group_count_y : 1;
    UINT gz = cmd->group_count_z > 0 ? (UINT)cmd->group_count_z : 1;
    ID3D11DeviceContext_Dispatch(ctx, gx, gy, gz);

    /* Copy each UAV output into its staging buffer for later readback. The copy
     * is ordered after Dispatch on the immediate context, so no explicit barrier
     * is required (D3D11 inserts the dependency); read_buffer Maps with the
     * implicit flush. */
    for (int i = 0; i < cp->uav_count; i++) {
        vio_d3d11_buffer *buf = cp->uavs[i].buffer;
        if (!buf || !buf->buffer) continue;
        if (!buf->readback_staging || buf->readback_size < buf->size) {
            if (buf->readback_staging) { ID3D11Buffer_Release(buf->readback_staging); buf->readback_staging = NULL; }
            D3D11_BUFFER_DESC sd = {0};
            sd.ByteWidth = (UINT)buf->size;
            sd.Usage = D3D11_USAGE_STAGING;
            sd.BindFlags = 0;
            sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            sd.MiscFlags = 0;
            HRESULT shr = ID3D11Device_CreateBuffer(vio_d3d11.device, &sd, NULL, &buf->readback_staging);
            if (FAILED(shr)) {
                php_error_docref(NULL, E_WARNING, "D3D11: compute staging buffer create failed (0x%08lx)", shr);
                buf->readback_staging = NULL;
                continue;
            }
            buf->readback_size = buf->size;
        }
        ID3D11DeviceContext_CopyResource(ctx,
            (ID3D11Resource *)buf->readback_staging, (ID3D11Resource *)buf->buffer);
    }

    /* UNBIND UAVs and SRVs so they are not left bound to the CS stage — D3D11
     * raises hazard warnings if a resource is later bound elsewhere (or rebound)
     * while still attached as a UAV/SRV. Unbind each slot we touched. */
    ID3D11UnorderedAccessView *null_uav = NULL;
    for (int i = 0; i < uav_n; i++) {
        ID3D11DeviceContext_CSSetUnorderedAccessViews(ctx, uav_slots[i], 1, &null_uav, NULL);
        ID3D11UnorderedAccessView_Release(uavs[i]);
    }
    ID3D11ShaderResourceView *null_srv = NULL;
    for (int i = 0; i < srv_n; i++) {
        ID3D11DeviceContext_CSSetShaderResources(ctx, srv_slots[i], 1, &null_srv);
        ID3D11ShaderResourceView_Release(srvs[i]);
    }
    ID3D11DeviceContext_CSSetShader(ctx, NULL, NULL, 0);

    d3d11_drain_info_queue("dispatch_compute");
}

/* GPU->CPU readback. The output buffer's bytes were copied into its STAGING
 * buffer by dispatch_compute; Map(READ) flushes+waits, then memcpy out. Returns
 * bytes written. */
static size_t d3d11_read_buffer(void *backend_buffer, void *out, size_t size)
{
    vio_d3d11_buffer *buf = (vio_d3d11_buffer *)backend_buffer;
    if (!buf || !out || size == 0) return 0;
    if (!buf->readback_staging) {
        php_error_docref(NULL, E_WARNING,
            "D3D11: read_buffer before any compute dispatch produced a staging copy");
        return 0;
    }

    D3D11_MAPPED_SUBRESOURCE mapped = {0};
    HRESULT hr = ID3D11DeviceContext_Map(vio_d3d11.context,
        (ID3D11Resource *)buf->readback_staging, 0, D3D11_MAP_READ, 0, &mapped);
    if (FAILED(hr) || !mapped.pData) {
        php_error_docref(NULL, E_WARNING, "D3D11: read_buffer map failed (0x%08lx)", hr);
        return 0;
    }
    size_t n = size < buf->readback_size ? size : buf->readback_size;
    memcpy(out, mapped.pData, n);
    ID3D11DeviceContext_Unmap(vio_d3d11.context, (ID3D11Resource *)buf->readback_staging, 0);
    return n;
}

/* ── Feature Query ────────────────────────────────────────────────── */

static int d3d11_supports_feature(vio_feature feature)
{
    switch (feature) {
        case VIO_FEATURE_COMPUTE:      return 1; /* compute pipeline + dispatch + readback wired */
        case VIO_FEATURE_TESSELLATION: return 1;
        case VIO_FEATURE_GEOMETRY:     return 1;
        case VIO_FEATURE_RAYTRACING:   return 0; /* No DXR in D3D11 */
        case VIO_FEATURE_MULTIVIEW:    return 0;
        case VIO_FEATURE_3D_PIPELINE:  return 1;
        case VIO_FEATURE_READ_PIXELS:  return 1;
        case VIO_FEATURE_INSTANCED_DRAW: return 1;
        case VIO_FEATURE_RENDER_TARGET:       return 1;
        case VIO_FEATURE_RENDER_TARGET_HDR:   return 1;
        case VIO_FEATURE_RENDER_TARGET_DEPTH: return 1;
        case VIO_FEATURE_RENDER_TARGET_MSAA:  return 1;
        case VIO_FEATURE_CUBEMAP:      return 1;
        case VIO_FEATURE_DEPTH_BIAS:   return 1; /* rasterizer state */
        case VIO_FEATURE_SCISSOR:      return 1;
        case VIO_FEATURE_TEXTURE_SWIZZLE: return 0; /* needs CPU expansion */
        case VIO_FEATURE_NATIVE_2D_BATCH: return 1; /* vio_2d_d3d11_* */
        case VIO_FEATURE_TEXTURE_3D:   return 1; /* ID3D11Texture3D */
        default:                       return 0;
    }
}

/* ── State binding ────────────────────────────────────────────────── */

static void d3d11_set_uniform(const char *name, const void *data, int count, int type)
{
    /* Size in bytes based on type */
    size_t data_size;
    switch (type) {
        case VIO_UNIFORM_INT:   data_size = sizeof(int) * count; break;
        case VIO_UNIFORM_FLOAT: data_size = sizeof(float) * count; break;
        case VIO_UNIFORM_VEC2:  data_size = sizeof(float) * 2 * count; break;
        case VIO_UNIFORM_VEC3:  data_size = sizeof(float) * 3 * count; break;
        case VIO_UNIFORM_VEC4:  data_size = sizeof(float) * 4 * count; break;
        case VIO_UNIFORM_MAT3:  data_size = sizeof(float) * 9 * count; break;
        case VIO_UNIFORM_MAT4:  data_size = sizeof(float) * 16 * count; break;
        default: return;
    }

    UINT aligned_size = (UINT)((data_size + 15) & ~15); /* 16-byte align */

    /* Use cached dynamic constant buffer at slot 0 to avoid per-call CreateBuffer */
    vio_d3d11_cb_cache_entry *entry = &vio_d3d11.cb_cache[0];

    if (!entry->buffer || entry->capacity < aligned_size) {
        /* Need a bigger buffer — release old one and create new */
        if (entry->buffer) {
            ID3D11Buffer_Release(entry->buffer);
            entry->buffer = NULL;
        }

        D3D11_BUFFER_DESC cb_desc = {0};
        cb_desc.ByteWidth = aligned_size;
        cb_desc.Usage = D3D11_USAGE_DYNAMIC;
        cb_desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        cb_desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;

        HRESULT hr = ID3D11Device_CreateBuffer(vio_d3d11.device, &cb_desc, NULL, &entry->buffer);
        if (FAILED(hr)) return;

        entry->capacity = aligned_size;
    }

    /* Update via Map/Unmap (WRITE_DISCARD) */
    D3D11_MAPPED_SUBRESOURCE mapped = {0};
    HRESULT hr = ID3D11DeviceContext_Map(vio_d3d11.context,
                                          (ID3D11Resource *)entry->buffer,
                                          0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
    if (FAILED(hr)) return;

    memcpy(mapped.pData, data, data_size);
    ID3D11DeviceContext_Unmap(vio_d3d11.context,
                              (ID3D11Resource *)entry->buffer, 0);

    ID3D11DeviceContext_VSSetConstantBuffers(vio_d3d11.context, 0, 1, &entry->buffer);
    ID3D11DeviceContext_PSSetConstantBuffers(vio_d3d11.context, 0, 1, &entry->buffer);
}

static void d3d11_bind_texture(void *texture, int slot)
{
    if (!texture) return;
    vio_d3d11_texture *tex = (vio_d3d11_texture *)texture;

    if (tex->srv) {
        ID3D11DeviceContext_PSSetShaderResources(vio_d3d11.context, (UINT)slot, 1, &tex->srv);
    }
    /* Bind regular sampler by default.
     * Comparison sampler is bound via d3d11_bind_texture_cmp (for sampler2DShadow). */
    if (tex->sampler) {
        ID3D11DeviceContext_PSSetSamplers(vio_d3d11.context, (UINT)slot, 1, &tex->sampler);
    }
}

/* Block until the GPU has finished all submitted work (analogue of glFinish).
 * D3D11 is immediate-mode with an implicit command queue; Flush submits,
 * and an event query pins execution until the GPU drains. */
static void d3d11_gpu_flush(void)
{
    if (!vio_d3d11.context || !vio_d3d11.device) return;

    D3D11_QUERY_DESC qd = {0};
    qd.Query = D3D11_QUERY_EVENT;
    ID3D11Query *query = NULL;
    if (FAILED(ID3D11Device_CreateQuery(vio_d3d11.device, &qd, &query)) || !query) {
        ID3D11DeviceContext_Flush(vio_d3d11.context);
        return;
    }

    ID3D11DeviceContext_End(vio_d3d11.context, (ID3D11Asynchronous *)query);
    ID3D11DeviceContext_Flush(vio_d3d11.context);

    BOOL done = FALSE;
    while (ID3D11DeviceContext_GetData(vio_d3d11.context, (ID3D11Asynchronous *)query,
                                       &done, sizeof(done), 0) != S_OK) {
        /* spin — typical drain is microseconds */
    }
    ID3D11Query_Release(query);
}

static void d3d11_set_viewport(int x, int y, int width, int height)
{
    D3D11_VIEWPORT vp = {0};
    vp.TopLeftX = (float)x;
    vp.TopLeftY = (float)y;
    vp.Width = (float)width;
    vp.Height = (float)height;
    vp.MinDepth = 0.0f;
    vp.MaxDepth = 1.0f;
    ID3D11DeviceContext_RSSetViewports(vio_d3d11.context, 1, &vp);
}

/* ── Setup context (called from vio_create after window creation) ── */

int vio_d3d11_setup_context(void *glfw_window, vio_config *cfg)
{
    vio_d3d11.glfw_window = glfw_window;

    /* Create surface (swapchain + render targets) */
    void *surface = d3d11_create_surface(cfg);
    if (!surface) {
        return -1;
    }

    return 0;
}

/* ── Backend registration ─────────────────────────────────────────── */

static const vio_backend d3d11_backend = {
    .name              = "d3d11",
    .api_version       = VIO_BACKEND_API_VERSION,
    .init              = d3d11_init,
    .shutdown          = d3d11_shutdown,
    .create_surface    = d3d11_create_surface,
    .destroy_surface   = d3d11_destroy_surface,
    .resize            = d3d11_resize,
    .create_pipeline   = d3d11_create_pipeline,
    .destroy_pipeline  = d3d11_destroy_pipeline,
    .bind_pipeline     = d3d11_bind_pipeline,
    .create_buffer     = d3d11_create_buffer,
    .update_buffer     = d3d11_update_buffer,
    .destroy_buffer    = d3d11_destroy_buffer,
    .create_texture    = d3d11_create_texture,
    .create_texture_3d = d3d11_create_texture_3d,
    .destroy_texture   = d3d11_destroy_texture,
    .compile_shader    = d3d11_compile_shader,
    .destroy_shader    = d3d11_destroy_shader,
    .begin_frame       = d3d11_begin_frame,
    .end_frame         = d3d11_end_frame,
    .draw              = d3d11_draw,
    .draw_indexed      = d3d11_draw_indexed,
    .present           = d3d11_present,
    .clear             = d3d11_clear,
    .set_uniform       = d3d11_set_uniform,
    .bind_texture      = d3d11_bind_texture,
    .set_viewport      = d3d11_set_viewport,
    .gpu_flush         = d3d11_gpu_flush,
    .dispatch_compute  = d3d11_dispatch_compute,
    .create_compute_pipeline  = d3d11_create_compute_pipeline,
    .destroy_compute_pipeline = d3d11_destroy_compute_pipeline,
    .compute_bind_buffer      = d3d11_compute_bind_buffer,
    .compute_set_uniforms     = d3d11_compute_set_uniforms,
    .read_buffer              = d3d11_read_buffer,
    .supports_feature  = d3d11_supports_feature,
    .destroy_cubemap   = d3d11_destroy_cubemap,
    .destroy_font_atlas = d3d11_destroy_font_atlas,
    .destroy_render_target = d3d11_destroy_render_target,
};

void vio_backend_d3d11_register(void)
{
    vio_register_backend(&d3d11_backend);
}

#endif /* HAVE_D3D11 */

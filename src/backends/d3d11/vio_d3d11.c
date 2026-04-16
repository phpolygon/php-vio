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

    /* Disable ALT+Enter fullscreen toggle */
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
    if (!vio_d3d11.swapchain || (width == vio_d3d11.width && height == vio_d3d11.height)) {
        return;
    }

    /* Must release all references to back buffer before resizing */
    ID3D11DeviceContext_OMSetRenderTargets(vio_d3d11.context, 0, NULL, NULL);
    d3d11_release_views();

    HRESULT hr = IDXGISwapChain1_ResizeBuffers(vio_d3d11.swapchain,
                                                0,  /* keep buffer count */
                                                width, height,
                                                DXGI_FORMAT_UNKNOWN,  /* keep format */
                                                0);
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
            bd.BindFlags = D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_SHADER_RESOURCE;
            bd.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
            bd.StructureByteStride = 4; /* TODO: derive from reflection */
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
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
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
        init_data.SysMemPitch = desc->width * 4; /* RGBA */
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

static void d3d11_destroy_texture(void *texture_ptr)
{
    vio_d3d11_texture *tex = (vio_d3d11_texture *)texture_ptr;
    if (!tex) return;

    if (tex->sampler_cmp) ID3D11SamplerState_Release(tex->sampler_cmp);
    if (tex->sampler) ID3D11SamplerState_Release(tex->sampler);
    if (tex->srv) ID3D11ShaderResourceView_Release(tex->srv);
    if (tex->texture) ID3D11Texture2D_Release(tex->texture);
    free(tex);
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

static void d3d11_end_frame(void)
{
    /* D3D11 is immediate mode — nothing to finalize */
}

static void d3d11_draw(vio_draw_cmd *cmd)
{
    if (!cmd) return;

    vio_d3d11_buffer *vb = (vio_d3d11_buffer *)cmd->vertex_buffer;
    if (vb) {
        UINT stride = cmd->vertex_stride > 0 ? (UINT)cmd->vertex_stride
                    : (d3d11_current_pipeline ? d3d11_current_pipeline->vertex_stride : 0);
        UINT offset = 0;
        ID3D11DeviceContext_IASetVertexBuffers(vio_d3d11.context, 0, 1,
                                               &vb->buffer, &stride, &offset);
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
        UINT offset = 0;
        ID3D11DeviceContext_IASetVertexBuffers(vio_d3d11.context, 0, 1,
                                               &vb->buffer, &stride, &offset);
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
    IDXGISwapChain1_Present(vio_d3d11.swapchain, vio_d3d11.vsync ? 1 : 0, 0);
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

static void d3d11_dispatch_compute(vio_compute_cmd *cmd)
{
    /* TODO: Implement compute shader dispatch
     * 1. Set compute shader (from pipeline)
     * 2. Bind UAVs
     * 3. ID3D11DeviceContext_Dispatch(context, x, y, z)
     */
    (void)cmd;
    php_error_docref(NULL, E_NOTICE, "D3D11: Compute dispatch not yet implemented");
}

/* ── Feature Query ────────────────────────────────────────────────── */

static int d3d11_supports_feature(vio_feature feature)
{
    switch (feature) {
        case VIO_FEATURE_COMPUTE:      return 0; /* TODO: not yet implemented */
        case VIO_FEATURE_TESSELLATION: return 1;
        case VIO_FEATURE_GEOMETRY:     return 1;
        case VIO_FEATURE_RAYTRACING:   return 0; /* No DXR in D3D11 */
        case VIO_FEATURE_MULTIVIEW:    return 0;
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
    .gpu_flush         = NULL,
    .dispatch_compute  = d3d11_dispatch_compute,
    .supports_feature  = d3d11_supports_feature,
};

void vio_backend_d3d11_register(void)
{
    vio_register_backend(&d3d11_backend);
}

#endif /* HAVE_D3D11 */

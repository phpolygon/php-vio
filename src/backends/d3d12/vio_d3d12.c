/*
 * php-vio - Direct3D 12 Backend implementation
 *
 * Uses D3D12 with explicit resource management, command lists, and fence synchronization.
 * Double-buffered swapchain with per-frame command allocators.
 * Shaders: GLSL -> SPIR-V -> HLSL (SM 5.1) via SPIRV-Cross -> DXBC via D3DCompile.
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "php.h"

#ifdef HAVE_D3D12

#define COBJMACROS
#define INITGUID
#include <d3d12.h>
#include <dxgi1_4.h>
#include <d3dcompiler.h>

#ifdef HAVE_GLFW
#define GLFW_INCLUDE_NONE
#define GLFW_EXPOSE_NATIVE_WIN32
#include <GLFW/glfw3.h>
#include <GLFW/glfw3native.h>
#endif

#include "vio_d3d12.h"
#include "../vio_d3d_common.h"
#include <string.h>
#include <stdlib.h>

vio_d3d12_state vio_d3d12 = {0};

/* Currently bound pipeline (for vertex stride in draw calls) */
static vio_d3d12_pipeline *d3d12_current_pipeline = NULL;

/* Forward declarations */
extern char *vio_spirv_to_hlsl(const uint32_t *spirv, size_t spirv_size,
                                int shader_model, char **error_msg);
extern uint32_t *vio_compile_glsl_to_spirv(const char *source, int stage,
                                            size_t *out_size, char **error_msg);

/* ── Helpers ──────────────────────────────────────────────────────── */
/* vio_format_to_dxgi, vio_format_byte_size, vio_usage_to_semantic from vio_d3d_common.h */

static D3D12_PRIMITIVE_TOPOLOGY vio_topology_to_d3d12(vio_topology t)
{
    switch (t) {
        case VIO_TRIANGLES:      return D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
        case VIO_TRIANGLE_STRIP: return D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP;
        case VIO_LINES:          return D3D_PRIMITIVE_TOPOLOGY_LINELIST;
        case VIO_LINE_STRIP:     return D3D_PRIMITIVE_TOPOLOGY_LINESTRIP;
        case VIO_POINTS:         return D3D_PRIMITIVE_TOPOLOGY_POINTLIST;
        case VIO_TRIANGLE_FAN:   return D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
        default:                 return D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
    }
}

static D3D12_PRIMITIVE_TOPOLOGY_TYPE vio_topology_to_d3d12_type(vio_topology t)
{
    switch (t) {
        case VIO_TRIANGLES:
        case VIO_TRIANGLE_STRIP:
        case VIO_TRIANGLE_FAN:   return D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        case VIO_LINES:
        case VIO_LINE_STRIP:     return D3D12_PRIMITIVE_TOPOLOGY_TYPE_LINE;
        case VIO_POINTS:         return D3D12_PRIMITIVE_TOPOLOGY_TYPE_POINT;
        default:                 return D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    }
}

/* ── GPU Synchronization ──────────────────────────────────────────── */

void vio_d3d12_wait_for_gpu(void)
{
    if (!vio_d3d12.cmd_queue || !vio_d3d12.fence) return;

    vio_d3d12.fence_value++;
    ID3D12CommandQueue_Signal(vio_d3d12.cmd_queue, vio_d3d12.fence, vio_d3d12.fence_value);

    if (ID3D12Fence_GetCompletedValue(vio_d3d12.fence) < vio_d3d12.fence_value) {
        ID3D12Fence_SetEventOnCompletion(vio_d3d12.fence, vio_d3d12.fence_value,
                                          vio_d3d12.fence_event);
        WaitForSingleObject(vio_d3d12.fence_event, INFINITE);
    }
}

static void d3d12_wait_for_frame(UINT frame_idx)
{
    vio_d3d12_frame *frame = &vio_d3d12.frames[frame_idx];

    if (ID3D12Fence_GetCompletedValue(vio_d3d12.fence) < frame->fence_value) {
        ID3D12Fence_SetEventOnCompletion(vio_d3d12.fence, frame->fence_value,
                                          vio_d3d12.fence_event);
        WaitForSingleObject(vio_d3d12.fence_event, INFINITE);
    }
}

/* ── Descriptor heap helpers ──────────────────────────────────────── */

static int d3d12_create_descriptor_heap(ID3D12DescriptorHeap **out, D3D12_DESCRIPTOR_HEAP_TYPE type,
                                         UINT count, D3D12_DESCRIPTOR_HEAP_FLAGS flags)
{
    D3D12_DESCRIPTOR_HEAP_DESC desc = {0};
    desc.Type = type;
    desc.NumDescriptors = count;
    desc.Flags = flags;
    desc.NodeMask = 0;

    HRESULT hr = ID3D12Device_CreateDescriptorHeap(vio_d3d12.device, &desc,
                                                    &IID_ID3D12DescriptorHeap, (void **)out);
    if (FAILED(hr)) {
        php_error_docref(NULL, E_WARNING, "D3D12: Failed to create descriptor heap (0x%08lx)", hr);
        return -1;
    }
    return 0;
}

/* Allocate a descriptor from the SRV heap, returns index or UINT_MAX on overflow */
static UINT d3d12_alloc_srv_descriptor(D3D12_CPU_DESCRIPTOR_HANDLE *out_cpu,
                                        D3D12_GPU_DESCRIPTOR_HANDLE *out_gpu)
{
    if (vio_d3d12.srv_heap.count >= vio_d3d12.srv_heap.capacity) {
        php_error_docref(NULL, E_WARNING, "D3D12: SRV descriptor heap full (%u/%u)",
                          vio_d3d12.srv_heap.count, vio_d3d12.srv_heap.capacity);
        memset(out_cpu, 0, sizeof(*out_cpu));
        memset(out_gpu, 0, sizeof(*out_gpu));
        return UINT_MAX;
    }
    UINT idx = vio_d3d12.srv_heap.count++;
    D3D12_CPU_DESCRIPTOR_HANDLE cpu_start;
    D3D12_GPU_DESCRIPTOR_HANDLE gpu_start;

    ID3D12DescriptorHeap_GetCPUDescriptorHandleForHeapStart(vio_d3d12.srv_heap.heap, &cpu_start);
    ID3D12DescriptorHeap_GetGPUDescriptorHandleForHeapStart(vio_d3d12.srv_heap.heap, &gpu_start);

    out_cpu->ptr = cpu_start.ptr + (SIZE_T)(idx * vio_d3d12.srv_heap.descriptor_size);
    out_gpu->ptr = gpu_start.ptr + (UINT64)(idx * vio_d3d12.srv_heap.descriptor_size);
    return idx;
}

/* ── Root Signature ───────────────────────────────────────────────── */

static int d3d12_create_root_signature(void)
{
    /*
     * Root signature layout:
     *   [0] CBV (b0) — per-frame constants
     *   [1] CBV (b1) — per-object constants
     *   [2] Descriptor table: SRV (t0..t8) — textures
     *   [3] Descriptor table: Sampler (s0..s8)
     */
    D3D12_ROOT_PARAMETER params[4] = {0};

    /* CBV b0 */
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    params[0].Descriptor.ShaderRegister = 0;
    params[0].Descriptor.RegisterSpace = 0;
    params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    /* CBV b1 */
    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    params[1].Descriptor.ShaderRegister = 1;
    params[1].Descriptor.RegisterSpace = 0;
    params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    /* SRV table t0-t8 */
    D3D12_DESCRIPTOR_RANGE srv_range = {0};
    srv_range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    srv_range.NumDescriptors = 8;
    srv_range.BaseShaderRegister = 0;
    srv_range.RegisterSpace = 0;
    srv_range.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

    params[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[2].DescriptorTable.NumDescriptorRanges = 1;
    params[2].DescriptorTable.pDescriptorRanges = &srv_range;
    params[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    /* Static sampler instead of sampler table (simpler) */
    D3D12_STATIC_SAMPLER_DESC static_sampler = {0};
    static_sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    static_sampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    static_sampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    static_sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    static_sampler.MaxAnisotropy = 1;
    static_sampler.MaxLOD = D3D12_FLOAT32_MAX;
    static_sampler.ShaderRegister = 0;
    static_sampler.RegisterSpace = 0;
    static_sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_ROOT_SIGNATURE_DESC rs_desc = {0};
    rs_desc.NumParameters = 3; /* CBV0, CBV1, SRV table */
    rs_desc.pParameters = params;
    rs_desc.NumStaticSamplers = 1;
    rs_desc.pStaticSamplers = &static_sampler;
    rs_desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

    ID3DBlob *signature_blob = NULL;
    ID3DBlob *error_blob = NULL;
    HRESULT hr = D3D12SerializeRootSignature(&rs_desc, D3D_ROOT_SIGNATURE_VERSION_1,
                                              &signature_blob, &error_blob);
    if (FAILED(hr)) {
        php_error_docref(NULL, E_WARNING, "D3D12: Failed to serialize root signature: %s",
                          error_blob ? (char *)ID3D10Blob_GetBufferPointer(error_blob) : "unknown");
        if (error_blob) ID3D10Blob_Release(error_blob);
        return -1;
    }

    hr = ID3D12Device_CreateRootSignature(vio_d3d12.device, 0,
                                           ID3D10Blob_GetBufferPointer(signature_blob),
                                           ID3D10Blob_GetBufferSize(signature_blob),
                                           &IID_ID3D12RootSignature,
                                           (void **)&vio_d3d12.root_signature);
    ID3D10Blob_Release(signature_blob);
    if (FAILED(hr)) {
        php_error_docref(NULL, E_WARNING, "D3D12: Failed to create root signature (0x%08lx)", hr);
        return -1;
    }

    return 0;
}

/* ── Render target views ──────────────────────────────────────────── */

static int d3d12_create_render_targets(void)
{
    D3D12_CPU_DESCRIPTOR_HANDLE rtv_handle;
    ID3D12DescriptorHeap_GetCPUDescriptorHandleForHeapStart(vio_d3d12.rtv_heap, &rtv_handle);

    for (UINT i = 0; i < VIO_D3D12_FRAME_COUNT; i++) {
        HRESULT hr = IDXGISwapChain3_GetBuffer(vio_d3d12.swapchain, i,
                                                &IID_ID3D12Resource,
                                                (void **)&vio_d3d12.frames[i].render_target);
        if (FAILED(hr)) {
            php_error_docref(NULL, E_WARNING, "D3D12: Failed to get swapchain buffer %u (0x%08lx)", i, hr);
            return -1;
        }

        vio_d3d12.frames[i].rtv_handle = rtv_handle;
        ID3D12Device_CreateRenderTargetView(vio_d3d12.device,
                                             vio_d3d12.frames[i].render_target,
                                             NULL, rtv_handle);
        rtv_handle.ptr += vio_d3d12.rtv_descriptor_size;
    }

    return 0;
}

static void d3d12_release_render_targets(void)
{
    for (UINT i = 0; i < VIO_D3D12_FRAME_COUNT; i++) {
        if (vio_d3d12.frames[i].render_target) {
            ID3D12Resource_Release(vio_d3d12.frames[i].render_target);
            vio_d3d12.frames[i].render_target = NULL;
        }
    }
}

/* ── Depth buffer ─────────────────────────────────────────────────── */

static int d3d12_create_depth_buffer(int width, int height)
{
    D3D12_HEAP_PROPERTIES heap_props = {0};
    heap_props.Type = D3D12_HEAP_TYPE_DEFAULT;

    D3D12_RESOURCE_DESC depth_desc = {0};
    depth_desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    depth_desc.Width = width;
    depth_desc.Height = height;
    depth_desc.DepthOrArraySize = 1;
    depth_desc.MipLevels = 1;
    depth_desc.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
    depth_desc.SampleDesc.Count = 1;
    depth_desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;

    D3D12_CLEAR_VALUE clear_value = {0};
    clear_value.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
    clear_value.DepthStencil.Depth = 1.0f;
    clear_value.DepthStencil.Stencil = 0;

    HRESULT hr = ID3D12Device_CreateCommittedResource(vio_d3d12.device,
                                                       &heap_props,
                                                       D3D12_HEAP_FLAG_NONE,
                                                       &depth_desc,
                                                       D3D12_RESOURCE_STATE_DEPTH_WRITE,
                                                       &clear_value,
                                                       &IID_ID3D12Resource,
                                                       (void **)&vio_d3d12.depth_buffer);
    if (FAILED(hr)) {
        php_error_docref(NULL, E_WARNING, "D3D12: Failed to create depth buffer (0x%08lx)", hr);
        return -1;
    }

    /* Create DSV */
    D3D12_DEPTH_STENCIL_VIEW_DESC dsv_desc = {0};
    dsv_desc.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
    dsv_desc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;

    D3D12_CPU_DESCRIPTOR_HANDLE dsv_handle;
    ID3D12DescriptorHeap_GetCPUDescriptorHandleForHeapStart(vio_d3d12.dsv_heap, &dsv_handle);
    ID3D12Device_CreateDepthStencilView(vio_d3d12.device, vio_d3d12.depth_buffer,
                                         &dsv_desc, dsv_handle);

    return 0;
}

/* ── Lifecycle ────────────────────────────────────────────────────── */

static void d3d12_shutdown(void);

static int d3d12_init(vio_config *cfg)
{
    HRESULT hr;

    /* Enable debug layer */
    if (cfg->debug) {
        ID3D12Debug *debug_controller = NULL;
        if (SUCCEEDED(D3D12GetDebugInterface(&IID_ID3D12Debug, (void **)&debug_controller))) {
            ID3D12Debug_EnableDebugLayer(debug_controller);
            ID3D12Debug_Release(debug_controller);
        }
        vio_d3d12.debug_enabled = 1;
    }

    /* Create DXGI factory */
    UINT factory_flags = vio_d3d12.debug_enabled ? DXGI_CREATE_FACTORY_DEBUG : 0;
    hr = CreateDXGIFactory2(factory_flags, &IID_IDXGIFactory4, (void **)&vio_d3d12.factory);
    if (FAILED(hr)) {
        php_error_docref(NULL, E_WARNING, "D3D12: Failed to create DXGI factory (0x%08lx)", hr);
        goto init_fail;
    }

    /* Select adapter (WARP for headless, hardware otherwise) */
    IDXGIAdapter1 *adapter = NULL;
    if (cfg->headless) {
        hr = IDXGIFactory4_EnumWarpAdapter(vio_d3d12.factory, &IID_IDXGIAdapter1, (void **)&adapter);
        if (FAILED(hr)) {
            php_error_docref(NULL, E_WARNING, "D3D12: WARP adapter not available (0x%08lx)", hr);
            goto init_fail;
        }
    } else {
        /* Pick first hardware adapter that supports D3D12 */
        for (UINT i = 0; IDXGIFactory4_EnumAdapters1(vio_d3d12.factory, i, &adapter) != DXGI_ERROR_NOT_FOUND; i++) {
            DXGI_ADAPTER_DESC1 desc;
            IDXGIAdapter1_GetDesc1(adapter, &desc);

            /* Skip software adapters */
            if (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) {
                IDXGIAdapter1_Release(adapter);
                adapter = NULL;
                continue;
            }

            /* Check if adapter supports D3D12 */
            if (SUCCEEDED(D3D12CreateDevice((IUnknown *)adapter, D3D_FEATURE_LEVEL_11_0,
                                             &IID_ID3D12Device, NULL))) {
                break;
            }

            IDXGIAdapter1_Release(adapter);
            adapter = NULL;
        }
    }

    if (!adapter) {
        php_error_docref(NULL, E_WARNING, "D3D12: No suitable adapter found");
        goto init_fail;
    }

    /* Create device */
    hr = D3D12CreateDevice((IUnknown *)adapter, D3D_FEATURE_LEVEL_11_0,
                            &IID_ID3D12Device, (void **)&vio_d3d12.device);
    IDXGIAdapter1_Release(adapter);
    if (FAILED(hr)) {
        php_error_docref(NULL, E_WARNING, "D3D12: Failed to create device (0x%08lx)", hr);
        goto init_fail;
    }

    /* Create command queue */
    D3D12_COMMAND_QUEUE_DESC queue_desc = {0};
    queue_desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    queue_desc.Priority = D3D12_COMMAND_QUEUE_PRIORITY_NORMAL;
    queue_desc.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;

    hr = ID3D12Device_CreateCommandQueue(vio_d3d12.device, &queue_desc,
                                          &IID_ID3D12CommandQueue,
                                          (void **)&vio_d3d12.cmd_queue);
    if (FAILED(hr)) {
        php_error_docref(NULL, E_WARNING, "D3D12: Failed to create command queue (0x%08lx)", hr);
        goto init_fail;
    }

    /* Create per-frame command allocators */
    for (UINT i = 0; i < VIO_D3D12_FRAME_COUNT; i++) {
        hr = ID3D12Device_CreateCommandAllocator(vio_d3d12.device,
                                                  D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                  &IID_ID3D12CommandAllocator,
                                                  (void **)&vio_d3d12.frames[i].cmd_allocator);
        if (FAILED(hr)) {
            php_error_docref(NULL, E_WARNING, "D3D12: Failed to create command allocator %u (0x%08lx)", i, hr);
            goto init_fail;
        }
    }

    /* Create command list (initially closed) */
    hr = ID3D12Device_CreateCommandList(vio_d3d12.device, 0,
                                         D3D12_COMMAND_LIST_TYPE_DIRECT,
                                         vio_d3d12.frames[0].cmd_allocator,
                                         NULL, /* initial PSO */
                                         &IID_ID3D12GraphicsCommandList,
                                         (void **)&vio_d3d12.cmd_list);
    if (FAILED(hr)) {
        php_error_docref(NULL, E_WARNING, "D3D12: Failed to create command list (0x%08lx)", hr);
        goto init_fail;
    }
    /* Close it immediately — will be reset in begin_frame */
    ID3D12GraphicsCommandList_Close(vio_d3d12.cmd_list);

    /* Create fence */
    hr = ID3D12Device_CreateFence(vio_d3d12.device, 0, D3D12_FENCE_FLAG_NONE,
                                   &IID_ID3D12Fence, (void **)&vio_d3d12.fence);
    if (FAILED(hr)) {
        php_error_docref(NULL, E_WARNING, "D3D12: Failed to create fence (0x%08lx)", hr);
        goto init_fail;
    }
    vio_d3d12.fence_event = CreateEvent(NULL, FALSE, FALSE, NULL);
    vio_d3d12.fence_value = 0;

    /* Create descriptor heaps */
    if (d3d12_create_descriptor_heap(&vio_d3d12.rtv_heap,
                                      D3D12_DESCRIPTOR_HEAP_TYPE_RTV,
                                      VIO_D3D12_FRAME_COUNT,
                                      D3D12_DESCRIPTOR_HEAP_FLAG_NONE) != 0) {
        goto init_fail;
    }
    vio_d3d12.rtv_descriptor_size = ID3D12Device_GetDescriptorHandleIncrementSize(
        vio_d3d12.device, D3D12_DESCRIPTOR_HEAP_TYPE_RTV);

    if (d3d12_create_descriptor_heap(&vio_d3d12.dsv_heap,
                                      D3D12_DESCRIPTOR_HEAP_TYPE_DSV, 1,
                                      D3D12_DESCRIPTOR_HEAP_FLAG_NONE) != 0) {
        goto init_fail;
    }

    /* GPU-visible SRV/CBV/UAV heap */
    if (d3d12_create_descriptor_heap(&vio_d3d12.srv_heap.heap,
                                      D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,
                                      VIO_D3D12_MAX_SRV_DESCRIPTORS,
                                      D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE) != 0) {
        goto init_fail;
    }
    vio_d3d12.srv_heap.descriptor_size = ID3D12Device_GetDescriptorHandleIncrementSize(
        vio_d3d12.device, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    vio_d3d12.srv_heap.capacity = VIO_D3D12_MAX_SRV_DESCRIPTORS;
    vio_d3d12.srv_heap.count = 0;

    /* Root signature */
    if (d3d12_create_root_signature() != 0) {
        goto init_fail;
    }

    vio_d3d12.vsync = cfg->vsync;
    vio_d3d12.initialized = 1;
    return 0;

init_fail:
    /* Clean up anything already created */
    vio_d3d12.initialized = 1; /* allow shutdown to run */
    d3d12_shutdown();
    return -1;
}

static void d3d12_shutdown(void)
{
    if (!vio_d3d12.initialized) return;

    /* Wait for GPU to finish all work */
    vio_d3d12_wait_for_gpu();

    d3d12_release_render_targets();

    if (vio_d3d12.depth_buffer) {
        ID3D12Resource_Release(vio_d3d12.depth_buffer);
    }

    for (UINT i = 0; i < VIO_D3D12_FRAME_COUNT; i++) {
        if (vio_d3d12.frames[i].cmd_allocator) {
            ID3D12CommandAllocator_Release(vio_d3d12.frames[i].cmd_allocator);
        }
    }

    if (vio_d3d12.cmd_list)       ID3D12GraphicsCommandList_Release(vio_d3d12.cmd_list);
    if (vio_d3d12.root_signature) ID3D12RootSignature_Release(vio_d3d12.root_signature);
    if (vio_d3d12.fence)          ID3D12Fence_Release(vio_d3d12.fence);
    if (vio_d3d12.fence_event)    CloseHandle(vio_d3d12.fence_event);
    if (vio_d3d12.rtv_heap)       ID3D12DescriptorHeap_Release(vio_d3d12.rtv_heap);
    if (vio_d3d12.dsv_heap)       ID3D12DescriptorHeap_Release(vio_d3d12.dsv_heap);
    if (vio_d3d12.srv_heap.heap)  ID3D12DescriptorHeap_Release(vio_d3d12.srv_heap.heap);
    if (vio_d3d12.swapchain)      IDXGISwapChain3_Release(vio_d3d12.swapchain);
    if (vio_d3d12.cmd_queue)      ID3D12CommandQueue_Release(vio_d3d12.cmd_queue);
    if (vio_d3d12.factory)        IDXGIFactory4_Release(vio_d3d12.factory);
    if (vio_d3d12.device)         ID3D12Device_Release(vio_d3d12.device);

    d3d12_current_pipeline = NULL;
    memset(&vio_d3d12, 0, sizeof(vio_d3d12));
}

/* ── Surface & Window ─────────────────────────────────────────────── */

static void *d3d12_create_surface(vio_config *cfg)
{
#ifdef HAVE_GLFW
    if (!vio_d3d12.glfw_window) {
        php_error_docref(NULL, E_WARNING, "D3D12: No GLFW window set");
        return NULL;
    }

    HWND hwnd = glfwGetWin32Window((GLFWwindow *)vio_d3d12.glfw_window);

    DXGI_SWAP_CHAIN_DESC1 sc_desc = {0};
    sc_desc.Width = cfg->width;
    sc_desc.Height = cfg->height;
    sc_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sc_desc.Stereo = FALSE;
    sc_desc.SampleDesc.Count = 1;
    sc_desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sc_desc.BufferCount = VIO_D3D12_FRAME_COUNT;
    sc_desc.Scaling = DXGI_SCALING_STRETCH;
    sc_desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    sc_desc.AlphaMode = DXGI_ALPHA_MODE_UNSPECIFIED;

    IDXGISwapChain1 *swapchain1 = NULL;
    HRESULT hr = IDXGIFactory4_CreateSwapChainForHwnd(
        vio_d3d12.factory,
        (IUnknown *)vio_d3d12.cmd_queue,  /* D3D12 uses command queue, not device */
        hwnd,
        &sc_desc,
        NULL, NULL,
        &swapchain1
    );
    if (FAILED(hr)) {
        php_error_docref(NULL, E_WARNING, "D3D12: Failed to create swapchain (0x%08lx)", hr);
        return NULL;
    }

    /* QI for IDXGISwapChain3 (needed for GetCurrentBackBufferIndex) */
    hr = IDXGISwapChain1_QueryInterface(swapchain1, &IID_IDXGISwapChain3,
                                         (void **)&vio_d3d12.swapchain);
    IDXGISwapChain1_Release(swapchain1);
    if (FAILED(hr)) {
        php_error_docref(NULL, E_WARNING, "D3D12: SwapChain3 not supported (0x%08lx)", hr);
        return NULL;
    }

    /* Disable ALT+Enter */
    IDXGIFactory4_MakeWindowAssociation(vio_d3d12.factory, hwnd, DXGI_MWA_NO_ALT_ENTER);

    vio_d3d12.width = cfg->width;
    vio_d3d12.height = cfg->height;
    vio_d3d12.frame_index = IDXGISwapChain3_GetCurrentBackBufferIndex(vio_d3d12.swapchain);

    /* Create render target views for each frame */
    if (d3d12_create_render_targets() != 0) {
        return NULL;
    }

    /* Create depth buffer */
    if (d3d12_create_depth_buffer(cfg->width, cfg->height) != 0) {
        return NULL;
    }

    return vio_d3d12.swapchain;
#else
    (void)cfg;
    php_error_docref(NULL, E_WARNING, "D3D12: Built without GLFW, cannot create surface");
    return NULL;
#endif
}

static void d3d12_destroy_surface(void *surface)
{
    (void)surface;
    vio_d3d12_wait_for_gpu();

    d3d12_release_render_targets();

    if (vio_d3d12.depth_buffer) {
        ID3D12Resource_Release(vio_d3d12.depth_buffer);
        vio_d3d12.depth_buffer = NULL;
    }

    if (vio_d3d12.swapchain) {
        IDXGISwapChain3_Release(vio_d3d12.swapchain);
        vio_d3d12.swapchain = NULL;
    }
}

static void d3d12_resize(int width, int height)
{
    if (!vio_d3d12.swapchain || (width == vio_d3d12.width && height == vio_d3d12.height)) {
        return;
    }

    /* GPU must be idle before resize */
    vio_d3d12_wait_for_gpu();

    d3d12_release_render_targets();
    if (vio_d3d12.depth_buffer) {
        ID3D12Resource_Release(vio_d3d12.depth_buffer);
        vio_d3d12.depth_buffer = NULL;
    }

    HRESULT hr = IDXGISwapChain3_ResizeBuffers(vio_d3d12.swapchain,
                                                VIO_D3D12_FRAME_COUNT,
                                                width, height,
                                                DXGI_FORMAT_R8G8B8A8_UNORM, 0);
    if (FAILED(hr)) {
        php_error_docref(NULL, E_WARNING, "D3D12: Failed to resize buffers (0x%08lx)", hr);
        return;
    }

    vio_d3d12.width = width;
    vio_d3d12.height = height;
    vio_d3d12.frame_index = IDXGISwapChain3_GetCurrentBackBufferIndex(vio_d3d12.swapchain);

    d3d12_create_render_targets();
    d3d12_create_depth_buffer(width, height);
}

/* ── Pipeline ─────────────────────────────────────────────────────── */

static void *d3d12_create_pipeline(vio_pipeline_desc *desc)
{
    vio_d3d12_pipeline *pipeline = calloc(1, sizeof(vio_d3d12_pipeline));
    if (!pipeline) return NULL;

    vio_d3d12_shader *shader = (vio_d3d12_shader *)desc->shader;
    if (!shader) { free(pipeline); return NULL; }

    pipeline->topology = vio_topology_to_d3d12(desc->topology);

    /* Build input layout */
    D3D12_INPUT_ELEMENT_DESC *elements = NULL;
    UINT vertex_stride = 0;
    if (desc->vertex_attrib_count > 0 && desc->vertex_layout) {
        elements = calloc(desc->vertex_attrib_count, sizeof(D3D12_INPUT_ELEMENT_DESC));
        UINT offset = 0;
        for (int i = 0; i < desc->vertex_attrib_count; i++) {
            elements[i].SemanticName = vio_usage_to_semantic(desc->vertex_layout[i].usage);
            elements[i].SemanticIndex = 0;
            elements[i].Format = vio_format_to_dxgi(desc->vertex_layout[i].format);
            elements[i].InputSlot = 0;
            elements[i].AlignedByteOffset = offset;
            elements[i].InputSlotClass = D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA;
            offset += vio_format_byte_size(desc->vertex_layout[i].format);
        }
        vertex_stride = offset;
    }
    pipeline->vertex_stride = vertex_stride;

    /* PSO description */
    D3D12_GRAPHICS_PIPELINE_STATE_DESC pso_desc = {0};
    pso_desc.pRootSignature = vio_d3d12.root_signature;

    /* Shaders */
    pso_desc.VS.pShaderBytecode = ID3D10Blob_GetBufferPointer(shader->vs_blob);
    pso_desc.VS.BytecodeLength = ID3D10Blob_GetBufferSize(shader->vs_blob);
    pso_desc.PS.pShaderBytecode = ID3D10Blob_GetBufferPointer(shader->ps_blob);
    pso_desc.PS.BytecodeLength = ID3D10Blob_GetBufferSize(shader->ps_blob);

    /* Input layout */
    pso_desc.InputLayout.pInputElementDescs = elements;
    pso_desc.InputLayout.NumElements = desc->vertex_attrib_count;

    /* Rasterizer */
    pso_desc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    switch (desc->cull_mode) {
        case VIO_CULL_NONE:  pso_desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE; break;
        case VIO_CULL_BACK:  pso_desc.RasterizerState.CullMode = D3D12_CULL_MODE_BACK; break;
        case VIO_CULL_FRONT: pso_desc.RasterizerState.CullMode = D3D12_CULL_MODE_FRONT; break;
    }
    pso_desc.RasterizerState.FrontCounterClockwise = TRUE;
    pso_desc.RasterizerState.DepthClipEnable = TRUE;

    /* Depth-stencil */
    pso_desc.DepthStencilState.DepthEnable = desc->depth_test ? TRUE : FALSE;
    pso_desc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
    pso_desc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS;
    pso_desc.DSVFormat = DXGI_FORMAT_D24_UNORM_S8_UINT;

    /* Blend */
    pso_desc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    if (desc->blend == VIO_BLEND_ALPHA) {
        pso_desc.BlendState.RenderTarget[0].BlendEnable = TRUE;
        pso_desc.BlendState.RenderTarget[0].SrcBlend = D3D12_BLEND_SRC_ALPHA;
        pso_desc.BlendState.RenderTarget[0].DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
        pso_desc.BlendState.RenderTarget[0].BlendOp = D3D12_BLEND_OP_ADD;
        pso_desc.BlendState.RenderTarget[0].SrcBlendAlpha = D3D12_BLEND_ONE;
        pso_desc.BlendState.RenderTarget[0].DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA;
        pso_desc.BlendState.RenderTarget[0].BlendOpAlpha = D3D12_BLEND_OP_ADD;
    } else if (desc->blend == VIO_BLEND_ADDITIVE) {
        pso_desc.BlendState.RenderTarget[0].BlendEnable = TRUE;
        pso_desc.BlendState.RenderTarget[0].SrcBlend = D3D12_BLEND_SRC_ALPHA;
        pso_desc.BlendState.RenderTarget[0].DestBlend = D3D12_BLEND_ONE;
        pso_desc.BlendState.RenderTarget[0].BlendOp = D3D12_BLEND_OP_ADD;
        pso_desc.BlendState.RenderTarget[0].SrcBlendAlpha = D3D12_BLEND_ONE;
        pso_desc.BlendState.RenderTarget[0].DestBlendAlpha = D3D12_BLEND_ONE;
        pso_desc.BlendState.RenderTarget[0].BlendOpAlpha = D3D12_BLEND_OP_ADD;
    }

    /* Topology type */
    pso_desc.PrimitiveTopologyType = vio_topology_to_d3d12_type(desc->topology);

    /* Render target format */
    pso_desc.NumRenderTargets = 1;
    pso_desc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;

    /* MSAA */
    pso_desc.SampleDesc.Count = 1;
    pso_desc.SampleMask = UINT_MAX;

    HRESULT hr = ID3D12Device_CreateGraphicsPipelineState(vio_d3d12.device, &pso_desc,
                                                           &IID_ID3D12PipelineState,
                                                           (void **)&pipeline->pso);
    if (elements) free(elements);
    if (FAILED(hr)) {
        php_error_docref(NULL, E_WARNING, "D3D12: Failed to create PSO (0x%08lx)", hr);
        free(pipeline);
        return NULL;
    }

    return pipeline;
}

static void d3d12_destroy_pipeline(void *pipeline_ptr)
{
    vio_d3d12_pipeline *p = (vio_d3d12_pipeline *)pipeline_ptr;
    if (!p) return;
    if (d3d12_current_pipeline == p) d3d12_current_pipeline = NULL;
    if (p->pso) ID3D12PipelineState_Release(p->pso);
    free(p);
}

static void d3d12_bind_pipeline(void *pipeline_ptr)
{
    vio_d3d12_pipeline *p = (vio_d3d12_pipeline *)pipeline_ptr;
    if (!p) return;

    d3d12_current_pipeline = p;
    ID3D12GraphicsCommandList_SetPipelineState(vio_d3d12.cmd_list, p->pso);
    ID3D12GraphicsCommandList_SetGraphicsRootSignature(vio_d3d12.cmd_list,
                                                        vio_d3d12.root_signature);
    ID3D12GraphicsCommandList_IASetPrimitiveTopology(vio_d3d12.cmd_list, p->topology);

    /* Bind SRV heap */
    ID3D12DescriptorHeap *heaps[] = { vio_d3d12.srv_heap.heap };
    ID3D12GraphicsCommandList_SetDescriptorHeaps(vio_d3d12.cmd_list, 1, heaps);
}

/* ── Resources: Buffers ───────────────────────────────────────────── */

static void *d3d12_create_buffer(vio_buffer_desc *desc)
{
    vio_d3d12_buffer *buf = calloc(1, sizeof(vio_d3d12_buffer));
    if (!buf) return NULL;

    buf->type = desc->type;
    buf->size = desc->size;
    buf->binding = desc->binding;

    D3D12_HEAP_PROPERTIES heap_props = {0};
    D3D12_RESOURCE_DESC res_desc = {0};
    D3D12_RESOURCE_STATES initial_state;

    res_desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    res_desc.Width = desc->size;
    res_desc.Height = 1;
    res_desc.DepthOrArraySize = 1;
    res_desc.MipLevels = 1;
    res_desc.SampleDesc.Count = 1;
    res_desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    if (desc->type == VIO_BUFFER_UNIFORM) {
        /* Uniform buffers: upload heap for frequent CPU writes */
        heap_props.Type = D3D12_HEAP_TYPE_UPLOAD;
        initial_state = D3D12_RESOURCE_STATE_GENERIC_READ;
        /* CB size must be 256-byte aligned */
        res_desc.Width = (res_desc.Width + 255) & ~255;
    } else if (desc->type == VIO_BUFFER_STORAGE) {
        heap_props.Type = D3D12_HEAP_TYPE_DEFAULT;
        initial_state = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        res_desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    } else {
        /* Vertex/index: upload heap for simplicity (can optimize to default+staging later) */
        heap_props.Type = D3D12_HEAP_TYPE_UPLOAD;
        initial_state = D3D12_RESOURCE_STATE_GENERIC_READ;
    }

    HRESULT hr = ID3D12Device_CreateCommittedResource(vio_d3d12.device,
                                                       &heap_props,
                                                       D3D12_HEAP_FLAG_NONE,
                                                       &res_desc,
                                                       initial_state,
                                                       NULL,
                                                       &IID_ID3D12Resource,
                                                       (void **)&buf->resource);
    if (FAILED(hr)) {
        php_error_docref(NULL, E_WARNING, "D3D12: Failed to create buffer (0x%08lx)", hr);
        free(buf);
        return NULL;
    }

    buf->gpu_address = ID3D12Resource_GetGPUVirtualAddress(buf->resource);

    /* Upload initial data */
    if (desc->data && heap_props.Type == D3D12_HEAP_TYPE_UPLOAD) {
        void *mapped = NULL;
        D3D12_RANGE read_range = {0, 0}; /* We don't read */
        hr = ID3D12Resource_Map(buf->resource, 0, &read_range, &mapped);
        if (SUCCEEDED(hr)) {
            memcpy(mapped, desc->data, desc->size);
            ID3D12Resource_Unmap(buf->resource, 0, NULL);
        }
    }

    return buf;
}

static void d3d12_update_buffer(void *buffer_ptr, const void *data, size_t size)
{
    vio_d3d12_buffer *buf = (vio_d3d12_buffer *)buffer_ptr;
    if (!buf || !buf->resource || !data) return;

    void *mapped = NULL;
    D3D12_RANGE read_range = {0, 0};
    HRESULT hr = ID3D12Resource_Map(buf->resource, 0, &read_range, &mapped);
    if (SUCCEEDED(hr)) {
        memcpy(mapped, data, size);
        ID3D12Resource_Unmap(buf->resource, 0, NULL);
    }
}

static void d3d12_destroy_buffer(void *buffer_ptr)
{
    vio_d3d12_buffer *buf = (vio_d3d12_buffer *)buffer_ptr;
    if (!buf) return;
    if (buf->upload_resource) ID3D12Resource_Release(buf->upload_resource);
    if (buf->resource) ID3D12Resource_Release(buf->resource);
    free(buf);
}

/* ── Resources: Textures ──────────────────────────────────────────── */

static void *d3d12_create_texture(vio_texture_desc *desc)
{
    vio_d3d12_texture *tex = calloc(1, sizeof(vio_d3d12_texture));
    if (!tex) return NULL;

    tex->width = desc->width;
    tex->height = desc->height;

    /* Create texture resource on default heap */
    D3D12_HEAP_PROPERTIES heap_props = {0};
    heap_props.Type = D3D12_HEAP_TYPE_DEFAULT;

    D3D12_RESOURCE_DESC res_desc = {0};
    res_desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    res_desc.Width = desc->width;
    res_desc.Height = desc->height;
    res_desc.DepthOrArraySize = 1;
    res_desc.MipLevels = 1;
    res_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    res_desc.SampleDesc.Count = 1;
    res_desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;

    HRESULT hr = ID3D12Device_CreateCommittedResource(vio_d3d12.device,
                                                       &heap_props,
                                                       D3D12_HEAP_FLAG_NONE,
                                                       &res_desc,
                                                       D3D12_RESOURCE_STATE_COPY_DEST,
                                                       NULL,
                                                       &IID_ID3D12Resource,
                                                       (void **)&tex->resource);
    if (FAILED(hr)) {
        free(tex);
        return NULL;
    }

    /* Upload data via staging buffer */
    if (desc->data) {
        UINT64 upload_size = 0;
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint = {0};
        ID3D12Device_GetCopyableFootprints(vio_d3d12.device, &res_desc,
                                            0, 1, 0, &footprint, NULL, NULL, &upload_size);

        D3D12_HEAP_PROPERTIES upload_heap = {0};
        upload_heap.Type = D3D12_HEAP_TYPE_UPLOAD;

        D3D12_RESOURCE_DESC upload_desc = {0};
        upload_desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        upload_desc.Width = upload_size;
        upload_desc.Height = 1;
        upload_desc.DepthOrArraySize = 1;
        upload_desc.MipLevels = 1;
        upload_desc.SampleDesc.Count = 1;
        upload_desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

        hr = ID3D12Device_CreateCommittedResource(vio_d3d12.device,
                                                   &upload_heap,
                                                   D3D12_HEAP_FLAG_NONE,
                                                   &upload_desc,
                                                   D3D12_RESOURCE_STATE_GENERIC_READ,
                                                   NULL,
                                                   &IID_ID3D12Resource,
                                                   (void **)&tex->upload_resource);
        if (SUCCEEDED(hr)) {
            /* Copy pixel data to upload buffer */
            void *mapped = NULL;
            D3D12_RANGE read_range = {0, 0};
            hr = ID3D12Resource_Map(tex->upload_resource, 0, &read_range, &mapped);
            if (SUCCEEDED(hr)) {
                const uint8_t *src = (const uint8_t *)desc->data;
                uint8_t *dst = (uint8_t *)mapped + footprint.Offset;
                UINT src_pitch = desc->width * 4;
                for (int row = 0; row < desc->height; row++) {
                    memcpy(dst + row * footprint.Footprint.RowPitch,
                           src + row * src_pitch,
                           src_pitch);
                }
                ID3D12Resource_Unmap(tex->upload_resource, 0, NULL);
            }

            /* Execute copy on a temporary command list */
            ID3D12CommandAllocator *upload_alloc = NULL;
            ID3D12GraphicsCommandList *upload_cmd = NULL;

            hr = ID3D12Device_CreateCommandAllocator(vio_d3d12.device,
                D3D12_COMMAND_LIST_TYPE_DIRECT, &IID_ID3D12CommandAllocator,
                (void **)&upload_alloc);
            if (SUCCEEDED(hr)) {
                hr = ID3D12Device_CreateCommandList(vio_d3d12.device, 0,
                    D3D12_COMMAND_LIST_TYPE_DIRECT, upload_alloc, NULL,
                    &IID_ID3D12GraphicsCommandList, (void **)&upload_cmd);
            }
            if (SUCCEEDED(hr)) {
                D3D12_TEXTURE_COPY_LOCATION dst_loc = {0};
                dst_loc.pResource = tex->resource;
                dst_loc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
                dst_loc.SubresourceIndex = 0;

                D3D12_TEXTURE_COPY_LOCATION src_loc = {0};
                src_loc.pResource = tex->upload_resource;
                src_loc.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
                src_loc.PlacedFootprint = footprint;

                ID3D12GraphicsCommandList_CopyTextureRegion(upload_cmd,
                    &dst_loc, 0, 0, 0, &src_loc, NULL);

                /* Transition: COPY_DEST -> PIXEL_SHADER_RESOURCE */
                D3D12_RESOURCE_BARRIER barrier = {0};
                barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                barrier.Transition.pResource = tex->resource;
                barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
                barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
                barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
                ID3D12GraphicsCommandList_ResourceBarrier(upload_cmd, 1, &barrier);

                ID3D12GraphicsCommandList_Close(upload_cmd);

                ID3D12CommandList *lists[] = { (ID3D12CommandList *)upload_cmd };
                ID3D12CommandQueue_ExecuteCommandLists(vio_d3d12.cmd_queue, 1, lists);

                /* Wait for upload to finish */
                vio_d3d12_wait_for_gpu();
            }
            if (upload_cmd) ID3D12GraphicsCommandList_Release(upload_cmd);
            if (upload_alloc) ID3D12CommandAllocator_Release(upload_alloc);

            /* Release staging buffer — data is on the GPU now */
            ID3D12Resource_Release(tex->upload_resource);
            tex->upload_resource = NULL;
        }
    }

    /* Create SRV */
    D3D12_SHADER_RESOURCE_VIEW_DESC srv_desc = {0};
    srv_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    srv_desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srv_desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv_desc.Texture2D.MipLevels = 1;

    d3d12_alloc_srv_descriptor(&tex->srv_cpu, &tex->srv_gpu);
    ID3D12Device_CreateShaderResourceView(vio_d3d12.device, tex->resource,
                                           &srv_desc, tex->srv_cpu);

    return tex;
}

static void d3d12_destroy_texture(void *texture_ptr)
{
    vio_d3d12_texture *tex = (vio_d3d12_texture *)texture_ptr;
    if (!tex) return;
    if (tex->upload_resource) ID3D12Resource_Release(tex->upload_resource);
    if (tex->resource) ID3D12Resource_Release(tex->resource);
    /* Note: descriptor in SRV heap is leaked (linear allocator doesn't support free).
     * A proper free-list allocator would reclaim the slot. */
    free(tex);
}

/* ── Shaders ──────────────────────────────────────────────────────── */

static void *d3d12_compile_shader(vio_shader_desc *desc)
{
    vio_d3d12_shader *shader = calloc(1, sizeof(vio_d3d12_shader));
    if (!shader) return NULL;

    const char *hlsl_vs = NULL;
    const char *hlsl_ps = NULL;
    char *allocated_vs = NULL;
    char *allocated_ps = NULL;

    if (desc->format == VIO_SHADER_GLSL || desc->format == VIO_SHADER_AUTO) {
        char *err = NULL;
        size_t vs_spirv_size = 0, ps_spirv_size = 0;

        uint32_t *vs_spirv = vio_compile_glsl_to_spirv(
            (const char *)desc->vertex_data, 0, &vs_spirv_size, &err);
        if (!vs_spirv) {
            php_error_docref(NULL, E_WARNING, "D3D12: VS GLSL->SPIR-V failed: %s", err ? err : "unknown");
            if (err) free(err);
            free(shader);
            return NULL;
        }

        uint32_t *ps_spirv = vio_compile_glsl_to_spirv(
            (const char *)desc->fragment_data, 1, &ps_spirv_size, &err);
        if (!ps_spirv) {
            php_error_docref(NULL, E_WARNING, "D3D12: PS GLSL->SPIR-V failed: %s", err ? err : "unknown");
            if (err) free(err);
            free(vs_spirv);
            free(shader);
            return NULL;
        }

        /* SPIR-V -> HLSL SM 5.1 (D3D12 needs register spaces) */
        allocated_vs = vio_spirv_to_hlsl(vs_spirv, vs_spirv_size, 51, &err);
        free(vs_spirv);
        if (!allocated_vs) {
            php_error_docref(NULL, E_WARNING, "D3D12: VS SPIR-V->HLSL failed: %s", err ? err : "unknown");
            if (err) free(err);
            free(ps_spirv);
            free(shader);
            return NULL;
        }

        allocated_ps = vio_spirv_to_hlsl(ps_spirv, ps_spirv_size, 51, &err);
        free(ps_spirv);
        if (!allocated_ps) {
            php_error_docref(NULL, E_WARNING, "D3D12: PS SPIR-V->HLSL failed: %s", err ? err : "unknown");
            if (err) free(err);
            free(allocated_vs);
            free(shader);
            return NULL;
        }

        hlsl_vs = allocated_vs;
        hlsl_ps = allocated_ps;
    } else {
        hlsl_vs = (const char *)desc->vertex_data;
        hlsl_ps = (const char *)desc->fragment_data;
    }

    UINT compile_flags = 0;
    if (vio_d3d12.debug_enabled) {
        compile_flags |= D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION;
    }

    ID3DBlob *error_blob = NULL;
    HRESULT hr;

    hr = D3DCompile(hlsl_vs, strlen(hlsl_vs), "vs_main", NULL, NULL,
                     "main", "vs_5_1", compile_flags, 0, &shader->vs_blob, &error_blob);
    if (FAILED(hr)) {
        php_error_docref(NULL, E_WARNING, "D3D12: VS compile failed: %s",
                          error_blob ? (char *)ID3D10Blob_GetBufferPointer(error_blob) : "unknown");
        if (error_blob) ID3D10Blob_Release(error_blob);
        goto fail;
    }

    hr = D3DCompile(hlsl_ps, strlen(hlsl_ps), "ps_main", NULL, NULL,
                     "main", "ps_5_1", compile_flags, 0, &shader->ps_blob, &error_blob);
    if (FAILED(hr)) {
        php_error_docref(NULL, E_WARNING, "D3D12: PS compile failed: %s",
                          error_blob ? (char *)ID3D10Blob_GetBufferPointer(error_blob) : "unknown");
        if (error_blob) ID3D10Blob_Release(error_blob);
        goto fail;
    }

    if (allocated_vs) free(allocated_vs);
    if (allocated_ps) free(allocated_ps);
    return shader;

fail:
    if (allocated_vs) free(allocated_vs);
    if (allocated_ps) free(allocated_ps);
    if (shader->vs_blob) ID3D10Blob_Release(shader->vs_blob);
    if (shader->ps_blob) ID3D10Blob_Release(shader->ps_blob);
    free(shader);
    return NULL;
}

static void d3d12_destroy_shader(void *shader_ptr)
{
    vio_d3d12_shader *s = (vio_d3d12_shader *)shader_ptr;
    if (!s) return;
    if (s->vs_blob) ID3D10Blob_Release(s->vs_blob);
    if (s->ps_blob) ID3D10Blob_Release(s->ps_blob);
    free(s);
}

/* ── Drawing ──────────────────────────────────────────────────────── */

static void d3d12_begin_frame(void)
{
    vio_d3d12_frame *frame = &vio_d3d12.frames[vio_d3d12.frame_index];

    /* Wait for this frame's previous work to complete */
    d3d12_wait_for_frame(vio_d3d12.frame_index);

    /* Reset command allocator and command list */
    ID3D12CommandAllocator_Reset(frame->cmd_allocator);
    ID3D12GraphicsCommandList_Reset(vio_d3d12.cmd_list, frame->cmd_allocator, NULL);

    /* Transition render target: PRESENT -> RENDER_TARGET */
    D3D12_RESOURCE_BARRIER barrier = {0};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = frame->render_target;
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    ID3D12GraphicsCommandList_ResourceBarrier(vio_d3d12.cmd_list, 1, &barrier);

    /* Set render target */
    D3D12_CPU_DESCRIPTOR_HANDLE dsv_handle;
    ID3D12DescriptorHeap_GetCPUDescriptorHandleForHeapStart(vio_d3d12.dsv_heap, &dsv_handle);
    ID3D12GraphicsCommandList_OMSetRenderTargets(vio_d3d12.cmd_list, 1,
                                                  &frame->rtv_handle, FALSE, &dsv_handle);

    /* Set viewport and scissor */
    D3D12_VIEWPORT vp = {0};
    vp.Width = (float)vio_d3d12.width;
    vp.Height = (float)vio_d3d12.height;
    vp.MinDepth = 0.0f;
    vp.MaxDepth = 1.0f;
    ID3D12GraphicsCommandList_RSSetViewports(vio_d3d12.cmd_list, 1, &vp);

    D3D12_RECT scissor = {0, 0, vio_d3d12.width, vio_d3d12.height};
    ID3D12GraphicsCommandList_RSSetScissorRects(vio_d3d12.cmd_list, 1, &scissor);
}

static void d3d12_end_frame(void)
{
    vio_d3d12_frame *frame = &vio_d3d12.frames[vio_d3d12.frame_index];

    /* Transition render target: RENDER_TARGET -> PRESENT */
    D3D12_RESOURCE_BARRIER barrier = {0};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = frame->render_target;
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    ID3D12GraphicsCommandList_ResourceBarrier(vio_d3d12.cmd_list, 1, &barrier);

    /* Close and execute command list */
    ID3D12GraphicsCommandList_Close(vio_d3d12.cmd_list);

    ID3D12CommandList *cmd_lists[] = { (ID3D12CommandList *)vio_d3d12.cmd_list };
    ID3D12CommandQueue_ExecuteCommandLists(vio_d3d12.cmd_queue, 1, cmd_lists);
}

static void d3d12_draw(vio_draw_cmd *cmd)
{
    if (!cmd) return;

    vio_d3d12_buffer *vb = (vio_d3d12_buffer *)cmd->vertex_buffer;
    if (vb) {
        D3D12_VERTEX_BUFFER_VIEW vbv = {0};
        vbv.BufferLocation = vb->gpu_address;
        vbv.SizeInBytes = (UINT)vb->size;
        vbv.StrideInBytes = d3d12_current_pipeline ? d3d12_current_pipeline->vertex_stride : 0;
        ID3D12GraphicsCommandList_IASetVertexBuffers(vio_d3d12.cmd_list, 0, 1, &vbv);
    }

    UINT instance_count = cmd->instance_count > 0 ? cmd->instance_count : 1;
    ID3D12GraphicsCommandList_DrawInstanced(vio_d3d12.cmd_list,
                                             cmd->vertex_count,
                                             instance_count,
                                             cmd->first_vertex, 0);
}

static void d3d12_draw_indexed(vio_draw_indexed_cmd *cmd)
{
    if (!cmd) return;

    vio_d3d12_buffer *vb = (vio_d3d12_buffer *)cmd->vertex_buffer;
    vio_d3d12_buffer *ib = (vio_d3d12_buffer *)cmd->index_buffer;

    if (vb) {
        D3D12_VERTEX_BUFFER_VIEW vbv = {0};
        vbv.BufferLocation = vb->gpu_address;
        vbv.SizeInBytes = (UINT)vb->size;
        vbv.StrideInBytes = d3d12_current_pipeline ? d3d12_current_pipeline->vertex_stride : 0;
        ID3D12GraphicsCommandList_IASetVertexBuffers(vio_d3d12.cmd_list, 0, 1, &vbv);
    }

    if (ib) {
        D3D12_INDEX_BUFFER_VIEW ibv = {0};
        ibv.BufferLocation = ib->gpu_address;
        ibv.SizeInBytes = (UINT)ib->size;
        ibv.Format = DXGI_FORMAT_R32_UINT;
        ID3D12GraphicsCommandList_IASetIndexBuffer(vio_d3d12.cmd_list, &ibv);
    }

    UINT instance_count = cmd->instance_count > 0 ? cmd->instance_count : 1;
    ID3D12GraphicsCommandList_DrawIndexedInstanced(vio_d3d12.cmd_list,
                                                    cmd->index_count,
                                                    instance_count,
                                                    cmd->first_index,
                                                    cmd->vertex_offset, 0);
}

static void d3d12_present(void)
{
    if (!vio_d3d12.swapchain) return;

    IDXGISwapChain3_Present(vio_d3d12.swapchain, vio_d3d12.vsync ? 1 : 0, 0);

    /* Signal fence for current frame */
    vio_d3d12.fence_value++;
    vio_d3d12.frames[vio_d3d12.frame_index].fence_value = vio_d3d12.fence_value;
    ID3D12CommandQueue_Signal(vio_d3d12.cmd_queue, vio_d3d12.fence, vio_d3d12.fence_value);

    /* Move to next frame */
    vio_d3d12.frame_index = IDXGISwapChain3_GetCurrentBackBufferIndex(vio_d3d12.swapchain);
}

static void d3d12_clear(float r, float g, float b, float a)
{
    vio_d3d12_frame *frame = &vio_d3d12.frames[vio_d3d12.frame_index];
    float color[4] = {r, g, b, a};

    ID3D12GraphicsCommandList_ClearRenderTargetView(vio_d3d12.cmd_list,
                                                     frame->rtv_handle,
                                                     color, 0, NULL);

    D3D12_CPU_DESCRIPTOR_HANDLE dsv_handle;
    ID3D12DescriptorHeap_GetCPUDescriptorHandleForHeapStart(vio_d3d12.dsv_heap, &dsv_handle);
    ID3D12GraphicsCommandList_ClearDepthStencilView(vio_d3d12.cmd_list,
                                                     dsv_handle,
                                                     D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL,
                                                     1.0f, 0, 0, NULL);
}

/* ── Compute ──────────────────────────────────────────────────────── */

static void d3d12_dispatch_compute(vio_compute_cmd *cmd)
{
    /* TODO: Implement compute pipeline + dispatch
     * 1. Create compute PSO (separate from graphics PSO)
     * 2. Bind UAVs via root signature
     * 3. ID3D12GraphicsCommandList_Dispatch(cmd_list, x, y, z)
     */
    (void)cmd;
    php_error_docref(NULL, E_NOTICE, "D3D12: Compute dispatch not yet implemented");
}

/* ── Feature Query ────────────────────────────────────────────────── */

static int d3d12_supports_feature(vio_feature feature)
{
    switch (feature) {
        case VIO_FEATURE_COMPUTE:      return 0; /* TODO: not yet implemented */
        case VIO_FEATURE_TESSELLATION: return 1;
        case VIO_FEATURE_GEOMETRY:     return 1;
        case VIO_FEATURE_RAYTRACING:   return 0; /* DXR possible but not implemented */
        case VIO_FEATURE_MULTIVIEW:    return 0;
        default:                       return 0;
    }
}

/* ── Backend registration ─────────────────────────────────────────── */

static const vio_backend d3d12_backend = {
    .name              = "d3d12",
    .api_version       = VIO_BACKEND_API_VERSION,
    .init              = d3d12_init,
    .shutdown          = d3d12_shutdown,
    .create_surface    = d3d12_create_surface,
    .destroy_surface   = d3d12_destroy_surface,
    .resize            = d3d12_resize,
    .create_pipeline   = d3d12_create_pipeline,
    .destroy_pipeline  = d3d12_destroy_pipeline,
    .bind_pipeline     = d3d12_bind_pipeline,
    .create_buffer     = d3d12_create_buffer,
    .update_buffer     = d3d12_update_buffer,
    .destroy_buffer    = d3d12_destroy_buffer,
    .create_texture    = d3d12_create_texture,
    .destroy_texture   = d3d12_destroy_texture,
    .compile_shader    = d3d12_compile_shader,
    .destroy_shader    = d3d12_destroy_shader,
    .begin_frame       = d3d12_begin_frame,
    .end_frame         = d3d12_end_frame,
    .draw              = d3d12_draw,
    .draw_indexed      = d3d12_draw_indexed,
    .present           = d3d12_present,
    .clear             = d3d12_clear,
    .gpu_flush         = NULL,
    .dispatch_compute  = d3d12_dispatch_compute,
    .supports_feature  = d3d12_supports_feature,
};

void vio_backend_d3d12_register(void)
{
    vio_register_backend(&d3d12_backend);
}

#endif /* HAVE_D3D12 */

/*
 * php-vio - Direct3D 12 Backend
 */

#ifndef VIO_D3D12_H
#define VIO_D3D12_H

#include "../../../include/vio_backend.h"

#ifdef HAVE_D3D12

#include <d3d12.h>
#include <dxgi1_4.h>
#include <d3dcompiler.h>

#define VIO_D3D12_FRAME_COUNT 2
#define VIO_D3D12_MAX_SRV_DESCRIPTORS 1024

/* Compiled shader pair (vertex + pixel) */
typedef struct _vio_d3d12_shader {
    ID3DBlob *vs_blob;
    ID3DBlob *ps_blob;
} vio_d3d12_shader;

/* Pipeline = PSO + root signature reference */
typedef struct _vio_d3d12_pipeline {
    ID3D12PipelineState    *pso;
    D3D12_PRIMITIVE_TOPOLOGY topology;
    UINT                     vertex_stride;
} vio_d3d12_pipeline;

/* Buffer wrapper */
typedef struct _vio_d3d12_buffer {
    ID3D12Resource  *resource;
    ID3D12Resource  *upload_resource;  /* staging buffer for default heap resources */
    vio_buffer_type  type;
    size_t           size;
    int              binding;
    D3D12_GPU_VIRTUAL_ADDRESS gpu_address;
} vio_d3d12_buffer;

/* Texture wrapper */
typedef struct _vio_d3d12_texture {
    ID3D12Resource          *resource;
    ID3D12Resource          *upload_resource;
    D3D12_CPU_DESCRIPTOR_HANDLE srv_cpu;
    D3D12_GPU_DESCRIPTOR_HANDLE srv_gpu;
    int width;
    int height;
} vio_d3d12_texture;

/* Per-frame resources */
typedef struct _vio_d3d12_frame {
    ID3D12CommandAllocator  *cmd_allocator;
    ID3D12Resource          *render_target;
    D3D12_CPU_DESCRIPTOR_HANDLE rtv_handle;
    UINT64                   fence_value;
} vio_d3d12_frame;

/* Simple linear descriptor allocator for CBV/SRV/UAV heap */
typedef struct _vio_d3d12_descriptor_heap {
    ID3D12DescriptorHeap *heap;
    UINT                  descriptor_size;
    UINT                  capacity;
    UINT                  count;
} vio_d3d12_descriptor_heap;

/* Global D3D12 state */
typedef struct _vio_d3d12_state {
    /* Device & queues */
    ID3D12Device              *device;
    ID3D12CommandQueue        *cmd_queue;
    ID3D12GraphicsCommandList *cmd_list;

    /* DXGI */
    IDXGISwapChain3           *swapchain;
    IDXGIFactory4             *factory;
    UINT                       frame_index;

    /* Descriptor heaps */
    ID3D12DescriptorHeap      *rtv_heap;
    UINT                       rtv_descriptor_size;
    ID3D12DescriptorHeap      *dsv_heap;
    vio_d3d12_descriptor_heap  srv_heap;  /* GPU-visible CBV/SRV/UAV */

    /* Depth buffer */
    ID3D12Resource            *depth_buffer;

    /* Synchronization */
    ID3D12Fence               *fence;
    HANDLE                     fence_event;
    UINT64                     fence_value;

    /* Per-frame resources */
    vio_d3d12_frame            frames[VIO_D3D12_FRAME_COUNT];

    /* Root signature (shared across all pipelines) */
    ID3D12RootSignature       *root_signature;

    /* Currently bound render target (NULL = backbuffer) */
    D3D12_CPU_DESCRIPTOR_HANDLE current_rtv;
    D3D12_CPU_DESCRIPTOR_HANDLE current_dsv;
    int current_rt_width;
    int current_rt_height;
    int current_has_rtv;  /* 0 = depth-only when offscreen */

    /* State */
    int   initialized;
    float clear_r, clear_g, clear_b, clear_a;
    int   width, height;
    int   vsync;

    /* Debug */
    int   debug_enabled;

    /* Window reference */
    void *glfw_window;
} vio_d3d12_state;

extern vio_d3d12_state vio_d3d12;

/* Registration */
void vio_backend_d3d12_register(void);

/* Called after GLFW window creation to set up D3D12 */
int vio_d3d12_setup_context(void *glfw_window, vio_config *cfg);

/* Waits for GPU to finish all pending work */
void vio_d3d12_wait_for_gpu(void);

#endif /* HAVE_D3D12 */
#endif /* VIO_D3D12_H */

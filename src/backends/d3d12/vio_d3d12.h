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
#define VIO_D3D12_MAX_SRV_DESCRIPTORS 32768

/* Size of the per-draw SRV descriptor table (root param [2], registers t0..tN-1).
 * Must cover the highest texture register any shader binds. The mesh shader uses
 * t0 (albedo) plus four sampler2DShadow at t4..t7 (SPIRV-Cross assigns depth
 * samplers starting at register 4 — see vio_shader_reflect.c). Sized to 16 to
 * leave headroom for shaders that combine several regular textures with the
 * shadow registers, so no shadow SRV is ever dropped at bind time. */
#define VIO_D3D12_SRV_TABLE_SIZE 16

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

    /* Index of the most recently presented backbuffer. frame_index is
     * updated to the NEXT buffer right after Present(), so code paths
     * that want to read the just-rendered frame (vio_read_pixels) must
     * use this instead. Initialised to 0 before any Present happens. */
    UINT                       last_presented_frame_idx;

    /* Descriptor heaps */
    ID3D12DescriptorHeap      *rtv_heap;
    UINT                       rtv_descriptor_size;
    ID3D12DescriptorHeap      *dsv_heap;
    vio_d3d12_descriptor_heap  srv_heap;          /* GPU-visible CBV/SRV/UAV */
    ID3D12DescriptorHeap      *srv_staging_heap;  /* CPU-only staging mirror for srv_heap. D3D12
                                                   * forbids CopyDescriptorsSimple from reading a
                                                   * shader-visible heap (CPU write-only); static
                                                   * texture SRVs live here so they can be the
                                                   * source operand when flush_srv_table mirrors
                                                   * them into the per-frame shader-visible region. */

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
    void *current_bound_rt; /* vio_render_target_object* for barrier tracking */
    void *pending_bound_rt; /* vio_render_target_object* requested via vio_bind_render_target
                             * while the command list was closed (before vio_begin); applied
                             * by vio_begin once the frame's command list is open. */

    /* Pending texture bindings (flushed before each draw into a contiguous SRV block) */
    D3D12_CPU_DESCRIPTOR_HANDLE pending_srvs[VIO_D3D12_SRV_TABLE_SIZE]; /* CPU handles of bound textures */
    int                          pending_srv_valid[VIO_D3D12_SRV_TABLE_SIZE]; /* 1 if slot has a texture */

    /* Per-frame linear SRV descriptor allocator (contiguous blocks for descriptor tables).
     *
     * The descriptor heap is split each begin_frame: indices [0, srv_heap.count)
     * are static SRVs (one per texture/render-target/cubemap, monotonically
     * appended via d3d12_alloc_srv_descriptor); the remainder is partitioned
     * into VIO_D3D12_FRAME_COUNT equal regions, one per in-flight frame slot.
     * Per-frame writes never touch indices below srv_heap.count, so they can
     * never overwrite a static SRV — even when the game loads enough textures
     * (fonts × sizes, sprites, language flags, etc.) to push the static count
     * past any compile-time reservation. */
    UINT                       srv_frame_offset;        /* current allocation offset in srv_heap */
    UINT                       srv_frame_base;          /* base of THIS frame's region (recomputed each begin_frame) */
    UINT                       srv_frame_capacity;      /* descriptors per frame slot (recomputed each begin_frame) */

    /* Per-frame linear cbuffer allocator (avoids overwriting between draw calls) */
    ID3D12Resource            *cbuffer_heap;          /* large UPLOAD heap */
    D3D12_GPU_VIRTUAL_ADDRESS  cbuffer_heap_gpu;
    unsigned char             *cbuffer_heap_mapped;    /* persistently mapped */
    UINT                       cbuffer_heap_offset;    /* current allocation offset */
    UINT                       cbuffer_heap_capacity;  /* total size */

    /* Dummy identity instance buffer (bound to slot 1 for non-instanced draws) */
    ID3D12Resource            *identity_instance_buf;
    D3D12_GPU_VIRTUAL_ADDRESS  identity_instance_gpu;

    /* State */
    int   initialized;
    int   in_frame;     /* 1 between begin_frame() and end_frame(); cmd_list is
                         * only safe to record into while this is set. Higher-
                         * level code (VioRenderer2D, etc.) issues some calls
                         * before begin_frame() to satisfy D3D11 semantics —
                         * those become no-ops for D3D12 instead of recording
                         * onto a closed command list. */
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

/* Flush pending texture bindings into a contiguous SRV block (call before draw) */
void vio_d3d12_flush_srv_table(void);

/* Waits for GPU to finish all pending work */
void vio_d3d12_wait_for_gpu(void);

/* Capture the composited backbuffer at its true (current swapchain) resolution
 * as top-down RGBA8. Works both mid-frame (in_frame==1: flushes the open frame
 * command list, copies the buffer being drawn THIS frame, then re-opens the
 * frame so vio_end can complete normally) and after Present (in_frame==0: reads
 * the last presented buffer). On success returns a malloc'd buffer the caller
 * must free() and writes *out_w/*out_h/*out_size; returns NULL on failure. */
unsigned char *vio_d3d12_capture_frame(int *out_w, int *out_h, size_t *out_size);

#endif /* HAVE_D3D12 */
#endif /* VIO_D3D12_H */

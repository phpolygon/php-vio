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
    int              stride;           /* structured element stride (bytes); 0 => raw/4 */
    D3D12_GPU_VIRTUAL_ADDRESS gpu_address;
    /* Compute readback: a READBACK-heap staging buffer the output (UAV) buffer's
     * contents are copied into by dispatch_compute, so vio_storage_buffer_read
     * can Map+memcpy without re-running the GPU. Lazily created on first read. */
    ID3D12Resource  *readback_resource;
    size_t           readback_size;    /* valid bytes currently in readback_resource */
} vio_d3d12_buffer;

/* Max storage-buffer bindings per compute pipeline (SRV t# + UAV u#). */
#define VIO_D3D12_COMPUTE_MAX_BINDINGS 8

/* One recorded storage-buffer binding on a compute pipeline. */
typedef struct _vio_d3d12_compute_binding {
    struct _vio_d3d12_buffer *buffer;  /* the bound storage buffer */
    int slot;                          /* shader register index (t# or u#) */
    int access;                        /* 0 = READ (SRV), 1 = WRITE (UAV) */
    int element_count;                 /* NumElements for the structured view */
    int stride;                        /* StructureByteStride */
} vio_d3d12_compute_binding;

/* Compute pipeline = compute PSO + cs blob + recorded bindings + params buffer.
 *
 * The root signature is PER-PIPELINE and DATA-DRIVEN from SPIR-V reflection: the
 * CBV register (b#), the SRV table base register (t#) and the UAV table base
 * register (u#) are read from the actual transpiled shader, NOT hardcoded. This
 * is required because the canonical SDF shader uses DISTINCT GLSL bindings
 * (boxes=0, dist=1, Params=2) which spirv-cross maps to t0 / u1 / b2 — a shared
 * t0/u0/b0 root signature would mismatch the shader (debug-layer error 882). */
typedef struct _vio_d3d12_compute_pipeline {
    ID3D12PipelineState *pso;
    ID3D12RootSignature *root_signature;  /* per-pipeline, built from reflection */
    ID3DBlob            *cs_blob;
    vio_d3d12_compute_binding srvs[VIO_D3D12_COMPUTE_MAX_BINDINGS];
    int                 srv_count;
    vio_d3d12_compute_binding uavs[VIO_D3D12_COMPUTE_MAX_BINDINGS];
    int                 uav_count;
    /* Reflected register layout (filled at pipeline creation):
     *   cbv_register   — the Params UBO's HLSL register (b#); -1 if no UBO.
     *   srv_base_reg   — lowest readonly  storage-buffer register (t#); table base.
     *   uav_base_reg   — lowest writeonly storage-buffer register (u#); table base.
     * The SRV/UAV descriptor ranges use these as BaseShaderRegister so a buffer
     * bound at GLSL binding=slot lands on register t{slot}/u{slot}. */
    int                 cbv_register;
    int                 srv_base_reg;
    int                 uav_base_reg;
    /* Params constant block. An UPLOAD-heap buffer, persistently re-mapped by
     * compute_set_uniforms (256-byte aligned per the CB requirement). Bound to
     * the reflected cbv_register. */
    ID3D12Resource     *params_buf;
    size_t              params_capacity;  /* allocated bytes (256-aligned) */
    size_t              params_size;      /* bytes actually staged */
} vio_d3d12_compute_pipeline;

/* Texture wrapper */
typedef struct _vio_d3d12_texture {
    ID3D12Resource          *resource;
    ID3D12Resource          *upload_resource;
    D3D12_CPU_DESCRIPTOR_HANDLE srv_cpu;
    D3D12_GPU_DESCRIPTOR_HANDLE srv_gpu;
    int width;
    int height;
    int depth;   /* > 0 for 3D / volume textures */
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

    /* Selected-adapter info, captured once at init from DXGI_ADAPTER_DESC1 of
     * the adapter we actually created the device on. Read back by vio_gpu_info().
     * gpu_name is UTF-8 (converted from the WCHAR Description); empty if unknown.
     * vram_bytes = DedicatedVideoMemory; 0 if unknown (e.g. WARP/headless). */
    char                       gpu_name[256];
    uint64_t                   vram_bytes;

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

    /* Compute root signature: [0] root CBV b0, [1] SRV table (t0..), [2] UAV
     * table (u0..), ALL visibility, no input-assembler flag. Created lazily on
     * first compute dispatch (so non-compute runs pay nothing), released at
     * shutdown. Paired with a dedicated shader-visible CBV/SRV/UAV heap used
     * solely by compute dispatches, kept fully separate from the graphics
     * srv_heap and its per-frame partitioning. */
    ID3D12RootSignature       *compute_root_signature;
    ID3D12DescriptorHeap      *compute_srv_heap;     /* shader-visible, compute-only */
    UINT                       compute_srv_descriptor_size;

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

    /* SRV-table flush dedup (Lever #1 amplifier).
     *
     * flush_srv_table builds a fresh 16-descriptor block + SetGraphicsRootDescriptorTable
     * before EVERY draw. When the bound texture set is unchanged between draws (the common
     * case in a material-sorted opaque batch) the previously-built block is still valid for
     * the rest of THIS frame, so we re-point root param 2 at the cached GPU handle and skip
     * the 16 CreateShaderResourceView (null) + up-to-16 CopyDescriptorsSimple rebuild.
     *
     * The dedup is CONTENT-based (compares pending_srvs against the last-flushed snapshot)
     * so it is correct for every caller, including the 2D batch renderer which writes
     * pending_srvs directly (bypassing d3d12_bind_texture). srv_table_bound is forced to 0
     * at begin_frame (the per-frame ring rebases, so last frame's GPU handle is stale) and on
     * d3d12_bind_pipeline (SetGraphicsRootSignature/SetDescriptorHeaps may reset root param 2).
     */
    D3D12_CPU_DESCRIPTOR_HANDLE srv_flushed[VIO_D3D12_SRV_TABLE_SIZE];   /* texture set baked into the cached block */
    int                         srv_flushed_valid[VIO_D3D12_SRV_TABLE_SIZE];
    D3D12_GPU_DESCRIPTOR_HANDLE srv_table_gpu;          /* GPU handle of the cached block (valid this frame) */
    int                         srv_table_bound;         /* 1 => srv_table_gpu holds a valid block bound this frame */

    /* Per-frame linear cbuffer allocator (avoids overwriting between draw calls).
     * The heap is split into VIO_D3D12_FRAME_COUNT equal slices (like the SRV
     * frame regions): frame N allocates ONLY inside its own slice, so the CPU
     * never overwrites memory another in-flight frame still reads via root CBV. */
    ID3D12Resource            *cbuffer_heap;          /* large UPLOAD heap */
    D3D12_GPU_VIRTUAL_ADDRESS  cbuffer_heap_gpu;
    unsigned char             *cbuffer_heap_mapped;    /* persistently mapped */
    UINT                       cbuffer_heap_offset;    /* current allocation offset */
    UINT                       cbuffer_heap_capacity;  /* total size */
    UINT                       cbuffer_frame_base;     /* base of THIS frame's slice */
    UINT                       cbuffer_frame_end;      /* end of THIS frame's slice */

    /* Dummy identity instance buffer (bound to slot 1 for non-instanced draws) */
    ID3D12Resource            *identity_instance_buf;
    D3D12_GPU_VIRTUAL_ADDRESS  identity_instance_gpu;

    /* Per-frame linear instance-data allocator (vio_draw_instanced slot-1 VBV).
     * Identical lifetime/sync semantics to the cbuffer heap above: a single
     * shared static buffer let every instanced draw in a frame alias the SAME
     * GPU VA, so at ExecuteCommandLists only the LAST-uploaded matrices survived
     * (earlier district/terminal/particle batches rendered with the wrong
     * instances -> invisible/displaced), and a mid-frame overflow Released the
     * buffer out from under already-recorded draws -> GPU use-after-free ->
     * intermittent DEVICE_REMOVED. Each draw now gets its own stable slice in
     * THIS frame's region; the other in-flight frame reads its own region. */
    ID3D12Resource            *instance_heap;          /* large UPLOAD heap */
    D3D12_GPU_VIRTUAL_ADDRESS  instance_heap_gpu;
    unsigned char             *instance_heap_mapped;    /* persistently mapped */
    UINT                       instance_heap_offset;    /* current allocation offset */
    UINT                       instance_heap_capacity;  /* total size */
    UINT                       instance_frame_base;     /* base of THIS frame's slice */
    UINT                       instance_frame_end;      /* end of THIS frame's slice */

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
    int   dred_enabled;   /* 1 if DRED auto-breadcrumbs/page-fault were FORCED_ON at init */
    int   device_lost;    /* set once on first detected device-removed; gates all further
                           * Present/record so we log the cause ONCE instead of spamming
                           * "Present failed" every frame until the 64KB log fills up */

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

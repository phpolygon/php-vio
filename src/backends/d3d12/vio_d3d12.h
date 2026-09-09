/*
 * php-vio - Direct3D 12 Backend
 */

#ifndef VIO_D3D12_H
#define VIO_D3D12_H

#include "../../../include/vio_backend.h"

#ifdef HAVE_D3D12

#include <d3d12.h>
#include <dxgi1_4.h>
/* dxgi1_5.h: IDXGIFactory5 + DXGI_FEATURE_PRESENT_ALLOW_TEARING, needed to query
 * variable-refresh / uncapped-present support. Ships in every Windows 10 SDK that
 * also has d3d12.h, which config.w32 already requires. Mirrors the D3D11 backend. */
#include <dxgi1_5.h>
#include <d3dcompiler.h>

/* Present flag is a #define in dxgi.h; guard for very old SDKs. The swapchain flag
 * (DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING = 2048) is an enum member from the same
 * header generation as dxgi1_5.h, so it needs no fallback. */
#ifndef DXGI_PRESENT_ALLOW_TEARING
#define DXGI_PRESENT_ALLOW_TEARING 0x00000200UL
#endif

/* Backbuffers == in-flight frame slots: how many frames the CPU may run ahead of
 * the GPU. begin_frame() waits on frames[frame_index].fence_value before it may
 * Reset that slot's allocator, so this number IS the pipeline depth.
 *
 * At 2 the CPU blocks on the submit->present->fence round trip of the frame two
 * back; with a cheap scene that latency — not the GPU work — becomes the
 * frame-time floor. A third slot absorbs it.
 *
 * It is NOT hardcoded to 3, because the count also divides the per-frame SRV,
 * constant-buffer and instance-heap slices: going 2 -> 3 shrinks every slice from
 * 1/2 to 1/3 of its heap. A scene already close to a heap limit would start
 * overflowing. So the default stays at the long-standing 2 (no silent regression
 * for existing games) and a game opts in:
 *
 *     vio_create('d3d12', ['frame_count' => 3, ...]);
 *
 * Runtime value: vio_d3d12.frame_count (clamped to [MIN, MAX]). Everything
 * downstream — swapchain BufferCount, ResizeBuffers, frames[], and the heap
 * slices — reads that field, never a macro. The macro below only sizes the
 * fixed-length frames[] array. */
#define VIO_D3D12_MIN_FRAME_COUNT     2
#define VIO_D3D12_MAX_FRAME_COUNT     3
#define VIO_D3D12_FRAME_COUNT_DEFAULT 2
#define VIO_D3D12_MAX_SRV_DESCRIPTORS 32768

/* Size of the per-draw SRV descriptor table (root param [2], registers t0..tN-1).
 * Must cover the highest texture register any shader binds. The mesh shader uses
 * t0 (albedo) plus four sampler2DShadow at t4..t7 (SPIRV-Cross assigns depth
 * samplers starting at register 4 — see vio_shader_reflect.c). Sized to 16 to
 * leave headroom for shaders that combine several regular textures with the
 * shadow registers, so no shadow SRV is ever dropped at bind time. */
#define VIO_D3D12_SRV_TABLE_SIZE 16

/* Per-draw SAMPLER descriptor table (root param [4], registers s0..s7). One
 * sampler per regular texture register t0..t7; the comparison samplers for
 * shadow maps stay STATIC at s8..s11 (SPIRV-Cross assigns depth samplers from
 * register 8 — see vio_shader_reflect.c).
 *
 * Before the GAP-PLAN (Phase 1) s0..s7 were static LINEAR/WRAP samplers, so a
 * texture's filter / wrap / anisotropy were silently ignored on D3D12 (NEAREST
 * pixel-art blurred, CLAMP sprites bled). Now every texture carries the index of
 * a pre-built sampler combination (filter x wrap x anisotropy) in a CPU-only
 * "combo" heap and the per-draw flush copies the eight bound combos into a
 * shader-visible per-frame ring, exactly like the SRV table. */
#define VIO_D3D12_SAMPLER_TABLE_SIZE  8
#define VIO_D3D12_SAMPLER_ANISO_LEVELS 5     /* 1, 2, 4, 8, 16 */
#define VIO_D3D12_SAMPLER_COMBOS      (2 * 3 * VIO_D3D12_SAMPLER_ANISO_LEVELS)
/* Shader-visible sampler heaps are capped at 2048 descriptors by D3D12. */
#define VIO_D3D12_SAMPLER_HEAP_CAPACITY 2048
/* Distinct sampler sets remembered per frame (each set = one 8-descriptor
 * block); the ring is only advanced for a set not seen this frame. */
#define VIO_D3D12_SAMPLER_SET_CACHE    64

/* Upload queue (GAP-PLAN 4.1): allocators in flight before a submit has to
 * wait for its own previous upload. */
#define VIO_D3D12_UPLOAD_ALLOCATORS    3

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
    /* 1 => DEFAULT-heap (GPU-local) resource: static vertex / index buffers
     * created with initial data (GAP-PLAN 4.2). Not CPU-mappable — updates go
     * through a staging copy on the upload queue. */
    int              default_heap;
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
    /* Storage images (RWTexture2D/3D): a texture UAV descriptor is written into
     * the UAV table region at (slot - uav_base_reg) at dispatch time. */
    struct { struct _vio_d3d12_texture *tex; int slot; int access; } images[VIO_D3D12_COMPUTE_MAX_BINDINGS];
    int                 image_count;
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
    /* Index into the sampler combo heap (vio_d3d12_sampler_combo()). 0 ==
     * LINEAR / REPEAT / no anisotropy, i.e. what the old static samplers did —
     * so a calloc'd wrapper (render-target textures) keeps the legacy look. */
    int sampler_index;
    int mip_levels;    /* 1, or the full chain for 'mipmaps' => true */
    int channels;      /* 1 (R8) or 4 (RGBA8) — for update_texture / mip gen */
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

    /* Tearing support (DXGI flag contract + VRR). Same as the D3D11 backend.
     *
     * NOT a throughput fix. Measured on a 144Hz display: without these flags a
     * windowed FLIP_DISCARD swapchain at SyncInterval=0 already presents at
     * thousands of fps. DWM does not block Present(0,0) — it drops frames it never
     * shows. Do not re-add a comment claiming otherwise.
     *
     * What the flags buy: VRR (G-Sync / FreeSync) does not engage on a flip-model
     * swapchain without DXGI_PRESENT_ALLOW_TEARING, and a tear-allowed present
     * reaches scan-out without waiting for a vblank boundary.
     *
     * Both halves are required and must agree:
     *   - DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING at creation AND on every ResizeBuffers
     *     (ResizeBuffers REPLACES the flag set — it does not keep it like it keeps
     *     BufferCount/Format), and
     *   - DXGI_PRESENT_ALLOW_TEARING in the Present() flags argument, only ever
     *     with SyncInterval == 0.
     *
     * tearing_supported is the IDXGIFactory5 CheckFeatureSupport result, cached once
     * at init. swapchain_flags is the exact flag set the swapchain was created with —
     * ResizeBuffers MUST be given this same value. */
    int                        tearing_supported;  /* DXGI_FEATURE_PRESENT_ALLOW_TEARING */
    UINT                       swapchain_flags;    /* DXGI_SWAP_CHAIN_FLAG_* used at creation */

    /* Debug-layer InfoQueue, resolved ONCE at init and owned for the device's
     * lifetime (released in shutdown). NULL whenever the debug layer is inactive,
     * i.e. in every release run.
     *
     * d3d12_drain_info_queue() runs on every begin_frame. It used to
     * QueryInterface the device each call — a COM QI that, in a release build,
     * failed every single frame to re-discover a fact already known at startup.
     * Caching turns the per-frame cost into one pointer test. */
    ID3D12InfoQueue           *info_queue;

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
    /* Sized to the maximum; only the first frame_count slots are ever used. */
    vio_d3d12_frame            frames[VIO_D3D12_MAX_FRAME_COUNT];

    /* In-flight frame slots actually in use. Set once from vio_config::frame_count
     * (clamped to [MIN, MAX]) and read by every downstream consumer — swapchain
     * BufferCount, ResizeBuffers, the frames[] loops, and the per-frame heap
     * slices. Never read the macro for these. */
    UINT                       frame_count;

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
    /* The compute heap is a ring of VIO_D3D12_COMPUTE_HEAP_BLOCKS descriptor
     * blocks (each MAX SRVs + MAX UAVs); every dispatch takes the next block so
     * async dispatches recorded into one frame do not overwrite each other's
     * descriptors before the GPU consumes them. */
    UINT                       compute_heap_block;   /* next block index */
    int                        compute_async_pending;/* async dispatches recorded, not yet waited */

    /* Currently bound render target (NULL = backbuffer). current_rtv is
     * attachment 0; MRT targets fill current_rtvs[1..count-1] as well so
     * d3d12_clear can clear every attachment. */
    D3D12_CPU_DESCRIPTOR_HANDLE current_rtv;
    D3D12_CPU_DESCRIPTOR_HANDLE current_rtvs[VIO_MAX_COLOR_ATTACHMENTS];
    int                         current_rtv_count;
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
    /* Sampler combo index bound at register s0..s7 (parallel to pending_srvs
     * for t0..t7). Written by d3d12_bind_texture / vio_d3d12_bind_srv_slot. */
    int                          pending_samplers[VIO_D3D12_SAMPLER_TABLE_SIZE];

    /* Sampler heaps (GAP-PLAN Phase 1). sampler_combo_heap is CPU-only and
     * holds every filter x wrap x anisotropy combination once; sampler_heap is
     * the shader-visible per-frame ring the flush copies 8-descriptor blocks
     * into. sampler_set_cache remembers the blocks built THIS frame so a
     * sampler set that repeats (the common case — a whole frame usually uses
     * one or two) re-points root param 4 instead of consuming ring space. */
    ID3D12DescriptorHeap      *sampler_combo_heap;
    ID3D12DescriptorHeap      *sampler_heap;
    UINT                       sampler_descriptor_size;
    UINT                       sampler_frame_base;
    UINT                       sampler_frame_offset;
    UINT                       sampler_frame_capacity;
    struct {
        int                         combos[VIO_D3D12_SAMPLER_TABLE_SIZE];
        D3D12_GPU_DESCRIPTOR_HANDLE gpu;
    }                          sampler_set_cache[VIO_D3D12_SAMPLER_SET_CACHE];
    int                        sampler_set_count;
    D3D12_GPU_DESCRIPTOR_HANDLE sampler_table_gpu;      /* block bound at root param 4 */
    int                         sampler_table_bound;    /* 1 => sampler_table_gpu is current */

    /* Upload queue (GAP-PLAN 4.1): allocator ring + shared list, the fence
     * value each allocator's last submission signalled, and the staging
     * buffers waiting for their fence before they can be released. */
    ID3D12CommandAllocator    *upload_allocs[VIO_D3D12_UPLOAD_ALLOCATORS];
    UINT64                     upload_alloc_fence[VIO_D3D12_UPLOAD_ALLOCATORS];
    int                        upload_alloc_idx;
    ID3D12GraphicsCommandList *upload_list;
    UINT64                     upload_last_fence;
    struct { ID3D12Resource *res; UINT64 fence; } *upload_retire;
    int                        upload_retire_count;
    int                        upload_retire_cap;

    /* Pre-built block of VIO_D3D12_SRV_TABLE_SIZE null SRVs in the staging heap
     * (GAP-PLAN 4.3): flush_srv_table copies it with one CopyDescriptorsSimple
     * instead of issuing 16 CreateShaderResourceView(NULL) calls per rebuild. */
    D3D12_CPU_DESCRIPTOR_HANDLE null_srv_block;
    int                         null_srv_block_valid;

    /* Per-frame linear SRV descriptor allocator (contiguous blocks for descriptor tables).
     *
     * The descriptor heap is split each begin_frame: indices [0, srv_heap.count)
     * are static SRVs (one per texture/render-target/cubemap, monotonically
     * appended via d3d12_alloc_srv_descriptor); the remainder is partitioned
     * into vio_d3d12.frame_count equal regions, one per in-flight frame slot.
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
     * The heap is split into vio_d3d12.frame_count equal slices (like the SRV
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
    int   clear_pending;   /* vio_clear() before vio_begin(): applied (colour + depth) by begin_frame */
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

/* Index of the pre-built sampler for (vio_filter, vio_wrap, anisotropy 1..16)
 * in the combo heap; 0 is LINEAR / REPEAT / 1x. */
int  vio_d3d12_sampler_combo(int filter, int wrap, int anisotropy);

/* Stage an SRV (by staging-heap CPU handle) + its sampler combo at texture
 * register `slot` for the next draw. The one entry point for every caller that
 * used to poke pending_srvs[] directly (cubemaps, the 2D batch). */
void vio_d3d12_bind_srv_slot(D3D12_CPU_DESCRIPTOR_HANDLE srv_cpu, int slot, int sampler_index);

/* SetDescriptorHeaps with BOTH graphics heaps (CBV/SRV/UAV + sampler). Every
 * place that (re)binds the graphics root signature must use this — a
 * SetDescriptorHeaps that omits the sampler heap makes the sampler table root
 * argument invalid at the next draw. */
void vio_d3d12_bind_graphics_heaps(ID3D12GraphicsCommandList *list);

/* Waits for GPU to finish all pending work */
void vio_d3d12_wait_for_gpu(void);

/* Record a render-target bind that vio_bind_render_target deferred because the
 * command list was closed (called before vio_begin). No-op unless one is
 * pending and the frame is open. Called from vio_begin() after begin_frame. */
void vio_d3d12_apply_pending_render_target(void);

/* Capture the composited backbuffer at its true (current swapchain) resolution
 * as top-down RGBA8. Works both mid-frame (in_frame==1: flushes the open frame
 * command list, copies the buffer being drawn THIS frame, then re-opens the
 * frame so vio_end can complete normally) and after Present (in_frame==0: reads
 * the last presented buffer). On success returns a malloc'd buffer the caller
 * must free() and writes *out_w/*out_h/*out_size; returns NULL on failure. */
unsigned char *vio_d3d12_capture_frame(int *out_w, int *out_h, size_t *out_size);

#endif /* HAVE_D3D12 */
#endif /* VIO_D3D12_H */

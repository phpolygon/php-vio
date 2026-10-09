/*
 * php-vio - Vulkan Backend
 */

#ifndef VIO_VULKAN_H
#define VIO_VULKAN_H

#include "../../../include/vio_backend.h"

#ifdef HAVE_VULKAN

#include <vulkan/vulkan.h>

#define VIO_VK_MAX_FRAMES_IN_FLIGHT 2

/* Backend texture wrapper. Stored as the opaque backend_texture handle on
 * vio_texture_object / vio_font_object->atlas_backend_texture. The image is
 * VMA DEVICE_LOCAL (R8G8B8A8_UNORM), sampled in the 2D sprites pipeline via a
 * combined image sampler descriptor (set 0, binding 0). */
typedef struct _vio_vulkan_texture {
    VkImage       image;
    void         *allocation;   /* VmaAllocation (opaque) */
    VkImageView   view;
    VkSampler     sampler;
    int           width;
    int           height;
    int           depth;        /* > 0 for 3D / volume textures */
    /* Intrusive doubly-linked list of all live textures, anchored on
     * vio_vk.live_textures. vulkan_shutdown sweeps any survivors before
     * vkDestroyDevice so a PHP texture/font object outliving vio_destroy()
     * does not leave a VkImage alive at device destruction (a validation
     * error). vulkan_destroy_texture unlinks itself. */
    struct _vio_vulkan_texture *next;
    struct _vio_vulkan_texture *prev;
    /* 3D pipeline (GAP-PHASE5 Block 10) */
    VkSampler     sampler_cmp;  /* comparison sampler (sampler2DShadow), created on first use, owned */
    int           view_type;    /* VkImageViewType of view; 0 = 2D */
    int           layout;       /* VkImageLayout for descriptors; 0 = SHADER_READ_ONLY_OPTIMAL */
    int           is_depth;     /* depth-format view (depth render target) */
    int           filter, wrap;
    int           mip_levels;   /* > 1 => mip chain (sampler maxLod follows it) */
    int           layers;       /* > 1 => 2D array view (Block 10c); 0/1 = single image */
    int           vio_format;   /* VIO_FORMAT_* of the extended path (BC data cannot be blitted) */
} vio_vulkan_texture;

/* Per-frame synchronization and command buffer resources.
 *
 * NOTE: render_finished is intentionally NOT here. A binary semaphore signalled
 * by the submit and waited by vkQueuePresentKHR must be tied to the SWAPCHAIN
 * IMAGE, not the frame-in-flight: with FIFO present and (typically) 3 swapchain
 * images vs 2 frames in flight, a per-frame render_finished gets reused for a
 * present while a prior present that still references it (on a different,
 * not-yet-reacquired image) is pending — illegal binary-semaphore reuse
 * (VUID-vkQueueSubmit-pSignalSemaphores-00067). render_finished therefore lives
 * in vio_vk.render_finished_per_image[], indexed by current_image_index. */
typedef struct _vio_vk_frame {
    VkCommandPool   cmd_pool;
    VkCommandBuffer cmd_buf;
    VkSemaphore     image_available;
    /* Timeline value of the slot's last submission (VULKAN-MODERN-PLAN phase 2):
     * begin_frame waits it before reusing the command buffer, pools and ring
     * slices of the slot. 0 = never submitted. */
    uint64_t        value;
} vio_vk_frame;

/* Buffer wrapper for every vio buffer type on this backend: compute / graphics
 * storage buffers, mesh vertex and index buffers. HOST_VISIBLE | HOST_COHERENT. */
typedef struct _vio_vulkan_compute_buffer {
    VkBuffer       buffer;
    void          *allocation;   /* VmaAllocation (opaque) */
    VkDeviceSize   size;         /* bytes */
    int            stride;       /* element stride (informational) */
    /* Intrusive list (vio_vk.live_compute_buffers) so vulkan_shutdown can sweep
     * survivors before vkDestroyDevice. */
    struct _vio_vulkan_compute_buffer *next, *prev;
} vio_vulkan_compute_buffer;

/* A pass on dynamic rendering (VULKAN-MODERN-PLAN phase 4): one attachment -
 * the view drawn into, its image and subresource range, and the layout the
 * image rests in between passes (SHADER_READ_ONLY for a sampled target,
 * COLOR_ATTACHMENT for an MSAA colour image, READ_ONLY for a depth-only
 * target, PRESENT_SRC for the swapchain). A multisampled colour attachment
 * names its single-sample resolve target too. */
typedef struct _vio_vk_pass_att {
    VkImageView              view;
    VkImage                  image;
    VkImageSubresourceRange  range;
    VkImageLayout            rest;
    VkImageView              resolve_view;    /* VK_NULL_HANDLE: no resolve */
    VkImage                  resolve_image;
    VkImageSubresourceRange  resolve_range;
    VkImageLayout            resolve_rest;
} vio_vk_pass_att;

typedef struct _vio_vk_pass {
    int              count;              /* colour attachments, 0..VIO_MAX_COLOR_ATTACHMENTS */
    vio_vk_pass_att  color[VIO_MAX_COLOR_ATTACHMENTS];
    VkFormat         color_format[VIO_MAX_COLOR_ATTACHMENTS];
    int              has_depth;
    vio_vk_pass_att  depth;
    int              samples;
    uint32_t         width, height;
    uint32_t         layers;             /* layered bind: every layer; 1 otherwise */
    uint32_t         view_mask;          /* multiview: views 0..n-1; 0 = off */
    int              clear;              /* CLEAR instead of LOAD (the swapchain's first pass of a frame) */
    int              internal;           /* vio's own pass (depth mips / resolve): no shading-rate image */
    int              secondary;          /* the pass executes secondary command buffers (vio_draw_bundle) */
    int              swapchain;          /* colour 0 is the acquired swapchain image */
} vio_vk_pass;

/* Render target resources (rt->vulkan_rt, vio_vulkan_rt.c, GAP-PHASE5 Block 10b). */
typedef struct _vio_vk_rt {
    int            count;          /* colour attachments (0 = depth-only) */
    int            cube;           /* 6-layer colour image, a view per (face, level) */
    int            layers;         /* bindable layers: 6 (cube), N (array, 'layers' => N) or 1 */
    int            levels;         /* mip levels of the colour image */
    int            samples;        /* effective sample count (1 = off) */
    VkFormat       color_format[VIO_MAX_COLOR_ATTACHMENTS];
    VkImage        color_image[VIO_MAX_COLOR_ATTACHMENTS]; /* single-sample; the resolve target when MSAA */
    void          *color_alloc[VIO_MAX_COLOR_ATTACHMENTS];
    VkImageView    color_view[VIO_MAX_COLOR_ATTACHMENTS];
    VkImage        msaa_image[VIO_MAX_COLOR_ATTACHMENTS];
    void          *msaa_alloc[VIO_MAX_COLOR_ATTACHMENTS];
    VkImageView    msaa_view[VIO_MAX_COLOR_ATTACHMENTS];
    VkImageView   *msaa_face_view; /* cube / array MSAA (A24): MS colour view per layer */
    VkImageView    msaa_all_view;  /* ... and over every layer (VIO_RT_ALL_LAYERS) */
    /* depth_only MSAA (A24): the multisampled depth drawn into, resolved into
     * depth_image (max / min of the samples) when the binding leaves it. */
    VkImage        msaa_depth_image;
    void          *msaa_depth_alloc;
    VkImageView    msaa_depth_view;
    VkDescriptorPool dres_pool;
    VkDescriptorSet  dres_set;
    VkImage        depth_image;
    void          *depth_alloc;
    VkImageView    depth_view;
    VkImageView   *face_view;      /* cube / array: colour view per [layer * levels + level] */
    VkImageView   *depth_face_view;/* cube / array: depth view per layer (level 0) */
    VkImageView    cube_view;      /* colour CUBE view, or the colour 2D_ARRAY view of an array */
    VkImageView    all_color_view; /* layered bind: colour 2D_ARRAY over every layer, level 0 */
    VkImageView    all_depth_view; /* layered bind: depth 2D_ARRAY over every layer */
    VkSampler      sampler;
    /* depth_only + 'mipmaps' (A26): the depth chain, a sampling view over every
     * level and, built on the first vio_generate_mipmaps, per level the
     * attachment view, the source view and a descriptor set. */
    int            depth_levels;
    VkImageView    depth_sample_view;
    VkImageView   *dmip_att;
    VkImageView   *dmip_src;
    VkDescriptorPool dmip_pool;
    VkDescriptorSet *dmip_set;
    struct _vio_vulkan_texture *wrap[VIO_MAX_COLOR_ATTACHMENTS];   /* sampling wrappers (vio_render_target_texture) */
    struct _vio_vulkan_texture *cube_wrap; /* vio_render_target_cubemap */
    /* VIO_RT_DEPTH on a single-sample 2D colour target: the depth is SAMPLED and
     * rests in DEPTH_STENCIL_READ_ONLY between passes (like a depth_only one),
     * sampled NEAREST / white border through its own wrapper. */
    int            depth_sampled;
    int            storage;        /* 'storage' => true: STORAGE colour images resting in GENERAL */
    VkSampler      depth_sampler;
    struct _vio_vulkan_texture *depth_wrap;
} vio_vk_rt;

/* Global Vulkan state */
typedef struct _vio_vulkan_state {
    /* Instance & device */
    VkInstance               instance;
    VkPhysicalDevice         physical_device;
    VkDevice                 device;
    VkQueue                  graphics_queue;
    VkQueue                  present_queue;
    uint32_t                 graphics_family;
    uint32_t                 present_family;

    /* Surface & swapchain */
    VkSurfaceKHR             surface;
    VkSwapchainKHR           swapchain;
    VkFormat                 swapchain_format;
    VkExtent2D               swapchain_extent;
    VkImage                 *swapchain_images;
    VkImageView             *swapchain_image_views;
    uint32_t                 swapchain_image_count;

    /* render_finished semaphores, one PER SWAPCHAIN IMAGE (array sized
     * swapchain_image_count). Signalled by the end-of-frame submit and waited by
     * vkQueuePresentKHR, both indexed by current_image_index. Per-image (not
     * per-frame-in-flight) so a given image's present always uses the same
     * semaphore and that present must complete — the image re-acquired — before
     * the semaphore is reused, which avoids VUID-vkQueueSubmit-pSignalSemaphores-
     * 00067. Created in create_swapchain(), destroyed in cleanup_swapchain()
     * (both recreate and shutdown wait the device idle first). */
    VkSemaphore             *render_finished_per_image;


    /* Depth buffer */
    VkImage                  depth_image;
    VkDeviceMemory           depth_memory;
    VkImageView              depth_view;

    /* Per-frame resources */
    vio_vk_frame             frames[VIO_VK_MAX_FRAMES_IN_FLIGHT];
    uint32_t                 current_frame;
    uint32_t                 current_image_index;

    /* VMA allocator */
    void                    *vma_allocator; /* VmaAllocator, opaque from C */

    /* Head of the intrusive list of live backend textures (see
     * vio_vulkan_texture). Swept in vulkan_shutdown before vkDestroyDevice. */
    struct _vio_vulkan_texture *live_textures;

    /* Heads of the intrusive lists of live compute storage buffers and compute
     * pipelines (see vio_vulkan.c). Swept in vulkan_shutdown BEFORE
     * vkDestroyDevice for the same reason as live_textures: the owning PHP
     * VioBuffer / VioComputePipeline objects' Zend free handlers run at REQUEST
     * shutdown, AFTER vio_destroy() has already destroyed the device — so without
     * a pre-destroy sweep their VkBuffer/VkPipeline/etc. would still be alive at
     * vkDestroyDevice (a VUID-vkDestroyDevice-device-05137 validation error and a
     * real GPU leak). The sweep frees each survivor's GPU objects while the device
     * is still alive; the later free handler then no-ops (handles already NULL).
     * void* to avoid leaking the (file-local) struct types into this header. */
    void                       *live_compute_buffers;
    void                       *live_compute_pipelines;

    /* Live offscreen render targets (vio_render_target_object*), tracked the
     * same way as live_textures and for the same reason: vio_destroy() runs
     * vulkan_shutdown (+ vkDestroyDevice) while PHP VioRenderTarget objects are
     * still alive (their Zend free handlers run later, at request shutdown).
     * Without a sweep their VkImage/View/RenderPass/Framebuffer/Sampler would
     * still be alive at vkDestroyDevice — a VUID-vkDestroyDevice-device-05137
     * leak. The sweep frees each survivor's GPU objects (device still alive)
     * before vkDestroyDevice; the later free handler then no-ops (device gone).
     * Stored as a malloc'd growable array of opaque RT pointers (the public RT
     * struct has no room for intrusive links). */
    void                   **live_render_targets;
    uint32_t                 live_rt_count;
    uint32_t                 live_rt_capacity;

    /* State */
    int                      initialized;
    int                      swapchain_needs_recreate;
    /* vio_create(['vsync' => …]) — picks the present mode (GAP-PLAN 2.7):
     * 0 => IMMEDIATE (uncapped; MAILBOX, then FIFO as fallbacks),
     * 1 => FIFO (true vsync). */
    int                      vsync;
    /* samplerAnisotropy device feature: enabled at device creation when the
     * physical device offers it; max_anisotropy is the device limit. */
    int                      anisotropy_supported;
    float                    max_anisotropy;
    /* Persistent pool for one-shot uploads / compute dispatches (GAP-PLAN 4.4);
     * lazily created, destroyed in vulkan_shutdown. */
    VkCommandPool            transient_pool;
    /* VULKAN-MODERN-PLAN phase 2: every queue submission signals the next value
     * of this timeline semaphore (vio_vk_submit); waiting for work means waiting
     * for its value (vio_vk_wait_value). Binary semaphores remain only for
     * acquire / present. */
    VkSemaphore              timeline;
    uint64_t                 timeline_value;   /* last value a submission signals */
    /* GPU timestamps (GAP-PHASE5 Block 3): two queries per frame in flight,
     * reset + written in the frame's command buffer, read after its fence. */
    /* On-disk pipeline cache (GAP-PHASE5 Block 4): loaded at init from the
     * shader-cache directory, written back at shutdown. VK_NULL_HANDLE when the
     * cache is disabled (pipelines are then created without a cache). */
    VkPipelineCache          pipeline_cache;
    uint64_t                 pipeline_cache_key;
    VkQueryPool              ts_pool;
    float                    ts_period;     /* ns per tick (timestampPeriod) */
    int                      ts_pending[VIO_VK_MAX_FRAMES_IN_FLIGHT];
    double                   last_gpu_ms;
    /* Named marks (vio_gpu_timestamp): VIO_GPU_TS_PER_FRAME queries per slot. */
    vio_gpu_mark_names       ts_marks[VIO_VK_MAX_FRAMES_IN_FLIGHT];
    vio_gpu_mark_result      ts_result;
    /* Layout of the mesh being drawn (apply_mesh_layout): pipeline variants
     * read each vertex input at the mesh's offset (OPEN-ITEMS-PLAN A31). */
    vio_mesh_layout          mesh_layout;
    int                      ts_result_valid;
    int                      in_frame;          /* 1 while the command buffer is recording (begin_frame..end_frame) */
    /* Phase 4 — warm-render present-skip. Captured at vulkan_begin_frame: 1 when
     * the frame is OFFSCREEN-ONLY (vio_vk.pending_bound_rt was set BEFORE
     * begin_frame, i.e. the warm-render "bind then begin" order), 0 for a normal
     * presented frame. An offscreen-only frame does NOT vkAcquireNextImageKHR and
     * never begins the swapchain pass, so its end-of-frame submit signals/waits
     * NO swapchain semaphores and vulkan_present skips vkQueuePresentKHR. Skipping
     * present WITHOUT skipping the acquire would exhaust the swapchain (3 images)
     * after a few acquires-without-present and hang vkAcquireNextImageKHR(...,
     * UINT64_MAX); the two skips are therefore paired. Read by end_frame (chooses
     * the zero-semaphore submit) and present (chooses the no-present path); the
     * normal path (frame_is_offscreen==0) is byte-for-byte the pre-Phase-4 flow. */
    int                      frame_is_offscreen;
    /* B1 — set by vulkan_begin_frame when it successfully opens a NORMAL
     * (presentable) frame: a swapchain image was acquired, the command buffer
     * begun and the swapchain render pass started. Stays 0 when begin_frame
     * aborts (acquire returned OUT_OF_DATE/error and it recreated the swapchain
     * without opening a pass) and 0 for an offscreen-only frame. vulkan_present
     * presents ONLY when this is set, because vulkan_end_frame clears in_frame
     * before present runs, so present cannot key off in_frame. Cleared by
     * present (and by end_frame's early-out / offscreen path). */
    int                      frame_presentable;
    float                    clear_r, clear_g, clear_b, clear_a;

    /* The pass open on the frame command buffer (dynamic rendering,
     * VULKAN-MODERN-PLAN phase 4) and its attachment signature - pipeline
     * variants are keyed by the formats, samples, depth and view mask. */
    int                      in_pass;
    vio_vk_pass              cur_pass;
    uint32_t                 cur_view_mask;
    int                      cur_color_count;
    VkFormat                 cur_color_formats[VIO_MAX_COLOR_ATTACHMENTS];
    int                      cur_samples;
    int                      cur_has_depth;
    uint32_t                 cur_width, cur_height;
    uint32_t                 cur_layers;            /* layers of the open pass's framebuffer (VIO_RT_ALL_LAYERS bind > 1) */
    /* Viewports of the open pass (vio_viewports). 3D pipelines are built with
     * max_viewports viewports; vk3d_prepare sets every one before a draw. */
    uint32_t                 max_viewports;         /* 1, or min(16, maxViewports) with multiViewport */
    VkViewport               cur_vp[16];
    VkRect2D                 cur_sc[16];
    uint32_t                 cur_vp_count;
    int                      depth_has_stencil;    /* depth attachments carry 8 stencil bits */
    int                      multi_draw_indirect;  /* device feature enabled */
    int                      independent_blend;    /* device feature enabled */
    int                      fragment_stores;      /* fragmentStoresAndAtomics enabled (A15) */
    int                      fs_storage_active;    /* a fragment storage buffer is bound */
    int                      fs_storage_pending;   /* a frame wrote one: readbacks drain the queue first */
    int                      geometry_supported;   /* geometryShader enabled (vio_shader 'geometry') */
    int                      tessellation_supported; /* tessellationShader enabled */
    int                      vertex_layer_supported; /* VK_EXT_shader_viewport_index_layer enabled (gl_Layer in the VS) */
    /* Block 10c: textureCompressionBC, VK_KHR_fragment_shading_rate (pipeline rate). */
    int                      instance_api_11;      /* instance created with apiVersion 1.1 */
    /* VULKAN-MODERN-PLAN (A37): the instance API version (1.2 or 1.3), whether the
     * device runs the 1.3 core entry points, and the entry points of timeline
     * semaphores, synchronization2 and dynamic rendering (core or KHR names).
     * A device without the three features is never selected. */
    uint32_t                 instance_api;
    int                      core13;
    void                    *fn_begin_rendering, *fn_end_rendering;   /* vkCmdBeginRendering / vkCmdEndRendering */
    void                    *fn_barrier2, *fn_submit2;                /* vkCmdPipelineBarrier2 / vkQueueSubmit2 */
    void                    *fn_wait_semaphores, *fn_counter_value;   /* vkWaitSemaphores / vkGetSemaphoreCounterValue */
    int                      bc_supported;
    int                      astc_supported;   /* textureCompressionASTC_LDR enabled (A20) */
    int                      vrs_supported;
    int                      vrs_primitive;        /* primitiveFragmentShadingRate enabled */
    int                      vrs_primitive_multi_viewport;   /* primitiveFragmentShadingRateWithMultipleViewports */
    int                      tess_geometry_point_size;       /* shaderTessellationAndGeometryPointSize enabled */
    /* VkPhysicalDeviceSubgroupProperties: basic / vote / ballot / arithmetic /
     * shuffle in the compute and fragment stages (VIO_FEATURE_SUBGROUP). */
    int                      subgroup_supported;
    int                      subgroup_quad_supported;   /* QUAD operations in the fragment stage */
    int                      barycentrics_supported;    /* VK_KHR_fragment_shader_barycentric enabled */
    int                      atomic64_supported;        /* shaderInt64 + shaderBufferInt64Atomics enabled */
    int                      long_vector_supported;     /* VK_EXT_shader_long_vector: longVector enabled (SM69-PLAN) */
    int                      ser_supported;             /* VK_EXT_ray_tracing_invocation_reorder enabled (SM69-PLAN) */
    int                      omm_supported;             /* VK_EXT_opacity_micromap enabled (SM69-PLAN) */
    void                    *fn_create_micromap, *fn_destroy_micromap, *fn_cmd_build_micromaps, *fn_get_micromap_sizes;
    int                      float16_supported;         /* VK_KHR_shader_float16_int8 shaderFloat16 enabled */
    int                      draw_parameters_supported; /* shaderDrawParameters + drawIndirectFirstInstance enabled */
    int                      compute_derivatives_supported; /* VK_NV / KHR_compute_shader_derivatives (quads) enabled */
    int                      multiview_supported;       /* VkPhysicalDeviceMultiviewFeatures.multiview enabled (core 1.1) */
    int                      multiview_geometry;        /* ... multiviewGeometryShader (with geometryShader) */
    int                      multiview_tessellation;    /* ... multiviewTessellationShader (with tessellationShader) */
    /* Inline ray tracing (VIO_FEATURE_RAY_QUERY): accelerationStructure + rayQuery +
     * bufferDeviceAddress enabled, the KHR entry points, and the structure bound
     * with vio_bind_acceleration_structure (a VkAccelerationStructureKHR handle). */
    int                      ray_query_supported;
    void                    *fn_get_as_build_sizes, *fn_create_as, *fn_destroy_as, *fn_cmd_build_as,
                            *fn_get_as_address, *fn_get_buffer_address;
    uint64_t                 bound_accel;
    int                      bound_accel_binding;
    /* Ray tracing pipeline (VIO_FEATURE_RAYTRACING): VK_KHR_ray_tracing_pipeline
     * enabled on top of the ray query set, its entry points and the shader
     * group handle layout of the device. */
    int                      rt_pipeline_supported;
    void                    *fn_create_rt_pipelines, *fn_get_rt_group_handles, *fn_cmd_trace_rays;
    uint32_t                 rt_handle_size, rt_handle_alignment, rt_base_alignment, rt_max_recursion;
    /* Bindless table (vio_texture_index, BINDLESS-PLAN.md): descriptor indexing
     * enabled; one global Set 1 (1024 sampled images, partially bound, update
     * after bind, + an immutable linear / repeat sampler), created lazily. */
    int                      bindless_supported;
    VkDescriptorSetLayout    bindless_layout;
    VkDescriptorPool         bindless_pool;
    VkDescriptorSet          bindless_set;
    VkSampler                bindless_sampler;
    VkSampler                bindless_sampler_variants[3];   /* nearest, clamp, nearest + clamp (bindings 2..4) */
    int                      vrs_rates;            /* bit (1 << VIO_SHADING_RATE_*) per supported size */
    int                      shading_rate;         /* sticky VIO_SHADING_RATE_* for 3D draws */
    void                    *vrs_cmd_set;          /* vkCmdSetFragmentShadingRateKHR via vkGetDeviceProcAddr */
    /* Shading-rate image (A18, VIO_FEATURE_SHADING_RATE_IMAGE): attachmentFragmentShadingRate
     * with non-trivial combiners; an R8_UINT image of one rate per vrs_tile x vrs_tile
     * tile, attached to every application pass (dynamic rendering) while active. */
    int                      vrs_attachment;
    uint32_t                 vrs_tile;
    VkImage                  vrs_image;
    void                    *vrs_image_alloc;
    VkImageView              vrs_image_view;
    int                      vrs_image_w, vrs_image_h;
    int                      vrs_image_active;
    int                      cur_pass_vrs;         /* the open pass carries the image */    /* VK_EXT_mesh_shader (VIO_FEATURE_MESH_SHADER): meshShader + taskShader
     * enabled, draw entry points via vkGetDeviceProcAddr. */
    int                      mesh_supported;
    void                    *mesh_cmd_draw;          /* vkCmdDrawMeshTasksEXT */
    void                    *mesh_cmd_draw_indirect; /* vkCmdDrawMeshTasksIndirectEXT */
    /* VK_KHR_cooperative_matrix (VIO_FEATURE_COOPERATIVE_MATRIX): the
     * subgroup-scope shapes whose component types the device can run (float16
     * needs shaderFloat16 + 16-bit storage buffers), read once at device creation. */
    int                      coopmat_shape_count;
    /* VK_EXT_subgroup_size_control computeFullSubgroups (enabled with cooperative
     * matrices, whose kernels must run on full subgroups) and maxSubgroupSize. */
    int                      full_subgroups;
    uint32_t                 max_subgroup_size;
    vio_coopmat_shape        coopmat_shapes[VIO_COOPMAT_MAX_SHAPES];
    /* VIO_UPSCALE_VK_* enabled at device creation because a native upscaler's
     * runtime was found (vio_upscale_vk_device_needs, TEMPORAL-S3). */
    unsigned                 upscale_features;
    /* HDR10 output (GAP-PHASE5 Block 10d): vio_create(['hdr_output' => 1|2]). */
    int                      hdr_request;          /* 0 off, 1 when the surface offers HDR10 ST 2084, 2 forced 10-bit */
    int                      hdr_output;           /* 1 => 10-bit swapchain, the 2D batch PQ-encodes */
    int                      hdr10_capable;        /* the surface lists a 10-bit UNORM format */
    int                      colorspace_ext;       /* VK_EXT_swapchain_colorspace enabled on the instance */
    float                    hdr_paper_white;      /* nits for PQ encoding (default 200) */
    /* Headless frame capture (Block 10): every presented frame is copied into
     * capture_buf in its own command buffer; vio_read_pixels maps it. */
    int                      headless;
    int                      swapchain_transfer_src;
    VkBuffer                 capture_buf;
    void                    *capture_alloc;
    VkDeviceSize             capture_size;
    uint32_t                 capture_w, capture_h;
    int                      capture_valid;
    int                      acquire_consumed;   /* a mid-frame readback submit already waited image_available */
    /* Timeline value of the submission that carries the newest capture copy
     * (A36): vio_read_pixels waits it instead of vkDeviceWaitIdle. */
    uint64_t                 capture_value;

    /* Offscreen render-target binding (mirrors vio_d3d12). current_bound_rt is
     * the vio_render_target_object* whose render pass is active, or NULL =
     * swapchain. pending_bound_rt holds a target requested before vio_begin;
     * vio_begin applies it once the command buffer / swapchain pass is open. */
    void                    *current_bound_rt;
    void                    *pending_bound_rt;

    /* Debug */
    VkDebugUtilsMessengerEXT debug_messenger;
    int                      debug_enabled;

    /* Window reference (for surface creation and resize) */
    void                    *platform_window;
    int                      framebuffer_width;
    int                      framebuffer_height;
} vio_vulkan_state;

extern vio_vulkan_state vio_vk;

/* Registration */
void vio_backend_vulkan_register(void);

/* Called after GLFW window creation to set up Vulkan */
int vio_vulkan_setup_context(void *platform_window, vio_config *cfg);

/* Swapchain recreation (on resize) */
int vio_vulkan_recreate_swapchain(void);

/* VMA wrapper functions (implemented in C++ translation unit) */
int  vio_vma_create(VkInstance instance, VkPhysicalDevice phys, VkDevice device, void **out_allocator);
void vio_vma_destroy(void *allocator);

int  vio_vma_create_buffer(void *allocator, VkDeviceSize size, VkBufferUsageFlags usage,
                            VkMemoryPropertyFlags mem_props,
                            VkBuffer *out_buffer, void **out_allocation);
void vio_vma_destroy_buffer(void *allocator, VkBuffer buffer, void *allocation);
void *vio_vma_map(void *allocator, void *allocation);
void  vio_vma_unmap(void *allocator, void *allocation);

int  vio_vma_create_image(void *allocator, const VkImageCreateInfo *info,
                           VkMemoryPropertyFlags mem_props,
                           VkImage *out_image, void **out_allocation);
void vio_vma_destroy_image(void *allocator, VkImage image, void *allocation);

/* Reset the 2D descriptor-set pool for the given frame-in-flight. Implemented
 * in vio_2d_vulkan.c; called from vulkan_begin_frame AFTER the in_flight fence
 * has been waited (the sync point that makes reset of an in-flight pool safe).
 * No-op when the 2D Vulkan state has not been initialised yet. */
void vio_2d_vulkan_reset_frame_descriptors(uint32_t frame_index);

/* ── Offscreen render-target operations (Phase 3) ─────────────────────
 *
 * These are invoked from php_vio.c (the vio_render_target / vio_bind_render_target
 * / vio_unbind_render_target / vio_render_target dispatchers) and operate on a
 * vio_render_target_object* passed as void* to avoid a header dependency on
 * vio_render_target.h here. The implementations live in vio_vulkan_rt.c. */

/* Register / unregister an RT in vio_vk.live_render_targets. Registration
 * happens at the end of a successful vulkan_create_render_target; unregistration
 * at the start of vulkan_destroy_render_target (idempotent — safe if absent). */
void vulkan_rt_track(void *rt);
void vulkan_rt_untrack(void *rt);

/* Create the per-target Vulkan resources (color image+view+alloc, optional
 * depth, a render-pass-compatible VkRenderPass, framebuffer, sampler) and store
 * them on the vio_render_target_object. Returns 0 on success, non-zero on
 * failure (caller discards the RT object). */
int  vulkan_create_render_target(void *rt, int width, int height, int hdr, int depth_only);

/* Destroy a render target's Vulkan resources. vkDeviceWaitIdle first (an
 * offscreen frame may still be in flight), then destroy fb/rp/views/images/
 * sampler and null the fields + any vio_vk.current/pending_bound_rt that point
 * at this RT. Safe to call with a NULL device (no-op). */
void vulkan_destroy_render_target(void *rt);

/* Record the mid-frame switch from the swapchain pass to the offscreen RT pass:
 * vkCmdEndRenderPass (swapchain) -> vkCmdBeginRenderPass (offscreen, CLEAR) ->
 * set viewport/scissor to the RT extent -> vio_vk.current_bound_rt = rt. Caller
 * MUST ensure vio_vk.in_frame (a swapchain pass is open on the frame cmd buffer). */
void vulkan_record_bind_render_target(void *rt);

/* Phase 4 — begin the offscreen RT pass DIRECTLY (no preceding vkCmdEndRenderPass)
 * on an OFFSCREEN-ONLY frame, where vulkan_begin_frame opened the command buffer
 * but never began the swapchain pass (frame_is_offscreen==1). vkCmdBeginRenderPass
 * (offscreen, CLEAR) -> RT-extent viewport/scissor -> vio_vk.current_bound_rt = rt.
 * Caller MUST ensure vio_vk.in_frame and that NO render pass is currently open.
 * (vulkan_record_bind_render_target is the mid-frame sibling that first ends the
 * open swapchain/offscreen pass; both share the same begin logic internally.) */
void vulkan_begin_offscreen_render_pass(void *rt);

/* Record the mid-frame switch back to the swapchain: vkCmdEndRenderPass
 * (offscreen, which transitions its color to SHADER_READ_ONLY via finalLayout)
 * -> begin the loadOp=LOAD swapchain resume pass (created lazily) -> restore the
 * swapchain viewport/scissor -> vio_vk.current_bound_rt = NULL. Caller MUST
 * ensure vio_vk.in_frame and that current_bound_rt is set. */
void vulkan_record_unbind_render_target(void);

/* Phase 5 — read back swapchain content into a CPU buffer as TOP-DOWN RGBA8.
 *
 * CONTRACT / INTENDED USE: stable-frame / screenshot capture (golden-image
 * tests, splash screenshot). This RE-ACQUIRES a swapchain image rather than
 * reading the just-presented one directly (see below). The re-acquired buffer
 * holds the most-recent render of that image; under FIFO/vsync with a STATIC
 * scene that equals the just-presented frame, but for an ANIMATING scene with
 * >=3 swapchain images it may be 1-2 frames STALE. Call it after rendering a
 * steady frame; it is not a reliable per-frame live capture.
 *
 * Implementation: vkDeviceWaitIdle (drain the device queues); RE-ACQUIRE a
 * swapchain image (vkAcquireNextImageKHR) so the readback submit can wait on the
 * acquire semaphore — this resolves the WRITE_AFTER_PRESENT sync hazard, since
 * vkDeviceWaitIdle does NOT synchronize with the presentation engine and
 * transitioning a just-presented image directly would race the present's read.
 * The acquired image holds the most recent render of that buffer (the swapchain
 * never clears presented images), so under vsync/FIFO with a stable scene this
 * is the just-presented content. Then: a transient one-time-submit command
 * buffer transitions the image PRESENT_SRC_KHR -> TRANSFER_SRC_OPTIMAL,
 * vkCmdCopyImageToBuffer into a HOST_VISIBLE readback VkBuffer sized to the
 * tightly-packed copy footprint, transitions back to PRESENT_SRC_KHR; the submit
 * waits the acquire semaphore + signals a done semaphore; map + memcpy honoring
 * the buffer row stride and SWIZZLE the swapchain's B8G8R8A8 byte order to
 * R8G8B8A8 so the output matches the D3D12 R8G8B8A8_UNORM readback byte-for-byte
 * (apples-to-apples golden compare); finally RE-PRESENT the acquired image
 * (waiting the done semaphore) to keep the acquire/present balance — an acquire
 * without a matching present eventually hangs future acquires (the Phase 4
 * lesson) — and vkQueueWaitIdle the present queue before freeing the semaphores.
 *
 * `out_rgba` must point to at least width*height*4 bytes; width/height are the
 * caller's expected dimensions (the actual swapchain extent is used for the copy
 * and the lesser of the two is written per row/column, so a size mismatch never
 * overruns). Returns 0 on success, non-zero on failure (out_rgba untouched on
 * failure). Safe to call only when vio_vk.initialized and a frame has rendered. */
int vulkan_read_pixels(int width, int height, void *out_rgba);

/* ── Shared helpers (vio_vulkan.c) ── */
/* Submit `cmd` (may be NULL) on the graphics queue: waits `wait_bin` at
 * `wait_stage` when set, signals `signal_bin` when set, and always the next
 * timeline value, which it returns (0 when the submission failed). */
uint64_t vio_vk_submit(VkCommandBuffer cmd, VkSemaphore wait_bin, VkPipelineStageFlags2 wait_stage, VkSemaphore signal_bin);
void     vio_vk_wait_value(uint64_t value);   /* host wait until the timeline reaches value */
VkFormat vio_vk_depth_format(void);
int      vio_vk_begin_transient(VkCommandBuffer *out_cmd);
int      vio_vk_submit_transient(VkCommandBuffer cmd);
/* Sampling wrapper for a render target (colour, or depth for depth_only targets),
 * cached on the target and owned by it. */
void    *vulkan_rt_sampling_texture(void *rt, int attachment);

/* ── Deferred destruction + barriers (vio_vulkan_3d.c) ── */
#define VIO_VK_GRAVE_IMAGE           1
#define VIO_VK_GRAVE_VIEW            2
#define VIO_VK_GRAVE_SAMPLER         3
#define VIO_VK_GRAVE_BUFFER          4
#define VIO_VK_GRAVE_PIPELINE        5
#define VIO_VK_GRAVE_PIPELINE_LAYOUT 6
#define VIO_VK_GRAVE_SET_LAYOUT      7
#define VIO_VK_GRAVE_SHADER_MODULE   8
#define VIO_VK_GRAVE_FRAMEBUFFER     9
#define VIO_VK_GRAVE_RENDER_PASS     10
#define VIO_VK_GRAVE_DESCRIPTOR_POOL 11
#define VIO_VK_GRAVE_COMMAND_POOL    12
void vio_vk_defer_destroy(int kind, uint64_t handle, void *allocation);
/* synchronization2 (VULKAN-MODERN-PLAN phase 3): the vkCmdPipelineBarrier
 * signature, recorded as vkCmdPipelineBarrier2 (the 1.0 stage / access bits
 * have the same values in the *2 flags). Layout transitions go through
 * vio_vk_image_barrier*, whose scopes follow the layouts. */
void vio_vk_pipeline_barrier(VkCommandBuffer cmd, VkPipelineStageFlags src, VkPipelineStageFlags dst, VkDependencyFlags dep,
                             uint32_t nmem, const VkMemoryBarrier *mem, uint32_t nbuf, const VkBufferMemoryBarrier *buf,
                             uint32_t nimg, const VkImageMemoryBarrier *img);
void vio_vk_image_barrier(VkCommandBuffer cmd, VkImage image, VkImageAspectFlags aspect, uint32_t layers,
                          VkImageLayout from, VkImageLayout to);
void vio_vk_image_barrier_range(VkCommandBuffer cmd, VkImage image, VkImageAspectFlags aspect,
                                uint32_t base_level, uint32_t levels, uint32_t base_layer, uint32_t layers,
                                VkImageLayout from, VkImageLayout to);
void vio_vk_release_texture(vio_vulkan_texture *tex);   /* GPU objects (deferred mid-frame) + the wrapper */

/* ── Render targets, cubemaps, mips (GAP-PHASE5 Block 10b, vio_vulkan_rt.c / vio_vulkan_cube.c) ── */
/* Dynamic rendering (VULKAN-MODERN-PLAN phase 4): begin = attachments from
 * their resting layouts into the attachment layouts + vkCmdBeginRendering +
 * viewport / scissor over the pass + the cur_* signature; end =
 * vkCmdEndRendering + back to the resting layouts. vio_vk_pass_end is a no-op
 * without an open pass. */
void  vio_vk_pass_begin(VkCommandBuffer cmd, const vio_vk_pass *pass);
void  vio_vk_pass_end(VkCommandBuffer cmd);
/* The swapchain pass of the acquired image (clear = the frame's first). */
void  vio_vk_swapchain_pass(vio_vk_pass *pass, int clear);
void  vio_vk_resume_swapchain_pass(VkCommandBuffer cmd);
/* Attachment formats of a pipeline for the pass open now (VkPipelineRenderingCreateInfo). */
void  vio_vk_pass_rendering_info(VkPipelineRenderingCreateInfo *info);
/* Reopen the pass that was open before (bound render target layer / level, or the
 * swapchain) with LOAD, after vkCmdEndRenderPass for a compute dispatch or a flush. */
void  vio_vk_resume_pass(VkCommandBuffer cmd);
/* The bindless Set 1 layout (created with its pool / set on first use);
 * VK_NULL_HANDLE without descriptor indexing. */
VkDescriptorSetLayout vio_vk_bindless_layout(void);
/* Multiview (VIO_FEATURE_MULTIVIEW): make the open render pass of the layered
 * target bound with VIO_RT_ALL_LAYERS a multiview pass for `views` views (2..4),
 * or switch it back to the plain layered pass for `views` = 0. Returns -1 when
 * a multiview draw has no fitting target. */
int   vio_vk_rt_ensure_views(int views);
/* Submit the open frame's commands so far, wait, and reopen it (vio_compute_wait). */
void  vio_vk_flush_frame(void);
void  vio_vk_fs_storage_host_barrier(VkCommandBuffer cmd);
int   vio_vk_bind_render_target_face(void *rt, int face, int level);
void  vio_vk_clear_attachments(float r, float g, float b, float a);
int   vio_vk_render_target_cubemap(void *rt, void *cm_obj);
int   vio_vk_read_render_target(void *rt, int face, int attachment, void *out_rgba);
int   vio_vk_record_mips(VkCommandBuffer cmd, VkImage img, int w, int h, int layers, int levels);
void  vio_vk_texture_finish_mips(vio_vulkan_texture *tex);
int   vio_vk_generate_mipmaps(void *obj, int kind);
int   vio_vk_generate_depth_mips(void *rt_obj);   /* depth_only + 'mipmaps' (A26) */
void  vio_vk_depth_mip_shutdown(void);
int   vio_vk_upload_cubemap(void *cm_obj, int width, int height, const void *faces[6]);
void  vio_vk_destroy_cubemap(void *cm_obj);
void  vio_vk_bind_cubemap(void *cm_obj, int slot);
void *vio_vk_create_texture_ex(vio_texture_desc *desc);   /* arrays, BC, stored chains (Block 10c) */
int   vio_vk_set_shading_rate(int rate);
void  vio_vk_apply_shading_rate(VkCommandBuffer cmd, int primitive);   /* after binding a 3D pipeline;
                                                                        primitive: its VS writes the rate */

/* ── 3D pipeline (GAP-PHASE5 Block 10, vio_vulkan_3d*.c) ── */
/* Recorded draw sequences (BUNDLE-PLAN phase 2): secondary command buffers. */
void       *vio_vk3d_begin_bundle(void);
int         vio_vk3d_end_bundle(void *bundle);
int         vio_vk3d_draw_bundle(void *bundle);
void        vio_vk3d_destroy_bundle(void *bundle);int   vio_vk3d_available(void);
void  vio_vk3d_begin_frame(uint32_t frame_slot);
VkImageView vio_vk3d_dummy_view(int bindless_kind);   /* 1x1 2D / 2D array / cube view (cleared bindless slots) */
/* Copy bytes into the current frame's upload ring (uniform-buffer aligned). */
int   vio_vk3d_upload_bytes(const void *data, VkDeviceSize size, VkBuffer *out_buf, VkDeviceSize *out_off);
void  vio_vk3d_shutdown(void);
void  vio_vk3d_forget_texture(vio_vulkan_texture *tex);
void  vio_vk3d_forget_buffer(vio_vulkan_compute_buffer *buf);
void *vio_vk3d_compile_shader(vio_shader_desc *desc);
void  vio_vk3d_destroy_shader_obj(void *shader_obj);
void *vio_vk3d_create_pipeline(vio_pipeline_desc *desc);
void  vio_vk3d_destroy_pipeline(void *pipeline);
void  vio_vk3d_bind_pipeline(void *pipeline);
void  vio_vk3d_push_cbuffers(const void *vs_data, int vs_size, const void *fs_data, int fs_size);
void  vio_vk3d_bind_stage_constants(int stage, void *backend_buffer, const void *data, size_t size);
int   vio_vk3d_set_viewports(const int *rects, int count);
void  vio_vk_note_viewport(const VkViewport *vp, const VkRect2D *sc);
void  vio_vk3d_bind_texture(void *texture, int slot);
void  vio_vk3d_set_viewport(int x, int y, int width, int height);
void  vio_vk3d_draw(vio_draw_cmd *cmd);
void  vio_vk3d_draw_indexed(vio_draw_indexed_cmd *cmd);
void  vio_vk3d_draw_mesh_instanced(void *mesh_obj, const float *matrices, int count);
void  vio_vk3d_bind_storage_buffer(void *buf, int binding, int access, int element_count, int stride);
void  vio_vk3d_draw_instanced_from_storage(void *mesh_obj, int count);
void  vio_vk3d_draw_indirect(void *mesh_obj, void *args_buffer, int max_draws, size_t offset);
void  vio_vk3d_draw_mesh_tasks(uint32_t x, uint32_t y, uint32_t z);
void  vio_vk3d_draw_mesh_tasks_indirect(void *args_buffer, int max_draws, size_t offset);

/* ── Native upscalers (vio_vulkan_upscale.c, TEMPORAL-S3) ── */
struct _vio_upscale_create_desc;
struct _vio_upscale_dispatch_desc;
struct _vio_upscale_query;
int   vulkan_upscaler_supported(int provider, char *reason, size_t reason_len);
int   vulkan_upscaler_any(void);
void *vulkan_upscaler_create(const struct _vio_upscale_create_desc *desc, char *reason, size_t reason_len);
int   vulkan_upscaler_dispatch(void *upscaler, const struct _vio_upscale_dispatch_desc *desc, char *err, size_t err_len);
int   vulkan_upscaler_query(void *upscaler, int provider, struct _vio_upscale_query *q);
void  vulkan_upscaler_destroy(void *upscaler);
int   vulkan_upscaler_device_requirements(int provider, char *out, size_t out_len);

#endif /* HAVE_VULKAN */
#endif /* VIO_VULKAN_H */

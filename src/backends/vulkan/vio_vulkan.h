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
    /* Intrusive doubly-linked list of all live textures, anchored on
     * vio_vk.live_textures. vulkan_shutdown sweeps any survivors before
     * vkDestroyDevice so a PHP texture/font object outliving vio_destroy()
     * does not leave a VkImage alive at device destruction (a validation
     * error). vulkan_destroy_texture unlinks itself. */
    struct _vio_vulkan_texture *next;
    struct _vio_vulkan_texture *prev;
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
    VkFence         in_flight;
} vio_vk_frame;

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

    /* Render pass & framebuffers */
    VkRenderPass             render_pass;
    VkFramebuffer           *framebuffers;

    /* Swapchain "resume" render pass — render-pass-compatible with render_pass
     * (identical attachment formats/samples) but with color/depth loadOp=LOAD and
     * initialLayout matching what the primary pass leaves behind (color
     * PRESENT_SRC_KHR, depth DEPTH_STENCIL_ATTACHMENT_OPTIMAL). Used by
     * vio_unbind_render_target to re-open the swapchain pass mid-frame WITHOUT
     * clearing prior swapchain draws after an offscreen pass ran. Created lazily
     * on first mid-frame unbind, destroyed in vulkan_shutdown. NULL until then,
     * so a normal frame (no offscreen RT) never touches it. */
    VkRenderPass             swapchain_resume_render_pass;

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
    void                    *glfw_window;
    int                      framebuffer_width;
    int                      framebuffer_height;
} vio_vulkan_state;

extern vio_vulkan_state vio_vk;

/* Registration */
void vio_backend_vulkan_register(void);

/* Called after GLFW window creation to set up Vulkan */
int vio_vulkan_setup_context(void *glfw_window, vio_config *cfg);

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
 * vio_render_target.h here. The implementations live in vio_vulkan.c. */

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

#endif /* HAVE_VULKAN */
#endif /* VIO_VULKAN_H */

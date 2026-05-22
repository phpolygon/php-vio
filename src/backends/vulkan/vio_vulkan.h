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

/* Record the mid-frame switch back to the swapchain: vkCmdEndRenderPass
 * (offscreen, which transitions its color to SHADER_READ_ONLY via finalLayout)
 * -> begin the loadOp=LOAD swapchain resume pass (created lazily) -> restore the
 * swapchain viewport/scissor -> vio_vk.current_bound_rt = NULL. Caller MUST
 * ensure vio_vk.in_frame and that current_bound_rt is set. */
void vulkan_record_unbind_render_target(void);

#endif /* HAVE_VULKAN */
#endif /* VIO_VULKAN_H */

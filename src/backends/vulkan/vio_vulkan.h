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

#endif /* HAVE_VULKAN */
#endif /* VIO_VULKAN_H */

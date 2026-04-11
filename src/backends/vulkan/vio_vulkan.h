/*
 * php-vio - Vulkan Backend
 */

#ifndef VIO_VULKAN_H
#define VIO_VULKAN_H

#include "../../../include/vio_backend.h"

#ifdef HAVE_VULKAN

#include <vulkan/vulkan.h>

#define VIO_VK_MAX_FRAMES_IN_FLIGHT 2

/* Per-frame synchronization and command buffer resources */
typedef struct _vio_vk_frame {
    VkCommandPool   cmd_pool;
    VkCommandBuffer cmd_buf;
    VkSemaphore     image_available;
    VkSemaphore     render_finished;
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

    /* State */
    int                      initialized;
    int                      swapchain_needs_recreate;
    float                    clear_r, clear_g, clear_b, clear_a;

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

#endif /* HAVE_VULKAN */
#endif /* VIO_VULKAN_H */

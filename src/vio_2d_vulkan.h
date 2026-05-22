/*
 * php-vio - 2D Rendering Vulkan backend state
 */

#ifndef VIO_2D_VULKAN_H
#define VIO_2D_VULKAN_H

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "../include/vio_backend.h"

#ifdef HAVE_VULKAN

#include <vulkan/vulkan.h>

typedef struct _vio_2d_vulkan_state {
    /* Shader modules (kept alive for pipeline rebuilds; cheap to retain) */
    VkShaderModule         vs_module;
    VkShaderModule         fs_shapes_module;
    VkShaderModule         fs_sprites_module;

    /* Layout: set 0 = { binding 0: combined image sampler (fragment) }.
     * Projection mat4 is a vertex-stage push constant (64 bytes). Both
     * pipelines share this layout so they are pipeline-layout compatible. */
    VkDescriptorSetLayout  set_layout;
    VkPipelineLayout       pipeline_layout;

    /* Two pipelines built against vio_vk.render_pass, subpass 0. */
    VkPipeline             pipeline_shapes;
    VkPipeline             pipeline_sprites;

    /* Streaming vertex buffer: HOST_VISIBLE|HOST_COHERENT, persistently
     * mapped, sized for VIO_VK_MAX_FRAMES_IN_FLIGHT slices. The per-frame
     * in_flight fence (waited in vulkan_begin_frame) gates slice reuse. */
    VkBuffer               vbo;
    void                  *vbo_allocation;   /* VmaAllocation (opaque) */
    unsigned char         *vbo_mapped;       /* persistently mapped base */
    VkDeviceSize           vbo_slice_size;   /* per-frame slice in bytes */
    VkDeviceSize           vbo_size;         /* total = slice * frames */
} vio_2d_vulkan_state;

int  vio_2d_vulkan_init(vio_2d_vulkan_state *state);
void vio_2d_vulkan_shutdown(vio_2d_vulkan_state *state);

#endif /* HAVE_VULKAN */
#endif /* VIO_2D_VULKAN_H */

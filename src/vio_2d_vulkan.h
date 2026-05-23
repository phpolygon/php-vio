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

#include "backends/vulkan/vio_vulkan.h"   /* VIO_VK_MAX_FRAMES_IN_FLIGHT */

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

    /* Per-frame-in-flight descriptor pool ring (the analog of the D3D12
     * per-frame SRV ring). On each texture change the flush allocates a
     * combined-image-sampler set from the CURRENT frame's pool, updates it to
     * point at the texture's {view,sampler}, and binds it. The whole pool is
     * reset at the top of vulkan_begin_frame AFTER the in_flight fence wait —
     * that fence is the sync point that guarantees the prior occupant of these
     * sets is no longer in flight, so reset is hazard-free. Sized generously so
     * a frame with many distinct textures never exhausts the pool. */
    VkDescriptorPool       desc_pools[VIO_VK_MAX_FRAMES_IN_FLIGHT];
} vio_2d_vulkan_state;

int  vio_2d_vulkan_init(vio_2d_vulkan_state *state);
void vio_2d_vulkan_shutdown(vio_2d_vulkan_state *state);

/* Allocate a combined-image-sampler descriptor set from `frame_index`'s pool,
 * point binding 0 at the given image view + sampler, and return it. Returns
 * VK_NULL_HANDLE on pool exhaustion (caller should skip the textured draw). */
VkDescriptorSet vio_2d_vulkan_alloc_texture_set(vio_2d_vulkan_state *state,
                                                uint32_t frame_index,
                                                VkImageView view, VkSampler sampler);

#endif /* HAVE_VULKAN */
#endif /* VIO_2D_VULKAN_H */

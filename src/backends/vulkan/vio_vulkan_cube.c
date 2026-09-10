/*
 * php-vio - Vulkan cubemaps and mip generation (GAP-PHASE5 Block 10b)
 *
 * vio_cubemap uploads six RGBA8 faces into a cube-compatible image (with a blit
 * mip chain for 'mipmaps' => true); vio_generate_mipmaps blits the chain of a
 * mipmapped texture, a cubemap or a cube render target - inside a frame on the
 * frame command buffer (the swapchain pass is suspended around it), otherwise on
 * a transient command buffer.
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "php.h"

#ifdef HAVE_VULKAN

#include <vulkan/vulkan.h>
#include "vio_vulkan.h"
#include "../../vio_texture.h"
#include "../../vio_cubemap.h"
#include "../../vio_render_target.h"
#include "../../../include/vio_types.h"
#include <string.h>
#include <stdlib.h>

static int vkc_full_levels(int w, int h)
{
    int n = 1, m = w > h ? w : h;
    while (m > 1) { m >>= 1; n++; }
    return n;
}

/* Every level of every layer is SHADER_READ_ONLY before and after. */
int vio_vk_record_mips(VkCommandBuffer cmd, VkImage img, int w, int h, int layers, int levels)
{
    for (int l = 1; l < levels; l++) {
        int32_t sw = (w >> (l - 1)) > 0 ? (w >> (l - 1)) : 1, sh = (h >> (l - 1)) > 0 ? (h >> (l - 1)) : 1;
        int32_t dw = (w >> l) > 0 ? (w >> l) : 1, dh = (h >> l) > 0 ? (h >> l) : 1;
        vio_vk_image_barrier_range(cmd, img, VK_IMAGE_ASPECT_COLOR_BIT, (uint32_t)(l - 1), 1, 0, (uint32_t)layers,
                                   VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        vio_vk_image_barrier_range(cmd, img, VK_IMAGE_ASPECT_COLOR_BIT, (uint32_t)l, 1, 0, (uint32_t)layers,
                                   VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        VkImageBlit b;
        memset(&b, 0, sizeof(b));
        b.srcSubresource.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;
        b.srcSubresource.mipLevel       = (uint32_t)(l - 1);
        b.srcSubresource.layerCount     = (uint32_t)layers;
        b.srcOffsets[1].x = sw; b.srcOffsets[1].y = sh; b.srcOffsets[1].z = 1;
        b.dstSubresource.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;
        b.dstSubresource.mipLevel       = (uint32_t)l;
        b.dstSubresource.layerCount     = (uint32_t)layers;
        b.dstOffsets[1].x = dw; b.dstOffsets[1].y = dh; b.dstOffsets[1].z = 1;
        vkCmdBlitImage(cmd, img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &b, VK_FILTER_LINEAR);
        vio_vk_image_barrier_range(cmd, img, VK_IMAGE_ASPECT_COLOR_BIT, (uint32_t)(l - 1), 1, 0, (uint32_t)layers,
                                   VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        vio_vk_image_barrier_range(cmd, img, VK_IMAGE_ASPECT_COLOR_BIT, (uint32_t)l, 1, 0, (uint32_t)layers,
                                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    }
    return 0;
}

static int vkc_run_mips(VkImage img, int w, int h, int layers, int levels)
{
    if (levels <= 1 || !img) return 0;
    if (!vio_vk.in_frame) {
        VkCommandBuffer cmd = VK_NULL_HANDLE;
        if (vio_vk_begin_transient(&cmd) != 0) return -1;
        vio_vk_record_mips(cmd, img, w, h, layers, levels);
        return vio_vk_submit_transient(cmd);
    }
    if (vio_vk.current_bound_rt) {
        php_error_docref(NULL, E_WARNING, "vio_generate_mipmaps: unbind the render target first (Vulkan)");
        return -1;
    }
    /* The frame's draws into the image are recorded on the frame command buffer,
     * so the blits must follow them there. */
    VkCommandBuffer cmd = vio_vk.frames[vio_vk.current_frame].cmd_buf;
    int had_pass = vio_vk.cur_render_pass != VK_NULL_HANDLE;
    if (had_pass) {
        vkCmdEndRenderPass(cmd);
        vio_vk.cur_render_pass = VK_NULL_HANDLE;
    }
    vio_vk_record_mips(cmd, img, w, h, layers, levels);
    if (had_pass && !vio_vk.frame_is_offscreen) vio_vk_resume_swapchain_pass(cmd);
    return 0;
}

/* A freshly created mipmapped texture: level 0 is SHADER_READ_ONLY, the others
 * are still UNDEFINED - bring them in line, then blit the chain. */
void vio_vk_texture_finish_mips(vio_vulkan_texture *tex)
{
    if (!tex || tex->mip_levels <= 1 || !tex->image) return;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    if (vio_vk_begin_transient(&cmd) != 0) return;
    vio_vk_image_barrier_range(cmd, tex->image, VK_IMAGE_ASPECT_COLOR_BIT, 1, (uint32_t)(tex->mip_levels - 1), 0, 1,
                               VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    vio_vk_record_mips(cmd, tex->image, tex->width, tex->height, 1, tex->mip_levels);
    vio_vk_submit_transient(cmd);
}

int vio_vk_generate_mipmaps(void *obj, int kind)
{
    if (!obj || !vio_vk.device) return -1;
    switch (kind) {
        case 0: {
            vio_render_target_object *rt = (vio_render_target_object *)obj;
            vio_vk_rt *x = (vio_vk_rt *)rt->vulkan_rt;
            if (!x) return -1;
            if (!x->cube || x->levels <= 1) return 0;
            return vkc_run_mips(x->color_image[0], rt->width, rt->height, 6, x->levels);
        }
        case 1: {
            vio_texture_object *to = (vio_texture_object *)obj;
            vio_vulkan_texture *t = (vio_vulkan_texture *)to->backend_texture;
            if (!t) return -1;
            if (t->mip_levels <= 1 || t->depth > 0) return 0;
            return vkc_run_mips(t->image, t->width, t->height, 1, t->mip_levels);
        }
        case 2: {
            vio_cubemap_object *cm = (vio_cubemap_object *)obj;
            vio_vulkan_texture *t = (vio_vulkan_texture *)cm->vulkan_texture;
            if (!t) return -1;
            if (t->mip_levels <= 1) return 0;
            return vkc_run_mips(t->image, t->width, t->height, 6, t->mip_levels);
        }
        default:
            return -1;
    }
}

int vio_vk_upload_cubemap(void *cm_obj, int width, int height, const void *faces[6])
{
    vio_cubemap_object *cm = (vio_cubemap_object *)cm_obj;
    if (!cm || !vio_vk.device || width <= 0 || height != width) return -1;
    for (int f = 0; f < 6; f++) if (!faces[f]) return -1;
    int levels = cm->mipmaps ? vkc_full_levels(width, height) : 1;
    VkFormat fmt = VK_FORMAT_R8G8B8A8_UNORM;

    vio_vulkan_texture *tex = (vio_vulkan_texture *)calloc(1, sizeof(vio_vulkan_texture));
    if (!tex) return -1;
    VkImageCreateInfo ci = {0};
    ci.sType         = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    ci.imageType     = VK_IMAGE_TYPE_2D;
    ci.format        = fmt;
    ci.extent.width  = (uint32_t)width;
    ci.extent.height = (uint32_t)height;
    ci.extent.depth  = 1;
    ci.mipLevels     = (uint32_t)levels;
    ci.arrayLayers   = 6;
    ci.samples       = VK_SAMPLE_COUNT_1_BIT;
    ci.tiling        = VK_IMAGE_TILING_OPTIMAL;
    ci.usage         = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    ci.sharingMode   = VK_SHARING_MODE_EXCLUSIVE;
    ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    ci.flags         = VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;
    if (vio_vma_create_image(vio_vk.vma_allocator, &ci, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &tex->image, &tex->allocation) != 0) {
        free(tex);
        return -1;
    }

    VkDeviceSize face_bytes = (VkDeviceSize)width * (VkDeviceSize)height * 4;
    VkBuffer staging = VK_NULL_HANDLE;
    void *staging_alloc = NULL;
    int ok = vio_vma_create_buffer(vio_vk.vma_allocator, face_bytes * 6, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                                   VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                                   &staging, &staging_alloc) == 0;
    unsigned char *m = ok ? (unsigned char *)vio_vma_map(vio_vk.vma_allocator, staging_alloc) : NULL;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    if (m) {
        for (int f = 0; f < 6; f++) memcpy(m + (size_t)f * (size_t)face_bytes, faces[f], (size_t)face_bytes);
        vio_vma_unmap(vio_vk.vma_allocator, staging_alloc);
        ok = vio_vk_begin_transient(&cmd) == 0;
    } else {
        ok = 0;
    }
    if (ok) {
        vio_vk_image_barrier_range(cmd, tex->image, VK_IMAGE_ASPECT_COLOR_BIT, 0, (uint32_t)levels, 0, 6,
                                   VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        VkBufferImageCopy copies[6];
        memset(copies, 0, sizeof(copies));
        for (int f = 0; f < 6; f++) {
            copies[f].bufferOffset = (VkDeviceSize)f * face_bytes;
            copies[f].imageSubresource.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;
            copies[f].imageSubresource.baseArrayLayer = (uint32_t)f;
            copies[f].imageSubresource.layerCount     = 1;
            copies[f].imageExtent.width  = (uint32_t)width;
            copies[f].imageExtent.height = (uint32_t)height;
            copies[f].imageExtent.depth  = 1;
        }
        vkCmdCopyBufferToImage(cmd, staging, tex->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 6, copies);
        vio_vk_image_barrier_range(cmd, tex->image, VK_IMAGE_ASPECT_COLOR_BIT, 0, (uint32_t)levels, 0, 6,
                                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        if (levels > 1) vio_vk_record_mips(cmd, tex->image, width, height, 6, levels);
        ok = vio_vk_submit_transient(cmd) == 0;
    }
    if (staging) vio_vma_destroy_buffer(vio_vk.vma_allocator, staging, staging_alloc);
    if (ok) {
        VkImageViewCreateInfo iv = {0};
        iv.sType    = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        iv.image    = tex->image;
        iv.viewType = VK_IMAGE_VIEW_TYPE_CUBE;
        iv.format   = fmt;
        iv.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        iv.subresourceRange.levelCount = (uint32_t)levels;
        iv.subresourceRange.layerCount = 6;
        ok = vkCreateImageView(vio_vk.device, &iv, NULL, &tex->view) == VK_SUCCESS;
    }
    if (ok) {
        VkSamplerCreateInfo sci = {0};
        sci.sType        = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
        sci.magFilter    = sci.minFilter = VK_FILTER_LINEAR;
        sci.mipmapMode   = VK_SAMPLER_MIPMAP_MODE_LINEAR;
        sci.addressModeU = sci.addressModeV = sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        sci.maxLod       = (float)levels;
        ok = vkCreateSampler(vio_vk.device, &sci, NULL, &tex->sampler) == VK_SUCCESS;
    }
    if (!ok) {
        if (tex->view) vkDestroyImageView(vio_vk.device, tex->view, NULL);
        vio_vma_destroy_image(vio_vk.vma_allocator, tex->image, tex->allocation);
        free(tex);
        php_error_docref(NULL, E_WARNING, "Vulkan: cubemap upload failed (%dx%d)", width, height);
        return -1;
    }
    tex->width = width;
    tex->height = height;
    tex->view_type = VK_IMAGE_VIEW_TYPE_CUBE;
    tex->mip_levels = levels;
    tex->filter = VIO_FILTER_LINEAR;
    tex->wrap = VIO_WRAP_CLAMP;
    tex->prev = NULL;
    tex->next = vio_vk.live_textures;
    if (vio_vk.live_textures) vio_vk.live_textures->prev = tex;
    vio_vk.live_textures = tex;

    cm->vulkan_texture = tex;
    cm->backend_type   = 5;
    cm->resolution     = width;
    return 0;
}

void vio_vk_destroy_cubemap(void *cm_obj)
{
    vio_cubemap_object *cm = (vio_cubemap_object *)cm_obj;
    if (!cm || !cm->vulkan_texture) return;
    if (!cm->borrowed) vio_vk_release_texture((vio_vulkan_texture *)cm->vulkan_texture);
    cm->vulkan_texture = NULL;
}

void vio_vk_bind_cubemap(void *cm_obj, int slot)
{
    vio_cubemap_object *cm = (vio_cubemap_object *)cm_obj;
    if (cm && cm->vulkan_texture) vio_vk3d_bind_texture(cm->vulkan_texture, slot);
}

#endif /* HAVE_VULKAN */

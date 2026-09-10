/*
 * php-vio - Vulkan render targets (GAP-PHASE5 Block 10b)
 *
 * One vio_vk_rt per vio_render_target_object (rt->vulkan_rt):
 *   - up to 4 colour attachments in any vio_pixel_format (RGBA8 = B8G8R8A8, the
 *     swapchain byte order, so the 2D pipelines stay compatible),
 *   - MSAA: multisampled colour + depth, resolved by the render pass into the
 *     single-sample images that are sampled and read back,
 *   - cube targets: one 6-layer image with a mip chain, a framebuffer per
 *     (face, level) - level 0 with the depth attachment, the others without,
 *   - depth-only targets store and sample their depth (DEPTH_STENCIL_READ_ONLY).
 * Every pass clears on bind; colour ends SHADER_READ_ONLY, so an unbind needs no
 * barrier before sampling.
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "php.h"

#ifdef HAVE_VULKAN

#include <vulkan/vulkan.h>
#include "vio_vulkan.h"
#include "../../vio_render_target.h"
#include "../../vio_cubemap.h"
#include "../../../include/vio_types.h"
#include <string.h>
#include <stdlib.h>

/* ── Helpers ───────────────────────────────────────────────────────── */

static VkAccessFlags vkrt_access(VkImageLayout l)
{
    switch (l) {
        case VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL:            return VK_ACCESS_TRANSFER_WRITE_BIT;
        case VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL:            return VK_ACCESS_TRANSFER_READ_BIT;
        case VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL:
        case VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL: return VK_ACCESS_SHADER_READ_BIT;
        default:                                              return 0;
    }
}

void vio_vk_image_barrier_range(VkCommandBuffer cmd, VkImage image, VkImageAspectFlags aspect,
                                uint32_t base_level, uint32_t levels, uint32_t base_layer, uint32_t layers,
                                VkImageLayout from, VkImageLayout to)
{
    VkImageMemoryBarrier b = {0};
    b.sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    b.oldLayout           = from;
    b.newLayout           = to;
    b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image               = image;
    b.subresourceRange.aspectMask     = aspect;
    b.subresourceRange.baseMipLevel   = base_level;
    b.subresourceRange.levelCount     = levels ? levels : 1;
    b.subresourceRange.baseArrayLayer = base_layer;
    b.subresourceRange.layerCount     = layers ? layers : 1;
    b.srcAccessMask = vkrt_access(from);
    b.dstAccessMask = vkrt_access(to);
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                         0, 0, NULL, 0, NULL, 1, &b);
}

static VkImageAspectFlags vkrt_depth_aspect(void)
{
    return VK_IMAGE_ASPECT_DEPTH_BIT | (vio_vk.depth_has_stencil ? VK_IMAGE_ASPECT_STENCIL_BIT : 0);
}

static VkFormat vkrt_format(int f)
{
    switch (f) {
        case VIO_FORMAT_RGBA16F:    return VK_FORMAT_R16G16B16A16_SFLOAT;
        case VIO_FORMAT_RGBA32F:    return VK_FORMAT_R32G32B32A32_SFLOAT;
        case VIO_FORMAT_R11G11B10F: return VK_FORMAT_B10G11R11_UFLOAT_PACK32;
        case VIO_FORMAT_RG16F:      return VK_FORMAT_R16G16_SFLOAT;
        case VIO_FORMAT_R16F:       return VK_FORMAT_R16_SFLOAT;
        case VIO_FORMAT_R32F:       return VK_FORMAT_R32_SFLOAT;
        case VIO_FORMAT_R8:         return VK_FORMAT_R8_UNORM;
        case VIO_FORMAT_RGB10A2:    return VK_FORMAT_A2B10G10R10_UNORM_PACK32;
        default:                    return VK_FORMAT_B8G8R8A8_UNORM;
    }
}

static int vkrt_image(VkFormat fmt, int w, int h, int levels, int layers, int samples, VkImageUsageFlags usage,
                      int cube, VkImage *img, void **alloc)
{
    VkImageCreateInfo ci = {0};
    ci.sType         = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    ci.imageType     = VK_IMAGE_TYPE_2D;
    ci.format        = fmt;
    ci.extent.width  = (uint32_t)w;
    ci.extent.height = (uint32_t)h;
    ci.extent.depth  = 1;
    ci.mipLevels     = (uint32_t)(levels > 0 ? levels : 1);
    ci.arrayLayers   = (uint32_t)(layers > 0 ? layers : 1);
    ci.samples       = (VkSampleCountFlagBits)(samples > 1 ? samples : 1);
    ci.tiling        = VK_IMAGE_TILING_OPTIMAL;
    ci.usage         = usage;
    ci.sharingMode   = VK_SHARING_MODE_EXCLUSIVE;
    ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    ci.flags         = cube ? VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT : 0;
    return vio_vma_create_image(vio_vk.vma_allocator, &ci, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, img, alloc);
}

static VkImageView vkrt_view(VkImage img, VkFormat fmt, VkImageViewType type, VkImageAspectFlags aspect,
                             uint32_t base_level, uint32_t levels, uint32_t base_layer, uint32_t layers)
{
    VkImageViewCreateInfo iv = {0};
    iv.sType    = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    iv.image    = img;
    iv.viewType = type;
    iv.format   = fmt;
    iv.subresourceRange.aspectMask     = aspect;
    iv.subresourceRange.baseMipLevel   = base_level;
    iv.subresourceRange.levelCount     = levels;
    iv.subresourceRange.baseArrayLayer = base_layer;
    iv.subresourceRange.layerCount     = layers;
    VkImageView v = VK_NULL_HANDLE;
    if (vkCreateImageView(vio_vk.device, &iv, NULL, &v) != VK_SUCCESS) return VK_NULL_HANDLE;
    return v;
}

static void vkrt_dependency(VkSubpassDependency *dep)
{
    /* Byte-identical to vio_vk.render_pass's dependency (render-pass compatibility). */
    memset(dep, 0, sizeof(*dep));
    dep->srcSubpass    = VK_SUBPASS_EXTERNAL;
    dep->dstSubpass    = 0;
    dep->srcStageMask  = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
    dep->srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    dep->dstStageMask  = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
    dep->dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
}

/* Attachments: colour 0..count-1, depth, then (MSAA) the resolve targets. */
static VkRenderPass vkrt_pass(const vio_vk_rt *x, int with_depth)
{
    VkAttachmentDescription a[9];
    VkAttachmentReference cref[4], rref[4], dref;
    memset(a, 0, sizeof(a));
    uint32_t n = 0;
    int ms = x->samples > 1;
    for (int i = 0; i < x->count; i++) {
        a[n].format         = x->color_format[i];
        a[n].samples        = (VkSampleCountFlagBits)x->samples;
        a[n].loadOp         = VK_ATTACHMENT_LOAD_OP_CLEAR;
        a[n].storeOp        = ms ? VK_ATTACHMENT_STORE_OP_DONT_CARE : VK_ATTACHMENT_STORE_OP_STORE;
        a[n].stencilLoadOp  = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        a[n].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        a[n].initialLayout  = VK_IMAGE_LAYOUT_UNDEFINED;
        a[n].finalLayout    = ms ? VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        cref[i].attachment  = n;
        cref[i].layout      = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        n++;
    }
    if (with_depth) {
        int depth_only = x->count == 0;
        a[n].format         = vio_vk_depth_format();
        a[n].samples        = (VkSampleCountFlagBits)x->samples;
        a[n].loadOp         = VK_ATTACHMENT_LOAD_OP_CLEAR;
        a[n].storeOp        = depth_only ? VK_ATTACHMENT_STORE_OP_STORE : VK_ATTACHMENT_STORE_OP_DONT_CARE;
        a[n].stencilLoadOp  = vio_vk.depth_has_stencil ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        a[n].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        a[n].initialLayout  = VK_IMAGE_LAYOUT_UNDEFINED;
        a[n].finalLayout    = depth_only ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        dref.attachment     = n;
        dref.layout         = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        n++;
    }
    if (ms) {
        for (int i = 0; i < x->count; i++) {
            a[n].format         = x->color_format[i];
            a[n].samples        = VK_SAMPLE_COUNT_1_BIT;
            a[n].loadOp         = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
            a[n].storeOp        = VK_ATTACHMENT_STORE_OP_STORE;
            a[n].stencilLoadOp  = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
            a[n].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
            a[n].initialLayout  = VK_IMAGE_LAYOUT_UNDEFINED;
            a[n].finalLayout    = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            rref[i].attachment  = n;
            rref[i].layout      = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            n++;
        }
    }
    VkSubpassDescription sp = {0};
    sp.pipelineBindPoint       = VK_PIPELINE_BIND_POINT_GRAPHICS;
    sp.colorAttachmentCount    = (uint32_t)x->count;
    sp.pColorAttachments       = x->count ? cref : NULL;
    sp.pResolveAttachments     = (ms && x->count) ? rref : NULL;
    sp.pDepthStencilAttachment = with_depth ? &dref : NULL;
    VkSubpassDependency dep;
    vkrt_dependency(&dep);
    VkRenderPassCreateInfo ri = {0};
    ri.sType           = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    ri.attachmentCount = n;
    ri.pAttachments    = a;
    ri.subpassCount    = 1;
    ri.pSubpasses      = &sp;
    ri.dependencyCount = 1;
    ri.pDependencies   = &dep;
    VkRenderPass rp = VK_NULL_HANDLE;
    if (vkCreateRenderPass(vio_vk.device, &ri, NULL, &rp) != VK_SUCCESS) return VK_NULL_HANDLE;
    return rp;
}

VkRenderPass vio_vk_swapchain_resume_pass(void)
{
    if (vio_vk.swapchain_resume_render_pass) return vio_vk.swapchain_resume_render_pass;
    VkAttachmentDescription a[2];
    memset(a, 0, sizeof(a));
    a[0].format         = vio_vk.swapchain_format;
    a[0].samples        = VK_SAMPLE_COUNT_1_BIT;
    a[0].loadOp         = VK_ATTACHMENT_LOAD_OP_LOAD;
    a[0].storeOp        = VK_ATTACHMENT_STORE_OP_STORE;
    a[0].stencilLoadOp  = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    a[0].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    a[0].initialLayout  = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    a[0].finalLayout    = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    a[1].format         = vio_vk_depth_format();
    a[1].samples        = VK_SAMPLE_COUNT_1_BIT;
    a[1].loadOp         = VK_ATTACHMENT_LOAD_OP_LOAD;
    a[1].storeOp        = VK_ATTACHMENT_STORE_OP_STORE;
    a[1].stencilLoadOp  = vio_vk.depth_has_stencil ? VK_ATTACHMENT_LOAD_OP_LOAD : VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    a[1].stencilStoreOp = vio_vk.depth_has_stencil ? VK_ATTACHMENT_STORE_OP_STORE : VK_ATTACHMENT_STORE_OP_DONT_CARE;
    a[1].initialLayout  = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    a[1].finalLayout    = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    VkAttachmentReference cref = { 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
    VkAttachmentReference dref = { 1, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL };
    VkSubpassDescription sp = {0};
    sp.pipelineBindPoint       = VK_PIPELINE_BIND_POINT_GRAPHICS;
    sp.colorAttachmentCount    = 1;
    sp.pColorAttachments       = &cref;
    sp.pDepthStencilAttachment = &dref;
    VkSubpassDependency dep;
    vkrt_dependency(&dep);
    VkRenderPassCreateInfo ri = {0};
    ri.sType           = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    ri.attachmentCount = 2;
    ri.pAttachments    = a;
    ri.subpassCount    = 1;
    ri.pSubpasses      = &sp;
    ri.dependencyCount = 1;
    ri.pDependencies   = &dep;
    if (vkCreateRenderPass(vio_vk.device, &ri, NULL, &vio_vk.swapchain_resume_render_pass) != VK_SUCCESS) {
        vio_vk.swapchain_resume_render_pass = VK_NULL_HANDLE;
    }
    return vio_vk.swapchain_resume_render_pass;
}

/* Re-open the swapchain pass with LOAD on the frame command buffer (no pass open). */
void vio_vk_resume_swapchain_pass(VkCommandBuffer cmd)
{
    VkRenderPass resume = vio_vk_swapchain_resume_pass();
    if (resume == VK_NULL_HANDLE || !vio_vk.framebuffers) return;
    VkRenderPassBeginInfo rp = {0};
    rp.sType             = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    rp.renderPass        = resume;
    rp.framebuffer       = vio_vk.framebuffers[vio_vk.current_image_index];
    rp.renderArea.extent = vio_vk.swapchain_extent;
    vkCmdBeginRenderPass(cmd, &rp, VK_SUBPASS_CONTENTS_INLINE);
    VkViewport vp = { 0.0f, 0.0f, (float)vio_vk.swapchain_extent.width, (float)vio_vk.swapchain_extent.height, 0.0f, 1.0f };
    vkCmdSetViewport(cmd, 0, 1, &vp);
    VkRect2D sc = { { 0, 0 }, vio_vk.swapchain_extent };
    vkCmdSetScissor(cmd, 0, 1, &sc);
    vio_vk.cur_render_pass      = resume;
    vio_vk.cur_color_count      = 1;
    vio_vk.cur_color_formats[0] = vio_vk.swapchain_format;
    vio_vk.cur_samples          = 1;
    vio_vk.cur_has_depth        = 1;
    vio_vk.cur_width            = vio_vk.swapchain_extent.width;
    vio_vk.cur_height           = vio_vk.swapchain_extent.height;
}

/* ── Live tracking (swept before vkDestroyDevice) ──────────────────── */

void vulkan_rt_track(void *rt)
{
    if (!rt) return;
    for (uint32_t i = 0; i < vio_vk.live_rt_count; i++) if (vio_vk.live_render_targets[i] == rt) return;
    if (vio_vk.live_rt_count == vio_vk.live_rt_capacity) {
        uint32_t cap = vio_vk.live_rt_capacity ? vio_vk.live_rt_capacity * 2 : 8;
        void **grown = (void **)realloc(vio_vk.live_render_targets, cap * sizeof(void *));
        if (!grown) return;
        vio_vk.live_render_targets = grown;
        vio_vk.live_rt_capacity = cap;
    }
    vio_vk.live_render_targets[vio_vk.live_rt_count++] = rt;
}

void vulkan_rt_untrack(void *rt)
{
    if (!rt || !vio_vk.live_render_targets) return;
    for (uint32_t i = 0; i < vio_vk.live_rt_count; i++) {
        if (vio_vk.live_render_targets[i] == rt) {
            vio_vk.live_render_targets[i] = vio_vk.live_render_targets[vio_vk.live_rt_count - 1];
            vio_vk.live_rt_count--;
            return;
        }
    }
}

/* ── Create / destroy ──────────────────────────────────────────────── */

/* Mid-frame the handles may be referenced by the recording command buffer: park
 * them until the slot's fence (vio_vk_defer_destroy), otherwise destroy now. */
static void vkrt_kill(int kind, uint64_t h, void *alloc)
{
    if (!h || !vio_vk.device) return;
    if (vio_vk.in_frame) { vio_vk_defer_destroy(kind, h, alloc); return; }
    switch (kind) {
        case VIO_VK_GRAVE_IMAGE:       vio_vma_destroy_image(vio_vk.vma_allocator, (VkImage)h, alloc); break;
        case VIO_VK_GRAVE_VIEW:        vkDestroyImageView(vio_vk.device, (VkImageView)h, NULL); break;
        case VIO_VK_GRAVE_SAMPLER:     vkDestroySampler(vio_vk.device, (VkSampler)h, NULL); break;
        case VIO_VK_GRAVE_FRAMEBUFFER: vkDestroyFramebuffer(vio_vk.device, (VkFramebuffer)h, NULL); break;
        case VIO_VK_GRAVE_RENDER_PASS: vkDestroyRenderPass(vio_vk.device, (VkRenderPass)h, NULL); break;
        default: break;
    }
}

static void vkrt_free_wrapper(vio_vulkan_texture *w)
{
    if (!w) return;
    vio_vk3d_forget_texture(w);
    vkrt_kill(VIO_VK_GRAVE_SAMPLER, (uint64_t)w->sampler_cmp, NULL);
    free(w);
}

static void vkrt_free(vio_vk_rt *x)
{
    if (!x) return;
    for (int i = 0; i < 4; i++) vkrt_free_wrapper(x->wrap[i]);
    vkrt_free_wrapper(x->cube_wrap);
    int faces = 6 * (x->levels > 0 ? x->levels : 1);
    if (x->face_fb)   for (int i = 0; i < faces; i++) vkrt_kill(VIO_VK_GRAVE_FRAMEBUFFER, (uint64_t)x->face_fb[i], NULL);
    if (x->face_view) for (int i = 0; i < faces; i++) vkrt_kill(VIO_VK_GRAVE_VIEW, (uint64_t)x->face_view[i], NULL);
    vkrt_kill(VIO_VK_GRAVE_FRAMEBUFFER, (uint64_t)x->fb, NULL);
    vkrt_kill(VIO_VK_GRAVE_RENDER_PASS, (uint64_t)x->pass, NULL);
    vkrt_kill(VIO_VK_GRAVE_RENDER_PASS, (uint64_t)x->pass_nodepth, NULL);
    vkrt_kill(VIO_VK_GRAVE_SAMPLER, (uint64_t)x->sampler, NULL);
    vkrt_kill(VIO_VK_GRAVE_VIEW, (uint64_t)x->cube_view, NULL);
    for (int i = 0; i < 4; i++) {
        vkrt_kill(VIO_VK_GRAVE_VIEW, (uint64_t)x->color_view[i], NULL);
        vkrt_kill(VIO_VK_GRAVE_IMAGE, (uint64_t)x->color_image[i], x->color_alloc[i]);
        vkrt_kill(VIO_VK_GRAVE_VIEW, (uint64_t)x->msaa_view[i], NULL);
        vkrt_kill(VIO_VK_GRAVE_IMAGE, (uint64_t)x->msaa_image[i], x->msaa_alloc[i]);
    }
    vkrt_kill(VIO_VK_GRAVE_VIEW, (uint64_t)x->depth_view, NULL);
    vkrt_kill(VIO_VK_GRAVE_IMAGE, (uint64_t)x->depth_image, x->depth_alloc);
    free(x->face_fb);
    free(x->face_view);
    free(x);
}

static int vkrt_supported_samples(int want)
{
    if (want <= 1) return 1;
    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(vio_vk.physical_device, &props);
    VkSampleCountFlags ok = props.limits.framebufferColorSampleCounts & props.limits.framebufferDepthSampleCounts;
    int s = want >= 8 ? 8 : (want >= 4 ? 4 : 2);
    while (s > 1 && !(ok & (VkSampleCountFlags)s)) s >>= 1;
    return s;
}

static VkFramebuffer vkrt_framebuffer(VkRenderPass pass, VkImageView *views, uint32_t n, uint32_t w, uint32_t h)
{
    VkFramebufferCreateInfo fi = {0};
    fi.sType           = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
    fi.renderPass      = pass;
    fi.attachmentCount = n;
    fi.pAttachments    = views;
    fi.width           = w;
    fi.height          = h;
    fi.layers          = 1;
    VkFramebuffer fb = VK_NULL_HANDLE;
    if (vkCreateFramebuffer(vio_vk.device, &fi, NULL, &fb) != VK_SUCCESS) return VK_NULL_HANDLE;
    return fb;
}

int vulkan_create_render_target(void *rt_ptr, int width, int height, int hdr, int depth_only)
{
    (void)hdr;   /* rt->formats[] carries the attachment formats */
    vio_render_target_object *rt = (vio_render_target_object *)rt_ptr;
    if (!rt || !vio_vk.initialized || !vio_vk.device || width <= 0 || height <= 0) return -1;
    vio_vk_rt *x = (vio_vk_rt *)calloc(1, sizeof(vio_vk_rt));
    if (!x) return -1;
    x->count   = depth_only ? 0 : (rt->attachment_count > 1 ? (rt->attachment_count > 4 ? 4 : rt->attachment_count) : 1);
    x->cube    = rt->is_cube ? 1 : 0;
    x->levels  = (x->cube && rt->mip_levels > 1) ? rt->mip_levels : 1;
    x->samples = (x->cube || depth_only) ? 1 : vkrt_supported_samples(rt->samples);
    rt->samples = x->samples;
    VkFormat df = vio_vk_depth_format();
    int layers = x->cube ? 6 : 1;

    for (int i = 0; i < x->count; i++) {
        x->color_format[i] = vkrt_format(rt->formats[i]);
        if (vkrt_image(x->color_format[i], width, height, x->levels, layers, 1,
                       VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                       VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                       x->cube, &x->color_image[i], &x->color_alloc[i]) != 0) goto fail;
        x->color_view[i] = vkrt_view(x->color_image[i], x->color_format[i], VK_IMAGE_VIEW_TYPE_2D, VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1);
        if (!x->color_view[i]) goto fail;
        if (x->samples > 1) {
            if (vkrt_image(x->color_format[i], width, height, 1, 1, x->samples, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
                           0, &x->msaa_image[i], &x->msaa_alloc[i]) != 0) goto fail;
            x->msaa_view[i] = vkrt_view(x->msaa_image[i], x->color_format[i], VK_IMAGE_VIEW_TYPE_2D, VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1);
            if (!x->msaa_view[i]) goto fail;
        }
    }
    if (vkrt_image(df, width, height, 1, 1, x->samples,
                   VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT |
                   (depth_only ? (VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT) : 0),
                   0, &x->depth_image, &x->depth_alloc) != 0) goto fail;
    x->depth_view = vkrt_view(x->depth_image, df, VK_IMAGE_VIEW_TYPE_2D, VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1);
    if (!x->depth_view) goto fail;

    x->pass = vkrt_pass(x, 1);
    if (!x->pass) goto fail;
    if (x->cube) {
        x->pass_nodepth = vkrt_pass(x, 0);
        int n = 6 * x->levels;
        x->face_fb = (VkFramebuffer *)calloc((size_t)n, sizeof(VkFramebuffer));
        x->face_view = (VkImageView *)calloc((size_t)n, sizeof(VkImageView));
        if (!x->pass_nodepth || !x->face_fb || !x->face_view) goto fail;
        for (int f = 0; f < 6; f++) {
            for (int l = 0; l < x->levels; l++) {
                int idx = f * x->levels + l;
                uint32_t lw = (uint32_t)(width >> l) > 0 ? (uint32_t)(width >> l) : 1u;
                uint32_t lh = (uint32_t)(height >> l) > 0 ? (uint32_t)(height >> l) : 1u;
                x->face_view[idx] = vkrt_view(x->color_image[0], x->color_format[0], VK_IMAGE_VIEW_TYPE_2D, VK_IMAGE_ASPECT_COLOR_BIT, (uint32_t)l, 1, (uint32_t)f, 1);
                if (!x->face_view[idx]) goto fail;
                VkImageView views[2] = { x->face_view[idx], x->depth_view };
                x->face_fb[idx] = l == 0 ? vkrt_framebuffer(x->pass, views, 2, lw, lh)
                                         : vkrt_framebuffer(x->pass_nodepth, views, 1, lw, lh);
                if (!x->face_fb[idx]) goto fail;
            }
        }
        x->cube_view = vkrt_view(x->color_image[0], x->color_format[0], VK_IMAGE_VIEW_TYPE_CUBE, VK_IMAGE_ASPECT_COLOR_BIT, 0, (uint32_t)x->levels, 0, 6);
        if (!x->cube_view) goto fail;
    } else {
        VkImageView views[9];
        uint32_t n = 0;
        for (int i = 0; i < x->count; i++) views[n++] = x->samples > 1 ? x->msaa_view[i] : x->color_view[i];
        views[n++] = x->depth_view;
        if (x->samples > 1) for (int i = 0; i < x->count; i++) views[n++] = x->color_view[i];
        x->fb = vkrt_framebuffer(x->pass, views, n, (uint32_t)width, (uint32_t)height);
        if (!x->fb) goto fail;
    }

    {
        VkSamplerCreateInfo sci = {0};
        sci.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
        if (depth_only) {
            sci.magFilter = sci.minFilter = VK_FILTER_NEAREST;
            sci.addressModeU = sci.addressModeV = sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
            sci.borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE;
        } else {
            sci.magFilter = sci.minFilter = VK_FILTER_LINEAR;
            sci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
            sci.addressModeU = sci.addressModeV = sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
            sci.maxLod = (float)x->levels;
        }
        if (vkCreateSampler(vio_vk.device, &sci, NULL, &x->sampler) != VK_SUCCESS) goto fail;
    }

    /* Defined initial contents in the steady (samplable) layouts. */
    {
        VkCommandBuffer cmd = VK_NULL_HANDLE;
        if (vio_vk_begin_transient(&cmd) == 0) {
            for (int i = 0; i < x->count; i++) {
                VkImageSubresourceRange r = { VK_IMAGE_ASPECT_COLOR_BIT, 0, (uint32_t)x->levels, 0, (uint32_t)layers };
                VkClearColorValue cv = {{ 0.0f, 0.0f, 0.0f, 0.0f }};
                vio_vk_image_barrier_range(cmd, x->color_image[i], VK_IMAGE_ASPECT_COLOR_BIT, 0, (uint32_t)x->levels, 0, (uint32_t)layers,
                                           VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
                vkCmdClearColorImage(cmd, x->color_image[i], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &cv, 1, &r);
                vio_vk_image_barrier_range(cmd, x->color_image[i], VK_IMAGE_ASPECT_COLOR_BIT, 0, (uint32_t)x->levels, 0, (uint32_t)layers,
                                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
            }
            if (depth_only) {
                VkImageAspectFlags da = vkrt_depth_aspect();
                VkImageSubresourceRange r = { da, 0, 1, 0, 1 };
                VkClearDepthStencilValue dv = { 1.0f, 0 };
                vio_vk_image_barrier_range(cmd, x->depth_image, da, 0, 1, 0, 1, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
                vkCmdClearDepthStencilImage(cmd, x->depth_image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &dv, 1, &r);
                vio_vk_image_barrier_range(cmd, x->depth_image, da, 0, 1, 0, 1, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL);
            }
            vio_vk_submit_transient(cmd);
        }
    }

    rt->vulkan_rt    = x;
    rt->bound_face   = -1;
    rt->bound_level  = 0;
    rt->backend_type = VIO_RT_BACKEND_VULKAN;
    vulkan_rt_track(rt);
    return 0;

fail:
    php_error_docref(NULL, E_WARNING, "Vulkan: render target creation failed (%dx%d, %d attachment(s), %d sample(s)%s)",
                     width, height, x->count, x->samples, x->cube ? ", cube" : "");
    vkrt_free(x);
    return -1;
}

void vulkan_destroy_render_target(void *rt_ptr)
{
    vio_render_target_object *rt = (vio_render_target_object *)rt_ptr;
    if (!rt || rt->backend_type != VIO_RT_BACKEND_VULKAN) return;
    vulkan_rt_untrack(rt);
    if (vio_vk.current_bound_rt == rt) vio_vk.current_bound_rt = NULL;
    if (vio_vk.pending_bound_rt == rt) vio_vk.pending_bound_rt = NULL;
    vio_vk_rt *x = (vio_vk_rt *)rt->vulkan_rt;
    rt->vulkan_rt = NULL;
    if (!x) return;
    if (vio_vk.device && !vio_vk.in_frame) vkDeviceWaitIdle(vio_vk.device);
    vkrt_free(x);
}

/* ── Bind / unbind / clear ─────────────────────────────────────────── */

static void vkrt_begin(VkCommandBuffer cmd, vio_render_target_object *rt, int face, int level)
{
    vio_vk_rt *x = (vio_vk_rt *)rt->vulkan_rt;
    int l = x->cube ? level : 0;
    int f = x->cube ? face : 0;
    uint32_t w = (uint32_t)(rt->width >> l) > 0 ? (uint32_t)(rt->width >> l) : 1u;
    uint32_t h = (uint32_t)(rt->height >> l) > 0 ? (uint32_t)(rt->height >> l) : 1u;
    int has_depth = !(x->cube && l > 0);
    VkClearValue clears[5];
    memset(clears, 0, sizeof(clears));
    for (int i = 0; i < x->count; i++) {
        clears[i].color.float32[0] = vio_vk.clear_r;
        clears[i].color.float32[1] = vio_vk.clear_g;
        clears[i].color.float32[2] = vio_vk.clear_b;
        clears[i].color.float32[3] = vio_vk.clear_a;
    }
    clears[x->count].depthStencil.depth = 1.0f;
    clears[x->count].depthStencil.stencil = 0;

    VkRenderPassBeginInfo rp = {0};
    rp.sType             = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    rp.renderPass        = has_depth ? x->pass : x->pass_nodepth;
    rp.framebuffer       = x->cube ? x->face_fb[f * x->levels + l] : x->fb;
    rp.renderArea.extent.width  = w;
    rp.renderArea.extent.height = h;
    rp.clearValueCount   = (uint32_t)x->count + (has_depth ? 1u : 0u);
    rp.pClearValues      = clears;
    vkCmdBeginRenderPass(cmd, &rp, VK_SUBPASS_CONTENTS_INLINE);
    VkViewport vp = { 0.0f, 0.0f, (float)w, (float)h, 0.0f, 1.0f };
    vkCmdSetViewport(cmd, 0, 1, &vp);
    VkRect2D sc = { { 0, 0 }, { w, h } };
    vkCmdSetScissor(cmd, 0, 1, &sc);

    vio_vk.current_bound_rt = rt;
    vio_vk.cur_render_pass  = rp.renderPass;
    vio_vk.cur_color_count  = x->count;
    for (int i = 0; i < x->count && i < 4; i++) vio_vk.cur_color_formats[i] = x->color_format[i];
    vio_vk.cur_samples      = x->samples;
    vio_vk.cur_has_depth    = has_depth;
    vio_vk.cur_width        = w;
    vio_vk.cur_height       = h;
    rt->bound_face  = x->cube ? f : -1;
    rt->bound_level = l;
}

void vulkan_record_bind_render_target(void *rt_ptr)
{
    vio_render_target_object *rt = (vio_render_target_object *)rt_ptr;
    if (!rt || rt->backend_type != VIO_RT_BACKEND_VULKAN || !rt->vulkan_rt || !vio_vk.in_frame) return;
    VkCommandBuffer cmd = vio_vk.frames[vio_vk.current_frame].cmd_buf;
    if (vio_vk.cur_render_pass) vkCmdEndRenderPass(cmd);
    vio_vk.cur_render_pass = VK_NULL_HANDLE;
    vkrt_begin(cmd, rt, 0, 0);
}

void vulkan_begin_offscreen_render_pass(void *rt_ptr)
{
    vio_render_target_object *rt = (vio_render_target_object *)rt_ptr;
    if (!rt || rt->backend_type != VIO_RT_BACKEND_VULKAN || !rt->vulkan_rt || !vio_vk.in_frame) return;
    vkrt_begin(vio_vk.frames[vio_vk.current_frame].cmd_buf, rt, 0, 0);
}

int vio_vk_bind_render_target_face(void *rt_ptr, int face, int level)
{
    vio_render_target_object *rt = (vio_render_target_object *)rt_ptr;
    vio_vk_rt *x = rt ? (vio_vk_rt *)rt->vulkan_rt : NULL;
    if (!x || !x->cube || face < 0 || face > 5 || level < 0 || level >= x->levels) return -1;
    if (!vio_vk.in_frame) {
        php_error_docref(NULL, E_WARNING, "Vulkan: cube face binds are only valid between vio_begin and vio_end");
        return -1;
    }
    VkCommandBuffer cmd = vio_vk.frames[vio_vk.current_frame].cmd_buf;
    if (vio_vk.cur_render_pass) vkCmdEndRenderPass(cmd);
    vio_vk.cur_render_pass = VK_NULL_HANDLE;
    vkrt_begin(cmd, rt, face, level);
    return 0;
}

void vulkan_record_unbind_render_target(void)
{
    if (!vio_vk.in_frame) return;
    VkCommandBuffer cmd = vio_vk.frames[vio_vk.current_frame].cmd_buf;
    if (vio_vk.cur_render_pass) vkCmdEndRenderPass(cmd);   /* MSAA resolves here */
    vio_vk.cur_render_pass = VK_NULL_HANDLE;
    vio_vk.current_bound_rt = NULL;
    if (vio_vk.frame_is_offscreen) return;   /* no swapchain image in an offscreen-only frame */
    vio_vk_resume_swapchain_pass(cmd);
}

/* vio_clear inside a frame clears the attachments of the open pass (D3D12 semantics). */
void vio_vk_clear_attachments(float r, float g, float b, float a)
{
    if (!vio_vk.in_frame || !vio_vk.cur_render_pass) return;
    VkClearAttachment att[5];
    uint32_t n = 0;
    memset(att, 0, sizeof(att));
    for (int i = 0; i < vio_vk.cur_color_count && i < 4; i++) {
        att[n].aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        att[n].colorAttachment = (uint32_t)i;
        att[n].clearValue.color.float32[0] = r;
        att[n].clearValue.color.float32[1] = g;
        att[n].clearValue.color.float32[2] = b;
        att[n].clearValue.color.float32[3] = a;
        n++;
    }
    if (vio_vk.cur_has_depth) {
        att[n].aspectMask = vkrt_depth_aspect();
        att[n].clearValue.depthStencil.depth = 1.0f;
        att[n].clearValue.depthStencil.stencil = 0;
        n++;
    }
    if (!n) return;
    VkClearRect rect = { { { 0, 0 }, { vio_vk.cur_width, vio_vk.cur_height } }, 0, 1 };
    vkCmdClearAttachments(vio_vk.frames[vio_vk.current_frame].cmd_buf, n, att, 1, &rect);
}

/* ── Sampling / cubemap view / readback ────────────────────────────── */

void *vulkan_rt_sampling_texture(void *rt_ptr, int attachment)
{
    vio_render_target_object *rt = (vio_render_target_object *)rt_ptr;
    vio_vk_rt *x = rt ? (vio_vk_rt *)rt->vulkan_rt : NULL;
    if (!x) return NULL;
    int i = rt->depth_only ? 0 : attachment;
    if (i < 0 || i >= (rt->depth_only ? 1 : x->count)) return NULL;
    if (x->wrap[i]) return x->wrap[i];
    vio_vulkan_texture *w = (vio_vulkan_texture *)calloc(1, sizeof(vio_vulkan_texture));
    if (!w) return NULL;
    w->image      = rt->depth_only ? x->depth_image : x->color_image[i];   /* borrowed */
    w->view       = rt->depth_only ? x->depth_view : x->color_view[i];     /* borrowed */
    w->sampler    = x->sampler;                                           /* borrowed */
    w->width      = rt->width;
    w->height     = rt->height;
    w->view_type  = VK_IMAGE_VIEW_TYPE_2D;
    w->is_depth   = rt->depth_only;
    w->layout     = rt->depth_only ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    w->mip_levels = 1;
    x->wrap[i] = w;
    return w;
}

int vio_vk_render_target_cubemap(void *rt_ptr, void *cm_obj)
{
    vio_render_target_object *rt = (vio_render_target_object *)rt_ptr;
    vio_cubemap_object *cm = (vio_cubemap_object *)cm_obj;
    vio_vk_rt *x = rt ? (vio_vk_rt *)rt->vulkan_rt : NULL;
    if (!x || !cm || !x->cube || !x->cube_view) return -1;
    if (!x->cube_wrap) {
        vio_vulkan_texture *w = (vio_vulkan_texture *)calloc(1, sizeof(vio_vulkan_texture));
        if (!w) return -1;
        w->image      = x->color_image[0];
        w->view       = x->cube_view;
        w->sampler    = x->sampler;
        w->width      = rt->width;
        w->height     = rt->height;
        w->view_type  = VK_IMAGE_VIEW_TYPE_CUBE;
        w->layout     = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        w->mip_levels = x->levels;
        x->cube_wrap = w;
    }
    cm->vulkan_texture = x->cube_wrap;
    cm->mipmaps        = x->levels > 1;
    cm->borrowed       = 1;
    cm->resolution     = rt->width;
    cm->backend_type   = 5;
    return 0;
}

int vio_vk_read_render_target(void *rt_ptr, int face, int attachment, void *out_rgba)
{
    vio_render_target_object *rt = (vio_render_target_object *)rt_ptr;
    vio_vk_rt *x = rt ? (vio_vk_rt *)rt->vulkan_rt : NULL;
    if (!x || !out_rgba || !vio_vk.device) return -1;
    if (vio_vk.in_frame) {
        php_error_docref(NULL, E_WARNING, "vio_read_render_target: call it after vio_end on Vulkan");
        return -1;
    }
    int depth = rt->depth_only;
    if (!depth && (attachment < 0 || attachment >= x->count)) return -1;
    uint32_t layer = 0;
    if (x->cube) {
        int f = face >= 0 ? face : (rt->bound_face >= 0 ? rt->bound_face : 0);
        layer = (uint32_t)(f > 5 ? 0 : f);
    }
    VkImage image = depth ? x->depth_image : x->color_image[attachment];
    int vfmt = depth ? VIO_FORMAT_R32F : rt->formats[attachment];
    int bpp = depth ? 4 : vio_rt_format_bpp(vfmt);
    uint32_t w = (uint32_t)rt->width, h = (uint32_t)rt->height;
    VkDeviceSize bytes = (VkDeviceSize)w * h * (VkDeviceSize)bpp;

    vkDeviceWaitIdle(vio_vk.device);
    VkBuffer staging = VK_NULL_HANDLE;
    void *alloc = NULL;
    if (vio_vma_create_buffer(vio_vk.vma_allocator, bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                              VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                              &staging, &alloc) != 0) {
        return -1;
    }
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    if (vio_vk_begin_transient(&cmd) != 0) {
        vio_vma_destroy_buffer(vio_vk.vma_allocator, staging, alloc);
        return -1;
    }
    VkImageAspectFlags full = depth ? vkrt_depth_aspect() : VK_IMAGE_ASPECT_COLOR_BIT;
    VkImageLayout rest = depth ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    vio_vk_image_barrier_range(cmd, image, full, 0, 1, layer, 1, rest, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    VkBufferImageCopy copy = {0};
    copy.imageSubresource.aspectMask     = depth ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT;
    copy.imageSubresource.baseArrayLayer = layer;
    copy.imageSubresource.layerCount     = 1;
    copy.imageExtent.width  = w;
    copy.imageExtent.height = h;
    copy.imageExtent.depth  = 1;
    vkCmdCopyImageToBuffer(cmd, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, staging, 1, &copy);
    vio_vk_image_barrier_range(cmd, image, full, 0, 1, layer, 1, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, rest);
    int rc = vio_vk_submit_transient(cmd);
    unsigned char *src = rc == 0 ? (unsigned char *)vio_vma_map(vio_vk.vma_allocator, alloc) : NULL;
    if (src) {
        unsigned char *out = (unsigned char *)out_rgba;
        if (depth) {
            VkFormat df = vio_vk_depth_format();
            for (size_t i = 0; i < (size_t)w * h; i++) {
                float d;
                if (df == VK_FORMAT_D24_UNORM_S8_UINT) {
                    uint32_t v;
                    memcpy(&v, src + i * 4, 4);
                    d = (float)(v & 0xFFFFFFu) / 16777215.0f;
                } else {
                    memcpy(&d, src + i * 4, 4);
                }
                unsigned char g = (unsigned char)(d <= 0.0f ? 0 : (d >= 1.0f ? 255 : (int)(d * 255.0f + 0.5f)));
                out[i * 4] = out[i * 4 + 1] = out[i * 4 + 2] = g;
                out[i * 4 + 3] = 255;
            }
        } else {
            int bgra = x->color_format[attachment] == VK_FORMAT_B8G8R8A8_UNORM;
            vio_rt_convert_to_rgba8(vfmt, bgra, src, (size_t)w * (size_t)bpp, (int)w, (int)h, out);
        }
        vio_vma_unmap(vio_vk.vma_allocator, alloc);
    }
    vio_vma_destroy_buffer(vio_vk.vma_allocator, staging, alloc);
    return src ? 0 : -1;
}

#endif /* HAVE_VULKAN */

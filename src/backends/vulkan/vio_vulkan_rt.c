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
 * Binding a target keeps its contents (loadOp LOAD for colour and depth, like
 * OpenGL / D3D); vio_clear is the only clear (vkCmdClearAttachments). Every
 * image is initialised at creation and stays in a steady layout between
 * passes: colour SHADER_READ_ONLY (an unbind needs no barrier before
 * sampling), multisampled colour COLOR_ATTACHMENT, depth DEPTH_STENCIL_ATTACHMENT
 * (depth_only targets: DEPTH_STENCIL_READ_ONLY, they are sampled).
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

/* Attachments: colour 0..count-1, depth, then (MSAA) the resolve targets.
 * views > 1: a multiview pass rendering views 0..views-1 (VK_KHR_multiview, core 1.1). */
static VkRenderPass vkrt_pass_views(const vio_vk_rt *x, int with_depth, int views)
{
    VkAttachmentDescription a[9];
    VkAttachmentReference cref[4], rref[4], dref;
    memset(a, 0, sizeof(a));
    uint32_t n = 0;
    int ms = x->samples > 1;
    for (int i = 0; i < x->count; i++) {
        a[n].format         = x->color_format[i];
        a[n].samples        = (VkSampleCountFlagBits)x->samples;
        a[n].loadOp         = VK_ATTACHMENT_LOAD_OP_LOAD;    /* a bind keeps what was drawn */
        a[n].storeOp        = VK_ATTACHMENT_STORE_OP_STORE;  /* MSAA too: the next bind loads it */
        a[n].stencilLoadOp  = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        a[n].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        a[n].initialLayout  = ms ? VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        a[n].finalLayout    = ms ? VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        cref[i].attachment  = n;
        cref[i].layout      = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        n++;
    }
    if (with_depth) {
        int depth_only = x->count == 0;
        a[n].format         = vio_vk_depth_format();
        a[n].samples        = (VkSampleCountFlagBits)x->samples;
        a[n].loadOp         = VK_ATTACHMENT_LOAD_OP_LOAD;
        a[n].storeOp        = VK_ATTACHMENT_STORE_OP_STORE;  /* depth survives an unbind / rebind */
        a[n].stencilLoadOp  = vio_vk.depth_has_stencil ? VK_ATTACHMENT_LOAD_OP_LOAD : VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        a[n].stencilStoreOp = vio_vk.depth_has_stencil ? VK_ATTACHMENT_STORE_OP_STORE : VK_ATTACHMENT_STORE_OP_DONT_CARE;
        a[n].initialLayout  = depth_only ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
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
    uint32_t view_mask = views > 1 ? (1u << views) - 1u : 0u;
    VkRenderPassMultiviewCreateInfo mv = {0};
    if (views > 1) {
        mv.sType                = VK_STRUCTURE_TYPE_RENDER_PASS_MULTIVIEW_CREATE_INFO;
        mv.subpassCount         = 1;
        mv.pViewMasks           = &view_mask;
        mv.correlationMaskCount = 1;
        mv.pCorrelationMasks    = &view_mask;
        ri.pNext                = &mv;
    }
    VkRenderPass rp = VK_NULL_HANDLE;
    if (vkCreateRenderPass(vio_vk.device, &ri, NULL, &rp) != VK_SUCCESS) return VK_NULL_HANDLE;
    return rp;
}

static VkRenderPass vkrt_pass(const vio_vk_rt *x, int with_depth)
{
    return vkrt_pass_views(x, with_depth, 0);
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
    vio_vk_note_viewport(&vp, &sc);
    vio_vk.cur_render_pass      = resume;
    vio_vk.cur_color_count      = 1;
    vio_vk.cur_color_formats[0] = vio_vk.swapchain_format;
    vio_vk.cur_samples          = 1;
    vio_vk.cur_has_depth        = 1;
    vio_vk.cur_width            = vio_vk.swapchain_extent.width;
    vio_vk.cur_height           = vio_vk.swapchain_extent.height;
    vio_vk.cur_layers           = 1;
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
        case VIO_VK_GRAVE_DESCRIPTOR_POOL: vkDestroyDescriptorPool(vio_vk.device, (VkDescriptorPool)h, NULL); break;
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
    int faces = (x->layers > 0 ? x->layers : 1) * (x->levels > 0 ? x->levels : 1);
    if (x->face_fb)   for (int i = 0; i < faces; i++) vkrt_kill(VIO_VK_GRAVE_FRAMEBUFFER, (uint64_t)x->face_fb[i], NULL);
    if (x->face_view) for (int i = 0; i < faces; i++) vkrt_kill(VIO_VK_GRAVE_VIEW, (uint64_t)x->face_view[i], NULL);
    if (x->depth_face_view) for (int i = 0; i < x->layers; i++) vkrt_kill(VIO_VK_GRAVE_VIEW, (uint64_t)x->depth_face_view[i], NULL);
    vkrt_kill(VIO_VK_GRAVE_FRAMEBUFFER, (uint64_t)x->fb, NULL);
    vkrt_kill(VIO_VK_GRAVE_FRAMEBUFFER, (uint64_t)x->all_fb, NULL);
    for (int v = 0; v < 3; v++) {
        vkrt_kill(VIO_VK_GRAVE_FRAMEBUFFER, (uint64_t)x->mv_fb[v], NULL);
        vkrt_kill(VIO_VK_GRAVE_RENDER_PASS, (uint64_t)x->mv_pass[v], NULL);
    }
    vkrt_kill(VIO_VK_GRAVE_VIEW, (uint64_t)x->all_color_view, NULL);
    if (x->msaa_face_view) for (int i = 0; i < x->layers; i++) vkrt_kill(VIO_VK_GRAVE_VIEW, (uint64_t)x->msaa_face_view[i], NULL);
    free(x->msaa_face_view);
    vkrt_kill(VIO_VK_GRAVE_VIEW, (uint64_t)x->msaa_all_view, NULL);
    vkrt_kill(VIO_VK_GRAVE_VIEW, (uint64_t)x->all_depth_view, NULL);
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
    for (int l = 1; l < x->depth_levels; l++) {
        if (x->dmip_fb)  vkrt_kill(VIO_VK_GRAVE_FRAMEBUFFER, (uint64_t)x->dmip_fb[l], NULL);
        if (x->dmip_att) vkrt_kill(VIO_VK_GRAVE_VIEW, (uint64_t)x->dmip_att[l], NULL);
        if (x->dmip_src) vkrt_kill(VIO_VK_GRAVE_VIEW, (uint64_t)x->dmip_src[l], NULL);
    }
    vkrt_kill(VIO_VK_GRAVE_DESCRIPTOR_POOL, (uint64_t)x->dmip_pool, NULL);
    free(x->dmip_fb); free(x->dmip_att); free(x->dmip_src); free(x->dmip_set);
    vkrt_kill(VIO_VK_GRAVE_VIEW, (uint64_t)x->depth_sample_view, NULL);
    vkrt_kill(VIO_VK_GRAVE_FRAMEBUFFER, (uint64_t)x->dres_fb, NULL);
    vkrt_kill(VIO_VK_GRAVE_DESCRIPTOR_POOL, (uint64_t)x->dres_pool, NULL);
    vkrt_kill(VIO_VK_GRAVE_VIEW, (uint64_t)x->msaa_depth_view, NULL);
    vkrt_kill(VIO_VK_GRAVE_IMAGE, (uint64_t)x->msaa_depth_image, x->msaa_depth_alloc);
    vkrt_kill(VIO_VK_GRAVE_VIEW, (uint64_t)x->depth_view, NULL);
    vkrt_kill(VIO_VK_GRAVE_IMAGE, (uint64_t)x->depth_image, x->depth_alloc);
    free(x->face_fb);
    free(x->face_view);
    free(x->depth_face_view);
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

static VkFramebuffer vkrt_framebuffer_layers(VkRenderPass pass, VkImageView *views, uint32_t n, uint32_t w, uint32_t h,
                                             uint32_t layers)
{
    VkFramebufferCreateInfo fi = {0};
    fi.sType           = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
    fi.renderPass      = pass;
    fi.attachmentCount = n;
    fi.pAttachments    = views;
    fi.width           = w;
    fi.height          = h;
    fi.layers          = layers ? layers : 1;
    VkFramebuffer fb = VK_NULL_HANDLE;
    if (vkCreateFramebuffer(vio_vk.device, &fi, NULL, &fb) != VK_SUCCESS) return VK_NULL_HANDLE;
    return fb;
}

static VkFramebuffer vkrt_framebuffer(VkRenderPass pass, VkImageView *views, uint32_t n, uint32_t w, uint32_t h)
{
    return vkrt_framebuffer_layers(pass, views, n, w, h, 1);
}

/* ── Depth mip chain (A26) ─────────────────────────────────────────────
 *
 * Each level is the max / min of the 2x2 texels below (odd sizes fold the
 * extra column / row into the last texel), written as gl_FragDepth by a
 * full-screen triangle. One depth-only render pass per level: the target level
 * goes READ_ONLY -> ATTACHMENT -> READ_ONLY inside the pass while the source
 * level stays READ_ONLY and is sampled through a single-level view. The
 * pipeline and pass are per device; the views, framebuffers and descriptor
 * sets per target (they never change, so in-flight frames can keep them). */
static const char *vk_dmip_vs =
    "#version 450\n"
    "void main() { vec2 p = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2); gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0); }\n";
static const char *vk_dmip_fs =
    "#version 450\n"
    "layout(set = 0, binding = 0) uniform sampler2D u_src;\n"
    "layout(push_constant) uniform P { ivec4 sizes; ivec4 mode; } pc;\n"
    "void main() {\n"
    "    ivec2 o = ivec2(gl_FragCoord.xy);\n"
    "    ivec2 n = ivec2((o.x == pc.sizes.z - 1 && (pc.sizes.x & 1) == 1 && pc.sizes.x > 1) ? 3 : 2,\n"
    "                    (o.y == pc.sizes.w - 1 && (pc.sizes.y & 1) == 1 && pc.sizes.y > 1) ? 3 : 2);\n"
    "    float d = pc.mode.x == 0 ? 0.0 : 1.0;\n"
    "    for (int y = 0; y < 3; y++) for (int x = 0; x < 3; x++) {\n"
    "        if (x >= n.x || y >= n.y) continue;\n"
    "        float s = texelFetch(u_src, min(o * 2 + ivec2(x, y), pc.sizes.xy - 1), 0).r;\n"
    "        d = pc.mode.x == 0 ? max(d, s) : min(d, s);\n"
    "    }\n"
    "    gl_FragDepth = d;\n"
    "}\n";

static const char *vk_dmip_resolve_fs =
    "#version 450\n"
    "layout(set = 0, binding = 0) uniform sampler2DMS u_src;\n"
    "layout(push_constant) uniform P { ivec4 sizes; ivec4 mode; } pc;\n"
    "void main() {\n"
    "    ivec2 p = ivec2(gl_FragCoord.xy);\n"
    "    float d = pc.mode.x == 0 ? 0.0 : 1.0;\n"
    "    for (int s = 0; s < pc.mode.y; s++) { float v = texelFetch(u_src, p, s).r; d = pc.mode.x == 0 ? max(d, v) : min(d, v); }\n"
    "    gl_FragDepth = d;\n"
    "}\n";

extern uint32_t *vio_compile_glsl_to_spirv(const char *source, int stage, size_t *out_size, char **error_msg);

static struct {
    VkDevice              device;
    VkRenderPass          pass;
    VkDescriptorSetLayout dsl;
    VkPipelineLayout      layout;
    VkPipeline            pipeline;
    VkPipeline            resolve_pipeline;   /* depth_only MSAA (A24) */
} vk_dmip;

void vio_vk_depth_mip_shutdown(void)
{
    if (!vk_dmip.device) return;
    if (vk_dmip.pipeline) vkDestroyPipeline(vk_dmip.device, vk_dmip.pipeline, NULL);
    if (vk_dmip.resolve_pipeline) vkDestroyPipeline(vk_dmip.device, vk_dmip.resolve_pipeline, NULL);
    if (vk_dmip.layout)   vkDestroyPipelineLayout(vk_dmip.device, vk_dmip.layout, NULL);
    if (vk_dmip.dsl)      vkDestroyDescriptorSetLayout(vk_dmip.device, vk_dmip.dsl, NULL);
    if (vk_dmip.pass)     vkDestroyRenderPass(vk_dmip.device, vk_dmip.pass, NULL);
    memset(&vk_dmip, 0, sizeof(vk_dmip));
}

static VkShaderModule vk_dmip_module(const char *src, int fragment)
{
    size_t size = 0;
    char *err = NULL;
    uint32_t *spv = vio_compile_glsl_to_spirv(src, fragment, &size, &err);
    free(err);
    if (!spv) return VK_NULL_HANDLE;
    VkShaderModuleCreateInfo ci = {0};
    ci.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    ci.codeSize = size;
    ci.pCode = spv;
    VkShaderModule m = VK_NULL_HANDLE;
    vkCreateShaderModule(vio_vk.device, &ci, NULL, &m);
    free(spv);
    return m;
}

/* A full-screen depth-writing pipeline on the depth-only pass and layout. */
static int vk_dmip_pipeline(const char *fs_src, VkPipeline *out)
{
    VkShaderModule vs = vk_dmip_module(vk_dmip_vs, 0), fs = vk_dmip_module(fs_src, 1);
    int ok = vs && fs;
    if (ok) {
        VkPipelineShaderStageCreateInfo st[2] = {0};
        st[0].sType = st[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        st[0].stage = VK_SHADER_STAGE_VERTEX_BIT;   st[0].module = vs; st[0].pName = "main";
        st[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT; st[1].module = fs; st[1].pName = "main";
        VkPipelineVertexInputStateCreateInfo vi = { VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
        VkPipelineInputAssemblyStateCreateInfo ia = { VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO };
        ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        VkPipelineViewportStateCreateInfo vp = { VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO };
        vp.viewportCount = 1;
        vp.scissorCount = 1;
        VkPipelineRasterizationStateCreateInfo rs = { VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO };
        rs.polygonMode = VK_POLYGON_MODE_FILL;
        rs.cullMode = VK_CULL_MODE_NONE;
        rs.lineWidth = 1.0f;
        VkPipelineMultisampleStateCreateInfo ms = { VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO };
        ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
        VkPipelineDepthStencilStateCreateInfo ds = { VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO };
        ds.depthTestEnable = VK_TRUE;
        ds.depthWriteEnable = VK_TRUE;
        ds.depthCompareOp = VK_COMPARE_OP_ALWAYS;
        VkPipelineColorBlendStateCreateInfo cb = { VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO };
        VkDynamicState dyn[2] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
        VkPipelineDynamicStateCreateInfo dy = { VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO };
        dy.dynamicStateCount = 2;
        dy.pDynamicStates = dyn;
        VkGraphicsPipelineCreateInfo pci = { VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO };
        pci.stageCount = 2;
        pci.pStages = st;
        pci.pVertexInputState = &vi;
        pci.pInputAssemblyState = &ia;
        pci.pViewportState = &vp;
        pci.pRasterizationState = &rs;
        pci.pMultisampleState = &ms;
        pci.pDepthStencilState = &ds;
        pci.pColorBlendState = &cb;
        pci.pDynamicState = &dy;
        pci.layout = vk_dmip.layout;
        pci.renderPass = vk_dmip.pass;
        ok = vkCreateGraphicsPipelines(vio_vk.device, VK_NULL_HANDLE, 1, &pci, NULL, out) == VK_SUCCESS;
    }
    if (vs) vkDestroyShaderModule(vio_vk.device, vs, NULL);
    if (fs) vkDestroyShaderModule(vio_vk.device, fs, NULL);
    return ok ? 0 : -1;
}

static int vk_dmip_ensure(void)
{
    if (vk_dmip.device == vio_vk.device && vk_dmip.pipeline) return 0;
    vio_vk_depth_mip_shutdown();
    vk_dmip.device = vio_vk.device;
    vio_vk_rt shape;
    memset(&shape, 0, sizeof(shape));
    shape.samples = 1;   /* depth-only, single-sample: compatible with every depth_only target pass */
    vk_dmip.pass = vkrt_pass_views(&shape, 1, 0);
    VkDescriptorSetLayoutBinding b = {0};
    b.binding = 0;
    b.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    b.descriptorCount = 1;
    b.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    VkDescriptorSetLayoutCreateInfo dci = {0};
    dci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    dci.bindingCount = 1;
    dci.pBindings = &b;
    VkPushConstantRange pr = { VK_SHADER_STAGE_FRAGMENT_BIT, 0, 32 };
    if (!vk_dmip.pass || vkCreateDescriptorSetLayout(vio_vk.device, &dci, NULL, &vk_dmip.dsl) != VK_SUCCESS) return -1;
    VkPipelineLayoutCreateInfo lci = {0};
    lci.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    lci.setLayoutCount = 1;
    lci.pSetLayouts = &vk_dmip.dsl;
    lci.pushConstantRangeCount = 1;
    lci.pPushConstantRanges = &pr;
    if (vkCreatePipelineLayout(vio_vk.device, &lci, NULL, &vk_dmip.layout) != VK_SUCCESS) return -1;

    if (vk_dmip_pipeline(vk_dmip_fs, &vk_dmip.pipeline) != 0) return -1;
    return vk_dmip_pipeline(vk_dmip_resolve_fs, &vk_dmip.resolve_pipeline);
}

/* Per-target views, framebuffers and descriptor sets, built once. */
static int vk_dmip_target(vio_render_target_object *rt, vio_vk_rt *x)
{
    if (x->dmip_fb) return 0;
    int n = x->depth_levels;
    VkFormat df = vio_vk_depth_format();
    x->dmip_att = (VkImageView *)calloc((size_t)n, sizeof(VkImageView));
    x->dmip_src = (VkImageView *)calloc((size_t)n, sizeof(VkImageView));
    x->dmip_fb  = (VkFramebuffer *)calloc((size_t)n, sizeof(VkFramebuffer));
    x->dmip_set = (VkDescriptorSet *)calloc((size_t)n, sizeof(VkDescriptorSet));
    if (!x->dmip_att || !x->dmip_src || !x->dmip_fb || !x->dmip_set) return -1;
    VkDescriptorPoolSize ps = { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, (uint32_t)n };
    VkDescriptorPoolCreateInfo pci = { VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
    pci.maxSets = (uint32_t)n;
    pci.poolSizeCount = 1;
    pci.pPoolSizes = &ps;
    if (vkCreateDescriptorPool(vio_vk.device, &pci, NULL, &x->dmip_pool) != VK_SUCCESS) return -1;
    for (int l = 1; l < n; l++) {
        uint32_t w = (uint32_t)(rt->width >> l), h = (uint32_t)(rt->height >> l);
        x->dmip_att[l] = vkrt_view(x->depth_image, df, VK_IMAGE_VIEW_TYPE_2D, VK_IMAGE_ASPECT_DEPTH_BIT, (uint32_t)l, 1, 0, 1);
        x->dmip_src[l] = vkrt_view(x->depth_image, df, VK_IMAGE_VIEW_TYPE_2D, VK_IMAGE_ASPECT_DEPTH_BIT, (uint32_t)(l - 1), 1, 0, 1);
        if (!x->dmip_att[l] || !x->dmip_src[l]) return -1;
        x->dmip_fb[l] = vkrt_framebuffer(vk_dmip.pass, &x->dmip_att[l], 1, w ? w : 1, h ? h : 1);
        if (!x->dmip_fb[l]) return -1;
        VkDescriptorSetAllocateInfo ai = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
        ai.descriptorPool = x->dmip_pool;
        ai.descriptorSetCount = 1;
        ai.pSetLayouts = &vk_dmip.dsl;
        if (vkAllocateDescriptorSets(vio_vk.device, &ai, &x->dmip_set[l]) != VK_SUCCESS) return -1;
        VkDescriptorImageInfo ii = { x->sampler, x->dmip_src[l], VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL };
        VkWriteDescriptorSet wd = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
        wd.dstSet = x->dmip_set[l];
        wd.descriptorCount = 1;
        wd.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        wd.pImageInfo = &ii;
        vkUpdateDescriptorSets(vio_vk.device, 1, &wd, 0, NULL);
    }
    return 0;
}

static void vk_dmip_record(VkCommandBuffer cmd, vio_render_target_object *rt, vio_vk_rt *x)
{
    VkImageAspectFlags da = vkrt_depth_aspect();
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, vk_dmip.pipeline);
    for (int l = 1; l < x->depth_levels; l++) {
        int sw = rt->width >> (l - 1), sh = rt->height >> (l - 1), dw = rt->width >> l, dh = rt->height >> l;
        if (sw < 1) sw = 1;
        if (sh < 1) sh = 1;
        if (dw < 1) dw = 1;
        if (dh < 1) dh = 1;
        /* The level below was written (by the user's pass or the previous step):
         * make it visible to the fragment shader. */
        VkImageMemoryBarrier b = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
        b.srcAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
        b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        b.oldLayout = b.newLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
        b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image = x->depth_image;
        b.subresourceRange.aspectMask = da;
        b.subresourceRange.baseMipLevel = (uint32_t)(l - 1);
        b.subresourceRange.levelCount = 1;
        b.subresourceRange.layerCount = 1;
        vio_vk_pipeline_barrier(cmd, VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
                             VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, NULL, 0, NULL, 1, &b);
        VkRenderPassBeginInfo rb = { VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO };
        rb.renderPass = vk_dmip.pass;
        rb.framebuffer = x->dmip_fb[l];
        rb.renderArea.extent.width = (uint32_t)dw;
        rb.renderArea.extent.height = (uint32_t)dh;
        vkCmdBeginRenderPass(cmd, &rb, VK_SUBPASS_CONTENTS_INLINE);
        VkViewport vp = { 0.0f, 0.0f, (float)dw, (float)dh, 0.0f, 1.0f };
        VkRect2D sc = { { 0, 0 }, { (uint32_t)dw, (uint32_t)dh } };
        vkCmdSetViewport(cmd, 0, 1, &vp);
        vkCmdSetScissor(cmd, 0, 1, &sc);
        int32_t pc[8] = { sw, sh, dw, dh, rt->depth_reduction == VIO_DEPTH_REDUCE_MIN ? 1 : 0, 0, 0, 0 };
        vkCmdPushConstants(cmd, vk_dmip.layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(pc), pc);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, vk_dmip.layout, 0, 1, &x->dmip_set[l], 0, NULL);
        vkCmdDraw(cmd, 3, 1, 0, 0);
        vkCmdEndRenderPass(cmd);
    }
    /* The last level, for whoever samples next. */
    VkImageMemoryBarrier b = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
    b.srcAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    b.oldLayout = b.newLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = x->depth_image;
    b.subresourceRange.aspectMask = da;
    b.subresourceRange.levelCount = (uint32_t)x->depth_levels;
    b.subresourceRange.layerCount = 1;
    vio_vk_pipeline_barrier(cmd, VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
                         VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, NULL, 0, NULL, 1, &b);
}

int vio_vk_generate_depth_mips(void *rt_obj)
{
    vio_render_target_object *rt = (vio_render_target_object *)rt_obj;
    vio_vk_rt *x = rt ? (vio_vk_rt *)rt->vulkan_rt : NULL;
    if (!x || x->count != 0 || x->depth_levels < 2) return -1;
    if (vk_dmip_ensure() != 0 || vk_dmip_target(rt, x) != 0) return -1;
    if (!vio_vk.in_frame) {
        VkCommandBuffer cmd = VK_NULL_HANDLE;
        if (vio_vk_begin_transient(&cmd) != 0) return -1;
        vk_dmip_record(cmd, rt, x);
        return vio_vk_submit_transient(cmd);
    }
    if (vio_vk.current_bound_rt) {
        php_error_docref(NULL, E_WARNING, "vio_generate_mipmaps: unbind the render target first (Vulkan)");
        return -1;
    }
    /* After the frame's draws into the target, on the frame command buffer. */
    VkCommandBuffer cmd = vio_vk.frames[vio_vk.current_frame].cmd_buf;
    int had_pass = vio_vk.cur_render_pass != VK_NULL_HANDLE;
    if (had_pass) {
        vkCmdEndRenderPass(cmd);
        vio_vk.cur_render_pass = VK_NULL_HANDLE;
    }
    vk_dmip_record(cmd, rt, x);
    if (had_pass && !vio_vk.frame_is_offscreen) vio_vk_resume_swapchain_pass(cmd);
    return 0;
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
    x->layers  = vio_rt_layer_count(rt);
    int layered = x->layers > 1;
    x->levels  = (x->cube && rt->mip_levels > 1 && !depth_only) ? rt->mip_levels : 1;
    /* Cube / array targets multisample too (A24): an MS colour array, the depth
     * array multisampled, resolve attachments into level 0 of each face. */
    int want_ms = rt->samples;   /* depth_only MSAA below (A24) */
    x->samples = depth_only ? 1 : vkrt_supported_samples(rt->samples);
    rt->samples = x->samples;
    VkFormat df = vio_vk_depth_format();
    int layers = x->layers;
    VkImageViewType all_view = x->cube ? VK_IMAGE_VIEW_TYPE_CUBE : VK_IMAGE_VIEW_TYPE_2D_ARRAY;

    for (int i = 0; i < x->count; i++) {
        x->color_format[i] = vkrt_format(rt->formats[i]);
        if (vkrt_image(x->color_format[i], width, height, x->levels, layers, 1,
                       VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                       VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                       x->cube, &x->color_image[i], &x->color_alloc[i]) != 0) goto fail;
        x->color_view[i] = vkrt_view(x->color_image[i], x->color_format[i], VK_IMAGE_VIEW_TYPE_2D, VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1);
        if (!x->color_view[i]) goto fail;
        if (x->samples > 1) {
            if (vkrt_image(x->color_format[i], width, height, 1, layered ? layers : 1, x->samples,
                           VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                           0, &x->msaa_image[i], &x->msaa_alloc[i]) != 0) goto fail;
            x->msaa_view[i] = vkrt_view(x->msaa_image[i], x->color_format[i], VK_IMAGE_VIEW_TYPE_2D, VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1);
            if (!x->msaa_view[i]) goto fail;
        }
    }
    /* Depth carries the target's layer structure (a depth cube / depth array),
     * so every layer has its own depth and depth_only targets sample as
     * samplerCube / sampler2DArray. */
    x->depth_levels = (depth_only && !layered && rt->mip_levels > 1) ? rt->mip_levels : 1;
    /* depth_only MSAA (A24): the sampled depth stays single-sample. */
    int depth_ms = (depth_only && !layered && x->depth_levels == 1 && want_ms > 1) ? vkrt_supported_samples(want_ms) : 1;
    if (vkrt_image(df, width, height, x->depth_levels, layers, x->samples,
                   VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                   (depth_only ? (VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT) : 0),
                   x->cube && x->samples == 1, &x->depth_image, &x->depth_alloc) != 0) goto fail;   /* MS images cannot be cube-compatible */
    /* depth_view: the 2D attachment / sampling view, or for a layered
     * depth_only target the whole-image CUBE / 2D_ARRAY sampling view. */
    x->depth_view = (layered && depth_only)
        ? vkrt_view(x->depth_image, df, all_view, VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, (uint32_t)layers)
        : vkrt_view(x->depth_image, df, VK_IMAGE_VIEW_TYPE_2D, VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1);
    if (!x->depth_view) goto fail;
    if (depth_ms > 1) {
        if (vkrt_image(df, width, height, 1, 1, depth_ms,
                       VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                       0, &x->msaa_depth_image, &x->msaa_depth_alloc) != 0) goto fail;
        x->msaa_depth_view = vkrt_view(x->msaa_depth_image, df, VK_IMAGE_VIEW_TYPE_2D, VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1);
        if (!x->msaa_depth_view) goto fail;
        x->samples = depth_ms;   /* the pass and the pipelines render multisampled */
        rt->samples = depth_ms;
    }
    if (x->depth_levels > 1) {
        x->depth_sample_view = vkrt_view(x->depth_image, df, VK_IMAGE_VIEW_TYPE_2D, VK_IMAGE_ASPECT_DEPTH_BIT, 0, (uint32_t)x->depth_levels, 0, 1);
        if (!x->depth_sample_view) goto fail;
    }

    x->pass = vkrt_pass(x, 1);
    if (!x->pass) goto fail;
    if (layered) {
        /* Levels > 0 render single-sampled (the MS array has level 0 only). */
        if (x->samples > 1) {
            vio_vk_rt one = *x;
            one.samples = 1;
            x->pass_nodepth = vkrt_pass(&one, 0);
            x->msaa_face_view = (VkImageView *)calloc((size_t)layers, sizeof(VkImageView));
            if (!x->msaa_face_view) goto fail;
            for (int f = 0; f < layers; f++) {
                x->msaa_face_view[f] = vkrt_view(x->msaa_image[0], x->color_format[0], VK_IMAGE_VIEW_TYPE_2D, VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, (uint32_t)f, 1);
                if (!x->msaa_face_view[f]) goto fail;
            }
            x->msaa_all_view = vkrt_view(x->msaa_image[0], x->color_format[0], VK_IMAGE_VIEW_TYPE_2D_ARRAY, VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, (uint32_t)layers);
            if (!x->msaa_all_view) goto fail;
        } else {
            x->pass_nodepth = vkrt_pass(x, 0);
        }
        int n = layers * x->levels;
        x->face_fb = (VkFramebuffer *)calloc((size_t)n, sizeof(VkFramebuffer));
        x->face_view = (VkImageView *)calloc((size_t)n, sizeof(VkImageView));
        x->depth_face_view = (VkImageView *)calloc((size_t)layers, sizeof(VkImageView));
        if (!x->pass_nodepth || !x->face_fb || !x->face_view || !x->depth_face_view) goto fail;
        for (int f = 0; f < layers; f++) {
            x->depth_face_view[f] = vkrt_view(x->depth_image, df, VK_IMAGE_VIEW_TYPE_2D, VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, (uint32_t)f, 1);
            if (!x->depth_face_view[f]) goto fail;
            for (int l = 0; l < x->levels; l++) {
                int idx = f * x->levels + l;
                uint32_t lw = (uint32_t)(width >> l) > 0 ? (uint32_t)(width >> l) : 1u;
                uint32_t lh = (uint32_t)(height >> l) > 0 ? (uint32_t)(height >> l) : 1u;
                VkImageView views[3];
                uint32_t nv = 0;
                if (x->count) {
                    x->face_view[idx] = vkrt_view(x->color_image[0], x->color_format[0], VK_IMAGE_VIEW_TYPE_2D, VK_IMAGE_ASPECT_COLOR_BIT, (uint32_t)l, 1, (uint32_t)f, 1);
                    if (!x->face_view[idx]) goto fail;
                    /* MSAA level 0: MS layer, depth layer, then the face as resolve target. */
                    views[nv++] = (x->samples > 1 && l == 0) ? x->msaa_face_view[f] : x->face_view[idx];
                }
                if (l == 0) views[nv++] = x->depth_face_view[f];
                if (x->samples > 1 && l == 0 && x->count) views[nv++] = x->face_view[idx];
                x->face_fb[idx] = vkrt_framebuffer(l == 0 ? x->pass : x->pass_nodepth, views, nv, lw, lh);
                if (!x->face_fb[idx]) goto fail;
            }
        }
        if (x->count) {
            x->cube_view = vkrt_view(x->color_image[0], x->color_format[0], all_view, VK_IMAGE_ASPECT_COLOR_BIT, 0, (uint32_t)x->levels, 0, (uint32_t)layers);
            if (!x->cube_view) goto fail;
        }
        /* Layered bind (VIO_RT_ALL_LAYERS): 2D_ARRAY attachment views over every
         * layer at level 0 and a framebuffer with layers = N; gl_Layer picks the
         * destination. */
        {
            VkImageView views[3];
            uint32_t nv = 0;
            if (x->count) {
                x->all_color_view = vkrt_view(x->color_image[0], x->color_format[0], VK_IMAGE_VIEW_TYPE_2D_ARRAY, VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, (uint32_t)layers);
                if (!x->all_color_view) goto fail;
                views[nv++] = x->samples > 1 ? x->msaa_all_view : x->all_color_view;
            }
            x->all_depth_view = vkrt_view(x->depth_image, df, VK_IMAGE_VIEW_TYPE_2D_ARRAY, VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, (uint32_t)layers);
            if (!x->all_depth_view) goto fail;
            views[nv++] = x->all_depth_view;
            if (x->samples > 1 && x->count) views[nv++] = x->all_color_view;
            x->all_fb = vkrt_framebuffer_layers(x->pass, views, nv, (uint32_t)width, (uint32_t)height, (uint32_t)layers);
            if (!x->all_fb) goto fail;
        }
    } else {
        VkImageView views[9];
        uint32_t n = 0;
        for (int i = 0; i < x->count; i++) views[n++] = x->samples > 1 ? x->msaa_view[i] : x->color_view[i];
        views[n++] = x->msaa_depth_view ? x->msaa_depth_view : x->depth_view;
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
            sci.maxLod = (float)x->depth_levels;   /* textureLod over a depth chain (A26) */
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
            for (int i = 0; i < x->count && x->samples > 1; i++) {
                uint32_t ml = layered ? (uint32_t)layers : 1u;
                VkImageSubresourceRange r = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, ml };
                VkClearColorValue cv = {{ 0.0f, 0.0f, 0.0f, 0.0f }};
                vio_vk_image_barrier_range(cmd, x->msaa_image[i], VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, ml,
                                           VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
                vkCmdClearColorImage(cmd, x->msaa_image[i], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &cv, 1, &r);
                vio_vk_image_barrier_range(cmd, x->msaa_image[i], VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, ml,
                                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
            }
            {
                /* Depth 1.0 / stencil 0, so "bind + draw without clear" depth-tests
                 * (the GL / Metal / D3D initial contents). */
                VkImageAspectFlags da = vkrt_depth_aspect();
                VkImageSubresourceRange r = { da, 0, (uint32_t)x->depth_levels, 0, (uint32_t)layers };
                VkClearDepthStencilValue dv = { 1.0f, 0 };
                VkImageLayout steady = depth_only ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL
                                                  : VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
                vio_vk_image_barrier_range(cmd, x->depth_image, da, 0, (uint32_t)x->depth_levels, 0, (uint32_t)layers, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
                vkCmdClearDepthStencilImage(cmd, x->depth_image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &dv, 1, &r);
                vio_vk_image_barrier_range(cmd, x->depth_image, da, 0, (uint32_t)x->depth_levels, 0, (uint32_t)layers, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, steady);
                if (x->msaa_depth_image) {
                    VkImageSubresourceRange mr = { da, 0, 1, 0, 1 };
                    vio_vk_image_barrier_range(cmd, x->msaa_depth_image, da, 0, 1, 0, 1, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
                    vkCmdClearDepthStencilImage(cmd, x->msaa_depth_image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &dv, 1, &mr);
                    vio_vk_image_barrier_range(cmd, x->msaa_depth_image, da, 0, 1, 0, 1, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, steady);
                }
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
    php_error_docref(NULL, E_WARNING, "Vulkan: render target creation failed (%dx%d, %d attachment(s), %d sample(s), %d layer(s)%s)",
                     width, height, x->count, x->samples, x->layers, x->cube ? ", cube" : "");
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
    int layered = x->layers > 1;
    int all = layered && face == VIO_RT_ALL_LAYERS;
    int l = (layered && !all) ? level : 0;
    int f = (layered && !all) ? face : 0;
    uint32_t w = (uint32_t)(rt->width >> l) > 0 ? (uint32_t)(rt->width >> l) : 1u;
    uint32_t h = (uint32_t)(rt->height >> l) > 0 ? (uint32_t)(rt->height >> l) : 1u;
    int has_depth = !(layered && l > 0);
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
    rp.framebuffer       = all ? x->all_fb : (layered ? x->face_fb[f * x->levels + l] : x->fb);
    rp.renderArea.extent.width  = w;
    rp.renderArea.extent.height = h;
    rp.clearValueCount   = (uint32_t)x->count + (has_depth ? 1u : 0u);
    rp.pClearValues      = clears;
    vkCmdBeginRenderPass(cmd, &rp, VK_SUBPASS_CONTENTS_INLINE);
    VkViewport vp = { 0.0f, 0.0f, (float)w, (float)h, 0.0f, 1.0f };
    vkCmdSetViewport(cmd, 0, 1, &vp);
    VkRect2D sc = { { 0, 0 }, { w, h } };
    vkCmdSetScissor(cmd, 0, 1, &sc);
    vio_vk_note_viewport(&vp, &sc);

    vio_vk.current_bound_rt = rt;
    vio_vk.cur_render_pass  = rp.renderPass;
    vio_vk.cur_color_count  = x->count;
    for (int i = 0; i < x->count && i < 4; i++) vio_vk.cur_color_formats[i] = x->color_format[i];
    vio_vk.cur_samples      = has_depth ? x->samples : 1;   /* layered levels > 0 are single-sampled */
    vio_vk.cur_has_depth    = has_depth;
    vio_vk.cur_width        = w;
    vio_vk.cur_height       = h;
    vio_vk.cur_layers       = all ? (uint32_t)x->layers : 1u;   /* vio_clear covers every bound layer */
    rt->bound_face  = all ? VIO_RT_ALL_LAYERS : (layered ? f : -1);
    rt->bound_level = l;
}

/* depth_only MSAA (A24): reduce each texel's samples (max / min, the target's
 * depth_reduction) into the single-sample depth with a full-screen pass. */
static void vk_dres_record(VkCommandBuffer cmd, vio_render_target_object *rt, vio_vk_rt *x)
{
    if (vk_dmip_ensure() != 0) return;
    if (!x->dres_fb) {
        VkDescriptorPoolSize ps = { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1 };
        VkDescriptorPoolCreateInfo pci = { VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
        pci.maxSets = 1;
        pci.poolSizeCount = 1;
        pci.pPoolSizes = &ps;
        if (vkCreateDescriptorPool(vio_vk.device, &pci, NULL, &x->dres_pool) != VK_SUCCESS) return;
        VkDescriptorSetAllocateInfo ai = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
        ai.descriptorPool = x->dres_pool;
        ai.descriptorSetCount = 1;
        ai.pSetLayouts = &vk_dmip.dsl;
        if (vkAllocateDescriptorSets(vio_vk.device, &ai, &x->dres_set) != VK_SUCCESS) return;
        VkDescriptorImageInfo ii = { x->sampler, x->msaa_depth_view, VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL };
        VkWriteDescriptorSet wd = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
        wd.dstSet = x->dres_set;
        wd.descriptorCount = 1;
        wd.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        wd.pImageInfo = &ii;
        vkUpdateDescriptorSets(vio_vk.device, 1, &wd, 0, NULL);
        x->dres_fb = vkrt_framebuffer(vk_dmip.pass, &x->depth_view, 1, (uint32_t)rt->width, (uint32_t)rt->height);
        if (!x->dres_fb) return;
    }
    VkImageAspectFlags da = vkrt_depth_aspect();
    VkImageMemoryBarrier b = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
    b.srcAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    b.oldLayout = b.newLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = x->msaa_depth_image;
    b.subresourceRange.aspectMask = da;
    b.subresourceRange.levelCount = 1;
    b.subresourceRange.layerCount = 1;
    vio_vk_pipeline_barrier(cmd, VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
                         VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, NULL, 0, NULL, 1, &b);
    VkRenderPassBeginInfo rb = { VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO };
    rb.renderPass = vk_dmip.pass;
    rb.framebuffer = x->dres_fb;
    rb.renderArea.extent.width = (uint32_t)rt->width;
    rb.renderArea.extent.height = (uint32_t)rt->height;
    vkCmdBeginRenderPass(cmd, &rb, VK_SUBPASS_CONTENTS_INLINE);
    VkViewport vp = { 0.0f, 0.0f, (float)rt->width, (float)rt->height, 0.0f, 1.0f };
    VkRect2D sc = { { 0, 0 }, { (uint32_t)rt->width, (uint32_t)rt->height } };
    vkCmdSetViewport(cmd, 0, 1, &vp);
    vkCmdSetScissor(cmd, 0, 1, &sc);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, vk_dmip.resolve_pipeline);
    int32_t pc[8] = { rt->width, rt->height, rt->width, rt->height, rt->depth_reduction == VIO_DEPTH_REDUCE_MIN ? 1 : 0, x->samples, 0, 0 };
    vkCmdPushConstants(cmd, vk_dmip.layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(pc), pc);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, vk_dmip.layout, 0, 1, &x->dres_set, 0, NULL);
    vkCmdDraw(cmd, 3, 1, 0, 0);
    vkCmdEndRenderPass(cmd);
    b.image = x->depth_image;
    vio_vk_pipeline_barrier(cmd, VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
                         VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, NULL, 0, NULL, 1, &b);
}

/* End the open pass; a multisampled depth_only target being left resolves. */
static void vkrt_leave(VkCommandBuffer cmd)
{
    if (vio_vk.cur_render_pass) vkCmdEndRenderPass(cmd);
    vio_vk.cur_render_pass = VK_NULL_HANDLE;
    vio_render_target_object *prev = (vio_render_target_object *)vio_vk.current_bound_rt;
    vio_vk_rt *px = prev ? (vio_vk_rt *)prev->vulkan_rt : NULL;
    if (px && px->msaa_depth_image) vk_dres_record(cmd, prev, px);
}

void vulkan_record_bind_render_target(void *rt_ptr)
{
    vio_render_target_object *rt = (vio_render_target_object *)rt_ptr;
    if (!rt || rt->backend_type != VIO_RT_BACKEND_VULKAN || !rt->vulkan_rt || !vio_vk.in_frame) return;
    VkCommandBuffer cmd = vio_vk.frames[vio_vk.current_frame].cmd_buf;
    vkrt_leave(cmd);
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
    if (!x || x->layers <= 1 || level < 0 || level >= x->levels) return -1;
    if (face == VIO_RT_ALL_LAYERS ? (level != 0 || !x->all_fb) : (face < 0 || face >= x->layers)) return -1;
    if (!vio_vk.in_frame) {
        php_error_docref(NULL, E_WARNING, "Vulkan: cube face / array layer binds are only valid between vio_begin and vio_end");
        return -1;
    }
    VkCommandBuffer cmd = vio_vk.frames[vio_vk.current_frame].cmd_buf;
    vkrt_leave(cmd);
    vkrt_begin(cmd, rt, face, level);
    return 0;
}

int vio_vk_rt_ensure_views(int views)
{
    vio_render_target_object *rt = vio_vk.current_bound_rt;
    vio_vk_rt *x = rt ? (vio_vk_rt *)rt->vulkan_rt : NULL;
    int in_mv = 0;
    if (x) for (int v = 0; v < 3; v++) if (x->mv_pass[v] && vio_vk.cur_render_pass == x->mv_pass[v]) in_mv = v + 2;
    if (views == in_mv) return 0;
    VkCommandBuffer cmd = vio_vk.frames[vio_vk.current_frame].cmd_buf;
    if (views <= 1) {
        /* Back to the plain layered pass (LOAD keeps what the views drew). */
        if (!in_mv) return 0;
        vkCmdEndRenderPass(cmd);
        vkrt_begin(cmd, rt, VIO_RT_ALL_LAYERS, 0);
        return 0;
    }
    if (!x || rt->bound_face != VIO_RT_ALL_LAYERS || x->layers < views || x->cube || !x->all_depth_view) return -1;
    int vi = views - 2;
    if (!x->mv_pass[vi]) {
        x->mv_pass[vi] = vkrt_pass_views(x, 1, views);
        if (!x->mv_pass[vi]) return -1;
        VkImageView att[2];
        uint32_t n = 0;
        if (x->count) att[n++] = x->all_color_view;
        att[n++] = x->all_depth_view;
        /* Multiview framebuffers have one layer; the view mask picks the slices. */
        x->mv_fb[vi] = vkrt_framebuffer_layers(x->mv_pass[vi], att, n, (uint32_t)rt->width, (uint32_t)rt->height, 1);
        if (!x->mv_fb[vi]) return -1;
    }
    vkCmdEndRenderPass(cmd);
    uint32_t w = (uint32_t)rt->width, h = (uint32_t)rt->height;
    VkRenderPassBeginInfo rp = {0};
    rp.sType             = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    rp.renderPass        = x->mv_pass[vi];
    rp.framebuffer       = x->mv_fb[vi];
    rp.renderArea.extent.width  = w;
    rp.renderArea.extent.height = h;
    vkCmdBeginRenderPass(cmd, &rp, VK_SUBPASS_CONTENTS_INLINE);
    VkViewport vp = { 0.0f, 0.0f, (float)w, (float)h, 0.0f, 1.0f };
    vkCmdSetViewport(cmd, 0, 1, &vp);
    VkRect2D sc = { { 0, 0 }, { w, h } };
    vkCmdSetScissor(cmd, 0, 1, &sc);
    vio_vk_note_viewport(&vp, &sc);
    vio_vk.cur_render_pass = x->mv_pass[vi];
    vio_vk.cur_layers      = 1u;   /* a clear in a multiview pass covers every view with layerCount 1 */
    return 0;
}

void vio_vk_resume_pass(VkCommandBuffer cmd)
{
    vio_render_target_object *rt = (vio_render_target_object *)vio_vk.current_bound_rt;
    if (rt && rt->vulkan_rt) {
        int face = rt->bound_face == VIO_RT_ALL_LAYERS ? VIO_RT_ALL_LAYERS : (rt->bound_face < 0 ? 0 : rt->bound_face);
        vkrt_begin(cmd, rt, face, rt->bound_level < 0 ? 0 : rt->bound_level);
        return;
    }
    if (!vio_vk.frame_is_offscreen) vio_vk_resume_swapchain_pass(cmd);
}

void vulkan_record_unbind_render_target(void)
{
    if (!vio_vk.in_frame) return;
    VkCommandBuffer cmd = vio_vk.frames[vio_vk.current_frame].cmd_buf;
    vkrt_leave(cmd);   /* colour MSAA resolves in the pass, depth_only MSAA here */
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
    VkClearRect rect = { { { 0, 0 }, { vio_vk.cur_width, vio_vk.cur_height } }, 0, vio_vk.cur_layers ? vio_vk.cur_layers : 1 };
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
    if (x->cube) return NULL;   /* cube targets sample through vio_render_target_cubemap */
    int array = x->layers > 1;
    vio_vulkan_texture *w = (vio_vulkan_texture *)calloc(1, sizeof(vio_vulkan_texture));
    if (!w) return NULL;
    w->image      = rt->depth_only ? x->depth_image : x->color_image[i];   /* borrowed */
    /* Arrays: the whole-image 2D_ARRAY view (depth_view / cube_view hold it). */
    w->view       = rt->depth_only ? (x->depth_sample_view ? x->depth_sample_view : x->depth_view) : (array ? x->cube_view : x->color_view[i]);   /* borrowed */
    w->sampler    = x->sampler;                                           /* borrowed */
    w->width      = rt->width;
    w->height     = rt->height;
    w->view_type  = array ? VK_IMAGE_VIEW_TYPE_2D_ARRAY : VK_IMAGE_VIEW_TYPE_2D;
    w->is_depth   = rt->depth_only;
    w->layout     = rt->depth_only ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    w->mip_levels = rt->depth_only ? x->depth_levels : 1;
    x->wrap[i] = w;
    return w;
}

int vio_vk_render_target_cubemap(void *rt_ptr, void *cm_obj)
{
    vio_render_target_object *rt = (vio_render_target_object *)rt_ptr;
    vio_cubemap_object *cm = (vio_cubemap_object *)cm_obj;
    vio_vk_rt *x = rt ? (vio_vk_rt *)rt->vulkan_rt : NULL;
    int depth = rt->depth_only;
    if (!x || !cm || !x->cube || !(depth ? x->depth_view : x->cube_view)) return -1;
    if (!x->cube_wrap) {
        vio_vulkan_texture *w = (vio_vulkan_texture *)calloc(1, sizeof(vio_vulkan_texture));
        if (!w) return -1;
        /* depth_only: the depth cube (samplerCube .r / samplerCubeShadow). */
        w->image      = depth ? x->depth_image : x->color_image[0];
        w->view       = depth ? x->depth_view : x->cube_view;
        w->sampler    = x->sampler;
        w->width      = rt->width;
        w->height     = rt->height;
        w->view_type  = VK_IMAGE_VIEW_TYPE_CUBE;
        w->is_depth   = depth;
        w->layout     = depth ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
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
    /* Inside a frame: submit what the frame recorded so far and wait for it
     * (the open pass ends, the target is back in its resting layout and the
     * pass is reopened with LOAD on the frame command buffer), then read with a
     * transient submission, which the queue runs before the rest of the frame.
     * Outside a frame: wait for the last submission (VULKAN-MODERN-PLAN). */
    if (vio_vk.in_frame) vio_vk_flush_frame();
    else vio_vk_wait_value(vio_vk.timeline_value);
    int depth = rt->depth_only;
    if (!depth && (attachment < 0 || attachment >= x->count)) return -1;
    uint32_t layer = 0;
    if (x->layers > 1) {
        int f = face >= 0 ? face : (rt->bound_face >= 0 ? rt->bound_face : 0);
        layer = (uint32_t)(f >= x->layers ? 0 : f);
    }
    VkImage image = depth ? x->depth_image : x->color_image[attachment];
    int vfmt = depth ? VIO_FORMAT_R32F : rt->formats[attachment];
    int bpp = depth ? 4 : vio_rt_format_bpp(vfmt);
    uint32_t w = (uint32_t)rt->width, h = (uint32_t)rt->height;
    VkDeviceSize bytes = (VkDeviceSize)w * h * (VkDeviceSize)bpp;

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

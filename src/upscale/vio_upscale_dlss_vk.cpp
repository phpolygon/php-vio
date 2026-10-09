/*
 * php-vio - DLSS provider, Vulkan part (TEMPORAL-S4)
 *
 * NGX takes image views. The backend hands colour over in SHADER_READ_ONLY
 * (GENERAL for 'storage' targets) and a sampled depth in SHADER_READ_ONLY; NGX
 * reads its inputs as sampled images and writes the output as a storage image
 * in GENERAL. A storage input is moved to SHADER_READ_ONLY around the
 * evaluation and back. The instance / device extensions NGX needs are named
 * through vk_extensions, which the backend reads before vkCreateInstance /
 * vkCreateDevice.
 */

#include "vio_upscale_dlss_internal.h"

#ifdef HAVE_VULKAN
#include <vulkan/vulkan.h>
#include <cstdio>
#include <cstring>
#include "nvsdk_ngx_vk.h"
#include "nvsdk_ngx_helpers_vk.h"

namespace {

NVSDK_NGX_Result vk_init(const vio_upscale_device *dev, const VioDlssInit *in)
{
    return NVSDK_NGX_VULKAN_Init_with_ProjectID(in->project_id, NVSDK_NGX_ENGINE_TYPE_CUSTOM, in->engine_version,
                                                in->data_path, static_cast<VkInstance>(dev->instance),
                                                static_cast<VkPhysicalDevice>(dev->physical_device),
                                                static_cast<VkDevice>(dev->device),
                                                reinterpret_cast<PFN_vkGetInstanceProcAddr>(dev->get_instance_proc_addr),
                                                reinterpret_cast<PFN_vkGetDeviceProcAddr>(dev->get_device_proc_addr),
                                                in->feature_info, NVSDK_NGX_Version_API);
}

NVSDK_NGX_Result vk_shutdown(const vio_upscale_device *dev)
{
    return NVSDK_NGX_VULKAN_Shutdown1(static_cast<VkDevice>(dev->device));
}

NVSDK_NGX_Result vk_caps(NVSDK_NGX_Parameter **out) { return NVSDK_NGX_VULKAN_GetCapabilityParameters(out); }
NVSDK_NGX_Result vk_alloc(NVSDK_NGX_Parameter **out) { return NVSDK_NGX_VULKAN_AllocateParameters(out); }
NVSDK_NGX_Result vk_free(NVSDK_NGX_Parameter *p) { return NVSDK_NGX_VULKAN_DestroyParameters(p); }
NVSDK_NGX_Result vk_release(NVSDK_NGX_Handle *h) { return NVSDK_NGX_VULKAN_ReleaseFeature(h); }

NVSDK_NGX_Result vk_create(const vio_upscale_device *dev, NVSDK_NGX_Parameter *p,
                           NVSDK_NGX_DLSS_Create_Params *cp, NVSDK_NGX_Handle **out)
{
    return NGX_VULKAN_CREATE_DLSS_EXT1(static_cast<VkDevice>(dev->device), static_cast<VkCommandBuffer>(dev->command_list),
                                       1, 1, out, p, cp);
}

NVSDK_NGX_Resource_VK vk_resource(const vio_upscale_native_image *im)
{
    VkImageSubresourceRange r;
    r.aspectMask = im->depth ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT;
    r.baseMipLevel = 0;
    r.levelCount = 1;
    r.baseArrayLayer = 0;
    r.layerCount = 1;
    return NVSDK_NGX_Create_ImageView_Resource_VK(static_cast<VkImageView>(im->view), static_cast<VkImage>(im->handle), r,
                                                  static_cast<VkFormat>(im->format), im->width, im->height, im->storage != 0);
}

/* Storage colour inputs (resting in GENERAL) to SHADER_READ_ONLY and back. */
struct VkLayouts {
    VkImageMemoryBarrier b[6];
    uint32_t n = 0;
    void add(const vio_upscale_native_image *im)
    {
        if (!im->handle || im->depth || im->state != VIO_UPSCALE_STATE_GENERAL) return;
        for (uint32_t i = 0; i < n; i++) if (b[i].image == im->handle) return;
        VkImageMemoryBarrier &x = b[n++];
        memset(&x, 0, sizeof(x));
        x.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        x.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
        x.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        x.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
        x.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        x.srcQueueFamilyIndex = x.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        x.image = static_cast<VkImage>(im->handle);
        x.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        x.subresourceRange.levelCount = 1;
        x.subresourceRange.layerCount = 1;
    }
    void record(VkCommandBuffer cmd, int back)
    {
        if (!n) return;
        if (back) {
            for (uint32_t i = 0; i < n; i++) {
                VkImageLayout t = b[i].oldLayout; b[i].oldLayout = b[i].newLayout; b[i].newLayout = t;
                b[i].srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
                b[i].dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
            }
        }
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0,
                             0, nullptr, 0, nullptr, n, b);
    }
};

NVSDK_NGX_Result vk_evaluate(const vio_upscale_native_dispatch *nd, NVSDK_NGX_Handle *h, NVSDK_NGX_Parameter *p,
                             float jitter_x, float jitter_y)
{
    const vio_upscale_dispatch_desc *d = nd->desc;
    VkCommandBuffer cmd = static_cast<VkCommandBuffer>(nd->command_list);
    VkLayouts lay;
    lay.add(&nd->color);
    lay.add(&nd->motion);
    lay.add(&nd->reactive);
    lay.add(&nd->exposure);
    lay.record(cmd, 0);

    NVSDK_NGX_Resource_VK color = vk_resource(&nd->color), output = vk_resource(&nd->output);
    NVSDK_NGX_Resource_VK depth = vk_resource(&nd->depth), motion = vk_resource(&nd->motion);
    NVSDK_NGX_Resource_VK exposure, reactive;
    NVSDK_NGX_VK_DLSS_Eval_Params ev;
    memset(&ev, 0, sizeof(ev));
    ev.Feature.pInColor = &color;
    ev.Feature.pInOutput = &output;
    ev.pInDepth = &depth;
    ev.pInMotionVectors = &motion;
    if (nd->exposure.handle) { exposure = vk_resource(&nd->exposure); ev.pInExposureTexture = &exposure; }
    if (nd->reactive.handle) { reactive = vk_resource(&nd->reactive); ev.pInBiasCurrentColorMask = &reactive; }
    ev.InJitterOffsetX = jitter_x;
    ev.InJitterOffsetY = jitter_y;
    ev.InRenderSubrectDimensions.Width = (unsigned int)d->render_width;
    ev.InRenderSubrectDimensions.Height = (unsigned int)d->render_height;
    ev.InReset = d->reset ? 1 : 0;
    ev.InMVScaleX = d->mv_scale_x;
    ev.InMVScaleY = d->mv_scale_y;
    ev.InPreExposure = d->pre_exposure;
    ev.InExposureScale = 1.0f;
    ev.InFrameTimeDeltaInMsec = d->frame_time_ms;
    NVSDK_NGX_Result rc = NGX_VULKAN_EVALUATE_DLSS_EXT(cmd, h, p, &ev);

    lay.record(cmd, 1);
    return rc;
}

/* NVIDIA packs its driver version as major:10 | minor:8 | ... */
void vk_driver_version(const vio_upscale_device *dev, char *out, size_t out_len)
{
    VkPhysicalDeviceProperties pp;
    vkGetPhysicalDeviceProperties(static_cast<VkPhysicalDevice>(dev->physical_device), &pp);
    uint32_t v = pp.driverVersion;
    if (pp.vendorID == 0x10DE) snprintf(out, out_len, "%u.%02u", (v >> 22) & 0x3ff, (v >> 14) & 0xff);
    else snprintf(out, out_len, "%u.%u.%u", VK_VERSION_MAJOR(v), VK_VERSION_MINOR(v), VK_VERSION_PATCH(v));
}

int vk_extensions(void *physical_device, const char **names, int max)
{
    unsigned int ic = 0, dc = 0;
    const char **ie = nullptr, **de = nullptr;
    if (NVSDK_NGX_FAILED(NVSDK_NGX_VULKAN_RequiredExtensions(&ic, &ie, &dc, &de))) return 0;
    unsigned int c = physical_device ? dc : ic;
    const char **list = physical_device ? de : ie;
    int n = 0;
    for (unsigned int i = 0; i < c && n < max; i++) if (list && list[i]) names[n++] = list[i];
    return n;
}

const VioDlssApi g_api = {
    vk_init, vk_shutdown, vk_caps, vk_alloc, vk_free, vk_create, vk_evaluate, vk_release,
    vk_driver_version, vk_extensions,
};

} // namespace

const VioDlssApi *const vio_dlss_api_vk = &g_api;

#else

const VioDlssApi *const vio_dlss_api_vk = nullptr;

#endif

/*
 * php-vio - FidelityFX provider, Vulkan part (TEMPORAL-S3)
 */

#include "vio_upscale_ffx_internal.h"

#include <new>

#ifdef HAVE_VULKAN
#include <vulkan/vulkan.h>
#include "ffx_api/vk/ffx_api_vk.h"

ffxApiHeader *vio_ffx_backend_desc_vk(const vio_upscale_device *dev, VioFfxBackendDesc *storage)
{
    static_assert(sizeof(ffxCreateBackendVKDesc) <= sizeof(VioFfxBackendDesc), "backend description too large");
    ffxCreateBackendVKDesc *d = new (storage->bytes) ffxCreateBackendVKDesc{};
    d->header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_VK;
    d->vkDevice = static_cast<VkDevice>(dev->device);
    d->vkPhysicalDevice = static_cast<VkPhysicalDevice>(dev->physical_device);
    d->vkDeviceProcAddr = reinterpret_cast<PFN_vkGetDeviceProcAddr>(dev->get_device_proc_addr);
    return &d->header;
}

FfxApiResource vio_ffx_resource_vk(const vio_upscale_native_image *im)
{
    FfxApiResource r{};
    if (!im->handle) return r;
    r.resource = im->handle;
    r.description.type = FFX_API_RESOURCE_TYPE_TEXTURE2D;
    r.description.format = ffxApiGetSurfaceFormatVK(static_cast<VkFormat>(im->format));
    r.description.width = im->width;
    r.description.height = im->height;
    r.description.depth = 1;
    r.description.mipCount = 1;
    r.description.flags = FFX_API_RESOURCE_FLAGS_NONE;
    r.description.usage = FFX_API_RESOURCE_USAGE_READ_ONLY;
    if (im->depth)   r.description.usage |= FFX_API_RESOURCE_USAGE_DEPTHTARGET;
    if (im->stencil) r.description.usage |= FFX_API_RESOURCE_USAGE_STENCILTARGET;
    if (im->storage) r.description.usage |= FFX_API_RESOURCE_USAGE_UAV;
    /* Colour rests in SHADER_READ_ONLY (PIXEL_READ) or GENERAL (storage targets);
     * the backend moved the depth to SHADER_READ_ONLY and hands it over in the
     * state the upscaler reads it in, so no barrier touches it here. */
    if (im->depth) r.state = FFX_API_RESOURCE_STATE_COMPUTE_READ;
    else r.state = im->state == VIO_UPSCALE_STATE_GENERAL ? FFX_API_RESOURCE_STATE_UNORDERED_ACCESS
                                                          : FFX_API_RESOURCE_STATE_PIXEL_READ;
    return r;
}

#else

ffxApiHeader *vio_ffx_backend_desc_vk(const vio_upscale_device *, VioFfxBackendDesc *) { return nullptr; }
FfxApiResource vio_ffx_resource_vk(const vio_upscale_native_image *) { return FfxApiResource{}; }

#endif

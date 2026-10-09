/*
 * php-vio - FidelityFX provider, D3D12 part (TEMPORAL-S3)
 */

#include "vio_upscale_ffx_internal.h"

#include <new>

#ifdef HAVE_D3D12
#include <d3d12.h>
#include "ffx_api/dx12/ffx_api_dx12.h"

ffxApiHeader *vio_ffx_backend_desc_dx12(const vio_upscale_device *dev, VioFfxBackendDesc *storage)
{
    static_assert(sizeof(ffxCreateBackendDX12Desc) <= sizeof(VioFfxBackendDesc), "backend description too large");
    ffxCreateBackendDX12Desc *d = new (storage->bytes) ffxCreateBackendDX12Desc{};
    d->header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_DX12;
    d->device = static_cast<ID3D12Device *>(dev->device);
    return &d->header;
}

FfxApiResource vio_ffx_resource_dx12(const vio_upscale_native_image *im)
{
    if (!im->handle) return FfxApiResource{};
    uint32_t state = im->state == VIO_UPSCALE_STATE_GENERAL ? FFX_API_RESOURCE_STATE_UNORDERED_ACCESS
                                                            : FFX_API_RESOURCE_STATE_PIXEL_READ;
    /* The resource's own description; the provider makes SRVs / UAVs from its
     * format (a typeless R24G8 depth reads as R24_UNORM_X8). */
    FfxApiResource r = ffxApiGetResourceDX12(static_cast<ID3D12Resource *>(im->handle), state, 0);
    if (im->depth) r.description.usage |= FFX_API_RESOURCE_USAGE_DEPTHTARGET;
    return r;
}

#else

ffxApiHeader *vio_ffx_backend_desc_dx12(const vio_upscale_device *, VioFfxBackendDesc *) { return nullptr; }
FfxApiResource vio_ffx_resource_dx12(const vio_upscale_native_image *) { return FfxApiResource{}; }

#endif

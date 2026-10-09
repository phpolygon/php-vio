/*
 * php-vio - FidelityFX provider internals (TEMPORAL-S3)
 *
 * ffx_api_dx12.h and ffx_api_vk.h define the same enumerators, so each API's
 * part lives in its own translation unit: vio_upscale_ffx_dx12.cpp and
 * vio_upscale_ffx_vk.cpp. vio_upscale_ffx.cpp holds everything API-neutral.
 */

#ifndef VIO_UPSCALE_FFX_INTERNAL_H
#define VIO_UPSCALE_FFX_INTERNAL_H

#ifndef _WIN32
#ifndef __declspec
#define __declspec(x)   /* ffx_api.h declares its entry points dllexport */
#endif
#endif

extern "C" {
#include "vio_upscale.h"
}

/* Windows builds pass the detected headers, not HAVE_* (php_vio.h maps them). */
#if defined(HAVE_D3D12_H) && !defined(HAVE_D3D12)
#define HAVE_D3D12 1
#endif
#if defined(HAVE_VULKAN_VULKAN_H) && !defined(HAVE_VULKAN)
#define HAVE_VULKAN 1
#endif

#include "ffx_api/ffx_api.h"
#include "ffx_api/ffx_upscale.h"

/* Storage for the API's backend create description (kept alive with the context). */
struct VioFfxBackendDesc {
    alignas(16) unsigned char bytes[128];
};

/* Fill the backend description for `dev` into `storage`; its header, or nullptr. */
ffxApiHeader *vio_ffx_backend_desc_dx12(const vio_upscale_device *dev, VioFfxBackendDesc *storage);
ffxApiHeader *vio_ffx_backend_desc_vk(const vio_upscale_device *dev, VioFfxBackendDesc *storage);

/* An FfxApiResource for a native image (empty when im->handle is NULL). */
FfxApiResource vio_ffx_resource_dx12(const vio_upscale_native_image *im);
FfxApiResource vio_ffx_resource_vk(const vio_upscale_native_image *im);

#endif /* VIO_UPSCALE_FFX_INTERNAL_H */

/*
 * php-vio - DLSS provider internals (TEMPORAL-S4, --with-dlss=DIR)
 *
 * NGX's D3D12 and Vulkan entry points live in separate translation units
 * (vio_upscale_dlss_dx12.cpp, vio_upscale_dlss_vk.cpp) so a build without one
 * API never sees its headers; vio_upscale_dlss.cpp holds everything API-neutral.
 * The NGX headers come from the DLSS SDK the build points at - they are not
 * part of php-vio.
 */

#ifndef VIO_UPSCALE_DLSS_INTERNAL_H
#define VIO_UPSCALE_DLSS_INTERNAL_H

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

#include <cstddef>
#include <cwchar>
#include "nvsdk_ngx_defs.h"
#include "nvsdk_ngx_params.h"

/* What NGX needs at initialisation, shared by both APIs. */
struct VioDlssInit {
    const char    *project_id;
    const char    *engine_version;
    const wchar_t *data_path;
    const NVSDK_NGX_FeatureCommonInfo *feature_info;
};

/* One API's NGX entry points (vio_dlss_api_dx12 / vio_dlss_api_vk); NULL when
 * php-vio was built without that API. */
struct VioDlssApi {
    NVSDK_NGX_Result (*init)(const vio_upscale_device *dev, const VioDlssInit *in);
    NVSDK_NGX_Result (*shutdown)(const vio_upscale_device *dev);
    NVSDK_NGX_Result (*capability_parameters)(NVSDK_NGX_Parameter **out);
    NVSDK_NGX_Result (*allocate_parameters)(NVSDK_NGX_Parameter **out);
    NVSDK_NGX_Result (*destroy_parameters)(NVSDK_NGX_Parameter *p);
    /* NGX_*_CREATE_DLSS_EXT on dev->command_list. */
    NVSDK_NGX_Result (*create)(const vio_upscale_device *dev, NVSDK_NGX_Parameter *p,
                               NVSDK_NGX_DLSS_Create_Params *cp, NVSDK_NGX_Handle **out);
    /* NGX_*_EVALUATE_DLSS_EXT on d->command_list, the API's resource states
     * around it; jitter / motion scale already in NGX's convention. */
    NVSDK_NGX_Result (*evaluate)(const vio_upscale_native_dispatch *d, NVSDK_NGX_Handle *h, NVSDK_NGX_Parameter *p,
                                 float jitter_x, float jitter_y);
    NVSDK_NGX_Result (*release)(NVSDK_NGX_Handle *h);
    /* The graphics driver version as NVIDIA writes it ("581.57"), "" if unknown. */
    void (*driver_version)(const vio_upscale_device *dev, char *out, size_t out_len);
    /* Vulkan: the instance (physical_device NULL) or device extensions NGX needs. */
    int  (*vk_extensions)(void *physical_device, const char **names, int max);
};

extern const VioDlssApi *const vio_dlss_api_dx12;
extern const VioDlssApi *const vio_dlss_api_vk;

#endif /* VIO_UPSCALE_DLSS_INTERNAL_H */

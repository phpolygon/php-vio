/*
 * php-vio - AMD FidelityFX FSR 3.1 upscaler provider (TEMPORAL-S3, --with-ffx)
 *
 * Talks to the FidelityFX API (ffx-api, MIT, vendor/ffx-api) through the
 * signed runtime libraries amd_fidelityfx_dx12.dll / amd_fidelityfx_vk.dll.
 * They are not linked: the one matching the device's API is loaded on first
 * use (vio_upscale_load_library: vio.ffx_path / VIO_FFX_PATH, next to the PHP
 * executable, next to php_vio, PATH), so a build runs without them and a
 * process only maps the library of the backend it uses.
 *
 * C++ because the API's D3D12 / Vulkan helpers are C++ only; the API-specific
 * parts are in vio_upscale_ffx_dx12.cpp / vio_upscale_ffx_vk.cpp.
 */

#ifdef _WIN32
#include <windows.h>
#endif

#include "vio_upscale_ffx_internal.h"

#include <cfloat>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <new>

namespace {

struct FfxLib {
    int   state = 0;               /* 0 = not tried, 1 = loaded, -1 = missing */
    void *module = nullptr;
    PfnFfxCreateContext  create = nullptr;
    PfnFfxDestroyContext destroy = nullptr;
    PfnFfxConfigure      configure = nullptr;
    PfnFfxQuery          query = nullptr;
    PfnFfxDispatch       dispatch = nullptr;
    char path[512] = "";
};

FfxLib g_lib[3];   /* indexed by VIO_UPSCALE_API_* */

const char *ffx_library_name(int api)
{
#ifdef _WIN32
    return api == VIO_UPSCALE_API_D3D12 ? "amd_fidelityfx_dx12.dll" : "amd_fidelityfx_vk.dll";
#else
    (void)api;
    return "libamd_fidelityfx_vk.so";
#endif
}

/* A missing library is looked for again on the next call (a game may set
 * vio.ffx_path later); a loaded one stays for the process. */
FfxLib *ffx_load(int api, char *reason, size_t reason_len)
{
    if (api != VIO_UPSCALE_API_D3D12 && api != VIO_UPSCALE_API_VULKAN) {
        snprintf(reason, reason_len, "FSR 3.1 runs on D3D12 and Vulkan only");
        return nullptr;
    }
    FfxLib *L = &g_lib[api];
    if (L->state == 1) return L;
    char path[512], why[512];
    void *m = vio_upscale_load_library("vio.ffx_path", "VIO_FFX_PATH", ffx_library_name(api),
                                       path, sizeof(path), why, sizeof(why));
    if (!m) {
        L->state = -1;
        snprintf(reason, reason_len, "%s", why);
        return nullptr;
    }
    L->create    = reinterpret_cast<PfnFfxCreateContext>(vio_upscale_symbol(m, "ffxCreateContext"));
    L->destroy   = reinterpret_cast<PfnFfxDestroyContext>(vio_upscale_symbol(m, "ffxDestroyContext"));
    L->configure = reinterpret_cast<PfnFfxConfigure>(vio_upscale_symbol(m, "ffxConfigure"));
    L->query     = reinterpret_cast<PfnFfxQuery>(vio_upscale_symbol(m, "ffxQuery"));
    L->dispatch  = reinterpret_cast<PfnFfxDispatch>(vio_upscale_symbol(m, "ffxDispatch"));
    if (!L->create || !L->destroy || !L->configure || !L->query || !L->dispatch) {
        snprintf(reason, reason_len, "%s lacks the ffx* entry points", path);
        L->state = -1;
        return nullptr;
    }
    L->module = m;
    snprintf(L->path, sizeof(L->path), "%s", path);
    L->state = 1;
    return L;
}

void ffx_message(uint32_t type, const wchar_t *message)
{
    fprintf(stderr, "FFX[%s]: %ls\n", type == FFX_API_MESSAGE_TYPE_ERROR ? "error" : "warning", message ? message : L"");
    fflush(stderr);
}

const char *ffx_rc(ffxReturnCode_t rc)
{
    switch (rc) {
        case FFX_API_RETURN_OK:                     return "ok";
        case FFX_API_RETURN_ERROR:                  return "error";
        case FFX_API_RETURN_ERROR_UNKNOWN_DESCTYPE: return "unknown descriptor type";
        case FFX_API_RETURN_ERROR_RUNTIME_ERROR:    return "runtime error";
        case FFX_API_RETURN_NO_PROVIDER:            return "no provider";
        case FFX_API_RETURN_ERROR_MEMORY:           return "out of memory";
        case FFX_API_RETURN_ERROR_PARAMETER:        return "invalid parameter";
        default:                                    return "unknown error";
    }
}

struct FfxCtx {
    FfxLib    *lib = nullptr;
    int        api = 0;
    ffxContext ctx = nullptr;
    ffxCreateContextDescUpscale create{};   /* the API wants the chain alive until destroy */
    VioFfxBackendDesc backend{};
    ffxAllocationCallbacks alloc{};
    vio_upscale_create_desc desc{};
};

FfxApiResource ffx_resource(int api, const vio_upscale_native_image *im)
{
    return api == VIO_UPSCALE_API_D3D12 ? vio_ffx_resource_dx12(im) : vio_ffx_resource_vk(im);
}

/* ── Provider ops ──────────────────────────────────────────────────── */

int ffx_supported(const vio_upscale_device *dev, char *reason, size_t reason_len, vio_upscale_query *q)
{
    FfxLib *L = ffx_load(dev->api, reason, reason_len);
    if (!L) return 0;
    uint64_t count = 0;
    ffxQueryDescGetVersions qv{};
    qv.header.type = FFX_API_QUERY_DESC_TYPE_GET_VERSIONS;
    qv.createDescType = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE;
    qv.device = dev->api == VIO_UPSCALE_API_D3D12 ? dev->device : nullptr;
    qv.outputCount = &count;
    ffxReturnCode_t rc = L->query(nullptr, &qv.header);
    if (rc != FFX_API_RETURN_OK || count == 0) {
        snprintf(reason, reason_len, "%s offers no upscaler for this device (%s)", L->path, ffx_rc(rc));
        return 0;
    }
    uint64_t ids[16];
    const char *names[16] = {nullptr};
    if (count > 16) count = 16;
    qv.versionIds = ids;
    qv.versionNames = names;
    if (L->query(nullptr, &qv.header) == FFX_API_RETURN_OK && names[0]) {
        snprintf(q->version, sizeof(q->version), "%s", names[0]);
    }
    snprintf(q->library, sizeof(q->library), "%s", L->path);
    return 1;
}

void *ffx_create(const vio_upscale_device *dev, const vio_upscale_create_desc *d, char *reason, size_t reason_len)
{
    FfxLib *L = ffx_load(dev->api, reason, reason_len);
    if (!L) return nullptr;
    FfxCtx *c = new (std::nothrow) FfxCtx();
    if (!c) { snprintf(reason, reason_len, "out of memory"); return nullptr; }
    c->lib = L;
    c->api = dev->api;
    c->desc = *d;
    ffxApiHeader *backend = dev->api == VIO_UPSCALE_API_D3D12 ? vio_ffx_backend_desc_dx12(dev, &c->backend)
                                                              : vio_ffx_backend_desc_vk(dev, &c->backend);
    if (!backend) {
        snprintf(reason, reason_len, "php-vio was built without this graphics API");
        delete c;
        return nullptr;
    }
    c->create.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE;
    c->create.header.pNext = backend;
    uint32_t flags = 0;
    if (d->flags & VIO_UPSCALE_FLAG_HDR)            flags |= FFX_UPSCALE_ENABLE_HIGH_DYNAMIC_RANGE;
    if (d->flags & VIO_UPSCALE_FLAG_DEPTH_INVERTED) flags |= FFX_UPSCALE_ENABLE_DEPTH_INVERTED;
    if (d->flags & VIO_UPSCALE_FLAG_DEPTH_INFINITE) flags |= FFX_UPSCALE_ENABLE_DEPTH_INFINITE;
    if (d->flags & VIO_UPSCALE_FLAG_AUTO_EXPOSURE)  flags |= FFX_UPSCALE_ENABLE_AUTO_EXPOSURE;
    if (d->flags & VIO_UPSCALE_FLAG_DYNAMIC_RES)    flags |= FFX_UPSCALE_ENABLE_DYNAMIC_RESOLUTION;
    if (d->flags & VIO_UPSCALE_FLAG_DEBUG)          flags |= FFX_UPSCALE_ENABLE_DEBUG_CHECKING;
    if (d->flags & VIO_UPSCALE_FLAG_MV_JITTERED)    flags |= FFX_UPSCALE_ENABLE_MOTION_VECTORS_JITTER_CANCELLATION;
    c->create.flags = flags;
    c->create.maxRenderSize.width = (uint32_t)d->render_width;
    c->create.maxRenderSize.height = (uint32_t)d->render_height;
    c->create.maxUpscaleSize.width = (uint32_t)d->display_width;
    c->create.maxUpscaleSize.height = (uint32_t)d->display_height;
    c->create.fpMessage = ffx_message;
    c->alloc.pUserData = nullptr;
    c->alloc.alloc = vio_upscale_host_alloc;
    c->alloc.dealloc = vio_upscale_host_free;
    ffxReturnCode_t rc = L->create(&c->ctx, &c->create.header, &c->alloc);
    if (rc != FFX_API_RETURN_OK || !c->ctx) {
        snprintf(reason, reason_len, "ffxCreateContext failed (%s)", ffx_rc(rc));
        delete c;
        return nullptr;
    }
    return c;
}

int ffx_dispatch(void *ctx, const vio_upscale_native_dispatch *nd, char *err, size_t err_len)
{
    FfxCtx *c = static_cast<FfxCtx *>(ctx);
    const vio_upscale_dispatch_desc *d = nd->desc;
    ffxDispatchDescUpscale dd{};
    dd.header.type = FFX_API_DISPATCH_DESC_TYPE_UPSCALE;
    dd.commandList = nd->command_list;
    dd.color = ffx_resource(c->api, &nd->color);
    dd.depth = ffx_resource(c->api, &nd->depth);
    dd.motionVectors = ffx_resource(c->api, &nd->motion);
    dd.exposure = ffx_resource(c->api, &nd->exposure);
    dd.reactive = ffx_resource(c->api, &nd->reactive);
    dd.transparencyAndComposition = ffx_resource(c->api, &nd->transparency);
    dd.output = ffx_resource(c->api, &nd->output);
    /* FSR: jitter in render pixels as applied to the projection (x right,
     * y down), motion as previous - current in pixels after the scale - the
     * conventions of vio_upscale.h. */
    dd.jitterOffset.x = d->jitter_x;
    dd.jitterOffset.y = d->jitter_y;
    dd.motionVectorScale.x = d->mv_scale_x;
    dd.motionVectorScale.y = d->mv_scale_y;
    dd.renderSize.width = (uint32_t)d->render_width;
    dd.renderSize.height = (uint32_t)d->render_height;
    dd.upscaleSize.width = (uint32_t)c->desc.display_width;
    dd.upscaleSize.height = (uint32_t)c->desc.display_height;
    dd.enableSharpening = d->sharpness > 0.0f;
    dd.sharpness = d->sharpness;
    dd.frameTimeDelta = d->frame_time_ms;
    dd.preExposure = d->pre_exposure;
    dd.reset = d->reset != 0;
    /* FSR wants near / far as the depth buffer stores them: swapped for an
     * inverted depth, FLT_MAX for the infinite plane. */
    float n = d->camera_near, f = d->camera_far;
    if (c->desc.flags & VIO_UPSCALE_FLAG_DEPTH_INFINITE) f = FLT_MAX;
    if (c->desc.flags & VIO_UPSCALE_FLAG_DEPTH_INVERTED) { float t = n; n = f; f = t; }
    dd.cameraNear = n;
    dd.cameraFar = f;
    dd.cameraFovAngleVertical = d->fov_y;
    dd.viewSpaceToMetersFactor = d->view_to_meters;
    dd.flags = 0;
    ffxReturnCode_t rc = c->lib->dispatch(&c->ctx, &dd.header);
    if (rc != FFX_API_RETURN_OK) {
        snprintf(err, err_len, "ffxDispatch failed (%s)", ffx_rc(rc));
        return -1;
    }
    return 0;
}

int ffx_query(void *ctx, vio_upscale_query *q)
{
    FfxCtx *c = static_cast<FfxCtx *>(ctx);
    FfxApiEffectMemoryUsage mem{};
    ffxQueryDescUpscaleGetGPUMemoryUsage qm{};
    qm.header.type = FFX_API_QUERY_DESC_TYPE_UPSCALE_GPU_MEMORY_USAGE;
    qm.gpuMemoryUsageUpscaler = &mem;
    if (c->lib->query(&c->ctx, &qm.header) == FFX_API_RETURN_OK) q->gpu_memory = mem.totalUsageInBytes;
    int32_t phases = 0;
    ffxQueryDescUpscaleGetJitterPhaseCount qp{};
    qp.header.type = FFX_API_QUERY_DESC_TYPE_UPSCALE_GETJITTERPHASECOUNT;
    qp.renderWidth = (uint32_t)c->desc.render_width;
    qp.displayWidth = (uint32_t)c->desc.display_width;
    qp.pOutPhaseCount = &phases;
    if (c->lib->query(&c->ctx, &qp.header) == FFX_API_RETURN_OK && phases > 0) q->jitter_phases = phases;
    ffxQueryGetProviderVersion qv{};
    qv.header.type = FFX_API_QUERY_DESC_TYPE_GET_PROVIDER_VERSION;
    if (c->lib->query(&c->ctx, &qv.header) == FFX_API_RETURN_OK && qv.versionName) {
        snprintf(q->version, sizeof(q->version), "%s", qv.versionName);
    }
    snprintf(q->library, sizeof(q->library), "%s", c->lib->path);
    return 0;
}

void ffx_destroy(void *ctx)
{
    FfxCtx *c = static_cast<FfxCtx *>(ctx);
    if (!c) return;
    if (c->ctx) c->lib->destroy(&c->ctx, &c->alloc);
    delete c;
}

/* What the Vulkan provider picks from the physical device's OFFER (it reads the
 * available extensions / features, not the enabled ones): wave64 through
 * subgroup size control, FP16 / int16 shader variants, 16-bit storage, and
 * depth-only barriers on depth-stencil images. Only when the library is there. */
unsigned ffx_vk_device_needs(void *physical_device)
{
    (void)physical_device;
    char reason[512];
    if (!ffx_load(VIO_UPSCALE_API_VULKAN, reason, sizeof(reason))) return 0;
    return VIO_UPSCALE_VK_SUBGROUP_SIZE_CONTROL | VIO_UPSCALE_VK_FLOAT16 | VIO_UPSCALE_VK_INT16
         | VIO_UPSCALE_VK_STORAGE16 | VIO_UPSCALE_VK_SEPARATE_DEPTH_STENCIL;
}

} // namespace

extern "C" const vio_upscale_provider vio_upscale_provider_ffx = {
    VIO_UPSCALER_FSR3,
    "fsr3",
    0,
    ffx_supported,
    ffx_create,
    ffx_dispatch,
    ffx_query,
    ffx_destroy,
    ffx_vk_device_needs,
    nullptr,   /* render_size: the fixed FSR ratios */
    nullptr,   /* device_release: nothing per device */
    nullptr,   /* vk_extensions: chosen through vk_device_needs */
};

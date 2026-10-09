/*
 * php-vio - NVIDIA DLSS Super Resolution provider through NGX (TEMPORAL-S4,
 * --with-dlss=DIR)
 *
 * NGX directly (no Streamline): the static NGX library of the DLSS SDK the build
 * points at is linked in; the DLSS runtime itself - nvngx_dlss.dll /
 * libnvidia-ngx-dlss.so.<version>, shipped by the game, never by php-vio - is
 * loaded by NGX. Before NGX is touched the runtime is looked for like every
 * provider's library (vio.dlss_path / VIO_DLSS_PATH, next to the PHP executable,
 * next to php_vio, PATH); its directory is handed to NGX as the feature path.
 * Without it the provider is unsupported - no NGX call, no warning.
 *
 * Per device NGX is initialised once (NVSDK_NGX_*_Init_with_ProjectID, engine
 * type CUSTOM, vio.dlss_project_id / vio.dlss_engine_version) on the first
 * supported() call and shut down by device_release before the device goes.
 * The capability parameters it hands out carry SuperSampling.Available, the
 * driver requirement and the optimal-settings / stats callbacks.
 *
 * Conventions (vio_upscale.h) map onto NGX: motion = previous - current in
 * render pixels (x right, y down) times mv_scale - the same in NGX; jitter =
 * the projection offset in render pixels (x right, y down), passed unchanged.
 * Measured with tests/render3d/230 on all four sign combinations (RTX 2080,
 * DLSS 310.9.1, Performance 32 -> 64): unchanged converges to RMSE 0.0207
 * (bilinear 0.0741), a flipped x 0.0921, a flipped y 0.0916, both 0.1681 -
 * NGX's jitter has vio's (and FSR's) sign on both axes.
 */

#ifdef _WIN32
#include <windows.h>
#endif

#include "vio_upscale_dlss_internal.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>

namespace {

#ifdef _WIN32
const char *const DLSS_FILE = "nvngx_dlss.dll";
#else
const char *const DLSS_FILE = "libnvidia-ngx-dlss.so";
#endif

struct DlssDev {
    int    api = 0;
    void  *device = nullptr;
    int    state = 0;              /* 0 = not initialised, 1 = DLSS available, -1 = not */
    char   reason[512] = "";
    char   driver[64] = "";
    NVSDK_NGX_Parameter *caps = nullptr;
};

DlssDev g_dev[4];

const VioDlssApi *dlss_api(int api)
{
    if (api == VIO_UPSCALE_API_D3D12) return vio_dlss_api_dx12;
    if (api == VIO_UPSCALE_API_VULKAN) return vio_dlss_api_vk;
    return nullptr;
}

DlssDev *dlss_dev(const vio_upscale_device *dev, int create)
{
    for (auto &d : g_dev) if (d.device == dev->device && d.api == dev->api && d.device) return &d;
    if (!create) return nullptr;
    for (auto &d : g_dev) {
        if (!d.device) {
            d = DlssDev();
            d.api = dev->api;
            d.device = dev->device;
            return &d;
        }
    }
    return nullptr;
}

const char *ngx_rc(NVSDK_NGX_Result rc)
{
    switch (rc) {
        case NVSDK_NGX_Result_Success:                   return "ok";
        case NVSDK_NGX_Result_FAIL_FeatureNotSupported:  return "feature not supported";
        case NVSDK_NGX_Result_FAIL_PlatformError:        return "platform error";
        case NVSDK_NGX_Result_FAIL_FeatureAlreadyExists: return "feature already exists";
        case NVSDK_NGX_Result_FAIL_FeatureNotFound:      return "feature not found";
        case NVSDK_NGX_Result_FAIL_InvalidParameter:     return "invalid parameter";
        case NVSDK_NGX_Result_FAIL_ScratchBufferTooSmall:return "scratch buffer too small";
        case NVSDK_NGX_Result_FAIL_NotInitialized:       return "not initialised";
        case NVSDK_NGX_Result_FAIL_UnsupportedInputFormat: return "unsupported input format";
        case NVSDK_NGX_Result_FAIL_RWFlagMissing:        return "output lacks UAV / storage access";
        case NVSDK_NGX_Result_FAIL_MissingInput:         return "missing input";
        case NVSDK_NGX_Result_FAIL_UnableToInitializeFeature: return "unable to initialise the feature";
        case NVSDK_NGX_Result_FAIL_OutOfDate:            return "out of date (driver or runtime)";
        case NVSDK_NGX_Result_FAIL_OutOfGPUMemory:       return "out of GPU memory";
        case NVSDK_NGX_Result_FAIL_UnsupportedFormat:    return "unsupported format";
        case NVSDK_NGX_Result_FAIL_UnableToWriteToAppDataPath: return "cannot write to the data path";
        case NVSDK_NGX_Result_FAIL_UnsupportedParameter: return "unsupported parameter";
        case NVSDK_NGX_Result_FAIL_Denied:               return "denied for this application";
        case NVSDK_NGX_Result_FAIL_NotImplemented:       return "not implemented";
        default:                                         return "error";
    }
}

/* "310.9.1" from the runtime file. */
void dlss_file_version(const char *path, char *out, size_t out_len)
{
    out[0] = '\0';
#ifdef _WIN32
    DWORD handle = 0;
    DWORD size = GetFileVersionInfoSizeA(path, &handle);
    if (size == 0) return;
    void *buf = malloc(size);
    if (!buf) return;
    VS_FIXEDFILEINFO *fi = nullptr;
    UINT len = 0;
    if (GetFileVersionInfoA(path, 0, size, buf) && VerQueryValueA(buf, "\\", (void **)&fi, &len) && fi) {
        snprintf(out, out_len, "%u.%u.%u", HIWORD(fi->dwFileVersionMS), LOWORD(fi->dwFileVersionMS), HIWORD(fi->dwFileVersionLS));
    }
    free(buf);
#else
    const char *so = strstr(path, ".so.");
    if (so) snprintf(out, out_len, "%s", so + 4);
#endif
}

void dlss_dirname(const char *path, char *out, size_t out_len)
{
    snprintf(out, out_len, "%s", path);
    char *s1 = strrchr(out, '\\'), *s2 = strrchr(out, '/');
    char *s = s1 > s2 ? s1 : s2;
    if (s) *s = '\0';
}

void dlss_widen(const char *in, wchar_t *out, size_t out_len)
{
#ifdef _WIN32
    if (!MultiByteToWideChar(CP_ACP, 0, in, -1, out, (int)out_len)) out[0] = L'\0';
#else
    size_t n = mbstowcs(out, in, out_len - 1);
    if (n == (size_t)-1) n = 0;
    out[n] = L'\0';
#endif
}

const char *ini_or(const char *key, const char *def)
{
    const char *v = vio_upscale_ini(key);
    return v && v[0] ? v : def;
}

void dlss_drop(DlssDev *D, const VioDlssApi *api, const vio_upscale_device *dev)
{
    if (D->caps) { api->destroy_parameters(D->caps); D->caps = nullptr; }
    api->shutdown(dev);
}

/* NGX up for the device: 1 = DLSS available, 0 = not (D->reason). */
int dlss_init(DlssDev *D, const VioDlssApi *api, const vio_upscale_device *dev, const char *lib)
{
    api->driver_version(dev, D->driver, sizeof(D->driver));

    char dir[1024];
    dlss_dirname(lib, dir, sizeof(dir));
    static wchar_t wdir[1024];
    dlss_widen(dir, wdir, sizeof(wdir) / sizeof(wdir[0]));
    static const wchar_t *paths[1];
    paths[0] = wdir;
    static wchar_t data_path[1024];
#ifdef _WIN32
    if (!GetTempPathW((DWORD)(sizeof(data_path) / sizeof(data_path[0])), data_path)) wcscpy(data_path, L".");
#else
    const char *tmp = getenv("TMPDIR");
    dlss_widen(tmp && tmp[0] ? tmp : "/tmp", data_path, sizeof(data_path) / sizeof(data_path[0]));
#endif
    NVSDK_NGX_FeatureCommonInfo fi;
    memset(&fi, 0, sizeof(fi));
    fi.PathListInfo.Path = paths;
    fi.PathListInfo.Length = 1;
    fi.LoggingInfo.MinimumLoggingLevel = NVSDK_NGX_LOGGING_LEVEL_OFF;

    VioDlssInit in;
    in.project_id = ini_or("vio.dlss_project_id", "f2602eff-4605-46cb-82c5-cb557d9b7281");
    in.engine_version = ini_or("vio.dlss_engine_version", "1.0");
    in.data_path = data_path;
    in.feature_info = &fi;

    NVSDK_NGX_Result rc = api->init(dev, &in);
    if (rc == NVSDK_NGX_Result_FAIL_InvalidParameter) {
        /* The driver rejects ids that do not look like a random GUID. */
        snprintf(D->reason, sizeof(D->reason), "NGX rejected the project id '%s' (vio.dlss_project_id must be a random GUID)",
                 in.project_id);
        api->shutdown(dev);
        return 0;
    }
    if (NVSDK_NGX_FAILED(rc)) {
        snprintf(D->reason, sizeof(D->reason),
                 "NGX could not initialise (%s): DLSS needs an NVIDIA RTX GPU and its driver%s%s%s",
                 ngx_rc(rc), D->driver[0] ? " (driver " : "", D->driver, D->driver[0] ? ")" : "");
        api->shutdown(dev);
        return 0;
    }
    rc = api->capability_parameters(&D->caps);
    if (NVSDK_NGX_FAILED(rc) || !D->caps) {
        snprintf(D->reason, sizeof(D->reason), "NGX has no capability parameters (%s) - the driver is too old", ngx_rc(rc));
        D->caps = nullptr;
        dlss_drop(D, api, dev);
        return 0;
    }
    int needs_driver = 0;
    if (NVSDK_NGX_SUCCEED(NVSDK_NGX_Parameter_GetI(D->caps, NVSDK_NGX_Parameter_SuperSampling_NeedsUpdatedDriver, &needs_driver))
        && needs_driver) {
        unsigned int major = 0, minor = 0;
        NVSDK_NGX_Parameter_GetUI(D->caps, NVSDK_NGX_Parameter_SuperSampling_MinDriverVersionMajor, &major);
        NVSDK_NGX_Parameter_GetUI(D->caps, NVSDK_NGX_Parameter_SuperSampling_MinDriverVersionMinor, &minor);
        snprintf(D->reason, sizeof(D->reason), "DLSS needs NVIDIA driver %u.%02u or newer (installed: %s)",
                 major, minor, D->driver[0] ? D->driver : "unknown");
        dlss_drop(D, api, dev);
        return 0;
    }
    int available = 0;
    NVSDK_NGX_Parameter_GetI(D->caps, NVSDK_NGX_Parameter_SuperSampling_Available, &available);
    if (!available) {
        int init_rc = 0;
        NVSDK_NGX_Parameter_GetI(D->caps, NVSDK_NGX_Parameter_SuperSampling_FeatureInitResult, &init_rc);
        snprintf(D->reason, sizeof(D->reason), "DLSS Super Resolution is not available on this GPU (%s)",
                 ngx_rc((NVSDK_NGX_Result)init_rc));
        dlss_drop(D, api, dev);
        return 0;
    }
    return 1;
}

/* The device's state, initialising NGX on first use. nullptr + reason when DLSS
 * is not usable. `lib` receives the runtime found. */
DlssDev *dlss_ready(const vio_upscale_device *dev, char *lib, size_t lib_len, char *reason, size_t reason_len)
{
    const VioDlssApi *api = dlss_api(dev->api);
    if (dev->api != VIO_UPSCALE_API_D3D12 && dev->api != VIO_UPSCALE_API_VULKAN) {
        snprintf(reason, reason_len, "DLSS runs on D3D12 and Vulkan only");
        return nullptr;
    }
    if (!api) {
        snprintf(reason, reason_len, "php-vio was built without this graphics API");
        return nullptr;
    }
    if (!vio_upscale_find_file("vio.dlss_path", "VIO_DLSS_PATH", DLSS_FILE, lib, lib_len, reason, reason_len)) return nullptr;
    DlssDev *D = dlss_dev(dev, 1);
    if (!D) {
        snprintf(reason, reason_len, "too many devices");
        return nullptr;
    }
    if (D->state == 0) D->state = dlss_init(D, api, dev, lib) ? 1 : -1;
    if (D->state < 0) {
        snprintf(reason, reason_len, "%s", D->reason);
        return nullptr;
    }
    return D;
}

NVSDK_NGX_PerfQuality_Value dlss_quality(int quality)
{
    switch (quality) {
        case VIO_UPSCALE_NATIVE_AA:         return NVSDK_NGX_PerfQuality_Value_DLAA;
        case VIO_UPSCALE_QUALITY:           return NVSDK_NGX_PerfQuality_Value_MaxQuality;
        case VIO_UPSCALE_BALANCED:          return NVSDK_NGX_PerfQuality_Value_Balanced;
        case VIO_UPSCALE_PERFORMANCE:       return NVSDK_NGX_PerfQuality_Value_MaxPerf;
        case VIO_UPSCALE_ULTRA_PERFORMANCE: return NVSDK_NGX_PerfQuality_Value_UltraPerformance;
        default:                            return NVSDK_NGX_PerfQuality_Value_MaxQuality;
    }
}

/* 'preset' => letter to NGX's render preset. NGX's defaults (SDK 310.6) are the
 * transformer presets - K for DLAA / Quality / Balanced, M for Performance, L
 * for Ultra Performance - which cost on Turing 2-6x the CNN presets E / F
 * (deprecated, still shipped); the programming guide's execution-time table,
 * RTX 2080 Ti at 2560x1440: E/F 0.62 ms, J/K 1.80 ms, M 3.41 ms, L 5.45 ms.
 * G, H, I, N, O are reserved. */
int dlss_preset(char letter, unsigned int *out)
{
    switch (letter) {
        case 'e': *out = NVSDK_NGX_DLSS_Hint_Render_Preset_E; return 1;
        case 'f': *out = NVSDK_NGX_DLSS_Hint_Render_Preset_F; return 1;
        case 'j': *out = NVSDK_NGX_DLSS_Hint_Render_Preset_J; return 1;
        case 'k': *out = NVSDK_NGX_DLSS_Hint_Render_Preset_K; return 1;
        case 'l': *out = NVSDK_NGX_DLSS_Hint_Render_Preset_L; return 1;
        case 'm': *out = NVSDK_NGX_DLSS_Hint_Render_Preset_M; return 1;
        default:  return 0;
    }
}

const char *dlss_quality_name(int quality)
{
    switch (quality) {
        case VIO_UPSCALE_NATIVE_AA:         return "DLAA";
        case VIO_UPSCALE_QUALITY:           return "Quality";
        case VIO_UPSCALE_BALANCED:          return "Balanced";
        case VIO_UPSCALE_PERFORMANCE:       return "Performance";
        case VIO_UPSCALE_ULTRA_PERFORMANCE: return "Ultra Performance";
        default:                            return "?";
    }
}

/* ── Provider ops ──────────────────────────────────────────────────── */

int dlss_supported(const vio_upscale_device *dev, char *reason, size_t reason_len, vio_upscale_query *q)
{
    char lib[512];
    DlssDev *D = dlss_ready(dev, lib, sizeof(lib), reason, reason_len);
    if (!D) {
        /* The driver version helps reading a "not available" reason. */
        DlssDev *known = dlss_dev(dev, 0);
        if (known) snprintf(q->driver, sizeof(q->driver), "%s", known->driver);
        return 0;
    }
    dlss_file_version(lib, q->version, sizeof(q->version));
    snprintf(q->library, sizeof(q->library), "%s", lib);
    snprintf(q->driver, sizeof(q->driver), "%s", D->driver);
    return 1;
}

/* NGX's optimal settings for the mode (the callback the capability parameters
 * carry); DLAA renders at the display size. */
int dlss_render_size(const vio_upscale_device *dev, int quality, int display_w, int display_h,
                     int *render_w, int *render_h, char *reason, size_t reason_len)
{
    char lib[512];
    DlssDev *D = dlss_ready(dev, lib, sizeof(lib), reason, reason_len);
    if (!D) return 0;
    if (quality == VIO_UPSCALE_NATIVE_AA) {
        *render_w = display_w;
        *render_h = display_h;
        return 1;
    }
    void *cb = nullptr;
    NVSDK_NGX_Parameter_GetVoidPointer(D->caps, NVSDK_NGX_Parameter_DLSSOptimalSettingsCallback, &cb);
    if (!cb) {
        snprintf(reason, reason_len, "the DLSS runtime offers no optimal settings (out of date)");
        return 0;
    }
    NVSDK_NGX_Parameter_SetUI(D->caps, NVSDK_NGX_Parameter_Width, (unsigned int)display_w);
    NVSDK_NGX_Parameter_SetUI(D->caps, NVSDK_NGX_Parameter_Height, (unsigned int)display_h);
    NVSDK_NGX_Parameter_SetI(D->caps, NVSDK_NGX_Parameter_PerfQualityValue, dlss_quality(quality));
    NVSDK_NGX_Parameter_SetI(D->caps, NVSDK_NGX_Parameter_RTXValue, 0);
    NVSDK_NGX_Result rc = reinterpret_cast<PFN_NVSDK_NGX_DLSS_GetOptimalSettingsCallback>(cb)(D->caps);
    unsigned int w = 0, h = 0;
    if (NVSDK_NGX_SUCCEED(rc)) {
        NVSDK_NGX_Parameter_GetUI(D->caps, NVSDK_NGX_Parameter_OutWidth, &w);
        NVSDK_NGX_Parameter_GetUI(D->caps, NVSDK_NGX_Parameter_OutHeight, &h);
    }
    if (NVSDK_NGX_FAILED(rc) || w == 0 || h == 0) {
        snprintf(reason, reason_len, "DLSS offers no %s mode at %dx%d", dlss_quality_name(quality), display_w, display_h);
        return 0;
    }
    *render_w = (int)w;
    *render_h = (int)h;
    return 1;
}

struct DlssCtx {
    const VioDlssApi *api = nullptr;
    DlssDev *D = nullptr;
    NVSDK_NGX_Parameter *params = nullptr;
    NVSDK_NGX_Handle *handle = nullptr;
    vio_upscale_create_desc desc{};
    char version[64] = "";
    char library[512] = "";
};

void *dlss_create(const vio_upscale_device *dev, const vio_upscale_create_desc *d, char *reason, size_t reason_len)
{
    char lib[512];
    DlssDev *D = dlss_ready(dev, lib, sizeof(lib), reason, reason_len);
    if (!D) return nullptr;
    unsigned int preset = NVSDK_NGX_DLSS_Hint_Render_Preset_Default;
    if (d->preset && !dlss_preset(d->preset, &preset)) {
        snprintf(reason, reason_len, "DLSS has no preset '%c' (j, k, l, m; e and f are deprecated)", d->preset);
        return nullptr;
    }
    if (!dev->command_list) {
        snprintf(reason, reason_len, "no command list to create the DLSS feature on");
        return nullptr;
    }
    DlssCtx *c = new (std::nothrow) DlssCtx();
    if (!c) { snprintf(reason, reason_len, "out of memory"); return nullptr; }
    c->api = dlss_api(dev->api);
    c->D = D;
    c->desc = *d;
    dlss_file_version(lib, c->version, sizeof(c->version));
    snprintf(c->library, sizeof(c->library), "%s", lib);
    NVSDK_NGX_Result rc = c->api->allocate_parameters(&c->params);
    if (NVSDK_NGX_FAILED(rc) || !c->params) {
        snprintf(reason, reason_len, "NGX could not allocate parameters (%s)", ngx_rc(rc));
        delete c;
        return nullptr;
    }
    /* Free the feature's GPU memory on release, not at shutdown: resize and
     * settings changes create a new feature each time. */
    NVSDK_NGX_Parameter_SetI(c->params, NVSDK_NGX_Parameter_FreeMemOnReleaseFeature, 1);
    /* The model: NGX's default per mode unless the caller chose one. */
    if (preset != NVSDK_NGX_DLSS_Hint_Render_Preset_Default) {
        const char *const modes[] = {
            NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_DLAA, NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Quality,
            NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Balanced, NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Performance,
            NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_UltraPerformance, NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_UltraQuality,
        };
        for (const char *m : modes) NVSDK_NGX_Parameter_SetUI(c->params, m, preset);
    }
    NVSDK_NGX_DLSS_Create_Params cp;
    memset(&cp, 0, sizeof(cp));
    cp.Feature.InWidth = (unsigned int)d->render_width;
    cp.Feature.InHeight = (unsigned int)d->render_height;
    cp.Feature.InTargetWidth = (unsigned int)d->display_width;
    cp.Feature.InTargetHeight = (unsigned int)d->display_height;
    cp.Feature.InPerfQualityValue = dlss_quality(d->quality);
    int flags = NVSDK_NGX_DLSS_Feature_Flags_MVLowRes;   /* motion at render resolution */
    if (d->flags & VIO_UPSCALE_FLAG_HDR)            flags |= NVSDK_NGX_DLSS_Feature_Flags_IsHDR;
    if (d->flags & VIO_UPSCALE_FLAG_MV_JITTERED)    flags |= NVSDK_NGX_DLSS_Feature_Flags_MVJittered;
    if (d->flags & VIO_UPSCALE_FLAG_DEPTH_INVERTED) flags |= NVSDK_NGX_DLSS_Feature_Flags_DepthInverted;
    if (d->flags & VIO_UPSCALE_FLAG_AUTO_EXPOSURE)  flags |= NVSDK_NGX_DLSS_Feature_Flags_AutoExposure;
    cp.InFeatureCreateFlags = flags;
    rc = c->api->create(dev, c->params, &cp, &c->handle);
    if (NVSDK_NGX_FAILED(rc) || !c->handle) {
        snprintf(reason, reason_len, "DLSS feature creation failed (%s) for %dx%d -> %dx%d", ngx_rc(rc),
                 d->render_width, d->render_height, d->display_width, d->display_height);
        c->api->destroy_parameters(c->params);
        delete c;
        return nullptr;
    }
    return c;
}

int dlss_dispatch(void *ctx, const vio_upscale_native_dispatch *nd, char *err, size_t err_len)
{
    DlssCtx *c = static_cast<DlssCtx *>(ctx);
    const vio_upscale_dispatch_desc *d = nd->desc;
    NVSDK_NGX_Result rc = c->api->evaluate(nd, c->handle, c->params, d->jitter_x, d->jitter_y);
    if (NVSDK_NGX_FAILED(rc)) {
        snprintf(err, err_len, "DLSS evaluation failed (%s)", ngx_rc(rc));
        return -1;
    }
    return 0;
}

int dlss_query(void *ctx, vio_upscale_query *q)
{
    DlssCtx *c = static_cast<DlssCtx *>(ctx);
    snprintf(q->version, sizeof(q->version), "%s", c->version);
    snprintf(q->library, sizeof(q->library), "%s", c->library);
    snprintf(q->driver, sizeof(q->driver), "%s", c->D->driver);
    /* VRAM DLSS holds on the device (all its features), from the stats callback. */
    void *cb = nullptr;
    if (c->D->caps) NVSDK_NGX_Parameter_GetVoidPointer(c->D->caps, NVSDK_NGX_Parameter_DLSSGetStatsCallback, &cb);
    if (cb && NVSDK_NGX_SUCCEED(reinterpret_cast<PFN_NVSDK_NGX_DLSS_GetStatsCallback>(cb)(c->D->caps))) {
        unsigned long long bytes = 0;
        NVSDK_NGX_Parameter_GetULL(c->D->caps, NVSDK_NGX_Parameter_SizeInBytes, &bytes);
        q->gpu_memory = bytes;
    }
    return 0;
}

void dlss_destroy(void *ctx)
{
    DlssCtx *c = static_cast<DlssCtx *>(ctx);
    if (!c) return;
    if (c->handle) c->api->release(c->handle);
    if (c->params) c->api->destroy_parameters(c->params);
    delete c;
}

void dlss_device_release(const vio_upscale_device *dev)
{
    DlssDev *D = dlss_dev(dev, 0);
    if (!D) return;
    const VioDlssApi *api = dlss_api(D->api);
    if (D->state > 0 && api) dlss_drop(D, api, dev);
    *D = DlssDev();
}

/* NGX's instance / device extensions - only when the runtime is there, so a
 * device without DLSS keeps its extension list. */
int dlss_vk_extensions(void *physical_device, const char **names, int max)
{
    if (!vio_dlss_api_vk || !vio_dlss_api_vk->vk_extensions) return 0;
    char lib[512], reason[512];
    if (!vio_upscale_find_file("vio.dlss_path", "VIO_DLSS_PATH", DLSS_FILE, lib, sizeof(lib), reason, sizeof(reason))) return 0;
    return vio_dlss_api_vk->vk_extensions(physical_device, names, max);
}

} // namespace

extern "C" const vio_upscale_provider vio_upscale_provider_dlss = {
    VIO_UPSCALER_DLSS,
    "dlss",
    VIO_UPSCALE_PROVIDER_CREATE_COMMANDS,
    dlss_supported,
    dlss_create,
    dlss_dispatch,
    dlss_query,
    dlss_destroy,
    nullptr,   /* vk_device_needs: NGX names extensions (vk_extensions) */
    dlss_render_size,
    dlss_device_release,
    dlss_vk_extensions,
};

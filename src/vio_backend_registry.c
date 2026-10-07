/*
 * php-vio - Backend registry implementation
 */

#include "vio_backend_registry.h"
#include <string.h>
#include <stdio.h>

static const vio_backend *backends[VIO_MAX_BACKENDS];
static int backend_count = 0;

void vio_backend_registry_init(void)
{
    memset(backends, 0, sizeof(backends));
    backend_count = 0;
}

void vio_backend_registry_shutdown(void)
{
    backend_count = 0;
}

int vio_register_backend(const vio_backend *backend)
{
    if (!backend || !backend->name) {
        return -1;
    }

    if (backend->api_version != VIO_BACKEND_API_VERSION) {
        return -1;
    }

    if (backend_count >= VIO_MAX_BACKENDS) {
        return -1;
    }

    /* Check for duplicate registration */
    for (int i = 0; i < backend_count; i++) {
        if (strcmp(backends[i]->name, backend->name) == 0) {
            return -1;
        }
    }

    backends[backend_count++] = backend;
    return 0;
}

const vio_backend *vio_find_backend(const char *name)
{
    if (!name) {
        return NULL;
    }

    for (int i = 0; i < backend_count; i++) {
        if (strcmp(backends[i]->name, name) == 0) {
            return backends[i];
        }
    }

    return NULL;
}

const vio_backend *vio_get_auto_backend(void)
{
    return vio_get_auto_backend_skip(NULL, 0);
}

static int vio_backend_skipped(const vio_backend *b, const vio_backend **skip, int skip_count)
{
    for (int i = 0; i < skip_count; i++) if (skip[i] == b) return 1;
    return 0;
}

/* 'auto' minus the backends vio_create already failed to open (GAP-PHASE5 Block 10c). */
const vio_backend *vio_get_auto_backend_skip(const vio_backend **skip, int skip_count)
{
    /* Platform-specific priority:
     * macOS:   metal > opengl (Vulkan via MoltenVK is opt-in, not auto)
     * Windows: d3d12 > d3d11 > vulkan > opengl
     * Linux:   vulkan > opengl
     */
#ifdef __APPLE__
    const char *priority[] = {"metal", "opengl"};
    int priority_count = 2;
#elif defined(_WIN32)
    const char *priority[] = {"d3d12", "d3d11", "vulkan", "opengl"};
    int priority_count = 4;
#else
    const char *priority[] = {"vulkan", "opengl"};
    int priority_count = 2;
#endif
    /* First pass: the highest-priority backend that can actually draw 3D.
     * A registered backend whose supports_feature() reports no
     * VIO_FEATURE_3D_PIPELINE (today: Vulkan — vulkan_create_pipeline returns
     * NULL, so vio_pipeline()/vio_draw() would silently render nothing) must not
     * win "auto" over one that can, or a Linux box with a Vulkan ICD would get a
     * 2D-only context for a 3D game. supports_feature is a static table on every
     * backend, so this costs nothing and needs no device. Once a backend gains a
     * 3D pipeline it automatically becomes eligible again. */
    /* Pass 0 (GAP-PHASE5 Block 10): prefer a backend with the full 3D feature set an
     * engine renderer needs (HDR / depth / cube targets, MRT, cubemaps, instancing,
     * texture arrays), so a backend whose 3D path is still growing (Vulkan before
     * Block 10c) never wins 'auto' over a complete one. */
    for (int p = 0; p < priority_count; p++) {
        const vio_backend *b = vio_find_backend(priority[p]);
        if (b && !vio_backend_skipped(b, skip, skip_count) && b->supports_feature && b->supports_feature(VIO_FEATURE_3D_PIPELINE)
            && b->supports_feature(VIO_FEATURE_INSTANCED_DRAW) && b->supports_feature(VIO_FEATURE_RENDER_TARGET_HDR)
            && b->supports_feature(VIO_FEATURE_RENDER_TARGET_DEPTH) && b->supports_feature(VIO_FEATURE_RENDER_TARGET_CUBE)
            && b->supports_feature(VIO_FEATURE_MRT) && b->supports_feature(VIO_FEATURE_CUBEMAP)
            && b->supports_feature(VIO_FEATURE_TEXTURE_ARRAY)) {
            return b;
        }
    }
    for (int p = 0; p < priority_count; p++) {
        const vio_backend *b = vio_find_backend(priority[p]);
        if (b && !vio_backend_skipped(b, skip, skip_count) && b->supports_feature && b->supports_feature(VIO_FEATURE_3D_PIPELINE)) {
            return b;
        }
    }

    /* Second pass: plain priority order (2D-only deployments, e.g. a build with
     * only the Vulkan backend). */
    for (int p = 0; p < priority_count; p++) {
        const vio_backend *b = vio_find_backend(priority[p]);
        if (b && !vio_backend_skipped(b, skip, skip_count)) {
            return b;
        }
    }

    /* A retry (skip list) stays inside the platform priority list: never hand a
     * failed 'auto' over to the null backend. */
    if (skip_count > 0) return NULL;

    /* Fall back to the first registered backend that has a 3D pipeline, then
     * to the first registered backend at all. */
    for (int i = 0; i < backend_count; i++) {
        const vio_backend *b = backends[i];
        if (!vio_backend_skipped(b, skip, skip_count) && b->supports_feature && b->supports_feature(VIO_FEATURE_3D_PIPELINE)) {
            return b;
        }
    }
    for (int i = 0; i < backend_count; i++) {
        if (!vio_backend_skipped(backends[i], skip, skip_count)) return backends[i];
    }

    return NULL;
}

/* ── Scoring for 'auto' with prefer / require (OPEN-ITEMS-PLAN A7) ──────
 *
 * score = vendor profile (200 per rank step) + device type (discrete +60,
 * integrated +30, software -1000: WARP / llvmpipe / lavapipe only as the last
 * resort) + feature points weighted by `prefer` (compat: none). The profile
 * follows the vendor of the best hardware adapter on the machine. */
static const char *const vio_prof_nvidia[]       = { "d3d12", "vulkan", "d3d11", "opengl", NULL };
static const char *const vio_prof_amd[]          = { "vulkan", "d3d12", "d3d11", "opengl", NULL };
static const char *const vio_prof_intel_old[]    = { "d3d11", "d3d12", "vulkan", "opengl", NULL };
static const char *const vio_prof_windows[]      = { "d3d12", "d3d11", "vulkan", "opengl", NULL };
static const char *const vio_prof_win_compat[]   = { "d3d11", "opengl", "d3d12", "vulkan", NULL };
static const char *const vio_prof_macos[]        = { "metal", "opengl", "vulkan", NULL };
static const char *const vio_prof_linux[]        = { "vulkan", "opengl", NULL };
static const char *const vio_prof_linux_compat[] = { "opengl", "vulkan", NULL };

int vio_select_host_platform(void)
{
#ifdef __APPLE__
    return VIO_PLATFORM_MACOS;
#elif defined(_WIN32)
    return VIO_PLATFORM_WINDOWS;
#else
    return VIO_PLATFORM_LINUX;
#endif
}

static const char *const *vio_select_profile(int platform, int prefer, const vio_adapter_info *sys)
{
    if (platform == VIO_PLATFORM_MACOS) return vio_prof_macos;
    if (platform == VIO_PLATFORM_LINUX) return prefer == VIO_PREFER_COMPAT ? vio_prof_linux_compat : vio_prof_linux;
    if (prefer == VIO_PREFER_COMPAT) return vio_prof_win_compat;
    if (sys) {
        switch (sys->vendor_id) {
        case 0x10DE: return vio_prof_nvidia;
        case 0x1002: case 0x1022: return vio_prof_amd;
        /* Intel: Arc / Xe (mesh shaders) like NVIDIA, older iGPUs run D3D11 best. */
        case 0x8086: return (sys->features & VIO_FEATURE_BIT(VIO_FEATURE_MESH_SHADER)) ? vio_prof_nvidia : vio_prof_intel_old;
        default: break;
        }
    }
    return vio_prof_windows;
}

static int vio_select_weight(int prefer, int f)
{
    if (prefer == VIO_PREFER_COMPAT) return 0;
    if (prefer == VIO_PREFER_QUALITY) {
        switch (f) {
        case VIO_FEATURE_RAY_QUERY:          return 15;
        case VIO_FEATURE_MESH_SHADER:        return 15;
        case VIO_FEATURE_RAYTRACING:         return 10;
        case VIO_FEATURE_SHADING_RATE:       return 10;
        case VIO_FEATURE_BINDLESS:           return 10;
        case VIO_FEATURE_SHADING_RATE_IMAGE: return 5;
        default:                             return 1;
        }
    }
    switch (f) {
    case VIO_FEATURE_SUBGROUP:      return 15;
    case VIO_FEATURE_BINDLESS:      return 15;
    case VIO_FEATURE_INDIRECT_DRAW: return 10;
    case VIO_FEATURE_MULTIVIEW:     return 5;
    default:                        return 1;
    }
}

static int vio_select_type_rank(const char *t)
{
    if (!t) return 2;
    if (strcmp(t, "discrete") == 0) return 0;
    if (strcmp(t, "integrated") == 0) return 1;
    if (strcmp(t, "software") == 0) return 3;
    return 2;
}

void vio_select_rank(vio_select_candidate *c, int n, int platform, int prefer, uint64_t require)
{
    const vio_adapter_info *sys = NULL;
    int sys_rank = 3;
    for (int i = 0; i < n; i++) {
        if (!c[i].has_adapter) continue;
        int r = vio_select_type_rank(c[i].adapter.device_type);
        if (r < sys_rank) { sys_rank = r; sys = &c[i].adapter; }
    }
    const char *const *prof = vio_select_profile(platform, prefer, sys);
    int plen = 0;
    while (prof[plen]) plen++;
    for (int i = 0; i < n; i++) {
        vio_select_candidate *k = &c[i];
        int s = 0;
        for (int p = 0; p < plen; p++) {
            if (strcmp(prof[p], k->backend) == 0) { s += (plen - p) * 200; break; }
        }
        if (k->has_adapter) {
            int r = vio_select_type_rank(k->adapter.device_type);
            s += r == 0 ? 60 : r == 1 ? 30 : r == 3 ? -1000 : 0;
            if (!(k->adapter.features & VIO_FEATURE_BIT(VIO_FEATURE_3D_PIPELINE))) s -= 500;
            for (int f = 0; f < 64; f++)
                if (k->adapter.features & VIO_FEATURE_BIT(f)) s += vio_select_weight(prefer, f);
        }
        k->score = s;
        k->eligible = 1;
        k->reason[0] = '\0';
        /* Unknown before a context (no adapter info): vio_create checks the
         * flags once the device is open. */
        for (int f = 0; f < 64 && k->has_adapter; f++) {
            if (!(require & VIO_FEATURE_BIT(f)) || (k->adapter.features & VIO_FEATURE_BIT(f))) continue;
            if (k->be && k->be->supports_feature && k->be->supports_feature((vio_feature)f)) continue;
            k->eligible = 0;
            snprintf(k->reason, sizeof(k->reason), "feature %d not supported", f);
            break;
        }
    }
    /* Stable insertion sort: eligible first, then the higher score. */
    for (int i = 1; i < n; i++) {
        vio_select_candidate tmp = c[i];
        int j = i - 1;
        while (j >= 0 && (c[j].eligible < tmp.eligible || (c[j].eligible == tmp.eligible && c[j].score < tmp.score))) {
            c[j + 1] = c[j];
            j--;
        }
        c[j + 1] = tmp;
    }
}

int vio_backend_count(void)
{
    return backend_count;
}

const char *vio_get_backend_name(int index)
{
    if (index < 0 || index >= backend_count) return NULL;
    return backends[index]->name;
}

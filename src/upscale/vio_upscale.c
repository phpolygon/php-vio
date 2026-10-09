/*
 * php-vio - Native temporal upscalers: shared helpers, runtime library loading,
 * provider dispatch and the VioUpscaler object (TEMPORAL-S3).
 *
 * Nothing here knows a graphics API or an SDK: backends hand in their native
 * device / images (vio_upscale.h), providers (vio_upscale_ffx.c, ...) do the
 * SDK calls.
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "php.h"
#include "php_ini.h"
#include "vio_upscaler.h"
#include "../../include/vio_backend.h"

#include <math.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <dlfcn.h>
#endif

/* ── Quality modes ──────────────────────────────────────────────────── */

const char *vio_upscale_provider_name(int provider)
{
    switch (provider) {
        case VIO_UPSCALER_FSR3: return "fsr3";
        case VIO_UPSCALER_DLSS: return "dlss";
        case VIO_UPSCALER_XESS: return "xess";
        default:                return "unknown";
    }
}

float vio_upscale_quality_ratio(int quality)
{
    switch (quality) {
        case VIO_UPSCALE_NATIVE_AA:         return 1.0f;
        case VIO_UPSCALE_QUALITY:           return 1.5f;
        case VIO_UPSCALE_BALANCED:          return 1.7f;
        case VIO_UPSCALE_PERFORMANCE:       return 2.0f;
        case VIO_UPSCALE_ULTRA_PERFORMANCE: return 3.0f;
        default:                            return 1.0f;
    }
}

void vio_upscale_render_size(int quality, int display_w, int display_h, int *render_w, int *render_h)
{
    float r = vio_upscale_quality_ratio(quality);
    int w = (int)((float)display_w / r), h = (int)((float)display_h / r);
    *render_w = w > 0 ? w : 1;
    *render_h = h > 0 ? h : 1;
}

/* FSR's jitter sequence length: 8 * (display / render)^2 (ffxFsr3UpscalerGetJitterPhaseCount). */
int vio_upscale_jitter_phases(int render_w, int display_w)
{
    if (render_w <= 0 || display_w <= 0) return 8;
    float r = (float)display_w / (float)render_w;
    int n = (int)(8.0f * r * r);
    return n > 0 ? n : 1;
}

/* ── Runtime libraries ──────────────────────────────────────────────── */

static int vio_upscale_try(const char *dir, const char *file, void **module, char *path, size_t path_len)
{
    char full[1024];
    size_t dl = dir ? strlen(dir) : 0;
    if (dl == 0) return 0;
    if (dl >= 5 && (strcmp(dir + dl - 4, ".dll") == 0 || strcmp(dir + dl - 3, ".so") == 0 || strstr(dir, ".so.") != NULL)) {
        snprintf(full, sizeof(full), "%s", dir);   /* the file itself */
    } else {
        char last = dir[dl - 1];
        snprintf(full, sizeof(full), "%s%s%s", dir, (last == '/' || last == '\\') ? "" : "/", file);
    }
#ifdef _WIN32
    for (char *c = full; *c; c++) if (*c == '/') *c = '\\';
    if (GetFileAttributesA(full) == INVALID_FILE_ATTRIBUTES) return 0;
    /* Its own dependencies resolve from its directory first. */
    HMODULE m = LoadLibraryExA(full, NULL, LOAD_WITH_ALTERED_SEARCH_PATH);
#else
    void *m = dlopen(full, RTLD_NOW | RTLD_LOCAL);
#endif
    if (!m) return -1;
    *module = (void *)m;
    snprintf(path, path_len, "%s", full);
    return 1;
}

static void vio_upscale_dirname(char *p)
{
    char *s1 = strrchr(p, '\\'), *s2 = strrchr(p, '/');
    char *s = s1 > s2 ? s1 : s2;
    if (s) *s = '\0'; else p[0] = '\0';
}

void *vio_upscale_load_library(const char *ini_key, const char *env_key, const char *file,
                               char *path, size_t path_len, char *reason, size_t reason_len)
{
    void *module = NULL;
    path[0] = '\0';
    reason[0] = '\0';

    /* An explicit place is the only one searched: a test or a game pins it. */
    const char *expl = ini_key ? zend_ini_string_ex((char *)ini_key, strlen(ini_key), 0, NULL) : NULL;
    const char *src = ini_key;
    if (!expl || !expl[0]) { expl = env_key ? getenv(env_key) : NULL; src = env_key; }
    if (expl && expl[0]) {
        int r = vio_upscale_try(expl, file, &module, path, path_len);
        if (r == 1) return module;
        snprintf(reason, reason_len, "%s %s (%s = '%s')", file,
                 r < 0 ? "could not be loaded" : "not found", src, expl);
        return NULL;
    }

    char dir[1024];
#ifdef _WIN32
    /* Next to php.exe (or the game's executable). */
    DWORD n = GetModuleFileNameA(NULL, dir, (DWORD)sizeof(dir));
    if (n > 0 && n < sizeof(dir)) {
        vio_upscale_dirname(dir);
        if (vio_upscale_try(dir, file, &module, path, path_len) == 1) return module;
    }
    /* Next to php_vio.dll. */
    HMODULE self = NULL;
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (LPCSTR)(void *)&vio_upscale_load_library, &self) && self) {
        n = GetModuleFileNameA(self, dir, (DWORD)sizeof(dir));
        if (n > 0 && n < sizeof(dir)) {
            vio_upscale_dirname(dir);
            if (vio_upscale_try(dir, file, &module, path, path_len) == 1) return module;
        }
    }
    /* System search path (PATH). */
    HMODULE m = LoadLibraryA(file);
    if (m) {
        module = (void *)m;
        if (GetModuleFileNameA(m, path, (DWORD)path_len) == 0) snprintf(path, path_len, "%s", file);
        return module;
    }
#else
    Dl_info info;
    if (dladdr((void *)&vio_upscale_load_library, &info) && info.dli_fname) {
        snprintf(dir, sizeof(dir), "%s", info.dli_fname);
        vio_upscale_dirname(dir);
        if (vio_upscale_try(dir, file, &module, path, path_len) == 1) return module;
    }
    module = dlopen(file, RTLD_NOW | RTLD_LOCAL);
    if (module) { snprintf(path, path_len, "%s", file); return module; }
#endif
    snprintf(reason, reason_len, "%s not found (next to the PHP executable, next to php_vio, on PATH; or set %s / %s)",
             file, ini_key ? ini_key : "-", env_key ? env_key : "-");
    return NULL;
}

void *vio_upscale_symbol(void *module, const char *name)
{
    if (!module) return NULL;
#ifdef _WIN32
    return (void *)GetProcAddress((HMODULE)module, name);
#else
    return dlsym(module, name);
#endif
}

/* ── Host allocations (counted) ─────────────────────────────────────── */

static int64_t vio_upscale_host_total = 0;

/* 16-byte header keeps the size and the alignment malloc gives. */
void *vio_upscale_host_alloc(void *user, uint64_t size)
{
    (void)user;
    unsigned char *p = (unsigned char *)malloc((size_t)size + 16);
    if (!p) return NULL;
    memcpy(p, &size, sizeof(size));
    vio_upscale_host_total += (int64_t)size;
    return p + 16;
}

void vio_upscale_host_free(void *user, void *mem)
{
    (void)user;
    if (!mem) return;
    unsigned char *p = (unsigned char *)mem - 16;
    uint64_t size;
    memcpy(&size, p, sizeof(size));
    vio_upscale_host_total -= (int64_t)size;
    free(p);
}

int64_t vio_upscale_host_bytes(void)
{
    return vio_upscale_host_total;
}

/* ── Providers ──────────────────────────────────────────────────────── */

const vio_upscale_provider *vio_upscale_provider_get(int provider)
{
    switch (provider) {
#ifdef HAVE_FFX
        case VIO_UPSCALER_FSR3: return &vio_upscale_provider_ffx;
#endif
        default: return NULL;
    }
}

unsigned vio_upscale_vk_device_needs(void *physical_device)
{
    unsigned need = 0;
    for (int p = 1; p <= VIO_UPSCALER_COUNT; p++) {
        const vio_upscale_provider *pr = vio_upscale_provider_get(p);
        if (pr && pr->vk_device_needs) need |= pr->vk_device_needs(physical_device);
    }
    return need;
}

typedef struct _vio_upscale_instance {
    const vio_upscale_provider *provider;
    void                       *ctx;
    vio_upscale_create_desc     desc;
} vio_upscale_instance;

int vio_upscale_supported_on(const vio_upscale_device *dev, int provider, char *reason, size_t reason_len, vio_upscale_query *q)
{
    const vio_upscale_provider *p = vio_upscale_provider_get(provider);
    if (q) memset(q, 0, sizeof(*q));
    if (!p) {
        switch (provider) {
            case VIO_UPSCALER_FSR3:
                snprintf(reason, reason_len, "php-vio was built without FidelityFX (configure --with-ffx)");
                break;
            case VIO_UPSCALER_DLSS:
                snprintf(reason, reason_len, "php-vio was built without DLSS");
                break;
            default:
                snprintf(reason, reason_len, "php-vio was built without XeSS");
                break;
        }
        return 0;
    }
    vio_upscale_query tmp;
    return p->supported(dev, reason, reason_len, q ? q : &tmp);
}

void *vio_upscale_create_on(const vio_upscale_device *dev, const vio_upscale_create_desc *desc, char *reason, size_t reason_len)
{
    if (!vio_upscale_supported_on(dev, desc->provider, reason, reason_len, NULL)) return NULL;
    const vio_upscale_provider *p = vio_upscale_provider_get(desc->provider);
    void *ctx = p->create(dev, desc, reason, reason_len);
    if (!ctx) return NULL;
    vio_upscale_instance *in = (vio_upscale_instance *)calloc(1, sizeof(*in));
    if (!in) { p->destroy(ctx); snprintf(reason, reason_len, "out of memory"); return NULL; }
    in->provider = p;
    in->ctx = ctx;
    in->desc = *desc;
    return in;
}

int vio_upscale_dispatch_native(void *upscaler, const vio_upscale_native_dispatch *d, char *err, size_t err_len)
{
    vio_upscale_instance *in = (vio_upscale_instance *)upscaler;
    if (!in || !in->ctx) { snprintf(err, err_len, "upscaler destroyed"); return -1; }
    return in->provider->dispatch(in->ctx, d, err, err_len);
}

int vio_upscale_query_instance(void *upscaler, vio_upscale_query *q)
{
    vio_upscale_instance *in = (vio_upscale_instance *)upscaler;
    memset(q, 0, sizeof(*q));
    if (!in || !in->ctx) return -1;
    q->render_width = in->desc.render_width;
    q->render_height = in->desc.render_height;
    q->display_width = in->desc.display_width;
    q->display_height = in->desc.display_height;
    q->jitter_phases = vio_upscale_jitter_phases(in->desc.render_width, in->desc.display_width);
    return in->provider->query ? in->provider->query(in->ctx, q) : 0;
}

const vio_upscale_create_desc *vio_upscale_instance_desc(void *upscaler)
{
    vio_upscale_instance *in = (vio_upscale_instance *)upscaler;
    return in ? &in->desc : NULL;
}

void vio_upscale_destroy_instance(void *upscaler)
{
    vio_upscale_instance *in = (vio_upscale_instance *)upscaler;
    if (!in) return;
    if (in->ctx) in->provider->destroy(in->ctx);
    free(in);
}

/* ── VioUpscaler ────────────────────────────────────────────────────── */

zend_class_entry *vio_upscaler_ce = NULL;
static zend_object_handlers vio_upscaler_handlers;
static vio_upscaler_object *vio_upscaler_live = NULL;

void vio_upscaler_track(vio_upscaler_object *u)
{
    u->live_prev = NULL;
    u->live_next = vio_upscaler_live;
    if (vio_upscaler_live) vio_upscaler_live->live_prev = u;
    vio_upscaler_live = u;
}

static void vio_upscaler_untrack(vio_upscaler_object *u)
{
    if (u->live_prev) u->live_prev->live_next = u->live_next;
    else if (vio_upscaler_live == u) vio_upscaler_live = u->live_next;
    if (u->live_next) u->live_next->live_prev = u->live_prev;
    u->live_prev = u->live_next = NULL;
}

void vio_upscaler_release(vio_upscaler_object *u)
{
    if (!u->valid) return;
    const vio_backend *be = (const vio_backend *)u->backend;
    if (be && be->upscaler_destroy && u->handle) be->upscaler_destroy(u->handle);
    u->handle = NULL;
    u->valid = 0;
    vio_upscaler_untrack(u);
}

void vio_upscaler_sweep(const void *backend)
{
    vio_upscaler_object *u = vio_upscaler_live;
    while (u) {
        vio_upscaler_object *next = u->live_next;
        if (u->backend == backend) vio_upscaler_release(u);
        u = next;
    }
}

int vio_upscaler_live_count(const void *backend)
{
    int n = 0;
    for (vio_upscaler_object *u = vio_upscaler_live; u; u = u->live_next) {
        if (u->backend == backend) n++;
    }
    return n;
}

static zend_object *vio_upscaler_create_object(zend_class_entry *ce)
{
    vio_upscaler_object *u = zend_object_alloc(sizeof(vio_upscaler_object), ce);
    u->handle = NULL;
    u->backend = NULL;
    memset(&u->desc, 0, sizeof(u->desc));
    u->valid = 0;
    u->live_prev = u->live_next = NULL;
    zend_object_std_init(&u->std, ce);
    object_properties_init(&u->std, ce);
    u->std.handlers = &vio_upscaler_handlers;
    return &u->std;
}

static void vio_upscaler_free_object(zend_object *obj)
{
    vio_upscaler_object *u = vio_upscaler_from_obj(obj);
    vio_upscaler_release(u);
    zend_object_std_dtor(&u->std);
}

void vio_upscaler_register(void)
{
    zend_class_entry ce;
    INIT_CLASS_ENTRY(ce, "VioUpscaler", NULL);
    vio_upscaler_ce = zend_register_internal_class(&ce);
    vio_upscaler_ce->ce_flags |= ZEND_ACC_FINAL | ZEND_ACC_NO_DYNAMIC_PROPERTIES | ZEND_ACC_NOT_SERIALIZABLE;
    vio_upscaler_ce->create_object = vio_upscaler_create_object;

    memcpy(&vio_upscaler_handlers, &std_object_handlers, sizeof(zend_object_handlers));
    vio_upscaler_handlers.offset    = XtOffsetOf(vio_upscaler_object, std);
    vio_upscaler_handlers.free_obj  = vio_upscaler_free_object;
    vio_upscaler_handlers.clone_obj = NULL;
}

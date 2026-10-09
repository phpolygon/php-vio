/*
 * php-vio - Native temporal upscalers: shared helpers, runtime library loading,
 * provider dispatch and the VioUpscaler object (TEMPORAL-S3).
 *
 * Nothing here knows a graphics API or an SDK: backends hand in their native
 * device / images (vio_upscale.h), providers (vio_upscale_ffx.c, ...) do the
 * SDK calls.
 */

#if !defined(_WIN32) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE   /* dladdr / Dl_info (glibc) */
#endif

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "php.h"
#include "php_ini.h"
#include "vio_upscaler.h"
#include "../../php_vio.h"
#include "../../include/vio_backend.h"

#include <math.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <dlfcn.h>
#include <dirent.h>
#include <unistd.h>
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

/* Whether `dir` holds `file` (or is that file). Elsewhere than Windows `file`
 * is a prefix: versioned libraries (libnvidia-ngx-dlss.so.310.9.1). */
static int vio_upscale_has_file(const char *dir, const char *file, char *path, size_t path_len)
{
    size_t dl = dir ? strlen(dir) : 0;
    if (dl == 0) return 0;
    char full[1024];
    const char *base = strrchr(dir, '/');
#ifdef _WIN32
    const char *bs = strrchr(dir, '\\');
    if (bs && (!base || bs > base)) base = bs;
#endif
    base = base ? base + 1 : dir;
    if (strncmp(base, file, strlen(file)) == 0) {   /* the file itself */
        snprintf(full, sizeof(full), "%s", dir);
#ifdef _WIN32
        if (GetFileAttributesA(full) == INVALID_FILE_ATTRIBUTES) return 0;
#else
        if (access(full, R_OK) != 0) return 0;
#endif
        snprintf(path, path_len, "%s", full);
        return 1;
    }
    char last = dir[dl - 1];
    const char *sep = (last == '/' || last == '\\') ? "" : "/";
#ifdef _WIN32
    snprintf(full, sizeof(full), "%s%s%s", dir, sep, file);
    for (char *c = full; *c; c++) if (*c == '/') *c = '\\';
    DWORD attr = GetFileAttributesA(full);
    if (attr == INVALID_FILE_ATTRIBUTES || (attr & FILE_ATTRIBUTE_DIRECTORY)) return 0;
    snprintf(path, path_len, "%s", full);
    return 1;
#else
    DIR *d = opendir(dir);
    if (!d) return 0;
    struct dirent *e;
    int found = 0;
    while (!found && (e = readdir(d)) != NULL) {
        if (strncmp(e->d_name, file, strlen(file)) == 0) {
            snprintf(path, path_len, "%s%s%s", dir, sep, e->d_name);
            found = 1;
        }
    }
    closedir(d);
    return found;
#endif
}

int vio_upscale_find_file(const char *ini_key, const char *env_key, const char *file,
                          char *path, size_t path_len, char *reason, size_t reason_len)
{
    path[0] = '\0';
    reason[0] = '\0';
    const char *expl = ini_key ? zend_ini_string_ex((char *)ini_key, strlen(ini_key), 0, NULL) : NULL;
    const char *src = ini_key;
    if (!expl || !expl[0]) { expl = env_key ? getenv(env_key) : NULL; src = env_key; }
    if (expl && expl[0]) {
        if (vio_upscale_has_file(expl, file, path, path_len)) return 1;
        snprintf(reason, reason_len, "%s not found (%s = '%s')", file, src, expl);
        return 0;
    }
    char dir[1024];
#ifdef _WIN32
    DWORD n = GetModuleFileNameA(NULL, dir, (DWORD)sizeof(dir));
    if (n > 0 && n < sizeof(dir)) {
        vio_upscale_dirname(dir);
        if (vio_upscale_has_file(dir, file, path, path_len)) return 1;
    }
    HMODULE self = NULL;
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (LPCSTR)(void *)&vio_upscale_find_file, &self) && self) {
        n = GetModuleFileNameA(self, dir, (DWORD)sizeof(dir));
        if (n > 0 && n < sizeof(dir)) {
            vio_upscale_dirname(dir);
            if (vio_upscale_has_file(dir, file, path, path_len)) return 1;
        }
    }
    char *part = NULL;
    if (SearchPathA(NULL, file, NULL, (DWORD)sizeof(dir), dir, &part) > 0) {
        snprintf(path, path_len, "%s", dir);
        return 1;
    }
#else
    Dl_info info;
    if (dladdr((void *)&vio_upscale_find_file, &info) && info.dli_fname) {
        snprintf(dir, sizeof(dir), "%s", info.dli_fname);
        vio_upscale_dirname(dir);
        if (vio_upscale_has_file(dir, file, path, path_len)) return 1;
    }
    const char *ldp = getenv("LD_LIBRARY_PATH");
    if (ldp) {
        char list[2048];
        snprintf(list, sizeof(list), "%s", ldp);
        for (char *tok = strtok(list, ":"); tok; tok = strtok(NULL, ":")) {
            if (vio_upscale_has_file(tok, file, path, path_len)) return 1;
        }
    }
#endif
    snprintf(reason, reason_len, "%s not found (next to the PHP executable, next to php_vio, on PATH; or set %s / %s)",
             file, ini_key ? ini_key : "-", env_key ? env_key : "-");
    return 0;
}

const char *vio_upscale_ini(const char *key)
{
    return zend_ini_string_ex((char *)key, strlen(key), 0, NULL);
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

/* ── Plugins (include/vio_upscale_plugin.h) ─────────────────────────── */

/* Messages of a plugin: errors / warnings as PHP warnings (the provider is
 * called on the PHP thread), the rest on stderr when VIO_UPSCALE_PLUGIN_LOG is set. */
static void vio_upscale_host_log(int level, const char *message)
{
    if (!message) return;
    if (level <= VIO_UPSCALE_LOG_WARNING) {
        php_error_docref(NULL, E_WARNING, "upscaler plugin: %s", message);
        return;
    }
    const char *dbg = getenv("VIO_UPSCALE_PLUGIN_LOG");
    if (dbg && dbg[0] && strcmp(dbg, "0") != 0) {
        fprintf(stderr, "[vio upscaler plugin] %s\n", message);
        fflush(stderr);
    }
}

typedef struct _vio_upscale_plugin_slot {
    int         id;              /* VIO_UPSCALER_* it provides */
    const char *file;            /* library file name */
    const char *ini_key;         /* explicit place (php.ini) */
    const char *env_key;         /* explicit place (environment) */
    void       *module;          /* loaded and accepted, kept for the process */
    const vio_upscale_provider *provider;
    char        path[1024];
    vio_upscale_host_api host;
} vio_upscale_plugin_slot;

#if defined(_WIN32)
#define VIO_DLSS_PLUGIN_FILE "vio_dlss.dll"
#else
#define VIO_DLSS_PLUGIN_FILE "libvio_dlss.so"
#endif

static vio_upscale_plugin_slot vio_upscale_plugins[] = {
    { VIO_UPSCALER_DLSS, VIO_DLSS_PLUGIN_FILE, "vio.dlss_plugin_path", "VIO_DLSS_PLUGIN", NULL, NULL, "", { 0 } },
};
#define VIO_UPSCALE_PLUGIN_SLOTS ((int)(sizeof(vio_upscale_plugins) / sizeof(vio_upscale_plugins[0])))

static vio_upscale_plugin_slot *vio_upscale_plugin_slot_of(int provider)
{
    for (int i = 0; i < VIO_UPSCALE_PLUGIN_SLOTS; i++) {
        if (vio_upscale_plugins[i].id == provider) return &vio_upscale_plugins[i];
    }
    return NULL;
}

static void vio_upscale_unload(void *module)
{
    if (!module) return;
#ifdef _WIN32
    FreeLibrary((HMODULE)module);
#else
    dlclose(module);
#endif
}

/* The slot's provider: loaded once and kept; a missing or refused library is
 * looked for again on the next call (a game may set the path later, a test
 * swaps it). NULL + reason, never a warning. */
static const vio_upscale_provider *vio_upscale_plugin_load(vio_upscale_plugin_slot *s, char *reason, size_t reason_len)
{
    if (s->provider) return s->provider;
    char path[1024], why[512];
    void *m = vio_upscale_load_library(s->ini_key, s->env_key, s->file, path, sizeof(path), why, sizeof(why));
    if (!m) {
        snprintf(reason, reason_len, "%s", why);
        return NULL;
    }
    vio_upscale_plugin_get_fn get = (vio_upscale_plugin_get_fn)vio_upscale_symbol(m, VIO_UPSCALE_PLUGIN_ENTRY);
    if (!get) {
        snprintf(reason, reason_len, "%s has no %s() - not a php-vio upscaler plugin", path, VIO_UPSCALE_PLUGIN_ENTRY);
        vio_upscale_unload(m);
        return NULL;
    }
    snprintf(s->path, sizeof(s->path), "%s", path);
    memset(&s->host, 0, sizeof(s->host));
    s->host.abi = VIO_UPSCALE_PLUGIN_ABI;
    s->host.size = (uint32_t)sizeof(s->host);
    s->host.host_version = PHP_VIO_VERSION;
    s->host.plugin_path = s->path;
    s->host.log = vio_upscale_host_log;
    s->host.alloc = vio_upscale_host_alloc;
    s->host.free = vio_upscale_host_free;
    s->host.ini = vio_upscale_ini;
    s->host.find_file = vio_upscale_find_file;
    s->host.load_library = vio_upscale_load_library;
    s->host.symbol = vio_upscale_symbol;

    const vio_upscale_provider *p = get(VIO_UPSCALE_PLUGIN_ABI, &s->host);
    if (!p) {
        snprintf(reason, reason_len, "%s does not offer plugin ABI %d (php-vio %s) - it needs a php-vio it was built for",
                 path, VIO_UPSCALE_PLUGIN_ABI, PHP_VIO_VERSION);
    } else if (p->abi != VIO_UPSCALE_PLUGIN_ABI) {
        snprintf(reason, reason_len, "%s was built for plugin ABI %u, php-vio %s has plugin ABI %d",
                 path, (unsigned)p->abi, PHP_VIO_VERSION, VIO_UPSCALE_PLUGIN_ABI);
    } else if (p->size < sizeof(vio_upscale_provider)) {
        snprintf(reason, reason_len, "%s hands a provider table of %u bytes, plugin ABI %d has %u",
                 path, (unsigned)p->size, VIO_UPSCALE_PLUGIN_ABI, (unsigned)sizeof(vio_upscale_provider));
    } else if (p->id != s->id) {
        snprintf(reason, reason_len, "%s provides '%s', not '%s'", path,
                 vio_upscale_provider_name(p->id), vio_upscale_provider_name(s->id));
    } else if (!p->supported || !p->create || !p->dispatch || !p->destroy) {
        snprintf(reason, reason_len, "%s lacks supported / create / dispatch / destroy", path);
    } else {
        s->module = m;
        s->provider = p;
        return p;
    }
    s->path[0] = '\0';
    vio_upscale_unload(m);
    return NULL;
}

const char *vio_upscale_plugin_path(int provider)
{
    vio_upscale_plugin_slot *s = vio_upscale_plugin_slot_of(provider);
    return s && s->provider ? s->path : "";
}

/* ── Providers ──────────────────────────────────────────────────────── */

const vio_upscale_provider *vio_upscale_provider_resolve(int provider, char *reason, size_t reason_len)
{
    reason[0] = '\0';
    vio_upscale_plugin_slot *s = vio_upscale_plugin_slot_of(provider);
    if (s) return vio_upscale_plugin_load(s, reason, reason_len);
    switch (provider) {
        case VIO_UPSCALER_FSR3:
#ifdef HAVE_FFX
            return &vio_upscale_provider_ffx;
#else
            snprintf(reason, reason_len, "php-vio was built without FidelityFX (configure --with-ffx)");
            return NULL;
#endif
        case VIO_UPSCALER_XESS:
            snprintf(reason, reason_len, "php-vio has no XeSS provider yet");
            return NULL;
        default:
            snprintf(reason, reason_len, "unknown upscaler provider %d", provider);
            return NULL;
    }
}

const vio_upscale_provider *vio_upscale_provider_get(int provider)
{
    char reason[512];
    return vio_upscale_provider_resolve(provider, reason, sizeof(reason));
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

int vio_upscale_vk_extensions(void *physical_device, const char **names, int max)
{
    int n = 0;
    for (int p = 1; p <= VIO_UPSCALER_COUNT; p++) {
        const vio_upscale_provider *pr = vio_upscale_provider_get(p);
        if (!pr || !pr->vk_extensions) continue;
        const char *mine[32];
        int m = pr->vk_extensions(physical_device, mine, 32);
        for (int i = 0; i < m && n < max; i++) {
            int dup = 0;
            for (int k = 0; k < n; k++) if (strcmp(names[k], mine[i]) == 0) dup = 1;
            if (!dup) names[n++] = mine[i];
        }
    }
    return n;
}

int vio_upscale_create_needs_commands(int provider)
{
    const vio_upscale_provider *p = vio_upscale_provider_get(provider);
    return p && (p->flags & VIO_UPSCALE_PROVIDER_CREATE_COMMANDS);
}

void vio_upscale_device_release(const vio_upscale_device *dev)
{
    for (int p = 1; p <= VIO_UPSCALER_COUNT; p++) {
        /* Only what is loaded can hold device state: no plugin search at shutdown. */
        vio_upscale_plugin_slot *s = vio_upscale_plugin_slot_of(p);
        const vio_upscale_provider *pr = s ? s->provider : vio_upscale_provider_get(p);
        if (pr && pr->device_release) pr->device_release(dev);
    }
}

typedef struct _vio_upscale_instance {
    const vio_upscale_provider *provider;
    void                       *ctx;
    vio_upscale_create_desc     desc;
} vio_upscale_instance;

int vio_upscale_supported_on(const vio_upscale_device *dev, int provider, char *reason, size_t reason_len, vio_upscale_query *q)
{
    if (q) memset(q, 0, sizeof(*q));
    const vio_upscale_provider *p = vio_upscale_provider_resolve(provider, reason, reason_len);
    if (!p) return 0;
    vio_upscale_query tmp;
    return p->supported(dev, reason, reason_len, q ? q : &tmp);
}

int vio_upscale_render_size_on(const vio_upscale_device *dev, int provider, int quality, int display_w, int display_h,
                               int *render_w, int *render_h, char *reason, size_t reason_len)
{
    *render_w = *render_h = 0;
    if (!vio_upscale_supported_on(dev, provider, reason, reason_len, NULL)) return 0;
    const vio_upscale_provider *p = vio_upscale_provider_get(provider);
    if (p->render_size) return p->render_size(dev, quality, display_w, display_h, render_w, render_h, reason, reason_len);
    vio_upscale_render_size(quality, display_w, display_h, render_w, render_h);
    return 1;
}

/* desc->render_width / render_height 0: the provider's render size for the
 * quality mode (vio_upscale_render_size_on). */
void *vio_upscale_create_on(const vio_upscale_device *dev, const vio_upscale_create_desc *desc, char *reason, size_t reason_len)
{
    if (!vio_upscale_supported_on(dev, desc->provider, reason, reason_len, NULL)) return NULL;
    const vio_upscale_provider *p = vio_upscale_provider_get(desc->provider);
    vio_upscale_create_desc d = *desc;
    if (d.render_width <= 0 || d.render_height <= 0) {
        if (!vio_upscale_render_size_on(dev, d.provider, d.quality, d.display_width, d.display_height,
                                        &d.render_width, &d.render_height, reason, reason_len)) return NULL;
    }
    void *ctx = p->create(dev, &d, reason, reason_len);
    if (!ctx) return NULL;
    vio_upscale_instance *in = (vio_upscale_instance *)calloc(1, sizeof(*in));
    if (!in) { p->destroy(ctx); snprintf(reason, reason_len, "out of memory"); return NULL; }
    in->provider = p;
    in->ctx = ctx;
    in->desc = d;
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

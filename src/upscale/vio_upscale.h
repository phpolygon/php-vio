/*
 * php-vio - Native temporal upscalers (vio_upscaler_*, TEMPORAL-S3)
 *
 * Three layers:
 *   1. php_vio.c: the PHP functions. They validate arguments, resolve the
 *      render targets of a dispatch into vio_upscale_image and call the
 *      backend's upscaler_* vtable slots (include/vio_backend.h).
 *   2. The backend (D3D12, Vulkan): brings the images into a state a compute
 *      pass can read / write, hands their native handles to the provider
 *      (vio_upscale_native_dispatch) on the frame command list and restores
 *      its own state afterwards. It knows nothing about any SDK.
 *   3. A provider (vio_upscale_provider, include/vio_upscale_plugin.h): one
 *      SDK. FidelityFX FSR 3.1 is built in (vio_upscale_ffx*.cpp, --with-ffx,
 *      MIT); NVIDIA DLSS is a plugin library loaded at run time (vio_dlss.dll /
 *      libvio_dlss.so, TEMPORAL-S4) - php-vio holds no NVIDIA code. Every
 *      provider takes the same inputs: colour, depth, motion in render pixels
 *      (scaled by mv_scale), jitter in render pixels, optional reactive /
 *      transparency / exposure, and a storage output at display size.
 *
 * The provider-facing types and conventions (vio_upscale_device,
 * vio_upscale_native_dispatch, vio_upscale_provider, VIO_UPSCALER_*, ...) are
 * the plugin ABI in include/vio_upscale_plugin.h.
 */

#ifndef VIO_UPSCALE_H
#define VIO_UPSCALE_H

#include <stddef.h>
#include <stdint.h>

#include "../../include/vio_upscale_plugin.h"

#ifdef __cplusplus
extern "C" {
#endif

/* A dispatch input / output: colour attachment `attachment` of a render target
 * (vio_render_target_object *), or its depth when attachment = VIO_RT_DEPTH.
 * rt = NULL: not given. */
typedef struct _vio_upscale_image {
    void *rt;
    int   attachment;
} vio_upscale_image;

typedef struct _vio_upscale_dispatch_desc {
    vio_upscale_image color, depth, motion, reactive, transparency, exposure, output;
    vio_upscale_dispatch_params params;
} vio_upscale_dispatch_desc;

/* ── Shared helpers (vio_upscale.c) ─────────────────────────────────── */

const char *vio_upscale_provider_name(int provider);   /* "fsr3" / "dlss" / "xess" */
float vio_upscale_quality_ratio(int quality);
void  vio_upscale_render_size(int quality, int display_w, int display_h, int *render_w, int *render_h);
int   vio_upscale_jitter_phases(int render_w, int display_w);

/* Load a provider's runtime library. With the ini setting `ini_key` or the
 * environment variable `env_key` set (a directory or the file itself) only that
 * place is searched; otherwise the directory of the PHP executable, the
 * directory of php_vio, then the system search path. NULL + reason when it is
 * not found (no warning); `path` receives what was loaded. */
void *vio_upscale_load_library(const char *ini_key, const char *env_key, const char *file,
                               char *path, size_t path_len, char *reason, size_t reason_len);
void *vio_upscale_symbol(void *module, const char *name);
/* A php.ini string (NULL when unknown), for providers written in C++. */
const char *vio_upscale_ini(const char *key);
/* The same search without loading (for runtimes an SDK loads itself): 1 +
 * `path`, or 0 + reason. Elsewhere than Windows `file` is matched as a prefix
 * (versioned .so names); LD_LIBRARY_PATH replaces PATH. */
int   vio_upscale_find_file(const char *ini_key, const char *env_key, const char *file,
                            char *path, size_t path_len, char *reason, size_t reason_len);

/* Host memory handed to providers, counted (vio_upscaler_info()['host_bytes']). */
void   *vio_upscale_host_alloc(void *user, uint64_t size);
void    vio_upscale_host_free(void *user, void *mem);
int64_t vio_upscale_host_bytes(void);

/* The backend slots call these with their native device. */
int   vio_upscale_supported_on(const vio_upscale_device *dev, int provider, char *reason, size_t reason_len, vio_upscale_query *q);
void *vio_upscale_create_on(const vio_upscale_device *dev, const vio_upscale_create_desc *desc, char *reason, size_t reason_len);
int   vio_upscale_dispatch_native(void *upscaler, const vio_upscale_native_dispatch *d, char *err, size_t err_len);
int   vio_upscale_query_instance(void *upscaler, vio_upscale_query *q);
void  vio_upscale_destroy_instance(void *upscaler);
const vio_upscale_create_desc *vio_upscale_instance_desc(void *upscaler);
/* OR of every available provider's vk_device_needs. */
unsigned vio_upscale_vk_device_needs(void *physical_device);
/* Every available provider's vk_extensions, without duplicates (physical_device
 * NULL: instance extensions). Returns how many names were written. */
int vio_upscale_vk_extensions(void *physical_device, const char **names, int max);
/* The render size of `provider` for `quality` at the display size on the device:
 * its own (DLSS optimal settings) or the fixed ratios. 1 = filled; 0 = provider
 * not usable or mode not offered (reason filled). */
int vio_upscale_render_size_on(const vio_upscale_device *dev, int provider, int quality, int display_w, int display_h,
                               int *render_w, int *render_h, char *reason, size_t reason_len);
/* 1 when the provider's create() records into vio_upscale_device.command_list. */
int vio_upscale_create_needs_commands(int provider);
/* Before the device goes away: every provider drops its state for it. */
void vio_upscale_device_release(const vio_upscale_device *dev);

/* The provider for VIO_UPSCALER_*: built in, or loaded from its plugin library
 * (VIO_UPSCALER_DLSS: vio_dlss.dll / libvio_dlss.so through vio.dlss_plugin_path
 * / VIO_DLSS_PLUGIN, next to the PHP executable, next to php_vio, the system
 * search path). NULL + reason when there is none; never warns. */
const vio_upscale_provider *vio_upscale_provider_resolve(int provider, char *reason, size_t reason_len);
const vio_upscale_provider *vio_upscale_provider_get(int provider);
/* The plugin file the provider came from, "" when built in or not loaded. */
const char *vio_upscale_plugin_path(int provider);

/* Built-in providers. */
#ifdef HAVE_FFX
extern const vio_upscale_provider vio_upscale_provider_ffx;
#endif

#ifdef __cplusplus
}
#endif

#endif /* VIO_UPSCALE_H */

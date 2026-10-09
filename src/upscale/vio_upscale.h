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
 *   3. A provider (vio_upscale_provider): one SDK - FidelityFX FSR 3.1
 *      (vio_upscale_ffx*.cpp, --with-ffx), NVIDIA DLSS Super Resolution through
 *      NGX (vio_upscale_dlss*.cpp, --with-dlss=DIR, TEMPORAL-S4), later XeSS.
 *      Every provider takes the same inputs: colour, depth, motion in render
 *      pixels (scaled by mv_scale), jitter in render pixels, optional reactive /
 *      transparency / exposure, and a storage output at display size.
 *
 * Conventions (all providers):
 *   jitter : the sub-pixel offset applied to the projection this frame, in
 *            render pixels, x right / y down (row 0 = top). Geometry moves by
 *            +jitter, so a pixel samples the scene at its centre - jitter.
 *   motion : per render pixel (motion.rg * mv_scale) = previous position -
 *            current position, in render pixels, x right / y down.
 */

#ifndef VIO_UPSCALE_H
#define VIO_UPSCALE_H

#include <stddef.h>
#include <stdint.h>

/* Providers (PHP: VIO_UPSCALER_*). */
#define VIO_UPSCALER_FSR3  1
#define VIO_UPSCALER_DLSS  2
#define VIO_UPSCALER_XESS  3
#define VIO_UPSCALER_COUNT 3

/* Quality modes (PHP: VIO_UPSCALE_*). Per-axis ratios 1.0 / 1.5 / 1.7 / 2.0 / 3.0. */
#define VIO_UPSCALE_NATIVE_AA          0
#define VIO_UPSCALE_QUALITY            1
#define VIO_UPSCALE_BALANCED           2
#define VIO_UPSCALE_PERFORMANCE        3
#define VIO_UPSCALE_ULTRA_PERFORMANCE  4

/* vio_upscale_create_desc.flags */
#define VIO_UPSCALE_FLAG_HDR             (1u << 0)   /* colour is linear HDR (not 0..1) */
#define VIO_UPSCALE_FLAG_DEPTH_INVERTED  (1u << 1)   /* depth 1 = near, 0 = far */
#define VIO_UPSCALE_FLAG_DEPTH_INFINITE  (1u << 2)   /* infinite far plane */
#define VIO_UPSCALE_FLAG_AUTO_EXPOSURE   (1u << 3)   /* the provider computes the exposure */
#define VIO_UPSCALE_FLAG_DYNAMIC_RES     (1u << 4)   /* render size changes between dispatches */
#define VIO_UPSCALE_FLAG_DEBUG           (1u << 5)   /* the provider checks the API use and reports */
#define VIO_UPSCALE_FLAG_MV_JITTERED     (1u << 6)   /* motion vectors include the jitter */

typedef struct _vio_upscale_create_desc {
    int      provider;
    int      quality;
    int      render_width, render_height;     /* largest render size dispatched */
    int      display_width, display_height;   /* output size */
    unsigned flags;
    char     preset;                          /* the provider's model preset ('a'..'z'), 0 = its default */
} vio_upscale_create_desc;

/* A dispatch input / output: colour attachment `attachment` of a render target
 * (vio_render_target_object *), or its depth when attachment = VIO_RT_DEPTH.
 * rt = NULL: not given. */
typedef struct _vio_upscale_image {
    void *rt;
    int   attachment;
} vio_upscale_image;

typedef struct _vio_upscale_dispatch_desc {
    vio_upscale_image color, depth, motion, reactive, transparency, exposure, output;
    int   render_width, render_height;   /* the part of colour / depth / motion that was rendered */
    float jitter_x, jitter_y;            /* render pixels, see the conventions above */
    float mv_scale_x, mv_scale_y;        /* motion * mv_scale = render pixels */
    int   reset;                         /* drop the history (camera cut) */
    float sharpness;                     /* 0 = no sharpening pass, up to 1 */
    float frame_time_ms;
    float camera_near, camera_far;       /* view-space distances; far may be huge for infinite */
    float fov_y;                         /* radians */
    float pre_exposure;                  /* colour was multiplied by this (> 0) */
    float view_to_meters;                /* view-space unit in metres */
} vio_upscale_dispatch_desc;

/* What vio_upscaler_info() reports: provider level (ctx = NULL) or per upscaler. */
typedef struct _vio_upscale_query {
    char     version[64];      /* e.g. "3.1.4" (FSR) / "310.9.1" (DLSS) */
    char     driver[64];       /* graphics driver version the provider checked ("" = none) */
    char     library[512];     /* the loaded runtime library */
    int      render_width, render_height;
    int      display_width, display_height;
    int      jitter_phases;
    uint64_t gpu_memory;       /* bytes the provider allocated on the GPU */
} vio_upscale_query;

/* ── Backend → provider ─────────────────────────────────────────────── */

#define VIO_UPSCALE_API_D3D12  1
#define VIO_UPSCALE_API_VULKAN 2

typedef struct _vio_upscale_device {
    int   api;                    /* VIO_UPSCALE_API_* */
    void *device;                 /* ID3D12Device* / VkDevice */
    void *physical_device;        /* VkPhysicalDevice */
    void *instance;               /* VkInstance */
    void *get_device_proc_addr;   /* PFN_vkGetDeviceProcAddr */
    void *get_instance_proc_addr; /* PFN_vkGetInstanceProcAddr */
    /* create() only, for providers with VIO_UPSCALE_PROVIDER_CREATE_COMMANDS: an
     * open ID3D12GraphicsCommandList* / VkCommandBuffer (outside any pass) the
     * backend executes and waits for right after create() returns. */
    void *command_list;
} vio_upscale_device;

/* Where an image rests when it is handed over; the provider leaves it there. */
#define VIO_UPSCALE_STATE_SHADER_READ 0   /* D3D12 PIXEL_SHADER_RESOURCE, Vulkan SHADER_READ_ONLY_OPTIMAL */
#define VIO_UPSCALE_STATE_GENERAL     1   /* D3D12 UNORDERED_ACCESS, Vulkan GENERAL */

typedef struct _vio_upscale_native_image {
    void    *handle;              /* ID3D12Resource* / VkImage; NULL = not given */
    void    *view;                /* Vulkan: a 2D single-level VkImageView of it (depth: depth aspect) */
    uint32_t format;              /* DXGI_FORMAT / VkFormat of the resource */
    uint32_t width, height;
    int      depth;               /* a depth(-stencil) resource */
    int      stencil;             /* ... with a stencil plane */
    int      storage;             /* created for UAV / STORAGE access */
    int      state;               /* VIO_UPSCALE_STATE_* */
} vio_upscale_native_image;

typedef struct _vio_upscale_native_dispatch {
    void *command_list;           /* ID3D12GraphicsCommandList* / VkCommandBuffer, outside any pass */
    vio_upscale_native_image color, depth, motion, reactive, transparency, exposure, output;
    const vio_upscale_dispatch_desc *desc;
} vio_upscale_native_dispatch;

/* Vulkan device features a provider may use when the physical device has them
 * (vio_upscale_vk_device_needs, read before vkCreateDevice). */
#define VIO_UPSCALE_VK_SUBGROUP_SIZE_CONTROL (1u << 0)
#define VIO_UPSCALE_VK_FLOAT16               (1u << 1)
#define VIO_UPSCALE_VK_INT16                 (1u << 2)
#define VIO_UPSCALE_VK_STORAGE16             (1u << 3)
#define VIO_UPSCALE_VK_SEPARATE_DEPTH_STENCIL (1u << 4)

/* vio_upscale_provider.flags */
#define VIO_UPSCALE_PROVIDER_CREATE_COMMANDS (1u << 0)   /* create() records into dev->command_list */

typedef struct _vio_upscale_provider {
    int         id;               /* VIO_UPSCALER_* */
    const char *name;             /* "fsr3" */
    unsigned    flags;            /* VIO_UPSCALE_PROVIDER_* */
    /* 1 = usable on the device (version / library filled), 0 = not, reason filled. */
    int   (*supported)(const vio_upscale_device *dev, char *reason, size_t reason_len, vio_upscale_query *q);
    void *(*create)(const vio_upscale_device *dev, const vio_upscale_create_desc *desc, char *reason, size_t reason_len);
    int   (*dispatch)(void *ctx, const vio_upscale_native_dispatch *d, char *err, size_t err_len);
    int   (*query)(void *ctx, vio_upscale_query *q);
    void  (*destroy)(void *ctx);
    /* VIO_UPSCALE_VK_* the provider may use on this VkPhysicalDevice (0 when its
     * library is missing: then nothing changes at device creation). */
    unsigned (*vk_device_needs)(void *physical_device);
    /* Optional (NULL = the fixed ratios of vio_upscale_render_size). The render
     * size the provider wants for `quality` at the display size; 1 = filled,
     * 0 = the mode is not offered (reason filled). */
    int   (*render_size)(const vio_upscale_device *dev, int quality, int display_w, int display_h,
                         int *render_w, int *render_h, char *reason, size_t reason_len);
    /* Optional: drop every per-device state before the device is destroyed. */
    void  (*device_release)(const vio_upscale_device *dev);
    /* Optional, Vulkan: extension names the provider needs enabled when they are
     * offered - instance extensions with physical_device = NULL (read before
     * vkCreateInstance), else device extensions (before vkCreateDevice). Up to
     * `max` names into `names`; returns how many (0 when its runtime is missing). */
    int   (*vk_extensions)(void *physical_device, const char **names, int max);
} vio_upscale_provider;

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
/* The same search without loading (for runtimes the SDK loads itself, like
 * nvngx_dlss.dll): 1 + `path`, or 0 + reason. Elsewhere than Windows `file` is
 * matched as a prefix (versioned .so names); LD_LIBRARY_PATH replaces PATH. */
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
/* OR of every built provider's vk_device_needs. */
unsigned vio_upscale_vk_device_needs(void *physical_device);
/* Every built provider's vk_extensions, without duplicates (physical_device NULL:
 * instance extensions). Returns how many names were written. */
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

/* Built providers (NULL when not compiled in). */
const vio_upscale_provider *vio_upscale_provider_get(int provider);
#ifdef HAVE_FFX
extern const vio_upscale_provider vio_upscale_provider_ffx;
#endif
#ifdef HAVE_DLSS
extern const vio_upscale_provider vio_upscale_provider_dlss;
#endif

#endif /* VIO_UPSCALE_H */

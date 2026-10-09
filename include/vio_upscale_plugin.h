/*
 * php-vio - Native upscaler plugin ABI (vio_upscaler_*, TEMPORAL-S4)
 *
 * A provider of a temporal upscaler (VIO_UPSCALER_*) can live outside php-vio,
 * in a shared library the extension loads at run time: vio_dlss.dll /
 * libvio_dlss.so for VIO_UPSCALER_DLSS. This header is everything such a
 * plugin sees of php-vio - plain C, no php-vio internals, native graphics
 * objects only as void * (ID3D12Device *, VkDevice, ...). It is the contract
 * the in-tree providers (FidelityFX FSR 3.1) are written against as well.
 *
 * The library exports one function:
 *
 *   VIO_UPSCALE_PLUGIN_EXPORT const vio_upscale_provider *
 *   vio_upscale_plugin_get(uint32_t abi, const vio_upscale_host_api *host);
 *
 * php-vio calls it once, with VIO_UPSCALE_PLUGIN_ABI and its host table (valid
 * for the life of the process). The plugin returns its provider - a table that
 * stays valid until the process ends, with provider->abi = the ABI it was
 * built for - or NULL when it cannot serve that ABI. php-vio refuses a provider
 * of another ABI, one smaller than it expects (size) or one for another
 * VIO_UPSCALER_* than the library name says; the provider is then unsupported
 * and vio_upscaler_info()['reason'] says why. A plugin is never unloaded.
 *
 * Every call into the provider comes from the thread that owns the graphics
 * context (the PHP thread). Strings handed out (reason, err, query) are
 * NUL-terminated within the given length.
 *
 * Bump VIO_UPSCALE_PLUGIN_ABI on any change that moves a field or changes a
 * meaning; new fields go at the end of a struct together with its `size`.
 *
 * Conventions (all providers):
 *   jitter : the sub-pixel offset applied to the projection this frame, in
 *            render pixels, x right / y down (row 0 = top). Geometry moves by
 *            +jitter, so a pixel samples the scene at its centre - jitter.
 *   motion : per render pixel (motion.rg * mv_scale) = previous position -
 *            current position, in render pixels, x right / y down.
 */

#ifndef VIO_UPSCALE_PLUGIN_H
#define VIO_UPSCALE_PLUGIN_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define VIO_UPSCALE_PLUGIN_ABI   1
#define VIO_UPSCALE_PLUGIN_ENTRY "vio_upscale_plugin_get"

#if defined(_WIN32)
#  define VIO_UPSCALE_PLUGIN_EXPORT __declspec(dllexport)
#elif defined(__GNUC__)
#  define VIO_UPSCALE_PLUGIN_EXPORT __attribute__((visibility("default")))
#else
#  define VIO_UPSCALE_PLUGIN_EXPORT
#endif

/* ── Providers and modes ────────────────────────────────────────────── */

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
    int      provider;                        /* VIO_UPSCALER_* */
    int      quality;                         /* VIO_UPSCALE_* */
    int      render_width, render_height;     /* largest render size dispatched */
    int      display_width, display_height;   /* output size */
    unsigned flags;                           /* VIO_UPSCALE_FLAG_* */
    char     preset;                          /* the provider's model preset ('a'..'z'), 0 = its default */
} vio_upscale_create_desc;

/* The numbers of one dispatch (the images are in vio_upscale_native_dispatch). */
typedef struct _vio_upscale_dispatch_params {
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
} vio_upscale_dispatch_params;

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

/* ── Device and images (native handles) ─────────────────────────────── */

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
     * host executes and waits for right after create() returns - also when
     * create() is called inside a frame, the frame's list stays untouched. */
    void *command_list;
    /* 1 = a software rasteriser (D3D12 WARP): providers that need a GPU say no. */
    int   software_adapter;
} vio_upscale_device;

/* Where an image rests when it is handed over; the provider records whatever
 * transitions it needs on the command list and leaves it there again. */
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

/* One dispatch. Recorded on the frame's command list outside any render pass;
 * the host has made earlier writes visible to every stage before and makes the
 * provider's writes visible to whatever follows. The provider may change any
 * pipeline / descriptor-heap / root-signature state on the list (the host
 * restores its own); image states / layouts it changes it must restore. */
typedef struct _vio_upscale_native_dispatch {
    void *command_list;           /* ID3D12GraphicsCommandList* / VkCommandBuffer */
    vio_upscale_native_image color, depth, motion, reactive, transparency, exposure, output;
    const vio_upscale_dispatch_params *params;
} vio_upscale_native_dispatch;

/* Vulkan device features a provider may use when the physical device has them
 * (vk_device_needs, read before vkCreateDevice). */
#define VIO_UPSCALE_VK_SUBGROUP_SIZE_CONTROL  (1u << 0)
#define VIO_UPSCALE_VK_FLOAT16                (1u << 1)
#define VIO_UPSCALE_VK_INT16                  (1u << 2)
#define VIO_UPSCALE_VK_STORAGE16              (1u << 3)
#define VIO_UPSCALE_VK_SEPARATE_DEPTH_STENCIL (1u << 4)

/* ── Host → plugin ──────────────────────────────────────────────────── */

#define VIO_UPSCALE_LOG_ERROR   1   /* PHP warning */
#define VIO_UPSCALE_LOG_WARNING 2   /* PHP warning */
#define VIO_UPSCALE_LOG_INFO    3   /* stderr when VIO_UPSCALE_PLUGIN_LOG is set */
#define VIO_UPSCALE_LOG_DEBUG   4   /* stderr when VIO_UPSCALE_PLUGIN_LOG is set */

typedef struct _vio_upscale_host_api {
    uint32_t    abi;              /* VIO_UPSCALE_PLUGIN_ABI of php-vio */
    uint32_t    size;             /* sizeof(vio_upscale_host_api) of php-vio */
    const char *host_version;     /* php-vio's version, "2.32.0" */
    const char *plugin_path;      /* the file this plugin was loaded from */
    /* A message from the provider, on the PHP thread only. */
    void        (*log)(int level, const char *message);
    /* Host memory, counted in vio_upscaler_info()['host_bytes'] (leak checks).
     * `user` is passed through and ignored. Never NULL for size > 0 unless out of memory. */
    void       *(*alloc)(void *user, uint64_t size);
    void        (*free)(void *user, void *mem);
    /* A php.ini string (registered vio.* settings: vio.dlss_path,
     * vio.dlss_project_id, vio.dlss_engine_version, ...), NULL when unknown. */
    const char *(*ini)(const char *key);
    /* Where a runtime file is: with ini `ini_key` or environment `env_key` set
     * (a directory or the file) only there; otherwise next to the PHP
     * executable, next to php_vio, on PATH (Windows) / LD_LIBRARY_PATH.
     * Elsewhere than Windows `file` is matched as a prefix (versioned .so).
     * 1 + `path`, or 0 + reason. */
    int         (*find_file)(const char *ini_key, const char *env_key, const char *file,
                             char *path, size_t path_len, char *reason, size_t reason_len);
    /* The same search, loading the library (dependencies resolve from its
     * directory first); NULL + reason when missing. */
    void       *(*load_library)(const char *ini_key, const char *env_key, const char *file,
                                char *path, size_t path_len, char *reason, size_t reason_len);
    void       *(*symbol)(void *library, const char *name);
} vio_upscale_host_api;

/* ── Plugin → host: the provider ────────────────────────────────────── */

/* vio_upscale_provider.flags */
#define VIO_UPSCALE_PROVIDER_CREATE_COMMANDS (1u << 0)   /* create() records into dev->command_list */

typedef struct _vio_upscale_provider {
    uint32_t    abi;              /* VIO_UPSCALE_PLUGIN_ABI the provider was built for */
    uint32_t    size;             /* sizeof(vio_upscale_provider) it was built with */
    int         id;               /* VIO_UPSCALER_* */
    const char *name;             /* "fsr3", "dlss" */
    const char *version;          /* the provider's own build ("1.0.0"), "" = php-vio's */
    unsigned    flags;            /* VIO_UPSCALE_PROVIDER_* */
    /* 1 = usable on the device (q->version / library filled), 0 = not, reason filled. */
    int   (*supported)(const vio_upscale_device *dev, char *reason, size_t reason_len, vio_upscale_query *q);
    /* A context for desc (render size already chosen); NULL + reason. */
    void *(*create)(const vio_upscale_device *dev, const vio_upscale_create_desc *desc, char *reason, size_t reason_len);
    /* Record one dispatch on d->command_list; 0 = ok, -1 + err. */
    int   (*dispatch)(void *ctx, const vio_upscale_native_dispatch *d, char *err, size_t err_len);
    /* Fill what the context knows (version, library, gpu_memory, jitter_phases); 0 = ok. Optional. */
    int   (*query)(void *ctx, vio_upscale_query *q);
    /* The GPU is idle for the context's work when this is called. */
    void  (*destroy)(void *ctx);
    /* Optional: VIO_UPSCALE_VK_* the provider may use on this VkPhysicalDevice
     * (0 when its runtime is missing: then nothing changes at device creation). */
    unsigned (*vk_device_needs)(void *physical_device);
    /* Optional (NULL = the fixed ratios 1.0 / 1.5 / 1.7 / 2.0 / 3.0). The render
     * size the provider wants for `quality` at the display size; 1 = filled,
     * 0 = the mode is not offered (reason filled). */
    int   (*render_size)(const vio_upscale_device *dev, int quality, int display_w, int display_h,
                         int *render_w, int *render_h, char *reason, size_t reason_len);
    /* Optional: drop every per-device state before the device is destroyed. */
    void  (*device_release)(const vio_upscale_device *dev);
    /* Optional, Vulkan: extension names the provider needs enabled when they are
     * offered - instance extensions with physical_device = NULL (read before
     * vkCreateInstance), else device extensions (before vkCreateDevice). Up to
     * `max` names into `names` (strings that stay valid); returns how many (0
     * when its runtime is missing). */
    int   (*vk_extensions)(void *physical_device, const char **names, int max);
} vio_upscale_provider;

/* The plugin's one export. */
typedef const vio_upscale_provider *(*vio_upscale_plugin_get_fn)(uint32_t abi, const vio_upscale_host_api *host);

#ifdef __cplusplus
}
#endif

#endif /* VIO_UPSCALE_PLUGIN_H */

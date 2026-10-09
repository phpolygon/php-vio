/*
 * php-vio - test upscaler plugin (tests/render3d/234-236)
 *
 * A provider behind the plugin ABI (include/vio_upscale_plugin.h) that does
 * what a real one does with the host's objects, minus the upscaling: it copies
 * the rendered part of 'color' into the top-left corner of 'output'. On D3D12
 * it moves both resources to COPY_SOURCE / COPY_DEST and back (a copy: equal
 * formats only), on Vulkan to TRANSFER_SRC / TRANSFER_DST and back (a 1:1
 * blit: vio's colour targets may be BGRA, its storage targets RGBA) - so the
 * D3D12 debug layer / Vulkan validation see whether the host's states and the
 * restore contract hold. A jitter beyond one pixel is refused (an error the
 * host reports). It registers as VIO_UPSCALER_DLSS when loaded through
 * vio.dlss_plugin_path.
 *
 * Steering through the environment (read in vio_upscale_plugin_get):
 *   VIO_TEST_UPSCALER_REFUSE=1   return NULL (the ABI is not offered)
 *   VIO_TEST_UPSCALER_ABI=n      report ABI n
 *   VIO_TEST_UPSCALER_SIZE=n     report a provider table of n bytes
 *   VIO_TEST_UPSCALER_ID=n       register for VIO_UPSCALER_n
 *   VIO_TEST_UPSCALER_NO_RESTORE=1  leave the images in the copy states (breaks
 *                                the contract: the debug layer / validation must object)
 *
 * No SDK, no graphics library linked: D3D12 through its COM vtables, Vulkan
 * through the vkGetDeviceProcAddr the host hands over.
 *
 * Built by `nmake` next to php_vio.dll (Makefile.frag.w32); elsewhere:
 *   cc -shared -fPIC -I include -I <vulkan>/include tests/upscale_plugin/vio_test_upscaler.c -o vio_test_upscaler.so
 */

#ifdef _WIN32
#define COBJMACROS
#include <windows.h>
#include <d3d12.h>
#define VIO_TEST_D3D12 1
#endif

#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../include/vio_upscale_plugin.h"

static const vio_upscale_host_api *g_host = NULL;
static int g_no_restore = 0;

typedef struct {
    int api;
    vio_upscale_create_desc desc;
    unsigned dispatches;
    /* Vulkan entry points of the device */
    PFN_vkCmdPipelineBarrier cmd_barrier;
    PFN_vkCmdBlitImage       cmd_blit;
} test_ctx;

static void test_log(int level, const char *fmt, const char *arg)
{
    char msg[256];
    snprintf(msg, sizeof(msg), fmt, arg);
    if (g_host && g_host->log) g_host->log(level, msg);
}

static int test_supported(const vio_upscale_device *dev, char *reason, size_t reason_len, vio_upscale_query *q)
{
    if (dev->api != VIO_UPSCALE_API_D3D12 && dev->api != VIO_UPSCALE_API_VULKAN) {
        snprintf(reason, reason_len, "test upscaler: D3D12 and Vulkan only");
        return 0;
    }
    snprintf(q->version, sizeof(q->version), "0.1.0-test");
    snprintf(q->library, sizeof(q->library), "%s", g_host && g_host->plugin_path ? g_host->plugin_path : "");
    snprintf(q->driver, sizeof(q->driver), "test-driver");
    return 1;
}

/* Performance-like 2x for every mode, the display size for native AA; no Ultra Performance. */
static int test_render_size(const vio_upscale_device *dev, int quality, int display_w, int display_h,
                            int *render_w, int *render_h, char *reason, size_t reason_len)
{
    (void)dev;
    if (quality == VIO_UPSCALE_ULTRA_PERFORMANCE) {
        snprintf(reason, reason_len, "test upscaler offers no Ultra Performance mode");
        return 0;
    }
    int d = quality == VIO_UPSCALE_NATIVE_AA ? 1 : 2;
    *render_w = display_w / d > 0 ? display_w / d : 1;
    *render_h = display_h / d > 0 ? display_h / d : 1;
    return 1;
}

static void *test_create(const vio_upscale_device *dev, const vio_upscale_create_desc *desc, char *reason, size_t reason_len)
{
    if (!dev->command_list) {
        snprintf(reason, reason_len, "test upscaler: no command list at creation (VIO_UPSCALE_PROVIDER_CREATE_COMMANDS)");
        return NULL;
    }
    if (desc->preset == 'x') {
        snprintf(reason, reason_len, "test upscaler has no preset 'x'");
        return NULL;
    }
    if (desc->render_width < 1 || desc->render_height < 1) {
        snprintf(reason, reason_len, "test upscaler: no render size chosen");
        return NULL;
    }
    test_ctx *c = (test_ctx *)g_host->alloc(NULL, sizeof(test_ctx));
    if (!c) { snprintf(reason, reason_len, "out of memory"); return NULL; }
    memset(c, 0, sizeof(*c));
    c->api = dev->api;
    c->desc = *desc;
    if (dev->api == VIO_UPSCALE_API_VULKAN) {
        PFN_vkGetDeviceProcAddr gdpa = (PFN_vkGetDeviceProcAddr)dev->get_device_proc_addr;
        VkDevice device = (VkDevice)dev->device;
        c->cmd_barrier = gdpa ? (PFN_vkCmdPipelineBarrier)gdpa(device, "vkCmdPipelineBarrier") : NULL;
        c->cmd_blit = gdpa ? (PFN_vkCmdBlitImage)gdpa(device, "vkCmdBlitImage") : NULL;
        if (!c->cmd_barrier || !c->cmd_blit) {
            g_host->free(NULL, c);
            snprintf(reason, reason_len, "test upscaler: no Vulkan entry points");
            return NULL;
        }
        /* Something on the creation command buffer: it must be recording. */
        VkMemoryBarrier mb;
        memset(&mb, 0, sizeof(mb));
        mb.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
        mb.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
        mb.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT;
        c->cmd_barrier((VkCommandBuffer)dev->command_list, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                       VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1, &mb, 0, NULL, 0, NULL);
    }
#ifdef VIO_TEST_D3D12
    else {
        static const char marker[] = "vio_test_upscaler create";
        ID3D12GraphicsCommandList_SetMarker((ID3D12GraphicsCommandList *)dev->command_list, 1, marker, (UINT)sizeof(marker));
    }
#endif
    if (desc->preset == 'l') test_log(VIO_UPSCALE_LOG_WARNING, "test upscaler: preset '%s' logs a warning", "l");
    return c;
}

#ifdef VIO_TEST_D3D12
static D3D12_RESOURCE_STATES test_d3d12_state(int state)
{
    return state == VIO_UPSCALE_STATE_GENERAL ? D3D12_RESOURCE_STATE_UNORDERED_ACCESS : D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
}

static void test_d3d12_transition(ID3D12GraphicsCommandList *list, const vio_upscale_native_image *src, const vio_upscale_native_image *dst, int back)
{
    D3D12_RESOURCE_BARRIER b[2];
    memset(b, 0, sizeof(b));
    const vio_upscale_native_image *im[2] = { src, dst };
    D3D12_RESOURCE_STATES copy[2] = { D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COPY_DEST };
    for (int i = 0; i < 2; i++) {
        b[i].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b[i].Transition.pResource = (ID3D12Resource *)im[i]->handle;
        b[i].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        b[i].Transition.StateBefore = back ? copy[i] : test_d3d12_state(im[i]->state);
        b[i].Transition.StateAfter = back ? test_d3d12_state(im[i]->state) : copy[i];
    }
    ID3D12GraphicsCommandList_ResourceBarrier(list, 2, b);
}

static int test_dispatch_d3d12(const vio_upscale_native_dispatch *d, int w, int h)
{
    ID3D12GraphicsCommandList *list = (ID3D12GraphicsCommandList *)d->command_list;
    test_d3d12_transition(list, &d->color, &d->output, 0);
    D3D12_TEXTURE_COPY_LOCATION s, t;
    memset(&s, 0, sizeof(s));
    memset(&t, 0, sizeof(t));
    s.pResource = (ID3D12Resource *)d->color.handle;
    s.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    t.pResource = (ID3D12Resource *)d->output.handle;
    t.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    D3D12_BOX box = { 0, 0, 0, (UINT)w, (UINT)h, 1 };
    ID3D12GraphicsCommandList_CopyTextureRegion(list, &t, 0, 0, 0, &s, &box);
    if (!g_no_restore) test_d3d12_transition(list, &d->color, &d->output, 1);
    return 0;
}
#endif

static VkImageLayout test_vk_layout(int state)
{
    return state == VIO_UPSCALE_STATE_GENERAL ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
}

static void test_vk_transition(test_ctx *c, VkCommandBuffer cmd, const vio_upscale_native_dispatch *d, int back)
{
    VkImageMemoryBarrier b[2];
    memset(b, 0, sizeof(b));
    const vio_upscale_native_image *im[2] = { &d->color, &d->output };
    VkImageLayout copy[2] = { VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL };
    for (int i = 0; i < 2; i++) {
        b[i].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        b[i].srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
        b[i].dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
        b[i].oldLayout = back ? copy[i] : test_vk_layout(im[i]->state);
        b[i].newLayout = back ? test_vk_layout(im[i]->state) : copy[i];
        b[i].srcQueueFamilyIndex = b[i].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b[i].image = (VkImage)im[i]->handle;
        b[i].subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        b[i].subresourceRange.levelCount = 1;
        b[i].subresourceRange.layerCount = 1;
    }
    c->cmd_barrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, NULL, 0, NULL, 2, b);
}

static int test_dispatch_vk(test_ctx *c, const vio_upscale_native_dispatch *d, int w, int h)
{
    VkCommandBuffer cmd = (VkCommandBuffer)d->command_list;
    test_vk_transition(c, cmd, d, 0);
    VkImageBlit r;
    memset(&r, 0, sizeof(r));
    r.srcSubresource.aspectMask = r.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    r.srcSubresource.layerCount = r.dstSubresource.layerCount = 1;
    r.srcOffsets[1].x = r.dstOffsets[1].x = w;
    r.srcOffsets[1].y = r.dstOffsets[1].y = h;
    r.srcOffsets[1].z = r.dstOffsets[1].z = 1;
    c->cmd_blit(cmd, (VkImage)d->color.handle, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                (VkImage)d->output.handle, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &r, VK_FILTER_NEAREST);
    if (!g_no_restore) test_vk_transition(c, cmd, d, 1);
    return 0;
}

static int test_dispatch(void *ctx, const vio_upscale_native_dispatch *d, char *err, size_t err_len)
{
    test_ctx *c = (test_ctx *)ctx;
    if (!d->command_list || !d->params || !d->color.handle || !d->output.handle) {
        snprintf(err, err_len, "test upscaler: incomplete dispatch");
        return -1;
    }
    if (!d->output.storage) {
        snprintf(err, err_len, "test upscaler: the output is not a storage image");
        return -1;
    }
    if (d->params->jitter_x < -1.0f || d->params->jitter_x > 1.0f || d->params->jitter_y < -1.0f || d->params->jitter_y > 1.0f) {
        snprintf(err, err_len, "test upscaler: jitter (%.1f, %.1f) beyond one pixel", d->params->jitter_x, d->params->jitter_y);
        return -1;
    }
    if (c->api == VIO_UPSCALE_API_D3D12 && d->color.format != d->output.format) {
        snprintf(err, err_len, "test upscaler copies between equal formats only (%u -> %u)", d->color.format, d->output.format);
        return -1;
    }
    if (d->params->render_width > (int)d->output.width || d->params->render_height > (int)d->output.height) {
        snprintf(err, err_len, "test upscaler: render size larger than the output");
        return -1;
    }
    c->dispatches++;
#ifdef VIO_TEST_D3D12
    if (c->api == VIO_UPSCALE_API_D3D12) return test_dispatch_d3d12(d, d->params->render_width, d->params->render_height);
#endif
    if (c->api == VIO_UPSCALE_API_VULKAN) return test_dispatch_vk(c, d, d->params->render_width, d->params->render_height);
    snprintf(err, err_len, "test upscaler: unknown API");
    return -1;
}

/* gpu_memory = the dispatches so far: the host passes the query through. */
static int test_query(void *ctx, vio_upscale_query *q)
{
    test_ctx *c = (test_ctx *)ctx;
    snprintf(q->version, sizeof(q->version), "0.1.0-test");
    snprintf(q->library, sizeof(q->library), "%s", g_host && g_host->plugin_path ? g_host->plugin_path : "");
    q->gpu_memory = c->dispatches;
    return 0;
}

static void test_destroy(void *ctx)
{
    if (ctx) g_host->free(NULL, ctx);
}

static void test_device_release(const vio_upscale_device *dev)
{
    (void)dev;
    test_log(VIO_UPSCALE_LOG_DEBUG, "test upscaler: device %s released", dev->api == VIO_UPSCALE_API_D3D12 ? "d3d12" : "vulkan");
}

/* A device extension every desktop Vulkan driver offers - shows up in
 * vio_upscaler_info()['device'] when the host enabled it. */
static int test_vk_extensions(void *physical_device, const char **names, int max)
{
    if (!physical_device || max < 1) return 0;
    names[0] = "VK_KHR_push_descriptor";
    return 1;
}

static vio_upscale_provider g_provider = {
    VIO_UPSCALE_PLUGIN_ABI,
    sizeof(vio_upscale_provider),
    VIO_UPSCALER_DLSS,
    "test-copy",
    "0.1.0-test",
    VIO_UPSCALE_PROVIDER_CREATE_COMMANDS,
    test_supported,
    test_create,
    test_dispatch,
    test_query,
    test_destroy,
    NULL,
    test_render_size,
    test_device_release,
    test_vk_extensions,
};

/* The process environment as it is now (PHP's putenv() may bypass the CRT copy). */
static int env_int(const char *name, int def)
{
#ifdef _WIN32
    char v[32];
    DWORD n = GetEnvironmentVariableA(name, v, (DWORD)sizeof(v));
    return n > 0 && n < sizeof(v) ? atoi(v) : def;
#else
    const char *v = getenv(name);
    return v && v[0] ? atoi(v) : def;
#endif
}

VIO_UPSCALE_PLUGIN_EXPORT const vio_upscale_provider *vio_upscale_plugin_get(uint32_t abi, const vio_upscale_host_api *host)
{
    if (env_int("VIO_TEST_UPSCALER_REFUSE", 0)) return NULL;
    if (abi != VIO_UPSCALE_PLUGIN_ABI || !host || host->size < sizeof(vio_upscale_host_api)) return NULL;
    g_host = host;
    g_no_restore = env_int("VIO_TEST_UPSCALER_NO_RESTORE", 0);
    g_provider.abi = (uint32_t)env_int("VIO_TEST_UPSCALER_ABI", VIO_UPSCALE_PLUGIN_ABI);
    g_provider.size = (uint32_t)env_int("VIO_TEST_UPSCALER_SIZE", (int)sizeof(vio_upscale_provider));
    g_provider.id = env_int("VIO_TEST_UPSCALER_ID", VIO_UPSCALER_DLSS);
    return &g_provider;
}

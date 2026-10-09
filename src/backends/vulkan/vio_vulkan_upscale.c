/*
 * php-vio - Vulkan side of the native upscalers (vio_upscaler_*, TEMPORAL-S3)
 *
 * The provider (src/upscale/, FSR 3.1 ...) records compute passes on the frame
 * command buffer. Here: the open pass is closed (and resumed with LOAD
 * afterwards, like an in-frame compute dispatch), earlier work is made visible
 * to compute, sampled depths move from DEPTH_STENCIL_READ_ONLY to
 * SHADER_READ_ONLY (the providers have no read-only depth state) and back, and
 * the provider's writes are made visible to whatever follows. Colour images
 * are handed over where they rest: SHADER_READ_ONLY, or GENERAL for 'storage'
 * targets.
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "php.h"

#ifdef HAVE_VULKAN

#include "vio_vulkan.h"
#include "../../vio_render_target.h"
#include "../../upscale/vio_upscale.h"

#include <string.h>
#include <stdio.h>

static void vk_upscale_device(vio_upscale_device *dev)
{
    memset(dev, 0, sizeof(*dev));
    dev->api = VIO_UPSCALE_API_VULKAN;
    dev->device = (void *)vio_vk.device;
    dev->physical_device = (void *)vio_vk.physical_device;
    dev->instance = (void *)vio_vk.instance;
    dev->get_device_proc_addr = (void *)vkGetDeviceProcAddr;
    dev->get_instance_proc_addr = (void *)vkGetInstanceProcAddr;
}

int vulkan_upscaler_supported(int provider, char *reason, size_t reason_len)
{
    if (!vio_vk.device) {
        snprintf(reason, reason_len, "no Vulkan device");
        return 0;
    }
    if (!vio_vk3d_available()) {
        snprintf(reason, reason_len, "the Vulkan 3D pipeline is not available");
        return 0;
    }
    vio_upscale_device dev;
    vk_upscale_device(&dev);
    return vio_upscale_supported_on(&dev, provider, reason, reason_len, NULL);
}

int vulkan_upscaler_any(void)
{
    char reason[256];
    for (int p = 1; p <= VIO_UPSCALER_COUNT; p++) {
        if (vulkan_upscaler_supported(p, reason, sizeof(reason))) return 1;
    }
    return 0;
}

int vulkan_upscaler_render_size(int provider, int quality, int display_w, int display_h,
                                int *render_w, int *render_h, char *reason, size_t reason_len)
{
    *render_w = *render_h = 0;
    if (!vulkan_upscaler_supported(provider, reason, reason_len)) return 0;
    vio_upscale_device dev;
    vk_upscale_device(&dev);
    return vio_upscale_render_size_on(&dev, provider, quality, display_w, display_h, render_w, render_h, reason, reason_len);
}

/* A provider that records at creation (DLSS: NGX CreateFeature) gets a
 * transient command buffer, submitted and waited for here - the frame's
 * command buffer (and its open pass) stays untouched. */
void *vulkan_upscaler_create(const vio_upscale_create_desc *desc, char *reason, size_t reason_len)
{
    vio_upscale_device dev;
    vk_upscale_device(&dev);
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    if (vio_upscale_create_needs_commands(desc->provider)) {
        if (vio_vk_transient_begin(&cmd) != 0) {
            snprintf(reason, reason_len, "could not open a command buffer for the upscaler");
            return NULL;
        }
        dev.command_list = (void *)cmd;
    }
    void *u = vio_upscale_create_on(&dev, desc, reason, reason_len);
    if (cmd != VK_NULL_HANDLE) vio_vk_transient_submit(cmd);
    return u;
}

void vulkan_upscale_device_release(void)
{
    if (!vio_vk.device) return;
    vio_upscale_device dev;
    vk_upscale_device(&dev);
    vio_upscale_device_release(&dev);
}

int vulkan_upscaler_query(void *upscaler, int provider, vio_upscale_query *q)
{
    if (upscaler) return vio_upscale_query_instance(upscaler, q);
    char reason[256];
    vio_upscale_device dev;
    vk_upscale_device(&dev);
    return vio_upscale_supported_on(&dev, provider, reason, sizeof(reason), q) ? 0 : -1;
}

void vulkan_upscaler_destroy(void *upscaler)
{
    if (!upscaler) return;
    /* Its images and pipelines may be used by the open frame or frames in flight. */
    if (vio_vk.in_frame) vio_vk_flush_frame();
    if (vio_vk.device) vkDeviceWaitIdle(vio_vk.device);
    vio_upscale_destroy_instance(upscaler);
}

int vulkan_upscaler_device_requirements(int provider, char *out, size_t out_len)
{
    (void)provider;
    if (!out_len) return 0;
    out[0] = '\0';
    static const struct { unsigned bit; const char *name; } names[] = {
        { VIO_UPSCALE_VK_SUBGROUP_SIZE_CONTROL,  "subgroup_size_control" },
        { VIO_UPSCALE_VK_FLOAT16,                "float16" },
        { VIO_UPSCALE_VK_INT16,                  "int16" },
        { VIO_UPSCALE_VK_STORAGE16,              "storage16" },
        { VIO_UPSCALE_VK_SEPARATE_DEPTH_STENCIL, "separate_depth_stencil_layouts" },
    };
    size_t len = 0;
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        if (!(vio_vk.upscale_features & names[i].bit)) continue;
        int w = snprintf(out + len, out_len - len, "%s%s", len ? "," : "", names[i].name);
        if (w < 0 || (size_t)w >= out_len - len) break;
        len += (size_t)w;
    }
    if (vio_vk.upscale_extensions[0] && len < out_len) {
        snprintf(out + len, out_len - len, "%s%s", len ? "," : "", vio_vk.upscale_extensions);
    }
    return 0;
}

/* The native view of one dispatch image. 1 = filled, 0 = not given, -1 = unusable. */
static int vk_upscale_image(const vio_upscale_image *in, vio_upscale_native_image *out, const char *what, char *err, size_t err_len)
{
    memset(out, 0, sizeof(*out));
    vio_render_target_object *rt = (vio_render_target_object *)in->rt;
    if (!rt) return 0;
    vio_vk_rt *x = (vio_vk_rt *)rt->vulkan_rt;
    if (!rt->valid || rt->backend_type != VIO_RT_BACKEND_VULKAN || !x || x->samples > 1 || x->cube || x->layers > 1) {
        snprintf(err, err_len, "'%s' is not a single-sample 2D Vulkan render target", what);
        return -1;
    }
    out->width = (uint32_t)rt->width;
    out->height = (uint32_t)rt->height;
    if (in->attachment == VIO_RT_DEPTH) {
        if (!x->depth_image || !(x->depth_sampled || rt->depth_only)) {
            snprintf(err, err_len, "'%s': the target's depth cannot be sampled", what);
            return -1;
        }
        out->handle = (void *)x->depth_image;
        out->view = (void *)x->depth_view;
        out->format = (uint32_t)vio_vk_depth_format();
        out->depth = 1;
        out->stencil = vio_vk.depth_has_stencil;
        out->state = VIO_UPSCALE_STATE_SHADER_READ;   /* moved there by the dispatch */
        return 1;
    }
    if (rt->depth_only || in->attachment < 0 || in->attachment >= x->count || !x->color_image[in->attachment]) {
        snprintf(err, err_len, "'%s': no such colour attachment", what);
        return -1;
    }
    out->handle = (void *)x->color_image[in->attachment];
    out->view = (void *)x->color_view[in->attachment];
    out->format = (uint32_t)x->color_format[in->attachment];
    out->storage = x->storage;
    out->state = x->storage ? VIO_UPSCALE_STATE_GENERAL : VIO_UPSCALE_STATE_SHADER_READ;
    return 1;
}

int vulkan_upscaler_dispatch(void *upscaler, const vio_upscale_dispatch_desc *d, char *err, size_t err_len)
{
    if (!vio_vk.in_frame) {
        snprintf(err, err_len, "only between vio_begin and vio_end");
        return -1;
    }
    vio_upscale_native_dispatch nd;
    memset(&nd, 0, sizeof(nd));
    nd.desc = d;
    const vio_upscale_image *src[7] = { &d->color, &d->depth, &d->motion, &d->reactive, &d->transparency, &d->exposure, &d->output };
    vio_upscale_native_image *dst[7] = { &nd.color, &nd.depth, &nd.motion, &nd.reactive, &nd.transparency, &nd.exposure, &nd.output };
    static const char *names[7] = { "color", "depth", "motion", "reactive", "transparency", "exposure", "output" };
    for (int i = 0; i < 7; i++) {
        if (vk_upscale_image(src[i], dst[i], names[i], err, err_len) < 0) return -1;
    }
    if (!nd.output.storage) {
        snprintf(err, err_len, "'output' was not created with 'storage' => true");
        return -1;
    }

    VkCommandBuffer cmd = vio_vk.frames[vio_vk.current_frame].cmd_buf;
    nd.command_list = (void *)cmd;

    /* No pass may be open around compute work: close it, resume it (LOAD) after. */
    int had_pass = vio_vk.in_pass;
    VkViewport vp[16];
    VkRect2D sc[16];
    uint32_t vp_count = vio_vk.cur_vp_count ? vio_vk.cur_vp_count : 1;
    if (vp_count > 16) vp_count = 16;
    memcpy(vp, vio_vk.cur_vp, sizeof(VkViewport) * vp_count);
    memcpy(sc, vio_vk.cur_sc, sizeof(VkRect2D) * vp_count);
    vio_vk_pass_end(cmd);

    /* Everything recorded so far (draws into the inputs, earlier compute) before
     * the provider's work - its compute passes and also its own clears / copies
     * (DLSS clears its history with vkCmdClearColorImage), so every stage. */
    VkMemoryBarrier pre = {0};
    pre.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    pre.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
    pre.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    vio_vk_pipeline_barrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0,
                            1, &pre, 0, NULL, 0, NULL);
    VkImageAspectFlags da = VK_IMAGE_ASPECT_DEPTH_BIT | (vio_vk.depth_has_stencil ? VK_IMAGE_ASPECT_STENCIL_BIT : 0);
    if (nd.depth.handle) {
        vio_vk_image_barrier(cmd, (VkImage)nd.depth.handle, da, 1,
                             VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    }

    int rc = vio_upscale_dispatch_native(upscaler, &nd, err, err_len);

    if (nd.depth.handle) {
        vio_vk_image_barrier(cmd, (VkImage)nd.depth.handle, da, 1,
                             VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL);
    }
    /* The provider's writes before later draws, dispatches, copies and readbacks. */
    VkMemoryBarrier post = {0};
    post.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    post.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
    post.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    vio_vk_pipeline_barrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0,
                            1, &post, 0, NULL, 0, NULL);

    if (had_pass) {
        vio_vk_resume_pass(cmd);
        memcpy(vio_vk.cur_vp, vp, sizeof(VkViewport) * vp_count);
        memcpy(vio_vk.cur_sc, sc, sizeof(VkRect2D) * vp_count);
        vio_vk.cur_vp_count = vp_count;
        vkCmdSetViewport(cmd, 0, 1, &vp[0]);
        vkCmdSetScissor(cmd, 0, 1, &sc[0]);
    }
    return rc;
}

#endif /* HAVE_VULKAN */

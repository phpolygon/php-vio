/*
 * php-vio - Vulkan 3D pipeline: per-frame upload ring + descriptor pools,
 * deferred destruction, bound state, draws, render-target readback
 * (GAP-PHASE5 Block 10). Conventions: see vio_vulkan_3d.h.
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "php.h"

#ifdef HAVE_VULKAN

#include <vulkan/vulkan.h>
#include "vio_vulkan.h"
#include "vio_vulkan_3d.h"
#include "../../vio_mesh.h"
#include "../../vio_render_target.h"
#include "../../../include/vio_types.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

#define VK3D_CHUNK_SIZE     ((VkDeviceSize)4 * 1024 * 1024)
#define VK3D_MAX_CHUNKS     32
#define VK3D_MAX_POOLS      64
#define VK3D_SETS_PER_POOL  256
#define VK3D_ZERO_BUF_SIZE  ((VkDeviceSize)65536)

typedef struct { VkBuffer buf; void *alloc; unsigned char *mapped; VkDeviceSize size, used; } vk3d_chunk;
typedef struct { int kind; uint64_t handle; void *alloc; } vk3d_grave;

typedef struct {
    vk3d_chunk       chunks[VK3D_MAX_CHUNKS];
    int              chunk_count, cur_chunk;
    VkDescriptorPool pools[VK3D_MAX_POOLS];
    int              pool_count, cur_pool;
    vk3d_grave      *graves;
    int              grave_count, grave_cap;
} vk3d_frame;

typedef struct {
    VkBuffer      buf;
    VkDeviceSize  range;
    VkImageView   view;
    VkSampler     sampler;
    VkImageLayout layout;
} vk3d_res;

enum { VK3D_DUMMY_2D, VK3D_DUMMY_3D, VK3D_DUMMY_CUBE, VK3D_DUMMY_2D_ARRAY, VK3D_DUMMY_DEPTH, VK3D_DUMMY_COUNT };

static struct {
    int                 shutting_down;
    VkDeviceSize        ubo_align;
    vk3d_frame          frames[VIO_VK_MAX_FRAMES_IN_FLIGHT];
    vio_vulkan_texture *dummy[VK3D_DUMMY_COUNT];
    VkBuffer            zero_buf;     void *zero_alloc;
    VkBuffer            identity_buf; void *identity_alloc;
    /* bound state */
    vio_vk3d_pipeline          *pipeline;
    vio_vulkan_texture         *tex[VK3D_MAX_SAMPLERS];
    vio_vulkan_compute_buffer  *storage[VK3D_MAX_STORAGE];
    VkBuffer                    ubo_buf[2];
    uint32_t                    ubo_off[2];
    /* descriptor set reuse for consecutive identical draws */
    VkDescriptorSet             last_set;
    vio_vk3d_shader            *last_shader;
    vk3d_res                    last_res[VK3D_MAX_BINDINGS];
} vk3d;

/* ── Formats ───────────────────────────────────────────────────────── */

VkFormat vk3d_format_from_vio(vio_format f)
{
    switch (f) {
        case VIO_FLOAT1: return VK_FORMAT_R32_SFLOAT;
        case VIO_FLOAT2: return VK_FORMAT_R32G32_SFLOAT;
        case VIO_FLOAT3: return VK_FORMAT_R32G32B32_SFLOAT;
        case VIO_INT1:   return VK_FORMAT_R32_SINT;
        case VIO_INT2:   return VK_FORMAT_R32G32_SINT;
        case VIO_INT3:   return VK_FORMAT_R32G32B32_SINT;
        case VIO_INT4:   return VK_FORMAT_R32G32B32A32_SINT;
        case VIO_UINT1:  return VK_FORMAT_R32_UINT;
        case VIO_UINT2:  return VK_FORMAT_R32G32_UINT;
        case VIO_UINT3:  return VK_FORMAT_R32G32B32_UINT;
        case VIO_UINT4:  return VK_FORMAT_R32G32B32A32_UINT;
        default:         return VK_FORMAT_R32G32B32A32_SFLOAT;
    }
}

uint32_t vk3d_format_size(vio_format f)
{
    switch (f) {
        case VIO_FLOAT1: case VIO_INT1: case VIO_UINT1: return 4;
        case VIO_FLOAT2: case VIO_INT2: case VIO_UINT2: return 8;
        case VIO_FLOAT3: case VIO_INT3: case VIO_UINT3: return 12;
        default: return 16;
    }
}

static VkImageAspectFlags vk3d_depth_aspect(void)
{
    return VK_IMAGE_ASPECT_DEPTH_BIT | (vio_vk.depth_has_stencil ? VK_IMAGE_ASPECT_STENCIL_BIT : 0);
}

static VkAccessFlags vk3d_layout_access(VkImageLayout l)
{
    switch (l) {
        case VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL:             return VK_ACCESS_TRANSFER_WRITE_BIT;
        case VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL:             return VK_ACCESS_TRANSFER_READ_BIT;
        case VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL:
        case VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL:  return VK_ACCESS_SHADER_READ_BIT;
        case VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL:         return VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        case VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL: return VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
        default:                                               return 0;
    }
}

void vio_vk_image_barrier(VkCommandBuffer cmd, VkImage image, VkImageAspectFlags aspect, uint32_t layers,
                          VkImageLayout from, VkImageLayout to)
{
    VkImageMemoryBarrier b = {0};
    b.sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    b.oldLayout           = from;
    b.newLayout           = to;
    b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image               = image;
    b.subresourceRange.aspectMask = aspect;
    b.subresourceRange.levelCount = 1;
    b.subresourceRange.layerCount = layers ? layers : 1;
    b.srcAccessMask       = vk3d_layout_access(from);
    b.dstAccessMask       = vk3d_layout_access(to);
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                         0, 0, NULL, 0, NULL, 1, &b);
}

/* ── Deferred destruction ──────────────────────────────────────────── */

static void vk3d_destroy_now(int kind, uint64_t h, void *alloc)
{
    VkDevice d = vio_vk.device;
    if (!d || !h) return;
    switch (kind) {
        case VIO_VK_GRAVE_IMAGE:           vio_vma_destroy_image(vio_vk.vma_allocator, (VkImage)h, alloc); break;
        case VIO_VK_GRAVE_VIEW:            vkDestroyImageView(d, (VkImageView)h, NULL); break;
        case VIO_VK_GRAVE_SAMPLER:         vkDestroySampler(d, (VkSampler)h, NULL); break;
        case VIO_VK_GRAVE_BUFFER:          vio_vma_destroy_buffer(vio_vk.vma_allocator, (VkBuffer)h, alloc); break;
        case VIO_VK_GRAVE_PIPELINE:        vkDestroyPipeline(d, (VkPipeline)h, NULL); break;
        case VIO_VK_GRAVE_PIPELINE_LAYOUT: vkDestroyPipelineLayout(d, (VkPipelineLayout)h, NULL); break;
        case VIO_VK_GRAVE_SET_LAYOUT:      vkDestroyDescriptorSetLayout(d, (VkDescriptorSetLayout)h, NULL); break;
        case VIO_VK_GRAVE_SHADER_MODULE:   vkDestroyShaderModule(d, (VkShaderModule)h, NULL); break;
        case VIO_VK_GRAVE_FRAMEBUFFER:     vkDestroyFramebuffer(d, (VkFramebuffer)h, NULL); break;
        case VIO_VK_GRAVE_RENDER_PASS:     vkDestroyRenderPass(d, (VkRenderPass)h, NULL); break;
        default: break;
    }
}

/* Objects released while a frame is recording may be referenced by that frame's
 * command buffer: park them in the slot being recorded and destroy them when
 * vulkan_begin_frame has waited that slot's fence again. Outside a frame the
 * device is drained first. */
void vio_vk_defer_destroy(int kind, uint64_t handle, void *allocation)
{
    if (!vio_vk.device || !handle) return;
    if (!vio_vk.in_frame || vk3d.shutting_down) {
        if (!vk3d.shutting_down) vkDeviceWaitIdle(vio_vk.device);
        vk3d_destroy_now(kind, handle, allocation);
        return;
    }
    vk3d_frame *f = &vk3d.frames[vio_vk.current_frame % VIO_VK_MAX_FRAMES_IN_FLIGHT];
    if (f->grave_count == f->grave_cap) {
        int cap = f->grave_cap ? f->grave_cap * 2 : 64;
        vk3d_grave *g = (vk3d_grave *)realloc(f->graves, (size_t)cap * sizeof(vk3d_grave));
        if (!g) { vkDeviceWaitIdle(vio_vk.device); vk3d_destroy_now(kind, handle, allocation); return; }
        f->graves = g;
        f->grave_cap = cap;
    }
    f->graves[f->grave_count].kind   = kind;
    f->graves[f->grave_count].handle = handle;
    f->graves[f->grave_count].alloc  = allocation;
    f->grave_count++;
}

static void vk3d_flush_graves(vk3d_frame *f)
{
    for (int i = 0; i < f->grave_count; i++) vk3d_destroy_now(f->graves[i].kind, f->graves[i].handle, f->graves[i].alloc);
    f->grave_count = 0;
}

/* ── Frame resources ───────────────────────────────────────────────── */

static VkDeviceSize vk3d_ubo_align(void)
{
    if (!vk3d.ubo_align) {
        VkPhysicalDeviceProperties props;
        vkGetPhysicalDeviceProperties(vio_vk.physical_device, &props);
        vk3d.ubo_align = props.limits.minUniformBufferOffsetAlignment ? props.limits.minUniformBufferOffsetAlignment : 16;
    }
    return vk3d.ubo_align;
}

static int vk3d_upload(const void *data, VkDeviceSize data_size, VkDeviceSize total, VkDeviceSize align,
                       VkBuffer *out_buf, VkDeviceSize *out_off)
{
    vk3d_frame *f = &vk3d.frames[vio_vk.current_frame % VIO_VK_MAX_FRAMES_IN_FLIGHT];
    if (total == 0) total = 4;
    if (data_size > total) data_size = total;
    if (align == 0) align = 1;
    for (;;) {
        if (f->cur_chunk < f->chunk_count) {
            vk3d_chunk *c = &f->chunks[f->cur_chunk];
            VkDeviceSize o = (c->used + align - 1) & ~(align - 1);
            if (o + total <= c->size) {
                if (data && data_size) memcpy(c->mapped + o, data, (size_t)data_size);
                if (total > data_size) memset(c->mapped + o + data_size, 0, (size_t)(total - data_size));
                c->used = o + total;
                *out_buf = c->buf;
                *out_off = o;
                return 0;
            }
            f->cur_chunk++;
            continue;
        }
        if (f->chunk_count >= VK3D_MAX_CHUNKS) {
            php_error_docref(NULL, E_WARNING, "Vulkan: per-frame upload ring exhausted");
            return -1;
        }
        vk3d_chunk *c = &f->chunks[f->chunk_count];
        c->size = total > VK3D_CHUNK_SIZE ? total : VK3D_CHUNK_SIZE;
        c->used = 0;
        if (vio_vma_create_buffer(vio_vk.vma_allocator, c->size,
                                  VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT |
                                  VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                                  VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                                  &c->buf, &c->alloc) != 0 ||
            (c->mapped = (unsigned char *)vio_vma_map(vio_vk.vma_allocator, c->alloc)) == NULL) {
            if (c->buf) vio_vma_destroy_buffer(vio_vk.vma_allocator, c->buf, c->alloc);
            memset(c, 0, sizeof(*c));
            php_error_docref(NULL, E_WARNING, "Vulkan: upload ring chunk allocation failed");
            return -1;
        }
        f->chunk_count++;
    }
}

static VkDescriptorSet vk3d_alloc_set(VkDescriptorSetLayout layout)
{
    vk3d_frame *f = &vk3d.frames[vio_vk.current_frame % VIO_VK_MAX_FRAMES_IN_FLIGHT];
    for (;;) {
        if (f->cur_pool < f->pool_count) {
            VkDescriptorSetAllocateInfo ai = {0};
            ai.sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
            ai.descriptorPool     = f->pools[f->cur_pool];
            ai.descriptorSetCount = 1;
            ai.pSetLayouts        = &layout;
            VkDescriptorSet set = VK_NULL_HANDLE;
            VkResult r = vkAllocateDescriptorSets(vio_vk.device, &ai, &set);
            if (r == VK_SUCCESS) return set;
            if (r != VK_ERROR_OUT_OF_POOL_MEMORY && r != VK_ERROR_FRAGMENTED_POOL) return VK_NULL_HANDLE;
            f->cur_pool++;
            continue;
        }
        if (f->pool_count >= VK3D_MAX_POOLS) {
            php_error_docref(NULL, E_WARNING, "Vulkan: descriptor pools exhausted for this frame");
            return VK_NULL_HANDLE;
        }
        VkDescriptorPoolSize sizes[4] = {
            { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, VK3D_SETS_PER_POOL * 2 },
            { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,         VK3D_SETS_PER_POOL * VK3D_MAX_EXTRA_UBO },
            { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK3D_SETS_PER_POOL * VK3D_MAX_SAMPLERS },
            { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,         VK3D_SETS_PER_POOL * VK3D_MAX_STORAGE },
        };
        VkDescriptorPoolCreateInfo pi = {0};
        pi.sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        pi.maxSets       = VK3D_SETS_PER_POOL;
        pi.poolSizeCount = 4;
        pi.pPoolSizes    = sizes;
        if (vkCreateDescriptorPool(vio_vk.device, &pi, NULL, &f->pools[f->pool_count]) != VK_SUCCESS) {
            return VK_NULL_HANDLE;
        }
        f->pool_count++;
    }
}

void vio_vk3d_begin_frame(uint32_t slot)
{
    if (slot >= VIO_VK_MAX_FRAMES_IN_FLIGHT || !vio_vk.device) return;
    vk3d_frame *f = &vk3d.frames[slot];
    vk3d_flush_graves(f);
    for (int i = 0; i < f->chunk_count; i++) f->chunks[i].used = 0;
    f->cur_chunk = 0;
    for (int i = 0; i < f->pool_count; i++) vkResetDescriptorPool(vio_vk.device, f->pools[i], 0);
    f->cur_pool = 0;
    vk3d.last_set = VK_NULL_HANDLE;
    vk3d.last_shader = NULL;
    vk3d.ubo_buf[0] = vk3d.ubo_buf[1] = VK_NULL_HANDLE;
}

/* ── Dummy resources (unbound samplers / blocks / storage) ─────────── */

static vio_vulkan_texture *vk3d_dummy(int which)
{
    if (vk3d.dummy[which]) return vk3d.dummy[which];
    int depth = which == VK3D_DUMMY_DEPTH, cube = which == VK3D_DUMMY_CUBE;
    vio_vulkan_texture *t = (vio_vulkan_texture *)calloc(1, sizeof(vio_vulkan_texture));
    if (!t) return NULL;
    VkFormat fmt = depth ? vio_vk_depth_format() : VK_FORMAT_R8G8B8A8_UNORM;
    VkImageCreateInfo ci = {0};
    ci.sType         = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    ci.imageType     = which == VK3D_DUMMY_3D ? VK_IMAGE_TYPE_3D : VK_IMAGE_TYPE_2D;
    ci.format        = fmt;
    ci.extent.width  = 1; ci.extent.height = 1; ci.extent.depth = 1;
    ci.mipLevels     = 1;
    ci.arrayLayers   = cube ? 6 : 1;
    ci.samples       = VK_SAMPLE_COUNT_1_BIT;
    ci.tiling        = VK_IMAGE_TILING_OPTIMAL;
    ci.usage         = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    ci.sharingMode   = VK_SHARING_MODE_EXCLUSIVE;
    ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    ci.flags         = cube ? VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT : 0;
    if (vio_vma_create_image(vio_vk.vma_allocator, &ci, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &t->image, &t->allocation) != 0) {
        free(t);
        return NULL;
    }
    VkImageAspectFlags full = depth ? vk3d_depth_aspect() : VK_IMAGE_ASPECT_COLOR_BIT;
    VkImageLayout final_layout = depth ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    if (vio_vk_begin_transient(&cmd) == 0) {
        VkImageSubresourceRange range = { full, 0, 1, 0, cube ? 6u : 1u };
        vio_vk_image_barrier(cmd, t->image, full, range.layerCount, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        if (depth) {
            VkClearDepthStencilValue dv = { 1.0f, 0 };
            vkCmdClearDepthStencilImage(cmd, t->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &dv, 1, &range);
        } else {
            VkClearColorValue cv = {{ 0.0f, 0.0f, 0.0f, 0.0f }};
            vkCmdClearColorImage(cmd, t->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &cv, 1, &range);
        }
        vio_vk_image_barrier(cmd, t->image, full, range.layerCount, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, final_layout);
        vio_vk_submit_transient(cmd);
    }
    VkImageViewCreateInfo iv = {0};
    iv.sType    = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    iv.image    = t->image;
    iv.format   = fmt;
    iv.viewType = which == VK3D_DUMMY_3D ? VK_IMAGE_VIEW_TYPE_3D
                : cube ? VK_IMAGE_VIEW_TYPE_CUBE
                : which == VK3D_DUMMY_2D_ARRAY ? VK_IMAGE_VIEW_TYPE_2D_ARRAY : VK_IMAGE_VIEW_TYPE_2D;
    iv.subresourceRange.aspectMask = depth ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT;
    iv.subresourceRange.levelCount = 1;
    iv.subresourceRange.layerCount = cube ? 6 : 1;
    vkCreateImageView(vio_vk.device, &iv, NULL, &t->view);
    VkSamplerCreateInfo sci = {0};
    sci.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    sci.magFilter = sci.minFilter = VK_FILTER_NEAREST;
    sci.addressModeU = sci.addressModeV = sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    vkCreateSampler(vio_vk.device, &sci, NULL, &t->sampler);
    sci.compareEnable = VK_TRUE;
    sci.compareOp = VK_COMPARE_OP_LESS_OR_EQUAL;
    vkCreateSampler(vio_vk.device, &sci, NULL, &t->sampler_cmp);
    t->width = t->height = 1;
    t->view_type = (int)iv.viewType;
    t->layout = (int)final_layout;
    t->is_depth = depth;
    vk3d.dummy[which] = t;
    return t;
}

static VkBuffer vk3d_zero_buffer(void)
{
    if (!vk3d.zero_buf) {
        if (vio_vma_create_buffer(vio_vk.vma_allocator, VK3D_ZERO_BUF_SIZE,
                                  VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                                  VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                                  &vk3d.zero_buf, &vk3d.zero_alloc) != 0) {
            vk3d.zero_buf = VK_NULL_HANDLE;
            return VK_NULL_HANDLE;
        }
        void *m = vio_vma_map(vio_vk.vma_allocator, vk3d.zero_alloc);
        if (m) { memset(m, 0, (size_t)VK3D_ZERO_BUF_SIZE); vio_vma_unmap(vio_vk.vma_allocator, vk3d.zero_alloc); }
    }
    return vk3d.zero_buf;
}

static VkBuffer vk3d_identity_buffer(void)
{
    if (!vk3d.identity_buf) {
        static const float ident[16] = { 1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1 };
        if (vio_vma_create_buffer(vio_vk.vma_allocator, 64, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                                  VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                                  &vk3d.identity_buf, &vk3d.identity_alloc) != 0) {
            vk3d.identity_buf = VK_NULL_HANDLE;
            return VK_NULL_HANDLE;
        }
        void *m = vio_vma_map(vio_vk.vma_allocator, vk3d.identity_alloc);
        if (m) { memcpy(m, ident, sizeof(ident)); vio_vma_unmap(vio_vk.vma_allocator, vk3d.identity_alloc); }
    }
    return vk3d.identity_buf;
}

static void vk3d_free_texture_now(vio_vulkan_texture *t)
{
    if (!t) return;
    if (t->sampler_cmp) vkDestroySampler(vio_vk.device, t->sampler_cmp, NULL);
    if (t->sampler)     vkDestroySampler(vio_vk.device, t->sampler, NULL);
    if (t->view)        vkDestroyImageView(vio_vk.device, t->view, NULL);
    if (t->image)       vio_vma_destroy_image(vio_vk.vma_allocator, t->image, t->allocation);
    free(t);
}

void vio_vk3d_shutdown(void)
{
    if (!vio_vk.device) { memset(&vk3d, 0, sizeof(vk3d)); return; }
    vk3d.shutting_down = 1;
    vkDeviceWaitIdle(vio_vk.device);
    while (vk3d_live_pipelines) vk3d_pipeline_release_gpu(vk3d_live_pipelines);
    while (vk3d_live_shaders)   vk3d_shader_release_gpu(vk3d_live_shaders);
    for (int s = 0; s < VIO_VK_MAX_FRAMES_IN_FLIGHT; s++) {
        vk3d_frame *f = &vk3d.frames[s];
        vk3d_flush_graves(f);
        free(f->graves);
        for (int i = 0; i < f->chunk_count; i++) {
            if (f->chunks[i].mapped) vio_vma_unmap(vio_vk.vma_allocator, f->chunks[i].alloc);
            vio_vma_destroy_buffer(vio_vk.vma_allocator, f->chunks[i].buf, f->chunks[i].alloc);
        }
        for (int i = 0; i < f->pool_count; i++) vkDestroyDescriptorPool(vio_vk.device, f->pools[i], NULL);
    }
    for (int i = 0; i < VK3D_DUMMY_COUNT; i++) vk3d_free_texture_now(vk3d.dummy[i]);
    if (vk3d.zero_buf)     vio_vma_destroy_buffer(vio_vk.vma_allocator, vk3d.zero_buf, vk3d.zero_alloc);
    if (vk3d.identity_buf) vio_vma_destroy_buffer(vio_vk.vma_allocator, vk3d.identity_buf, vk3d.identity_alloc);
    memset(&vk3d, 0, sizeof(vk3d));
}

/* ── Bound state ───────────────────────────────────────────────────── */

void vio_vk3d_forget_texture(vio_vulkan_texture *tex)
{
    for (int i = 0; i < VK3D_MAX_SAMPLERS; i++) if (vk3d.tex[i] == tex) vk3d.tex[i] = NULL;
    vk3d.last_shader = NULL;
}

void vio_vk3d_forget_buffer(vio_vulkan_compute_buffer *buf)
{
    for (int i = 0; i < VK3D_MAX_STORAGE; i++) if (vk3d.storage[i] == buf) vk3d.storage[i] = NULL;
    vk3d.last_shader = NULL;
}

void vk3d_forget_pipeline(vio_vk3d_pipeline *p)
{
    if (vk3d.pipeline == p) vk3d.pipeline = NULL;
}

void vio_vk3d_bind_pipeline(void *pipeline)
{
    vk3d.pipeline = (vio_vk3d_pipeline *)pipeline;
}

/* slot: the sampler index php_vio.c resolved from the bound shader's GL-unit map
 * (fragment reflection order), or the raw unit when the map has no entry - then
 * it addresses register `slot` directly, like D3D11/D3D12. */
void vio_vk3d_bind_texture(void *texture, int slot)
{
    if (!texture || slot < 0 || slot >= VK3D_MAX_SAMPLERS) return;
    vio_vk3d_shader *sh = vk3d.pipeline ? vk3d.pipeline->shader : NULL;
    int binding = (sh && slot < sh->fs_sampler_count && sh->fs_sampler_binding[slot] >= VK3D_B_SAMPLER0)
                ? sh->fs_sampler_binding[slot] : VK3D_B_SAMPLER0 + slot;
    int idx = binding - VK3D_B_SAMPLER0;
    if (idx < 0 || idx >= VK3D_MAX_SAMPLERS) return;
    vk3d.tex[idx] = (vio_vulkan_texture *)texture;
}

void vio_vk3d_bind_storage_buffer(void *buf, int binding, int access, int count, int stride)
{
    (void)access; (void)count; (void)stride;
    if (binding < 0 || binding >= VK3D_MAX_STORAGE) return;
    vk3d.storage[binding] = (vio_vulkan_compute_buffer *)buf;
}

void vio_vk3d_push_cbuffers(const void *vs, int vs_size, const void *fs, int fs_size)
{
    vio_vk3d_pipeline *p = vk3d.pipeline;
    if (!p || !p->shader || p->shader->dead || !vio_vk.in_frame) return;
    for (int i = 0; i < p->shader->binding_count; i++) {
        const vk3d_binding *b = &p->shader->bindings[i];
        if (b->type != VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC || b->binding > 1) continue;
        const void *src = b->binding == 0 ? vs : fs;
        int n = b->binding == 0 ? vs_size : fs_size;
        VkDeviceSize size = b->size ? b->size : 4;
        VkBuffer buf = VK_NULL_HANDLE;
        VkDeviceSize off = 0;
        if (vk3d_upload(src, (VkDeviceSize)(n > 0 ? n : 0), size, vk3d_ubo_align(), &buf, &off) == 0) {
            vk3d.ubo_buf[b->binding] = buf;
            vk3d.ubo_off[b->binding] = (uint32_t)off;
        }
    }
}

void vio_vk3d_set_viewport(int x, int y, int w, int h)
{
    if (!vio_vk.in_frame || !vio_vk.cur_render_pass || w <= 0 || h <= 0) return;
    VkCommandBuffer cmd = vio_vk.frames[vio_vk.current_frame].cmd_buf;
    VkViewport vp = { (float)x, (float)y, (float)w, (float)h, 0.0f, 1.0f };
    vkCmdSetViewport(cmd, 0, 1, &vp);
    int x0 = x < 0 ? 0 : x, y0 = y < 0 ? 0 : y;
    int x1 = x + w, y1 = y + h;
    if (x1 > (int)vio_vk.cur_width)  x1 = (int)vio_vk.cur_width;
    if (y1 > (int)vio_vk.cur_height) y1 = (int)vio_vk.cur_height;
    if (x1 <= x0 || y1 <= y0) return;
    VkRect2D sc = { { x0, y0 }, { (uint32_t)(x1 - x0), (uint32_t)(y1 - y0) } };
    vkCmdSetScissor(cmd, 0, 1, &sc);
}

static VkSampler vk3d_cmp_sampler(vio_vulkan_texture *t)
{
    if (!t->sampler_cmp) {
        VkSamplerCreateInfo sci = {0};
        sci.sType         = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
        sci.magFilter     = sci.minFilter = VK_FILTER_LINEAR;
        sci.addressModeU  = sci.addressModeV = sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
        sci.borderColor   = VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE;
        sci.compareEnable = VK_TRUE;
        sci.compareOp     = VK_COMPARE_OP_LESS_OR_EQUAL;
        sci.maxLod        = 0.0f;
        if (vkCreateSampler(vio_vk.device, &sci, NULL, &t->sampler_cmp) != VK_SUCCESS) t->sampler_cmp = VK_NULL_HANDLE;
    }
    return t->sampler_cmp;
}

static void vk3d_resolve(vio_vk3d_shader *sh, vk3d_res *out)
{
    memset(out, 0, sizeof(vk3d_res) * (size_t)sh->binding_count);
    for (int i = 0; i < sh->binding_count; i++) {
        const vk3d_binding *b = &sh->bindings[i];
        vk3d_res *r = &out[i];
        switch (b->type) {
            case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC:
                r->buf = vk3d.ubo_buf[b->binding & 1];
                r->range = b->size ? b->size : 4;
                break;
            case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER:
                r->buf = vk3d_zero_buffer();
                r->range = b->size && b->size < VK3D_ZERO_BUF_SIZE ? b->size : VK3D_ZERO_BUF_SIZE;
                break;
            case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER: {
                vio_vulkan_compute_buffer *sb = vk3d.storage[(b->binding - VK3D_B_STORAGE0) % VK3D_MAX_STORAGE];
                r->buf = (sb && sb->buffer) ? sb->buffer : vk3d_zero_buffer();
                r->range = VK_WHOLE_SIZE;
                break;
            }
            default: {
                vio_vulkan_texture *t = vk3d.tex[(b->binding - VK3D_B_SAMPLER0) % VK3D_MAX_SAMPLERS];
                int vt = t ? (t->view_type ? t->view_type : VK_IMAGE_VIEW_TYPE_2D) : -1;
                if (t && t->view && vt == (int)b->dim && (!b->is_depth || t->is_depth)) {
                    r->view = t->view;
                    r->sampler = b->is_depth ? vk3d_cmp_sampler(t) : t->sampler;
                    r->layout = t->layout ? (VkImageLayout)t->layout : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
                }
                if (!r->view || !r->sampler) {
                    int which = b->is_depth ? VK3D_DUMMY_DEPTH
                              : b->dim == VK_IMAGE_VIEW_TYPE_3D ? VK3D_DUMMY_3D
                              : b->dim == VK_IMAGE_VIEW_TYPE_CUBE ? VK3D_DUMMY_CUBE
                              : b->dim == VK_IMAGE_VIEW_TYPE_2D_ARRAY ? VK3D_DUMMY_2D_ARRAY : VK3D_DUMMY_2D;
                    vio_vulkan_texture *d = vk3d_dummy(which);
                    if (d) {
                        r->view = d->view;
                        r->sampler = b->is_depth ? d->sampler_cmp : d->sampler;
                        r->layout = (VkImageLayout)d->layout;
                    }
                }
                break;
            }
        }
    }
}

static VkDescriptorSet vk3d_descriptor_set(vio_vk3d_shader *sh)
{
    vk3d_res res[VK3D_MAX_BINDINGS];
    vk3d_resolve(sh, res);
    if (vk3d.last_set && vk3d.last_shader == sh &&
        memcmp(res, vk3d.last_res, sizeof(vk3d_res) * (size_t)sh->binding_count) == 0) {
        return vk3d.last_set;
    }
    VkDescriptorSet set = vk3d_alloc_set(sh->set_layout);
    if (!set) return VK_NULL_HANDLE;
    VkWriteDescriptorSet w[VK3D_MAX_BINDINGS];
    VkDescriptorBufferInfo bi[VK3D_MAX_BINDINGS];
    VkDescriptorImageInfo ii[VK3D_MAX_BINDINGS];
    uint32_t n = 0;
    for (int i = 0; i < sh->binding_count; i++) {
        const vk3d_binding *b = &sh->bindings[i];
        memset(&w[n], 0, sizeof(w[n]));
        w[n].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        w[n].dstSet          = set;
        w[n].dstBinding      = b->binding;
        w[n].descriptorCount = 1;
        w[n].descriptorType  = b->type;
        if (b->type == VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER) {
            if (!res[i].view || !res[i].sampler) continue;
            ii[n].imageView   = res[i].view;
            ii[n].sampler     = res[i].sampler;
            ii[n].imageLayout = res[i].layout;
            w[n].pImageInfo   = &ii[n];
        } else {
            if (!res[i].buf) continue;
            bi[n].buffer = res[i].buf;
            bi[n].offset = 0;
            bi[n].range  = res[i].range;
            w[n].pBufferInfo = &bi[n];
        }
        n++;
    }
    if (n) vkUpdateDescriptorSets(vio_vk.device, n, w, 0, NULL);
    vk3d.last_set = set;
    vk3d.last_shader = sh;
    memcpy(vk3d.last_res, res, sizeof(vk3d_res) * (size_t)sh->binding_count);
    return set;
}

/* Bind pipeline variant, descriptor set (+ dynamic uniform offsets) and the
 * per-instance stream. The caller binds binding 0 / the index buffer. */
static int vk3d_prepare(uint32_t stride, VkBuffer inst_buf, VkDeviceSize inst_off)
{
    vio_vk3d_pipeline *p = vk3d.pipeline;
    if (!vio_vk.in_frame || !vio_vk.cur_render_pass || !p || p->dead || !p->shader || p->shader->dead) return -1;
    vio_vk3d_shader *sh = p->shader;
    VkPipeline pl = vk3d_pipeline_variant(p, stride);
    if (!pl) return -1;
    VkCommandBuffer cmd = vio_vk.frames[vio_vk.current_frame].cmd_buf;
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pl);
    vio_vk_apply_shading_rate(cmd);
    if (sh->binding_count > 0) {
        uint32_t dyn[2];
        uint32_t nd = 0;
        for (int i = 0; i < sh->binding_count; i++) {
            const vk3d_binding *b = &sh->bindings[i];
            if (b->type != VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC) continue;
            int idx = (int)(b->binding & 1);
            if (!vk3d.ubo_buf[idx]) {   /* no vio_set_uniform push for this draw: zeros */
                VkDeviceSize off = 0;
                if (vk3d_upload(NULL, 0, b->size ? b->size : 4, vk3d_ubo_align(), &vk3d.ubo_buf[idx], &off) != 0) return -1;
                vk3d.ubo_off[idx] = (uint32_t)off;
            }
            if (nd < 2) dyn[nd++] = vk3d.ubo_off[idx];
        }
        VkDescriptorSet set = vk3d_descriptor_set(sh);
        if (!set) return -1;
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, sh->layout, 0, 1, &set, nd, dyn);
    }
    if (p->has_instance_attrs) {
        VkBuffer b = inst_buf ? inst_buf : vk3d_identity_buffer();
        VkDeviceSize o = inst_buf ? inst_off : 0;
        if (!b) return -1;
        vkCmdBindVertexBuffers(cmd, 1, 1, &b, &o);
    }
    return 0;
}

static void vk3d_after_draw(void)
{
    vk3d.ubo_buf[0] = vk3d.ubo_buf[1] = VK_NULL_HANDLE;
}

static int vk3d_bind_mesh(VkCommandBuffer cmd, void *vb_ptr, void *ib_ptr, int index_bytes)
{
    vio_vulkan_compute_buffer *vb = (vio_vulkan_compute_buffer *)vb_ptr;
    if (!vb || !vb->buffer) return -1;
    VkDeviceSize zero = 0;
    vkCmdBindVertexBuffers(cmd, 0, 1, &vb->buffer, &zero);
    vio_vulkan_compute_buffer *ib = (vio_vulkan_compute_buffer *)ib_ptr;
    if (ib && ib->buffer) {
        vkCmdBindIndexBuffer(cmd, ib->buffer, 0, index_bytes == 2 ? VK_INDEX_TYPE_UINT16 : VK_INDEX_TYPE_UINT32);
    }
    return 0;
}

/* ── Draws ─────────────────────────────────────────────────────────── */

void vio_vk3d_draw(vio_draw_cmd *c)
{
    if (!c || !c->vertex_buffer || vk3d_prepare((uint32_t)c->vertex_stride, VK_NULL_HANDLE, 0) != 0) { vk3d_after_draw(); return; }
    VkCommandBuffer cmd = vio_vk.frames[vio_vk.current_frame].cmd_buf;
    if (vk3d_bind_mesh(cmd, c->vertex_buffer, NULL, 0) == 0) {
        vkCmdDraw(cmd, (uint32_t)c->vertex_count, (uint32_t)(c->instance_count > 0 ? c->instance_count : 1),
                  (uint32_t)c->first_vertex, 0);
    }
    vk3d_after_draw();
}

void vio_vk3d_draw_indexed(vio_draw_indexed_cmd *c)
{
    if (!c || !c->vertex_buffer || !c->index_buffer ||
        vk3d_prepare((uint32_t)c->vertex_stride, VK_NULL_HANDLE, 0) != 0) { vk3d_after_draw(); return; }
    VkCommandBuffer cmd = vio_vk.frames[vio_vk.current_frame].cmd_buf;
    if (vk3d_bind_mesh(cmd, c->vertex_buffer, c->index_buffer, c->index_bytes) == 0) {
        vkCmdDrawIndexed(cmd, (uint32_t)c->index_count, (uint32_t)(c->instance_count > 0 ? c->instance_count : 1),
                         (uint32_t)c->first_index, c->vertex_offset, 0);
    }
    vk3d_after_draw();
}

static void vk3d_draw_mesh(vio_mesh_object *mesh, uint32_t instances, VkBuffer inst_buf, VkDeviceSize inst_off)
{
    if (!mesh || !mesh->backend_vb || vk3d_prepare((uint32_t)mesh->stride, inst_buf, inst_off) != 0) { vk3d_after_draw(); return; }
    VkCommandBuffer cmd = vio_vk.frames[vio_vk.current_frame].cmd_buf;
    int indexed = mesh->index_count > 0 && mesh->backend_ib;
    if (vk3d_bind_mesh(cmd, mesh->backend_vb, indexed ? mesh->backend_ib : NULL, mesh->index_bytes) == 0) {
        if (indexed) vkCmdDrawIndexed(cmd, (uint32_t)mesh->index_count, instances, 0, 0, 0);
        else         vkCmdDraw(cmd, (uint32_t)mesh->vertex_count, instances, 0, 0);
    }
    vk3d_after_draw();
}

void vio_vk3d_draw_mesh_instanced(void *mesh_obj, const float *matrices, int count)
{
    if (!mesh_obj || !matrices || count <= 0 || !vio_vk.in_frame) return;
    VkBuffer buf = VK_NULL_HANDLE;
    VkDeviceSize off = 0;
    VkDeviceSize bytes = (VkDeviceSize)count * 64;
    if (vk3d_upload(matrices, bytes, bytes, 16, &buf, &off) != 0) return;
    vk3d_draw_mesh((vio_mesh_object *)mesh_obj, (uint32_t)count, buf, off);
}

void vio_vk3d_draw_instanced_from_storage(void *mesh_obj, int count)
{
    if (!mesh_obj || count <= 0) return;
    vk3d_draw_mesh((vio_mesh_object *)mesh_obj, (uint32_t)count, VK_NULL_HANDLE, 0);
}

void vio_vk3d_draw_indirect(void *mesh_obj, void *args_buffer, int max_draws, size_t offset)
{
    vio_mesh_object *mesh = (vio_mesh_object *)mesh_obj;
    vio_vulkan_compute_buffer *args = (vio_vulkan_compute_buffer *)args_buffer;
    if (!mesh || !mesh->backend_vb || !args || !args->buffer || max_draws <= 0) return;
    if (vk3d_prepare((uint32_t)mesh->stride, VK_NULL_HANDLE, 0) != 0) { vk3d_after_draw(); return; }
    VkCommandBuffer cmd = vio_vk.frames[vio_vk.current_frame].cmd_buf;
    int indexed = mesh->index_count > 0 && mesh->backend_ib;
    if (vk3d_bind_mesh(cmd, mesh->backend_vb, indexed ? mesh->backend_ib : NULL, mesh->index_bytes) == 0) {
        uint32_t stride = indexed ? 20u : 16u;
        if (vio_vk.multi_draw_indirect) {
            if (indexed) vkCmdDrawIndexedIndirect(cmd, args->buffer, (VkDeviceSize)offset, (uint32_t)max_draws, stride);
            else         vkCmdDrawIndirect(cmd, args->buffer, (VkDeviceSize)offset, (uint32_t)max_draws, stride);
        } else {
            for (int i = 0; i < max_draws; i++) {
                VkDeviceSize o = (VkDeviceSize)offset + (VkDeviceSize)i * stride;
                if (indexed) vkCmdDrawIndexedIndirect(cmd, args->buffer, o, 1, stride);
                else         vkCmdDrawIndirect(cmd, args->buffer, o, 1, stride);
            }
        }
    }
    vk3d_after_draw();
}

#endif /* HAVE_VULKAN */

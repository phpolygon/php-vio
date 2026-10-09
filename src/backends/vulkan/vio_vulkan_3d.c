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
    VkAccelerationStructureKHR accel;   /* ray query: the bound top-level structure */
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
    VkBuffer                    ubo_buf[VK3D_DYN_UBOS];   /* vk3d_dyn_index order: VS, FS, GS, TCS, TES */
    uint32_t                    ubo_off[VK3D_DYN_UBOS];
    /* descriptor set reuse for consecutive identical draws */
    VkDescriptorSet             last_set;
    vio_vk3d_shader            *last_shader;
    vk3d_res                    last_res[VK3D_MAX_BINDINGS];
} vk3d;

/* A recorded draw sequence (BUNDLE-PLAN phase 2): a secondary command buffer
 * for one attachment signature, with its own upload chunks and descriptor
 * pools (a vk3d_frame that no frame resets), so it can be played in any frame. */
typedef struct {
    VkCommandPool   pool;
    VkCommandBuffer cmd;
    vk3d_frame      res;
    int             count, samples, has_depth;
    VkFormat        formats[VIO_MAX_COLOR_ATTACHMENTS];
    uint32_t        view_mask, width, height;
    int             failed;          /* something could not be recorded: replay instead */
    int             released;        /* GPU objects freed at device shutdown, struct left for the PHP object */
    struct vk3d_bundle_link { void *prev, *next; } link;   /* live bundles, swept at shutdown */
} vk3d_bundle;

static vk3d_bundle *vk3d_rec;    /* the bundle being recorded, NULL otherwise */
static vk3d_bundle *vk3d_live;   /* every bundle with GPU objects */
static vio_vk3d_pipeline *vk3d_rec_saved_pipeline;   /* bound before the recording, bound again after it */
static void vk3d_bundles_sweep(void);
static vk3d_frame *vk3d_cur_frame(void)
{
    return vk3d_rec ? &vk3d_rec->res : &vk3d.frames[vio_vk.current_frame % VIO_VK_MAX_FRAMES_IN_FLIGHT];
}

static VkCommandBuffer vk3d_cmd(void)
{
    return vk3d_rec ? vk3d_rec->cmd : vio_vk.frames[vio_vk.current_frame].cmd_buf;
}

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

/* ── Barriers (synchronization2, VULKAN-MODERN-PLAN phase 3) ───────── */

/* Every shader stage that can sample an image on this device. */
static VkPipelineStageFlags2 vk3d_shader_stages(void)
{
    VkPipelineStageFlags2 s = VK_PIPELINE_STAGE_2_PRE_RASTERIZATION_SHADERS_BIT | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT
                            | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    if (vio_vk.rt_pipeline_supported) s |= VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR;
    return s;
}

/* The stages and accesses that use an image in `l`: the source scope of a
 * transition out of the layout (the work that last used it) and the
 * destination scope of a transition into it (the work that will). */
static void vk3d_layout_scope(VkImageLayout l, int src, VkPipelineStageFlags2 *stage, VkAccessFlags2 *access)
{
    switch (l) {
        case VK_IMAGE_LAYOUT_UNDEFINED:
        case VK_IMAGE_LAYOUT_PREINITIALIZED:
            *stage = VK_PIPELINE_STAGE_2_NONE; *access = VK_ACCESS_2_NONE; return;
        case VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL:
            *stage = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT; *access = VK_ACCESS_2_TRANSFER_WRITE_BIT; return;
        case VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL:
            *stage = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT; *access = VK_ACCESS_2_TRANSFER_READ_BIT; return;
        case VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL:
            *stage = vk3d_shader_stages(); *access = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT | VK_ACCESS_2_SHADER_READ_BIT; return;
        case VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL:
            /* sampled, or a read-only depth attachment */
            *stage  = vk3d_shader_stages() | VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT;
            *access = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT | VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT;
            return;
        case VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL:
            *stage = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
            *access = VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT; return;
        case VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL:
            *stage = VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT;
            *access = VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT; return;
        case VK_IMAGE_LAYOUT_FRAGMENT_SHADING_RATE_ATTACHMENT_OPTIMAL_KHR:   /* A18 */
            *stage = VK_PIPELINE_STAGE_2_FRAGMENT_SHADING_RATE_ATTACHMENT_BIT_KHR;
            *access = VK_ACCESS_2_FRAGMENT_SHADING_RATE_ATTACHMENT_READ_BIT_KHR; return;
        case VK_IMAGE_LAYOUT_PRESENT_SRC_KHR:
            /* out of it: the frame's colour writes that left it there; into it:
             * nothing (the present waits a semaphore) */
            if (src) { *stage = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT;
                       *access = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT; }
            else     { /* no access, but the stages a later transition out of it chains from */
                       *stage = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT;
                       *access = VK_ACCESS_2_NONE; }
            return;
        default:   /* GENERAL (storage images) and anything else */
            *stage = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
            *access = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT; return;
    }
}

void vio_vk_image_barrier_range(VkCommandBuffer cmd, VkImage image, VkImageAspectFlags aspect,
                                uint32_t base_level, uint32_t levels, uint32_t base_layer, uint32_t layers,
                                VkImageLayout from, VkImageLayout to)
{
    VkImageMemoryBarrier2 b = {0};
    b.sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
    vk3d_layout_scope(from, 1, &b.srcStageMask, &b.srcAccessMask);
    vk3d_layout_scope(to, 0, &b.dstStageMask, &b.dstAccessMask);
    b.oldLayout           = from;
    b.newLayout           = to;
    b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image               = image;
    b.subresourceRange.aspectMask     = aspect;
    b.subresourceRange.baseMipLevel   = base_level;
    b.subresourceRange.levelCount     = levels ? levels : 1;
    b.subresourceRange.baseArrayLayer = base_layer;
    b.subresourceRange.layerCount     = layers ? layers : 1;
    VkDependencyInfo di = {0};
    di.sType                   = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
    di.imageMemoryBarrierCount = 1;
    di.pImageMemoryBarriers    = &b;
    ((PFN_vkCmdPipelineBarrier2)vio_vk.fn_barrier2)(cmd, &di);
}

void vio_vk_image_barrier(VkCommandBuffer cmd, VkImage image, VkImageAspectFlags aspect, uint32_t layers,
                          VkImageLayout from, VkImageLayout to)
{
    vio_vk_image_barrier_range(cmd, image, aspect, 0, 1, 0, layers, from, to);
}

void vio_vk_pipeline_barrier(VkCommandBuffer cmd, VkPipelineStageFlags src, VkPipelineStageFlags dst, VkDependencyFlags dep,
                             uint32_t nmem, const VkMemoryBarrier *mem, uint32_t nbuf, const VkBufferMemoryBarrier *buf,
                             uint32_t nimg, const VkImageMemoryBarrier *img)
{
    VkMemoryBarrier2 m2[4];
    VkBufferMemoryBarrier2 b2[4];
    VkImageMemoryBarrier2 i2[8];
    if (nmem > 4 || nbuf > 4 || nimg > 8) return;
    /* TOP_OF_PIPE as a source and BOTTOM_OF_PIPE as a destination mean "none" */
    VkPipelineStageFlags2 s2 = src == VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT ? VK_PIPELINE_STAGE_2_NONE : (VkPipelineStageFlags2)src;
    VkPipelineStageFlags2 d2 = dst == VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT ? VK_PIPELINE_STAGE_2_NONE : (VkPipelineStageFlags2)dst;
    for (uint32_t i = 0; i < nmem; i++) {
        memset(&m2[i], 0, sizeof(m2[i]));
        m2[i].sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2;
        m2[i].srcStageMask = s2; m2[i].srcAccessMask = mem[i].srcAccessMask;
        m2[i].dstStageMask = d2; m2[i].dstAccessMask = mem[i].dstAccessMask;
    }
    for (uint32_t i = 0; i < nbuf; i++) {
        memset(&b2[i], 0, sizeof(b2[i]));
        b2[i].sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2;
        b2[i].srcStageMask = s2; b2[i].srcAccessMask = buf[i].srcAccessMask;
        b2[i].dstStageMask = d2; b2[i].dstAccessMask = buf[i].dstAccessMask;
        b2[i].srcQueueFamilyIndex = buf[i].srcQueueFamilyIndex;
        b2[i].dstQueueFamilyIndex = buf[i].dstQueueFamilyIndex;
        b2[i].buffer = buf[i].buffer; b2[i].offset = buf[i].offset; b2[i].size = buf[i].size;
    }
    for (uint32_t i = 0; i < nimg; i++) {
        memset(&i2[i], 0, sizeof(i2[i]));
        i2[i].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
        i2[i].srcStageMask = s2; i2[i].srcAccessMask = img[i].srcAccessMask;
        i2[i].dstStageMask = d2; i2[i].dstAccessMask = img[i].dstAccessMask;
        i2[i].oldLayout = img[i].oldLayout; i2[i].newLayout = img[i].newLayout;
        i2[i].srcQueueFamilyIndex = img[i].srcQueueFamilyIndex;
        i2[i].dstQueueFamilyIndex = img[i].dstQueueFamilyIndex;
        i2[i].image = img[i].image; i2[i].subresourceRange = img[i].subresourceRange;
    }
    VkDependencyInfo di = {0};
    di.sType                    = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
    di.dependencyFlags          = dep;
    di.memoryBarrierCount       = nmem;  di.pMemoryBarriers       = nmem ? m2 : NULL;
    di.bufferMemoryBarrierCount = nbuf;  di.pBufferMemoryBarriers = nbuf ? b2 : NULL;
    di.imageMemoryBarrierCount  = nimg;  di.pImageMemoryBarriers  = nimg ? i2 : NULL;
    ((PFN_vkCmdPipelineBarrier2)vio_vk.fn_barrier2)(cmd, &di);
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
        case VIO_VK_GRAVE_COMMAND_POOL:    vkDestroyCommandPool(d, (VkCommandPool)h, NULL); break;
        case VIO_VK_GRAVE_DESCRIPTOR_POOL: vkDestroyDescriptorPool(d, (VkDescriptorPool)h, NULL); break;
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
    vk3d_frame *f = vk3d_cur_frame();
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

int vio_vk3d_upload_bytes(const void *data, VkDeviceSize size, VkBuffer *out_buf, VkDeviceSize *out_off)
{
    if (!vio_vk.in_frame || size == 0) return -1;
    return vk3d_upload(data, size, size, vk3d_ubo_align(), out_buf, out_off);
}

static VkDescriptorSet vk3d_alloc_set(VkDescriptorSetLayout layout)
{
    vk3d_frame *f = vk3d_cur_frame();
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
        VkDescriptorPoolSize sizes[5] = {
            { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, VK3D_SETS_PER_POOL * VK3D_DYN_UBOS },
            { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,         VK3D_SETS_PER_POOL * VK3D_MAX_EXTRA_UBO },
            { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK3D_SETS_PER_POOL * VK3D_MAX_SAMPLERS },
            { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,         VK3D_SETS_PER_POOL * VK3D_MAX_STORAGE },
            { VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, VK3D_SETS_PER_POOL },
        };
        VkDescriptorPoolCreateInfo pi = {0};
        pi.sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        pi.maxSets       = VK3D_SETS_PER_POOL;
        pi.poolSizeCount = vio_vk.ray_query_supported ? 5 : 4;   /* the type is only valid with the extension */
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
    memset(vk3d.ubo_buf, 0, sizeof(vk3d.ubo_buf));
}

/* ── Dummy resources (unbound samplers / blocks / storage) ─────────── */

static vio_vulkan_texture *vk3d_dummy(int which);

VkImageView vio_vk3d_dummy_view(int bindless_kind)
{
    vio_vulkan_texture *t = vk3d_dummy(bindless_kind == VIO_BINDLESS_KIND_CUBE ? VK3D_DUMMY_CUBE
                                       : bindless_kind == VIO_BINDLESS_KIND_ARRAY ? VK3D_DUMMY_2D_ARRAY : VK3D_DUMMY_2D);
    return t ? t->view : VK_NULL_HANDLE;
}

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
    /* The depth dummy rests in DEPTH_STENCIL_READ_ONLY, which needs the
     * depth-attachment usage (VUID-VkImageMemoryBarrier2-oldLayout-01210). */
    ci.usage         = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT
                     | (depth ? VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT : 0);
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

static VkBuffer   vk3d_scratch_buf;
static void      *vk3d_scratch_alloc;

static VkBuffer vk3d_scratch_storage(void)
{
    if (!vk3d_scratch_buf) {
        if (vio_vma_create_buffer(vio_vk.vma_allocator, VK3D_ZERO_BUF_SIZE, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                                  VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                                  &vk3d_scratch_buf, &vk3d_scratch_alloc) != 0) {
            vk3d_scratch_buf = VK_NULL_HANDLE;
            return vk3d_zero_buffer();
        }
    }
    return vk3d_scratch_buf;
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
    vk3d_bundles_sweep();
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
    if (vk3d_scratch_buf) vio_vma_destroy_buffer(vio_vk.vma_allocator, vk3d_scratch_buf, vk3d_scratch_alloc);
    vk3d_scratch_buf = VK_NULL_HANDLE;
    vk3d_scratch_alloc = NULL;
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

/* Default uniform block of a geometry / tessellation stage (bind_stage_constants
 * slot): the shadow copy goes into the frame ring like the VS / FS blocks in
 * vio_vk3d_push_cbuffers; backend_buffer is not used on this backend. */
void vio_vk3d_bind_stage_constants(int stage, void *backend_buffer, const void *data, size_t size)
{
    (void)backend_buffer;
    vio_vk3d_pipeline *p = vk3d.pipeline;
    if (!p || !p->shader || p->shader->dead || !vio_vk.in_frame) return;
    if (stage < VIO_STAGE_GEOMETRY || stage > VIO_STAGE_TESS_EVAL) return;
    uint32_t binding = (uint32_t)(VK3D_B_STAGE_UBO0 + (stage - VIO_STAGE_GEOMETRY));
    for (int i = 0; i < p->shader->binding_count; i++) {
        const vk3d_binding *b = &p->shader->bindings[i];
        if (b->binding != binding || b->type != VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC) continue;
        VkBuffer buf = VK_NULL_HANDLE;
        VkDeviceSize off = 0;
        if (vk3d_upload(data, (VkDeviceSize)size, b->size ? b->size : 4, vk3d_ubo_align(), &buf, &off) == 0) {
            int idx = vk3d_dyn_index(binding);
            vk3d.ubo_buf[idx] = buf;
            vk3d.ubo_off[idx] = (uint32_t)off;
        }
        return;
    }
}

/* Remember the single viewport / scissor a pass or vio_viewport set: the 3D
 * pipelines carry max_viewports viewports, and vk3d_prepare sets them all. */
void vio_vk_note_viewport(const VkViewport *vp, const VkRect2D *sc)
{
    if (vp) vio_vk.cur_vp[0] = *vp;
    if (sc) vio_vk.cur_sc[0] = *sc;
    vio_vk.cur_vp_count = 1;
}

/* Viewport rect -> scissor clamped to the open pass (zero extent when outside). */
static VkRect2D vk3d_clamped_scissor(int x, int y, int w, int h)
{
    int x0 = x < 0 ? 0 : x, y0 = y < 0 ? 0 : y;
    int x1 = x + w, y1 = y + h;
    if (x1 > (int)vio_vk.cur_width)  x1 = (int)vio_vk.cur_width;
    if (y1 > (int)vio_vk.cur_height) y1 = (int)vio_vk.cur_height;
    VkRect2D sc = { { x0, y0 }, { 0, 0 } };
    if (x1 > x0 && y1 > y0) { sc.extent.width = (uint32_t)(x1 - x0); sc.extent.height = (uint32_t)(y1 - y0); }
    return sc;
}

void vio_vk3d_set_viewport(int x, int y, int w, int h)
{
    if (!vio_vk.in_frame || !vio_vk.in_pass || w <= 0 || h <= 0) return;
    VkCommandBuffer cmd = vk3d_cmd();
    VkViewport vp = { (float)x, (float)y, (float)w, (float)h, 0.0f, 1.0f };
    vkCmdSetViewport(cmd, 0, 1, &vp);
    VkRect2D sc = vk3d_clamped_scissor(x, y, w, h);
    if (sc.extent.width == 0) { vio_vk_note_viewport(&vp, NULL); return; }
    vkCmdSetScissor(cmd, 0, 1, &sc);
    vio_vk_note_viewport(&vp, &sc);
}

/* vio_viewports: recorded at the next draw (vk3d_prepare), where every one of
 * the pipeline's max_viewports viewports must be set. */
int vio_vk3d_set_viewports(const int *rects, int count)
{
    if (!vio_vk.in_frame || !vio_vk.in_pass || count < 1 || (uint32_t)count > vio_vk.max_viewports) return -1;
    for (int i = 0; i < count; i++) {
        int x = rects[i * 4], y = rects[i * 4 + 1], w = rects[i * 4 + 2], h = rects[i * 4 + 3];
        VkViewport vp = { (float)x, (float)y, (float)w, (float)h, 0.0f, 1.0f };
        vio_vk.cur_vp[i] = vp;
        vio_vk.cur_sc[i] = vk3d_clamped_scissor(x, y, w, h);
    }
    vio_vk.cur_vp_count = (uint32_t)count;
    return 0;
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
                r->buf = vk3d.ubo_buf[vk3d_dyn_index(b->binding)];
                r->range = b->size ? b->size : 4;
                break;
            case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER:
                r->buf = vk3d_zero_buffer();
                r->range = b->size && b->size < VK3D_ZERO_BUF_SIZE ? b->size : VK3D_ZERO_BUF_SIZE;
                break;
            case VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR:
                r->accel = (VkAccelerationStructureKHR)vio_vk.bound_accel;
                break;
            case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER: {
                vio_vulkan_compute_buffer *sb = vk3d.storage[(b->binding - VK3D_B_STORAGE0) % VK3D_MAX_STORAGE];
                r->buf = (sb && sb->buffer) ? sb->buffer : vk3d_scratch_storage();
                r->range = VK_WHOLE_SIZE;
                break;
            }
            default: {
                vio_vulkan_texture *t = vk3d.tex[(b->binding - VK3D_B_SAMPLER0) % VK3D_MAX_SAMPLERS];
                int vt = t ? (t->view_type ? t->view_type : VK_IMAGE_VIEW_TYPE_2D) : -1;
                /* A slot still naming an image the open pass renders into (a
                 * target sampled by an earlier pass, bound for drawing again):
                 * a feedback loop in the wrong layout - the dummy instead. */
                if (t && vio_vk_pass_writes_image(t->image, (uint32_t)(t->mip_levels > 0 ? t->mip_levels : 1))) t = NULL;
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
    VkWriteDescriptorSetAccelerationStructureKHR ai[VK3D_MAX_BINDINGS];
    uint32_t n = 0;
    for (int i = 0; i < sh->binding_count; i++) {
        const vk3d_binding *b = &sh->bindings[i];
        memset(&w[n], 0, sizeof(w[n]));
        w[n].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        w[n].dstSet          = set;
        w[n].dstBinding      = b->binding;
        w[n].descriptorCount = 1;
        w[n].descriptorType  = b->type;
        if (b->type == VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR) {
            if (!res[i].accel) continue;
            memset(&ai[n], 0, sizeof(ai[n]));
            ai[n].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR;
            ai[n].accelerationStructureCount = 1;
            ai[n].pAccelerationStructures = &res[i].accel;
            w[n].pNext = &ai[n];
        } else if (b->type == VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER) {
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
    if (!vio_vk.in_frame || !vio_vk.in_pass || !p || p->dead || !p->shader || p->shader->dead) return -1;
    vio_vk3d_shader *sh = p->shader;
    /* Multiview pipelines draw into a multiview pass over the layered target;
     * plain pipelines need the plain pass back. */
    if (vk3d_rec) {
        /* a recording cannot restart the pass for another view count */
        uint32_t want = p->desc.view_count > 1 ? (1u << p->desc.view_count) - 1u : 0u;
        if (want != vio_vk.cur_view_mask) { vk3d_rec->failed = 1; return -1; }
    } else    if (vio_vk_rt_ensure_views(p->desc.view_count > 1 ? p->desc.view_count : 0) != 0) {
        php_error_docref(NULL, E_WARNING, "Vulkan: a multiview pipeline (view_count %d) needs a layered render target "
                         "with at least that many layers bound with VIO_RT_ALL_LAYERS", p->desc.view_count);
        return -1;
    }
    VkPipeline pl = vk3d_pipeline_variant(p, stride);
    if (!pl) return -1;
    VkCommandBuffer cmd = vk3d_cmd();
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pl);
    vio_vk_apply_shading_rate(cmd, sh->writes_shading_rate);
    if (vio_vk.max_viewports > 1) {
        /* The pipeline has max_viewports viewports: set every one (unused ones
         * repeat viewport 0). */
        VkViewport vps[16];
        VkRect2D scs[16];
        uint32_t n = vio_vk.cur_vp_count ? vio_vk.cur_vp_count : 1;
        for (uint32_t i = 0; i < vio_vk.max_viewports; i++) {
            vps[i] = vio_vk.cur_vp[i < n ? i : 0];
            scs[i] = vio_vk.cur_sc[i < n ? i : 0];
        }
        vkCmdSetViewport(cmd, 0, vio_vk.max_viewports, vps);
        vkCmdSetScissor(cmd, 0, vio_vk.max_viewports, scs);
    }
    if (sh->binding_count > 0) {
        uint32_t dyn[VK3D_DYN_UBOS];
        uint32_t nd = 0;
        for (int i = 0; i < sh->binding_count; i++) {
            const vk3d_binding *b = &sh->bindings[i];
            if (b->type != VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC) continue;
            int idx = vk3d_dyn_index(b->binding);
            if (!vk3d.ubo_buf[idx]) {   /* no vio_set_uniform push for this draw: zeros */
                VkDeviceSize off = 0;
                if (vk3d_upload(NULL, 0, b->size ? b->size : 4, vk3d_ubo_align(), &vk3d.ubo_buf[idx], &off) != 0) return -1;
                vk3d.ubo_off[idx] = (uint32_t)off;
            }
            if (nd < VK3D_DYN_UBOS) dyn[nd++] = vk3d.ubo_off[idx];
        }
        VkDescriptorSet set = vk3d_descriptor_set(sh);
        if (!set) return -1;
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, sh->layout, 0, 1, &set, nd, dyn);
    }
    if (sh->uses_bindless && vio_vk.bindless_set) {
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, sh->layout, 1, 1, &vio_vk.bindless_set, 0, NULL);
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
    memset(vk3d.ubo_buf, 0, sizeof(vk3d.ubo_buf));
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
    VkCommandBuffer cmd = vk3d_cmd();
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
    VkCommandBuffer cmd = vk3d_cmd();
    if (vk3d_bind_mesh(cmd, c->vertex_buffer, c->index_buffer, c->index_bytes) == 0) {
        vkCmdDrawIndexed(cmd, (uint32_t)c->index_count, (uint32_t)(c->instance_count > 0 ? c->instance_count : 1),
                         (uint32_t)c->first_index, c->vertex_offset, 0);
    }
    vk3d_after_draw();
}

static void vk3d_draw_mesh(vio_mesh_object *mesh, uint32_t instances, VkBuffer inst_buf, VkDeviceSize inst_off)
{
    if (!mesh || !mesh->backend_vb || vk3d_prepare((uint32_t)mesh->stride, inst_buf, inst_off) != 0) { vk3d_after_draw(); return; }
    VkCommandBuffer cmd = vk3d_cmd();
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
    VkCommandBuffer cmd = vk3d_cmd();
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

/* Mesh pipelines (VIO_FEATURE_MESH_SHADER): no vertex buffers, the task /
 * mesh stages generate the geometry. */
static int vk3d_mesh_bound(void)
{
    return vk3d.pipeline && vk3d.pipeline->shader && vk3d.pipeline->shader->is_mesh && vio_vk.mesh_supported;
}

void vio_vk3d_draw_mesh_tasks(uint32_t x, uint32_t y, uint32_t z)
{
    if (!vk3d_mesh_bound() || !x || !y || !z) return;
    if (vk3d_prepare(0, VK_NULL_HANDLE, 0) != 0) { vk3d_after_draw(); return; }
#ifdef VK_EXT_MESH_SHADER_EXTENSION_NAME
    VkCommandBuffer cmd = vk3d_cmd();
    ((PFN_vkCmdDrawMeshTasksEXT)vio_vk.mesh_cmd_draw)(cmd, x, y, z);
#endif
    vk3d_after_draw();
}

void vio_vk3d_draw_mesh_tasks_indirect(void *args_buffer, int max_draws, size_t offset)
{
    vio_vulkan_compute_buffer *args = (vio_vulkan_compute_buffer *)args_buffer;
    if (!vk3d_mesh_bound() || !args || !args->buffer || max_draws <= 0) return;
    if (vk3d_prepare(0, VK_NULL_HANDLE, 0) != 0) { vk3d_after_draw(); return; }
#ifdef VK_EXT_MESH_SHADER_EXTENSION_NAME
    VkCommandBuffer cmd = vk3d_cmd();
    PFN_vkCmdDrawMeshTasksIndirectEXT fn = (PFN_vkCmdDrawMeshTasksIndirectEXT)vio_vk.mesh_cmd_draw_indirect;
    const uint32_t stride = 12u;   /* VkDrawMeshTasksIndirectCommandEXT */
    if (vio_vk.multi_draw_indirect) {
        fn(cmd, args->buffer, (VkDeviceSize)offset, (uint32_t)max_draws, stride);
    } else {
        for (int i = 0; i < max_draws; i++) fn(cmd, args->buffer, (VkDeviceSize)offset + (VkDeviceSize)i * stride, 1, stride);
    }
#endif
    vk3d_after_draw();
}

/* ── Recorded draw sequences (BUNDLE-PLAN phase 2) ─────────────────── */

static void vk3d_bundle_unlink(vk3d_bundle *b)
{
    vk3d_bundle *prev = (vk3d_bundle *)b->link.prev, *next = (vk3d_bundle *)b->link.next;
    if (prev) prev->link.next = next; else if (vk3d_live == b) vk3d_live = next;
    if (next) next->link.prev = prev;
    b->link.prev = b->link.next = NULL;
}

/* Free the bundle's GPU objects (parked until the frame's timeline value
 * inside a frame); the struct stays. */
static void vk3d_bundle_release(vk3d_bundle *b)
{
    if (!b || b->released) return;
    vk3d_bundle_unlink(b);
    b->released = 1;
    vk3d_frame *r = &b->res;
    int defer = vio_vk.in_frame;   /* the frame may execute it: park until its timeline value */
    for (int i = 0; i < r->chunk_count; i++) {
        if (!r->chunks[i].buf) continue;
        /* The chunk is persistently mapped: unmap now (CPU side only, the GPU may
         * still read it), destroy once the frame's timeline value passed. */
        vio_vma_unmap(vio_vk.vma_allocator, r->chunks[i].alloc);
        if (defer) vio_vk_defer_destroy(VIO_VK_GRAVE_BUFFER, (uint64_t)r->chunks[i].buf, r->chunks[i].alloc);
        else vio_vma_destroy_buffer(vio_vk.vma_allocator, r->chunks[i].buf, r->chunks[i].alloc);
    }
    for (int i = 0; i < r->pool_count; i++) {
        if (defer) vio_vk_defer_destroy(VIO_VK_GRAVE_DESCRIPTOR_POOL, (uint64_t)r->pools[i], NULL);
        else vkDestroyDescriptorPool(vio_vk.device, r->pools[i], NULL);
    }
    if (b->pool) {
        if (defer) vio_vk_defer_destroy(VIO_VK_GRAVE_COMMAND_POOL, (uint64_t)b->pool, NULL);
        else vkDestroyCommandPool(vio_vk.device, b->pool, NULL);
    }
    memset(r, 0, sizeof(*r));
    b->pool = VK_NULL_HANDLE;
    b->cmd = VK_NULL_HANDLE;
}

static void vk3d_bundle_free(vk3d_bundle *b)
{
    if (!b) return;
    vk3d_bundle_release(b);
    free(b);
}

/* Device shutdown: no bundle keeps a GPU object past vkDestroyDevice (the PHP
 * objects are freed later and only free the struct then). */
static void vk3d_bundles_sweep(void)
{
    while (vk3d_live) vk3d_bundle_release(vk3d_live);
    vk3d_rec = NULL;
}

void *vio_vk3d_begin_bundle(void)
{
    if (!vio_vk.in_frame || !vio_vk.in_pass || vk3d_rec || !vio_vk.device) return NULL;
    if (vio_vk.cur_pass_vrs) return NULL;   /* a pass with a shading-rate image: replay */
    vk3d_bundle *b = (vk3d_bundle *)calloc(1, sizeof(vk3d_bundle));
    if (!b) return NULL;
    VkCommandPoolCreateInfo pci = {0};
    pci.sType            = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    pci.queueFamilyIndex = vio_vk.graphics_family;
    if (vkCreateCommandPool(vio_vk.device, &pci, NULL, &b->pool) != VK_SUCCESS) { free(b); return NULL; }
    b->link.next = vk3d_live;
    if (vk3d_live) vk3d_live->link.prev = b;
    vk3d_live = b;
    VkCommandBufferAllocateInfo ai = {0};
    ai.sType              = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    ai.commandPool        = b->pool;
    ai.level              = VK_COMMAND_BUFFER_LEVEL_SECONDARY;
    ai.commandBufferCount = 1;
    if (vkAllocateCommandBuffers(vio_vk.device, &ai, &b->cmd) != VK_SUCCESS) { vk3d_bundle_free(b); return NULL; }
    b->count     = vio_vk.cur_color_count > VIO_MAX_COLOR_ATTACHMENTS ? VIO_MAX_COLOR_ATTACHMENTS : vio_vk.cur_color_count;
    for (int i = 0; i < b->count; i++) b->formats[i] = vio_vk.cur_color_formats[i];
    b->samples   = vio_vk.cur_samples;
    b->has_depth = vio_vk.cur_has_depth;
    b->view_mask = vio_vk.cur_view_mask;
    b->width     = vio_vk.cur_width;
    b->height    = vio_vk.cur_height;
    VkCommandBufferInheritanceRenderingInfo rin = {0};
    rin.sType                   = VK_STRUCTURE_TYPE_COMMAND_BUFFER_INHERITANCE_RENDERING_INFO;
    rin.viewMask                = b->view_mask;
    rin.colorAttachmentCount    = (uint32_t)b->count;
    rin.pColorAttachmentFormats = b->formats;
    rin.depthAttachmentFormat   = b->has_depth ? vio_vk_depth_format() : VK_FORMAT_UNDEFINED;
    rin.stencilAttachmentFormat = (b->has_depth && vio_vk.depth_has_stencil) ? vio_vk_depth_format() : VK_FORMAT_UNDEFINED;
    rin.rasterizationSamples    = (VkSampleCountFlagBits)(b->samples > 1 ? b->samples : 1);
    VkCommandBufferInheritanceInfo inh = {0};
    inh.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_INHERITANCE_INFO;
    inh.pNext = &rin;
    VkCommandBufferBeginInfo bi = {0};
    bi.sType            = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    /* played in later frames while earlier ones may still run it */
    bi.flags            = VK_COMMAND_BUFFER_USAGE_RENDER_PASS_CONTINUE_BIT | VK_COMMAND_BUFFER_USAGE_SIMULTANEOUS_USE_BIT;
    bi.pInheritanceInfo = &inh;
    if (vkBeginCommandBuffer(b->cmd, &bi) != VK_SUCCESS) { vk3d_bundle_free(b); return NULL; }
    /* a secondary command buffer inherits no dynamic state */
    uint32_t n = vio_vk.cur_vp_count ? vio_vk.cur_vp_count : 1;
    vkCmdSetViewport(b->cmd, 0, 1, &vio_vk.cur_vp[0]);
    vkCmdSetScissor(b->cmd, 0, 1, &vio_vk.cur_sc[0]);
    (void)n;
    vk3d.last_set = VK_NULL_HANDLE;   /* no frame descriptor set inside the bundle */
    memset(vk3d.ubo_buf, 0, sizeof(vk3d.ubo_buf));
    vk3d_rec_saved_pipeline = vk3d.pipeline;
    vk3d_rec = b;
    return b;
}

int vio_vk3d_end_bundle(void *bundle)
{
    vk3d_bundle *b = (vk3d_bundle *)bundle;
    if (!b || vk3d_rec != b) return -1;
    vk3d_rec = NULL;
    vk3d.pipeline = vk3d_rec_saved_pipeline;   /* the state from before the bundle stays */
    vk3d.last_set = VK_NULL_HANDLE;   /* no bundle descriptor set in the frame either */
    memset(vk3d.ubo_buf, 0, sizeof(vk3d.ubo_buf));
    if (vkEndCommandBuffer(b->cmd) != VK_SUCCESS || b->failed) return -1;
    return 0;
}

int vio_vk3d_draw_bundle(void *bundle)
{
    vk3d_bundle *b = (vk3d_bundle *)bundle;
    if (!b || b->released || !vio_vk.in_frame || !vio_vk.in_pass || vk3d_rec) return -1;
    /* the signature it was recorded for */
    if (b->count != vio_vk.cur_color_count || b->samples != vio_vk.cur_samples || b->has_depth != vio_vk.cur_has_depth
        || b->view_mask != vio_vk.cur_view_mask || b->width != vio_vk.cur_width || b->height != vio_vk.cur_height
        || vio_vk.cur_pass_vrs) return -1;
    for (int i = 0; i < b->count; i++) if (b->formats[i] != vio_vk.cur_color_formats[i]) return -1;
    /* A pass instance runs either inline draws or secondary command buffers
     * (without VK_KHR_maintenance7): close it, run the bundle in an instance of
     * the same pass, reopen it with LOAD. The viewports of the open pass stay. */
    VkCommandBuffer cmd = vio_vk.frames[vio_vk.current_frame].cmd_buf;
    VkViewport vp[16];
    VkRect2D sc[16];
    uint32_t vp_count = vio_vk.cur_vp_count ? vio_vk.cur_vp_count : 1;
    if (vp_count > 16) vp_count = 16;
    memcpy(vp, vio_vk.cur_vp, sizeof(VkViewport) * vp_count);
    memcpy(sc, vio_vk.cur_sc, sizeof(VkRect2D) * vp_count);
    vio_vk_pass pass = vio_vk.cur_pass;
    vio_vk_pass_end(cmd);
    pass.secondary = 1;
    vio_vk_pass_begin(cmd, &pass);
    vkCmdExecuteCommands(cmd, 1, &b->cmd);
    vio_vk_pass_end(cmd);
    pass.secondary = 0;
    vio_vk_pass_begin(cmd, &pass);
    memcpy(vio_vk.cur_vp, vp, sizeof(VkViewport) * vp_count);
    memcpy(vio_vk.cur_sc, sc, sizeof(VkRect2D) * vp_count);
    vio_vk.cur_vp_count = vp_count;
    vkCmdSetViewport(cmd, 0, 1, &vp[0]);
    vkCmdSetScissor(cmd, 0, 1, &sc[0]);
    vk3d.last_set = VK_NULL_HANDLE;
    return 0;
}

void vio_vk3d_destroy_bundle(void *bundle)
{
    vk3d_bundle *b = (vk3d_bundle *)bundle;
    if (!b) return;
    if (vk3d_rec == b) { vk3d_rec = NULL; vkEndCommandBuffer(b->cmd); }
    if (b->released || !vio_vk.device) { free(b); return; }
    if (!vio_vk.in_frame) vio_vk_wait_value(vio_vk.timeline_value);
    vk3d_bundle_free(b);
}
#endif /* HAVE_VULKAN */

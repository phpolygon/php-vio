/*
 * php-vio - Vulkan 3D pipeline, internal interface (GAP-PHASE5 Block 10)
 *
 * Shared by vio_vulkan_3d_shader.c (shader round trip, descriptor layouts,
 * pipeline variants) and vio_vulkan_3d.c (frame upload ring, descriptor pools,
 * bound state, draws, deferred destruction, render-target readback).
 *
 * Conventions (identical to the D3D backends, so engine code needs no Vulkan
 * special case beyond BackendConventions):
 *   - GL-style GLSL goes GLSL -> SPIR-V (glslang) -> Vulkan GLSL (SPIRV-Cross,
 *     bindings remapped) -> SPIR-V. The vertex stage gets
 *     gl_Position.y = -y and z = (z + w) / 2 appended, so NDC +Y is the top of
 *     the render target (row 0, like D3D) and depth lands in [0, 1].
 *   - Descriptor set 0: binding 0 = vertex default uniform block, 1 = fragment
 *     default uniform block (both UNIFORM_BUFFER_DYNAMIC into the per-frame
 *     upload ring), 2..17 = combined image samplers (regular samplers 0..7,
 *     shadow samplers 8..15 - the D3D12 register scheme), 18..25 = storage
 *     buffers (18 + GLSL binding), 26..29 = further uniform blocks (bound to a
 *     zero buffer; vio has no user UBO binding on this backend).
 *   - Vertex input: locations 3..6 are per-instance (binding 1, 64-byte mat4
 *     columns), everything else per-vertex (binding 0) - the D3D input layout.
 */

#ifndef VIO_VULKAN_3D_H
#define VIO_VULKAN_3D_H

#ifdef HAVE_VULKAN

#include <vulkan/vulkan.h>
#include "vio_vulkan.h"

#define VK3D_B_VS_UBO       0
#define VK3D_B_FS_UBO       1
#define VK3D_B_SAMPLER0     2
#define VK3D_MAX_SAMPLERS   16
#define VK3D_B_STORAGE0     18
#define VK3D_MAX_STORAGE    8
#define VK3D_B_EXTRA_UBO0   26
#define VK3D_MAX_EXTRA_UBO  4
#define VK3D_MAX_BINDINGS   30
#define VK3D_MAX_VARIANTS   12

typedef struct _vk3d_binding {
    uint32_t           binding;
    VkDescriptorType   type;
    VkShaderStageFlags stages;
    VkImageViewType    dim;       /* samplers */
    int                is_depth;  /* sampler2DShadow */
    uint32_t           size;      /* uniform blocks: declared size in bytes */
} vk3d_binding;

typedef struct _vio_vk3d_shader {
    VkShaderModule        vs, fs;
    VkDescriptorSetLayout set_layout;
    VkPipelineLayout      layout;
    vk3d_binding          bindings[VK3D_MAX_BINDINGS];
    int                   binding_count;
    /* Fragment samplers in the ORIGINAL SPIR-V reflection order - the order
     * php_vio.c's GL-unit -> sampler-index map uses. */
    int                   fs_sampler_binding[VK3D_MAX_SAMPLERS];
    int                   fs_sampler_depth[VK3D_MAX_SAMPLERS];
    int                   fs_sampler_count;
    int                   fs_needed_without_color; /* fragment stage discards or writes depth */
    int                   dead;       /* GPU objects released (context torn down) */
    struct _vio_vk3d_shader *next, *prev;
} vio_vk3d_shader;

typedef struct _vk3d_variant {
    uint64_t   key;
    VkPipeline pipeline;
} vk3d_variant;

typedef struct _vio_vk3d_pipeline {
    vio_vk3d_shader   *shader;
    vio_pipeline_desc  desc;
    vio_vertex_attrib  attribs[16];
    int                attrib_count;
    uint32_t           vertex_stride;      /* sum of per-vertex attribute sizes */
    int                has_instance_attrs; /* any input at locations 3..6 */
    vk3d_variant       variants[VK3D_MAX_VARIANTS];
    int                variant_count;
    int                dead;
    struct _vio_vk3d_pipeline *next, *prev;
} vio_vk3d_pipeline;

/* vio_vulkan_3d_shader.c */
void        vk3d_forget_pipeline(vio_vk3d_pipeline *p);
void        vk3d_shader_release_gpu(vio_vk3d_shader *sh);
void        vk3d_pipeline_release_gpu(vio_vk3d_pipeline *p);
VkPipeline  vk3d_pipeline_variant(vio_vk3d_pipeline *p, uint32_t stride);
extern vio_vk3d_shader   *vk3d_live_shaders;
extern vio_vk3d_pipeline *vk3d_live_pipelines;

/* vio_vulkan_3d.c */
VkFormat    vk3d_format_from_vio(vio_format f);
uint32_t    vk3d_format_size(vio_format f);

#endif /* HAVE_VULKAN */
#endif /* VIO_VULKAN_3D_H */

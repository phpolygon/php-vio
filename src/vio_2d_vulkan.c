/*
 * php-vio - 2D Rendering Vulkan backend
 *
 * Mirrors vio_2d_d3d12.c: owns the 2D pipelines, pipeline layout and the
 * streaming vertex buffer ring. Per-frame recording lives in the HAVE_VULKAN
 * branch of vio_2d_flush() (src/vio_2d.c).
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "php.h"

#ifdef HAVE_VULKAN

#include <vulkan/vulkan.h>
#include <string.h>
#include <stddef.h>
#include <stdlib.h>

#include "vio_2d.h"
#include "vio_2d_vulkan.h"
#include "backends/vulkan/vio_vulkan.h"
#include "shaders/shaders_2d.h"

/* glslang GLSL -> SPIR-V (implemented in vio_shader_compiler.c) */
extern uint32_t *vio_compile_glsl_to_spirv(const char *source, int stage /*0=vertex,1=fragment*/,
                                           size_t *out_size, char **error_msg);

/* Sets per frame-in-flight pool. Generous so a frame touching many distinct
 * textures (one set per distinct texture per frame) never exhausts the pool. */
#define VIO_2D_VK_DESCRIPTORS_PER_POOL 1024

/* Single active 2D state, set in init / cleared in shutdown. vulkan_begin_frame
 * (a backend function with no context pointer) reaches the per-frame descriptor
 * pools through this so it can reset them after the in_flight fence wait. There
 * is effectively one rendering context, so a single pointer suffices; a second
 * init would simply re-point it (and the first would already be shut down). */
static vio_2d_vulkan_state *g_active_2d_vk = NULL;

/* ── Compile a GLSL stage into a VkShaderModule ──────────────────── */

static VkShaderModule vio_2d_vk_make_module(const char *glsl, int is_fragment)
{
    size_t spirv_size = 0;
    char  *err = NULL;
    uint32_t *spirv = vio_compile_glsl_to_spirv(glsl, is_fragment, &spirv_size, &err);
    if (!spirv) {
        php_error_docref(NULL, E_WARNING, "Vulkan 2D: %s shader GLSL->SPIR-V failed: %s",
                          is_fragment ? "FS" : "VS", err ? err : "unknown");
        free(err);
        return VK_NULL_HANDLE;
    }
    /* glslang may emit non-fatal SPIR-V messages into err even on success. */
    free(err);

    VkShaderModuleCreateInfo info = {0};
    info.sType    = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    info.codeSize = spirv_size;          /* bytes */
    info.pCode    = spirv;               /* uint32_t* */

    VkShaderModule module = VK_NULL_HANDLE;
    VkResult r = vkCreateShaderModule(vio_vk.device, &info, NULL, &module);
    free(spirv);
    if (r != VK_SUCCESS) {
        php_error_docref(NULL, E_WARNING, "Vulkan 2D: vkCreateShaderModule failed (VkResult %d)", r);
        return VK_NULL_HANDLE;
    }
    return module;
}

/* ── Build one graphics pipeline (shapes or sprites) ─────────────── */

static VkPipeline vio_2d_vk_make_pipeline(vio_2d_vulkan_state *state, VkShaderModule fs)
{
    VkPipelineShaderStageCreateInfo stages[2] = {0};
    stages[0].sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage  = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = state->vs_module;
    stages[0].pName  = "main";
    stages[1].sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage  = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = fs;
    stages[1].pName  = "main";

    /* Vertex input: binding 0, stride 32 (sizeof vio_2d_vertex). */
    VkVertexInputBindingDescription binding = {0};
    binding.binding   = 0;
    binding.stride    = sizeof(vio_2d_vertex);
    binding.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;

    VkVertexInputAttributeDescription attrs[3] = {0};
    attrs[0].location = 0; attrs[0].binding = 0;
    attrs[0].format   = VK_FORMAT_R32G32_SFLOAT;        attrs[0].offset = offsetof(vio_2d_vertex, x);
    attrs[1].location = 1; attrs[1].binding = 0;
    attrs[1].format   = VK_FORMAT_R32G32_SFLOAT;        attrs[1].offset = offsetof(vio_2d_vertex, u);
    attrs[2].location = 2; attrs[2].binding = 0;
    attrs[2].format   = VK_FORMAT_R32G32B32A32_SFLOAT;  attrs[2].offset = offsetof(vio_2d_vertex, r);

    VkPipelineVertexInputStateCreateInfo vi = {0};
    vi.sType                           = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vi.vertexBindingDescriptionCount   = 1;
    vi.pVertexBindingDescriptions      = &binding;
    vi.vertexAttributeDescriptionCount = 3;
    vi.pVertexAttributeDescriptions    = attrs;

    VkPipelineInputAssemblyStateCreateInfo ia = {0};
    ia.sType    = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    /* Viewport + scissor are dynamic; counts must still be 1. */
    VkPipelineViewportStateCreateInfo vp = {0};
    vp.sType         = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    vp.viewportCount = 1;
    vp.scissorCount  = 1;

    VkPipelineRasterizationStateCreateInfo rs = {0};
    rs.sType       = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.cullMode    = VK_CULL_MODE_NONE;
    rs.frontFace   = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rs.lineWidth   = 1.0f;

    VkPipelineMultisampleStateCreateInfo ms = {0};
    ms.sType                = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    /* Depth test/write OFF (matches the D3D12 PSO). The swapchain render pass
     * still has a depth attachment, so we must supply a depth-stencil state. */
    VkPipelineDepthStencilStateCreateInfo ds = {0};
    ds.sType            = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    ds.depthTestEnable  = VK_FALSE;
    ds.depthWriteEnable = VK_FALSE;
    ds.depthCompareOp   = VK_COMPARE_OP_ALWAYS;

    /* Alpha blend: src.a / (1-src.a), alpha 1 / (1-src.a), ADD. */
    VkPipelineColorBlendAttachmentState blend = {0};
    blend.blendEnable         = VK_TRUE;
    blend.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
    blend.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    blend.colorBlendOp        = VK_BLEND_OP_ADD;
    blend.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    blend.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    blend.alphaBlendOp        = VK_BLEND_OP_ADD;
    blend.colorWriteMask      = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT
                              | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;

    VkPipelineColorBlendStateCreateInfo cb = {0};
    cb.sType           = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    cb.attachmentCount = 1;
    cb.pAttachments    = &blend;

    VkDynamicState dyn_states[] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
    VkPipelineDynamicStateCreateInfo dyn = {0};
    dyn.sType             = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dyn.dynamicStateCount = 2;
    dyn.pDynamicStates    = dyn_states;

    VkGraphicsPipelineCreateInfo info = {0};
    info.sType               = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    info.stageCount          = 2;
    info.pStages             = stages;
    info.pVertexInputState   = &vi;
    info.pInputAssemblyState = &ia;
    info.pViewportState      = &vp;
    info.pRasterizationState = &rs;
    info.pMultisampleState   = &ms;
    info.pDepthStencilState  = &ds;
    info.pColorBlendState    = &cb;
    info.pDynamicState       = &dyn;
    info.layout              = state->pipeline_layout;
    info.renderPass          = vio_vk.render_pass;
    info.subpass             = 0;
    info.basePipelineIndex   = -1;

    VkPipeline pipeline = VK_NULL_HANDLE;
    VkResult r = vkCreateGraphicsPipelines(vio_vk.device, VK_NULL_HANDLE, 1, &info, NULL, &pipeline);
    if (r != VK_SUCCESS) {
        php_error_docref(NULL, E_WARNING, "Vulkan 2D: vkCreateGraphicsPipelines failed (VkResult %d)", r);
        return VK_NULL_HANDLE;
    }
    return pipeline;
}

/* ── Init ────────────────────────────────────────────────────────── */

int vio_2d_vulkan_init(vio_2d_vulkan_state *state)
{
    memset(state, 0, sizeof(*state));

    /* 1. Shader modules from the explicit-binding Vulkan GLSL variants. */
    state->vs_module         = vio_2d_vk_make_module(vio_2d_vk_vs,         0);
    state->fs_shapes_module  = vio_2d_vk_make_module(vio_2d_vk_fs_shapes,  1);
    state->fs_sprites_module = vio_2d_vk_make_module(vio_2d_vk_fs_sprites, 1);
    if (!state->vs_module || !state->fs_shapes_module || !state->fs_sprites_module) {
        vio_2d_vulkan_shutdown(state);
        return -1;
    }

    /* 2. Descriptor set layout: set 0 binding 0 = combined image sampler
     *    (fragment). Used by the sprites pipeline in Phase 2; the shapes
     *    pipeline ignores it but shares the layout for compatibility. */
    VkDescriptorSetLayoutBinding sampler_binding = {0};
    sampler_binding.binding         = 0;
    sampler_binding.descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    sampler_binding.descriptorCount = 1;
    sampler_binding.stageFlags      = VK_SHADER_STAGE_FRAGMENT_BIT;

    VkDescriptorSetLayoutCreateInfo set_info = {0};
    set_info.sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    set_info.bindingCount = 1;
    set_info.pBindings    = &sampler_binding;

    if (vkCreateDescriptorSetLayout(vio_vk.device, &set_info, NULL, &state->set_layout) != VK_SUCCESS) {
        php_error_docref(NULL, E_WARNING, "Vulkan 2D: vkCreateDescriptorSetLayout failed");
        vio_2d_vulkan_shutdown(state);
        return -1;
    }

    /* 3. Pipeline layout: 1 set + a 64-byte vertex-stage push constant (mat4). */
    VkPushConstantRange pc = {0};
    pc.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
    pc.offset     = 0;
    pc.size       = sizeof(float) * 16;

    VkPipelineLayoutCreateInfo pl_info = {0};
    pl_info.sType                  = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pl_info.setLayoutCount         = 1;
    pl_info.pSetLayouts            = &state->set_layout;
    pl_info.pushConstantRangeCount = 1;
    pl_info.pPushConstantRanges    = &pc;

    if (vkCreatePipelineLayout(vio_vk.device, &pl_info, NULL, &state->pipeline_layout) != VK_SUCCESS) {
        php_error_docref(NULL, E_WARNING, "Vulkan 2D: vkCreatePipelineLayout failed");
        vio_2d_vulkan_shutdown(state);
        return -1;
    }

    /* 4. Two pipelines. */
    state->pipeline_shapes  = vio_2d_vk_make_pipeline(state, state->fs_shapes_module);
    state->pipeline_sprites = vio_2d_vk_make_pipeline(state, state->fs_sprites_module);
    if (!state->pipeline_shapes || !state->pipeline_sprites) {
        vio_2d_vulkan_shutdown(state);
        return -1;
    }

    /* 5. Streaming VBO ring (HOST_VISIBLE|HOST_COHERENT, persistently mapped).
     *    One slice per frame-in-flight; reuse is gated by the in_flight fence
     *    waited at the top of vulkan_begin_frame. */
    state->vbo_slice_size = (VkDeviceSize)sizeof(vio_2d_vertex) * VIO_2D_MAX_VERTICES;
    state->vbo_size       = state->vbo_slice_size * VIO_VK_MAX_FRAMES_IN_FLIGHT;

    if (vio_vma_create_buffer(vio_vk.vma_allocator, state->vbo_size,
                              VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                              VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                              &state->vbo, &state->vbo_allocation) != 0) {
        php_error_docref(NULL, E_WARNING, "Vulkan 2D: failed to create streaming VBO");
        vio_2d_vulkan_shutdown(state);
        return -1;
    }

    state->vbo_mapped = (unsigned char *)vio_vma_map(vio_vk.vma_allocator, state->vbo_allocation);
    if (!state->vbo_mapped) {
        php_error_docref(NULL, E_WARNING, "Vulkan 2D: failed to map streaming VBO");
        vio_2d_vulkan_shutdown(state);
        return -1;
    }

    /* 6. Per-frame descriptor pools (one combined-image-sampler set per distinct
     *    texture per frame). VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT is
     *    deliberately NOT set: we never free individual sets, we vkResetDescriptorPool
     *    the whole pool each frame, which is cheaper and the recommended pattern. */
    VkDescriptorPoolSize pool_size = {0};
    pool_size.type            = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    pool_size.descriptorCount = VIO_2D_VK_DESCRIPTORS_PER_POOL;

    VkDescriptorPoolCreateInfo pool_info = {0};
    pool_info.sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    pool_info.maxSets       = VIO_2D_VK_DESCRIPTORS_PER_POOL;
    pool_info.poolSizeCount = 1;
    pool_info.pPoolSizes    = &pool_size;

    for (int i = 0; i < VIO_VK_MAX_FRAMES_IN_FLIGHT; i++) {
        if (vkCreateDescriptorPool(vio_vk.device, &pool_info, NULL, &state->desc_pools[i]) != VK_SUCCESS) {
            php_error_docref(NULL, E_WARNING, "Vulkan 2D: failed to create descriptor pool %d", i);
            vio_2d_vulkan_shutdown(state);
            return -1;
        }
    }

    g_active_2d_vk = state;
    return 0;
}

/* ── Per-frame descriptor pool reset (called from vulkan_begin_frame) ── */

void vio_2d_vulkan_reset_frame_descriptors(uint32_t frame_index)
{
    vio_2d_vulkan_state *state = g_active_2d_vk;
    if (!state || !vio_vk.device) return;
    if (frame_index >= VIO_VK_MAX_FRAMES_IN_FLIGHT) return;
    if (state->desc_pools[frame_index] != VK_NULL_HANDLE) {
        /* Frees every set previously allocated from this pool in one shot.
         * Safe: the in_flight fence for this frame was waited just before this
         * call, so none of those sets is still referenced by the GPU. */
        vkResetDescriptorPool(vio_vk.device, state->desc_pools[frame_index], 0);
    }
}

/* ── Allocate + update a combined-image-sampler set for a texture ────── */

VkDescriptorSet vio_2d_vulkan_alloc_texture_set(vio_2d_vulkan_state *state,
                                                uint32_t frame_index,
                                                VkImageView view, VkSampler sampler)
{
    if (!state || frame_index >= VIO_VK_MAX_FRAMES_IN_FLIGHT) return VK_NULL_HANDLE;
    VkDescriptorPool pool = state->desc_pools[frame_index];
    if (pool == VK_NULL_HANDLE || view == VK_NULL_HANDLE || sampler == VK_NULL_HANDLE) {
        return VK_NULL_HANDLE;
    }

    VkDescriptorSetAllocateInfo ai = {0};
    ai.sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    ai.descriptorPool     = pool;
    ai.descriptorSetCount = 1;
    ai.pSetLayouts        = &state->set_layout;

    VkDescriptorSet set = VK_NULL_HANDLE;
    VkResult r = vkAllocateDescriptorSets(vio_vk.device, &ai, &set);
    if (r != VK_SUCCESS) {
        /* VK_ERROR_OUT_OF_POOL_MEMORY / fragmentation: warn once and let the
         * caller skip this textured draw rather than binding a stale set. */
        php_error_docref(NULL, E_WARNING,
            "Vulkan 2D: descriptor set allocation failed (VkResult %d); skipping textured draw", r);
        return VK_NULL_HANDLE;
    }

    VkDescriptorImageInfo img = {0};
    img.sampler     = sampler;
    img.imageView   = view;
    img.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    VkWriteDescriptorSet w = {0};
    w.sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    w.dstSet          = set;
    w.dstBinding      = 0;
    w.dstArrayElement = 0;
    w.descriptorCount = 1;
    w.descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    w.pImageInfo      = &img;
    vkUpdateDescriptorSets(vio_vk.device, 1, &w, 0, NULL);

    return set;
}

/* ── Shutdown ────────────────────────────────────────────────────── */

void vio_2d_vulkan_shutdown(vio_2d_vulkan_state *state)
{
    if (!state) return;

    if (g_active_2d_vk == state) g_active_2d_vk = NULL;

    /* If the device is already gone (vio_vk zeroed by a prior vulkan_shutdown),
     * there is nothing to destroy and every handle dangles — bail out. The
     * teardown paths now run this BEFORE vulkan_shutdown, so this only trips on
     * a double-shutdown, which is a no-op. */
    if (!vio_vk.device) {
        memset(state, 0, sizeof(*state));
        return;
    }

    /* The last submitted command buffer references these pipelines and the
     * VBO. Wait for the GPU to finish before destroying them — destroying an
     * in-use pipeline/buffer is a use-after-free the layers flag. (Cheap: only
     * runs once at context teardown.) */
    vkDeviceWaitIdle(vio_vk.device);

    for (int i = 0; i < VIO_VK_MAX_FRAMES_IN_FLIGHT; i++) {
        if (state->desc_pools[i]) {
            vkDestroyDescriptorPool(vio_vk.device, state->desc_pools[i], NULL);
        }
    }

    if (state->vbo) {
        if (state->vbo_mapped) {
            vio_vma_unmap(vio_vk.vma_allocator, state->vbo_allocation);
        }
        vio_vma_destroy_buffer(vio_vk.vma_allocator, state->vbo, state->vbo_allocation);
    }
    if (state->pipeline_sprites) vkDestroyPipeline(vio_vk.device, state->pipeline_sprites, NULL);
    if (state->pipeline_shapes)  vkDestroyPipeline(vio_vk.device, state->pipeline_shapes, NULL);
    if (state->pipeline_layout)  vkDestroyPipelineLayout(vio_vk.device, state->pipeline_layout, NULL);
    if (state->set_layout)       vkDestroyDescriptorSetLayout(vio_vk.device, state->set_layout, NULL);
    if (state->fs_sprites_module) vkDestroyShaderModule(vio_vk.device, state->fs_sprites_module, NULL);
    if (state->fs_shapes_module)  vkDestroyShaderModule(vio_vk.device, state->fs_shapes_module, NULL);
    if (state->vs_module)         vkDestroyShaderModule(vio_vk.device, state->vs_module, NULL);

    memset(state, 0, sizeof(*state));
}

#endif /* HAVE_VULKAN */

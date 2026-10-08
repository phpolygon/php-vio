/*
 * php-vio - PHP Video Input Output
 * Common type definitions shared between php-vio and backend extensions
 */

#ifndef VIO_TYPES_H
#define VIO_TYPES_H

/* Maximum colour attachments of one render target (MRT). */
#define VIO_MAX_COLOR_ATTACHMENTS 4

#include <stddef.h>
#include <stdint.h>

/* ── Vertex format enums ──────────────────────────────────────────── */

typedef enum _vio_format {
    VIO_FLOAT1 = 1,
    VIO_FLOAT2 = 2,
    VIO_FLOAT3 = 3,
    VIO_FLOAT4 = 4,
    VIO_INT1   = 5,
    VIO_INT2   = 6,
    VIO_INT3   = 7,
    VIO_INT4   = 8,
    VIO_UINT1  = 9,
    VIO_UINT2  = 10,
    VIO_UINT3  = 11,
    VIO_UINT4  = 12,
} vio_format;

/* ── Topology ─────────────────────────────────────────────────────── */

typedef enum _vio_topology {
    VIO_TRIANGLES      = 0,
    VIO_TRIANGLE_STRIP = 1,
    VIO_TRIANGLE_FAN   = 2,
    VIO_LINES          = 3,
    VIO_LINE_STRIP     = 4,
    VIO_POINTS         = 5,
    /* Tessellation patches: `patch_vertices` control points per primitive
     * (vio_pipeline 'patch_vertices', default 3). Implied whenever the
     * pipeline's shader carries a tessellation-control stage. */
    VIO_PATCHES        = 6,
    /* Primitives with their neighbours (geometry stage only, gl_in has 4 / 6
     * vertices): silhouettes, outlines, shadow volumes. vio_mesh(['adjacency'
     * => true]) builds the TRIANGLES_ADJACENCY index buffer from a triangle list. */
    VIO_LINES_ADJACENCY          = 7,
    VIO_LINE_STRIP_ADJACENCY     = 8,
    VIO_TRIANGLES_ADJACENCY      = 9,
    VIO_TRIANGLE_STRIP_ADJACENCY = 10,
} vio_topology;

/* ── Cull mode ────────────────────────────────────────────────────── */

typedef enum _vio_cull_mode {
    VIO_CULL_NONE  = 0,
    VIO_CULL_BACK  = 1,
    VIO_CULL_FRONT = 2,
} vio_cull_mode;

/* ── Depth function ──────────────────────────────────────────────── */

typedef enum _vio_depth_func {
    VIO_DEPTH_LESS   = 0,
    VIO_DEPTH_LEQUAL = 1,
} vio_depth_func;

/* ── Comparison function (stencil test; GAP-PHASE5 Block 1) ───────── */
typedef enum _vio_compare_func {
    VIO_CMP_NEVER    = 0,
    VIO_CMP_LESS     = 1,
    VIO_CMP_EQUAL    = 2,
    VIO_CMP_LEQUAL   = 3,
    VIO_CMP_GREATER  = 4,
    VIO_CMP_NOTEQUAL = 5,
    VIO_CMP_GEQUAL   = 6,
    VIO_CMP_ALWAYS   = 7,
} vio_compare_func;

/* ── Stencil operation (what happens to the stencil value) ────────── */
typedef enum _vio_stencil_op {
    VIO_STENCIL_KEEP      = 0,
    VIO_STENCIL_ZERO      = 1,
    VIO_STENCIL_REPLACE   = 2,   /* write the reference value */
    VIO_STENCIL_INCR      = 3,   /* increment, clamp at 255 */
    VIO_STENCIL_DECR      = 4,   /* decrement, clamp at 0 */
    VIO_STENCIL_INVERT    = 5,
    VIO_STENCIL_INCR_WRAP = 6,
    VIO_STENCIL_DECR_WRAP = 7,
} vio_stencil_op;

/* ── Blend mode ───────────────────────────────────────────────────── */

typedef enum _vio_blend_mode {
    VIO_BLEND_NONE          = 0,
    VIO_BLEND_ALPHA         = 1,   /* src.a, 1-src.a */
    VIO_BLEND_ADDITIVE      = 2,   /* src.a, 1 */
    VIO_BLEND_PREMULTIPLIED = 3,   /* 1, 1-src.a (premultiplied alpha) */
    VIO_BLEND_MULTIPLY      = 4,   /* dst.rgb * src.rgb */
    VIO_BLEND_SCREEN        = 5,   /* 1, 1-src.rgb */
    VIO_BLEND_MIN           = 6,   /* min(src, dst) */
    VIO_BLEND_MAX           = 7,   /* max(src, dst) */
} vio_blend_mode;

/* ── Colour write mask (pipeline 'color_mask', bit flags) ─────────── */

#define VIO_COLOR_R    1
#define VIO_COLOR_G    2
#define VIO_COLOR_B    4
#define VIO_COLOR_A    8
#define VIO_COLOR_RGB  7
#define VIO_COLOR_RGBA 15

/* ── Shader format ────────────────────────────────────────────────── */

typedef enum _vio_shader_format {
    VIO_SHADER_AUTO      = 0,
    VIO_SHADER_SPIRV     = 1,
    VIO_SHADER_GLSL      = 2,  /* GLSL -> SPIR-V -> cross-compile */
    VIO_SHADER_MSL       = 3,
    VIO_SHADER_GLSL_RAW  = 4,  /* GLSL -> compile directly (OpenGL only, no SPIR-V) */
    VIO_SHADER_HLSL      = 5,  /* HLSL -> compile directly (D3D11/D3D12 only) */
} vio_shader_format;

/* ── Texture filter / wrap ────────────────────────────────────────── */

typedef enum _vio_filter {
    VIO_FILTER_NEAREST = 0,
    VIO_FILTER_LINEAR  = 1,
} vio_filter;

typedef enum _vio_wrap {
    VIO_WRAP_REPEAT = 0,
    VIO_WRAP_CLAMP  = 1,
    VIO_WRAP_MIRROR = 2,
} vio_wrap;

/* ── Vertex usage hints ───────────────────────────────────────────── */

typedef enum _vio_usage {
    VIO_POSITION = 0,
    VIO_COLOR    = 1,
    VIO_TEXCOORD = 2,
    VIO_NORMAL   = 3,
    VIO_TANGENT  = 4,
} vio_usage;

/* ── Buffer type ──────────────────────────────────────────────────── */

typedef enum _vio_buffer_type {
    VIO_BUFFER_VERTEX  = 0,
    VIO_BUFFER_INDEX   = 1,
    VIO_BUFFER_UNIFORM = 2,
    VIO_BUFFER_STORAGE = 3,
} vio_buffer_type;

/* ── Uniform types (for set_uniform vtable) ──────────────────────── */

typedef enum _vio_uniform_type {
    VIO_UNIFORM_INT     = 0,
    VIO_UNIFORM_FLOAT   = 1,
    VIO_UNIFORM_VEC2    = 2,
    VIO_UNIFORM_VEC3    = 3,
    VIO_UNIFORM_VEC4    = 4,
    VIO_UNIFORM_MAT3    = 5,
    VIO_UNIFORM_MAT4    = 6,
} vio_uniform_type;

/* ── Feature flags ────────────────────────────────────────────────── */

typedef enum _vio_feature {
    VIO_FEATURE_COMPUTE      = 0,
    VIO_FEATURE_RAYTRACING   = 1,   /* vio_rt_pipeline / vio_trace_rays (D3D12 DXR 1.0, Vulkan VK_KHR_ray_tracing_pipeline) */
    VIO_FEATURE_TESSELLATION = 2,
    VIO_FEATURE_GEOMETRY     = 3,
    /* vio_shader(['view_count' => 2..4]) + a layered target bound with
     * VIO_RT_ALL_LAYERS: one draw renders every view, view v (gl_ViewIndex,
     * GL_EXT_multiview) into layer v. Stereo / several shadow cascades per draw. */
    VIO_FEATURE_MULTIVIEW    = 4,
    /* Backend has a wired 3D draw pipeline (create_pipeline / create_buffer /
     * create_texture / draw / draw_indexed all functional). Returns 0 when
     * the backend's 3D path is stubbed out, so callers can pick a different
     * backend instead of silently rendering black. 2D-only paths (vio_rect,
     * vio_sprite, vio_text) are independent and may work even when this
     * flag is 0 (e.g. Metal currently has 2D wired but no 3D). */
    VIO_FEATURE_3D_PIPELINE  = 5,
    /* Frame readback to RGBA on the CPU side. Used by vio_read_pixels,
     * vio_save_screenshot, vio_recorder_capture, vio_stream_push. Returns
     * 0 on backends where the readback path is stubbed (Vulkan, Metal). */
    VIO_FEATURE_READ_PIXELS  = 6,
    /* Hardware instancing — vio_draw_instanced. */
    VIO_FEATURE_INSTANCED_DRAW = 7,
    /* Offscreen render-target API (vio_render_target / vio_bind_render_target). */
    VIO_FEATURE_RENDER_TARGET      = 8,
    VIO_FEATURE_RENDER_TARGET_HDR  = 9,   /* 16F color attachment */
    VIO_FEATURE_RENDER_TARGET_DEPTH = 10, /* depth-only RT (shadow maps) */
    VIO_FEATURE_RENDER_TARGET_MSAA = 11,  /* multisampled RT */
    /* Cubemap textures — vio_cubemap / vio_bind_cubemap. */
    VIO_FEATURE_CUBEMAP            = 12,
    /* Pipeline depth-bias state. GL's glPolygonOffset; D3D/Vulkan/Metal
     * expose it via pipeline state. */
    VIO_FEATURE_DEPTH_BIAS         = 13,
    /* Scissor rectangle for 2D push/pop. */
    VIO_FEATURE_SCISSOR            = 14,
    /* GPU-side texture component swizzle (R8 → alpha for font atlases).
     * Backends without it (D3D11/12) have to CPU-expand to RGBA8. */
    VIO_FEATURE_TEXTURE_SWIZZLE    = 15,
    /* Backend ships its own 2D-batch renderer (vio_2d_<backend>_*). The
     * generic geo path in vio_2d.c is only invoked when this is 0. */
    VIO_FEATURE_NATIVE_2D_BATCH    = 16,
    /* OpenGL-specific feature flags filling out vio_gl_info(). All n/a for
     * non-GL backends. */
    VIO_FEATURE_DEBUG_OUTPUT       = 17,  /* GL 4.3 / KHR_debug */
    VIO_FEATURE_DSA                = 18,  /* GL 4.5 / ARB_direct_state_access */
    VIO_FEATURE_BUFFER_STORAGE     = 19,  /* GL 4.4 / ARB_buffer_storage */
    VIO_FEATURE_TEXTURE_STORAGE    = 20,  /* GL 4.2 / ARB_texture_storage */
    VIO_FEATURE_SEPARATE_SHADERS   = 21,  /* GL 4.1 / ARB_separate_shader_objects */
    /* 3D / volume textures (vio_texture_3d / sampler3D). Used by Fieldtracing
     * to store a baked Signed Distance Field volume on the GPU. Wired and
     * reported (1) on all backends: GL (glTexImage3D, core since 1.2), D3D11/
     * D3D12 (Texture3D), Metal (MTLTextureType3D), Vulkan (VK_IMAGE_TYPE_3D).
     * A backend that ever lacks the upload path reports 0 and vio_texture_3d
     * returns false there (graceful — the engine stays on the analytic trace
     * path). */
    VIO_FEATURE_TEXTURE_3D         = 22,
    /* Cubemap render targets: vio_render_target(['cube' => true]) + per-face
     * bind (vio_bind_render_target($ctx, $rt, $face)) + vio_render_target_cubemap.
     * The environment-probe path (render the sky into 6 faces, sample with
     * textureLod by roughness). 0 on backends without the face-attach path. */
    VIO_FEATURE_RENDER_TARGET_CUBE = 23,
    /* vio_generate_mipmaps() on textures / cubemaps / render targets. */
    VIO_FEATURE_MIPMAP_GEN         = 24,
    /* Multiple render targets: vio_render_target(['attachments' => [...]]) with
     * up to VIO_MAX_COLOR_ATTACHMENTS colour attachments, fragment
     * layout(location = n) out. */
    VIO_FEATURE_MRT                = 25,
    /* Compute storage images: vio_texture(['storage' => true]) +
     * vio_compute_bind_image() (GLSL image2D / image3D). */
    VIO_FEATURE_STORAGE_IMAGE      = 26,
    /* A storage buffer (SSBO / StructuredBuffer SRV) can be bound to the
     * GRAPHICS pipeline and read from the VERTEX stage — the primitive that
     * lets a vertex shader pull per-instance data via gl_InstanceIndex from a
     * compute-written buffer, with no GPU->CPU readback. Requires the vertex
     * stage to support storage-buffer reads: core everywhere on D3D11 (SM5
     * SRV-in-VS), D3D12, Vulkan and Metal; on OpenGL only from 4.3 (SSBOs are
     * core 4.3), so GL < 4.3 reports 0 and callers stay on the readback path.
     * Value 30 (leaves 23-29 free for unrelated features). */
    VIO_FEATURE_SAMPLER_FEEDBACK_GLSL = 28,  /* vio_write_feedback() from VIO_SAMPLER_FEEDBACK_GLSL feeds vio_sampler_feedback_* (A15) */
    VIO_FEATURE_FRAGMENT_STORAGE = 27,  /* writable std430 buffers (and atomics) in the fragment stage: vio_bind_fragment_storage_buffer (A15) */
    VIO_FEATURE_VERTEX_STORAGE     = 30,
    /* Stencil test / write through vio_pipeline(['stencil' => [...]]) — the
     * depth attachment carries 8 stencil bits and the pipeline state exposes
     * compare function, reference, masks and the three operations. */
    VIO_FEATURE_STENCIL            = 31,
    /* vio_gpu_frame_time(): GPU timestamps around the frame's command stream
     * (D3D11/D3D12 timestamp queries, GL_TIMESTAMP, vkCmdWriteTimestamp, Metal
     * GPUStart/EndTime) read back one to two frames later. */
    VIO_FEATURE_GPU_TIMESTAMP      = 32,
    /* vio_create(['frame_latency' => n]): waitable swapchain that caps how many
     * frames the CPU runs ahead (input latency control). */
    VIO_FEATURE_FRAME_LATENCY      = 33,
    /* vio_create(['hdr_output' => 1]): HDR10 (RGB10A2 + ST 2084) backbuffer with
     * the 2D batch PQ-encoding its output; vio_swapchain_info()['hdr_output']
     * says whether it is active. */
    VIO_FEATURE_HDR_OUTPUT         = 34,
    /* vio_draw_indirect(): draw arguments (index/vertex count, instance count,
     * offsets) read from a storage buffer a compute pass wrote — GPU culling /
     * LOD selection without a CPU round trip. */
    VIO_FEATURE_INDIRECT_DRAW      = 35,
    /* vio_texture(['layers' => N]): 2D texture arrays (sampler2DArray). */
    VIO_FEATURE_TEXTURE_ARRAY      = 36,
    /* vio_texture(['format' => VIO_FORMAT_BC*]) / vio_texture_ktx2(): block-
     * compressed texture data uploaded as-is (BC1 / BC3 / BC4 / BC5; BC7 on
     * OpenGL additionally needs BPTC, GL 4.2). */
    VIO_FEATURE_TEXTURE_COMPRESSION_BC = 37,
    /* vio_set_shading_rate(): variable rate shading - the fragment shader runs
     * once per 1x2 / 2x1 / 2x2 / 4x4 pixel block while geometry, depth and the
     * resolution stay untouched (D3D12 VRS Tier 1+). */
    VIO_FEATURE_SHADING_RATE       = 38,
    /* Layered render targets: vio_render_target(['layers' => N]) 2D arrays
     * (colour or depth_only, sampled as sampler2DArray) and depth_only cube
     * targets (sampled through vio_render_target_cubemap). One layer / face is
     * bound at a time via vio_bind_render_target($ctx, $rt, $layer). */
    VIO_FEATURE_RENDER_TARGET_LAYERED = 39,
    /* Layered rendering: vio_bind_render_target($ctx, $rt, VIO_RT_ALL_LAYERS)
     * binds every layer / face of a layered target at once; a geometry stage
     * picks the layer per primitive with gl_Layer (single-pass cube / CSM). */
    VIO_FEATURE_LAYERED_RENDER     = 40,
    /* gl_Layer written by the VERTEX stage (no geometry stage needed:
     * gl_Layer = gl_InstanceIndex with one instance per layer). */
    VIO_FEATURE_VERTEX_LAYER       = 41,
    /* vio_viewports(): up to VIO_MAX_VIEWPORTS viewports at once; gl_ViewportIndex
     * in the geometry stage (or the vertex stage with VIO_FEATURE_VERTEX_LAYER)
     * picks one per primitive - all CSM cascades into one atlas in one pass. */
    VIO_FEATURE_MULTI_VIEWPORT     = 42,
    /* Geometry-shader instancing: layout(invocations = N) runs the GS N times
     * per input primitive (gl_InvocationID), e.g. one invocation per cube face. */
    VIO_FEATURE_GEOMETRY_INSTANCING = 43,
    /* vio_shader(['hlsl' => ['geometry' | 'tess_control' | 'tess_eval' => src]]):
     * the backend compiles that HLSL instead of transpiling the GLSL stage -
     * D3D tessellation (SPIRV-Cross has no hull / domain output) and HLSL-only
     * features such as [instance(N)]. The GLSL stage stays required: it defines
     * the uniform layout and serves the other backends. */
    VIO_FEATURE_HLSL_STAGE_OVERRIDE = 44,
    /* Subgroup operations (GL_KHR_shader_subgroup_basic / _vote / _ballot /
     * _arithmetic): gl_SubgroupSize, subgroupAdd, subgroupBroadcastFirst, ...
     * D3D12 maps them onto wave intrinsics, which need Shader Model 6
     * (vio_create(['shader_model' => 6]) with DXC) and a device with WaveOps. */
    VIO_FEATURE_SUBGROUP           = 45,
    /* Quad operations in the fragment stage (GL_KHR_shader_subgroup_quad):
     * subgroupQuadSwapHorizontal / Vertical / Diagonal, subgroupQuadBroadcast
     * across the 2x2 pixel quad. D3D12: SM 6 + WaveOps; Metal: quad_group. */
    VIO_FEATURE_SUBGROUP_QUAD      = 46,
    /* gl_BaryCoordEXT / gl_BaryCoordNoPerspEXT in the fragment stage
     * (GL_EXT_fragment_shader_barycentric): the interpolation weights of the
     * triangle's vertices, in vertex order. D3D12: SM 6.1 + BarycentricsSupported. */
    VIO_FEATURE_BARYCENTRICS       = 47,
    /* 64-bit integer atomics on storage-buffer elements in compute shaders
     * (GL_EXT_shader_atomic_int64: atomicAdd / Min / Max / And / Or / Xor /
     * Exchange / CompSwap on uint64_t / int64_t) - e.g. a visibility buffer that
     * packs depth and ID into one atomicMax. D3D12: SM 6.6 + Int64ShaderOps.
     * Metal: 0 (only min / max exist, and SPIRV-Cross refuses 64-bit atomics). */
    VIO_FEATURE_ATOMIC64           = 48,
    /* float16_t / f16vecN arithmetic in shaders (GL_EXT_shader_explicit_arithmetic_types_float16,
     * no 16-bit storage) that really runs at half precision. D3D12: SM 6.2 +
     * Native16BitShaderOpsSupported (`half`, DXC -enable-16bit-types) - without it
     * SPIRV-Cross emits min16float, a precision hint only. */
    VIO_FEATURE_SHADER_FLOAT16     = 49,
    /* gl_BaseVertex / gl_BaseInstance (GLSL 460 / GL_ARB_shader_draw_parameters):
     * an indirect draw's baseVertex / firstInstance, 0 for vio_draw. D3D12: SM 6.8
     * (SV_StartVertexLocation / SV_StartInstanceLocation). */
    VIO_FEATURE_BASE_VERTEX        = 50,
    /* dFdx / dFdy / fwidth in compute shaders with layout(derivative_group_quadsNV)
     * (GL_NV_compute_shader_derivatives): 2x2 quads of local x / y. D3D12: SM 6.6;
     * Metal: 0 (kernel functions have no derivatives). */
    VIO_FEATURE_COMPUTE_DERIVATIVES = 51,
    /* gl_PrimitiveShadingRateEXT written by the vertex stage
     * (GL_EXT_fragment_shading_rate): the primitive's rate replaces the one
     * vio_set_shading_rate set, for pipelines whose vertex stage writes it.
     * D3D12: VRS Tier 2 + SM 6.4 (SV_ShadingRate); Vulkan primitiveFragmentShadingRate. */
    VIO_FEATURE_SHADING_RATE_PRIMITIVE = 52,
    /* vio_set_shading_rate_image(): one VIO_SHADING_RATE_* byte per screen tile
     * (vio_shading_rate_tile_size() pixels); the final rate is the coarser of the
     * set / primitive rate and the tile's rate. D3D12: VRS Tier 2. */
    VIO_FEATURE_SHADING_RATE_IMAGE = 53,
    /* vio_texture_index(): a per-context texture table the shaders index
     * non-uniformly - `layout(set = 1, binding = 0) uniform texture2D
     * vio_textures[]` + `layout(set = 1, binding = 1) uniform sampler vio_sampler`
     * (BINDLESS-PLAN.md). D3D12 ResourceBindingTier 2, Vulkan descriptor
     * indexing, Metal3 argument buffers tier 2; OpenGL / D3D11 0. */
    VIO_FEATURE_BINDLESS           = 54,
    /* Inline ray tracing: vio_acceleration_structure() (a BLAS per mesh + a TLAS
     * over the instances), vio_bind_acceleration_structure(), GL_EXT_ray_query
     * (rayQueryEXT) in the fragment and compute stages. D3D12: DXR Tier 1.1 +
     * SM 6.5 (RayQuery); Vulkan: VK_KHR_acceleration_structure + VK_KHR_ray_query;
     * Metal: ray queries from render pipelines (MSL 2.4). The ray-tracing
     * PIPELINE (raygen / hit shaders) is VIO_FEATURE_RAYTRACING. */
    VIO_FEATURE_RAY_QUERY          = 56,
    /* Mesh and task (amplification / object) stages: vio_shader(['task' => ?,
     * 'mesh' => ..., 'fragment' => ...]) (GL_EXT_mesh_shader) drawn with
     * vio_draw_mesh_tasks / vio_draw_mesh_tasks_indirect - geometry generated on
     * the GPU per workgroup (meshlet culling, LOD). D3D12: SM 6.5 + MeshShaderTier;
     * Vulkan: VK_EXT_mesh_shader; Metal: mesh pipelines (Metal 3, Apple7 / Mac2). */
    VIO_FEATURE_MESH_SHADER        = 55,
    /* Sampler feedback (texture streaming): vio_sampler_feedback_bind() pairs a
     * MinMip feedback map with a texture, a fragment stage writes it with
     * WriteSamplerFeedback (HLSL override, register u0 space2), and
     * vio_sampler_feedback_read() decodes the lowest mip sampled per region.
     * D3D12 only: SM 6.5 + SamplerFeedbackTier 0.9 (and the bindless root
     * layout); Vulkan / Metal / OpenGL have no equivalent. */
    VIO_FEATURE_SAMPLER_FEEDBACK   = 57,
    /* Cooperative matrices (GL_KHR_cooperative_matrix): coopMatLoad /
     * coopMatMulAdd / coopMatStore on subgroup-scope tiles in compute kernels,
     * run on the hardware matrix units. vio_cooperative_matrix_shapes() lists
     * the M x N x K shapes and component types. Vulkan: VK_KHR_cooperative_matrix;
     * Metal: simdgroup_matrix (8x8 only, MSL 2.3, Apple7+); D3D12: 0 (SPIRV-Cross
     * has no HLSL mapping, SM 6.9 wave matrices are out of reach). */
    VIO_FEATURE_COOPERATIVE_MATRIX = 59,
    /* Work graphs (SM 6.8 node shaders): vio_work_graph() builds an HLSL lib_6_8
     * graph, vio_dispatch_graph() feeds CPU records to its entry node and the
     * graph schedules its own follow-up work on the GPU. HLSL only (GLSL has no
     * node shaders). D3D12: OPTIONS21.WorkGraphsTier >= 1_0 + SM 6.8 (usually
     * through the Agility SDK, vio_create(['agility_sdk' => dir])); 0 elsewhere. */
    VIO_FEATURE_WORK_GRAPHS        = 58,
    /* vio_shader 'view_count' together with a geometry stage / a tessellation pair
     * (gl_ViewIndex in those stages, OPEN-ITEMS-PLAN A27). Vulkan:
     * multiviewGeometryShader / multiviewTessellationShader; D3D12: view
     * instancing (SV_ViewID in every stage). 0 on GL (GL_OVR_multiview rules the
     * stages out), D3D11 and Metal (their multiview is instancing in the vertex stage). */
    VIO_FEATURE_MULTIVIEW_GEOMETRY     = 60,
    VIO_FEATURE_MULTIVIEW_TESSELLATION = 61,
    /* VIO_FORMAT_ASTC_* textures (A20): Metal on Apple GPUs, Vulkan with
     * textureCompressionASTC_LDR, GL with the KHR extension; D3D has no ASTC. */
    VIO_FEATURE_TEXTURE_COMPRESSION_ASTC = 62,
    /* depth_only render targets with a mip chain built by vio_generate_mipmaps as
     * a max / min reduction of the 2x2 texels below (Hi-Z, OPEN-ITEMS-PLAN A26). */
    VIO_FEATURE_DEPTH_MIPMAPS = 63,
    /* vio_render_target(['rate_map' => ['x' => [...], 'y' => [...]]]) renders into
     * a physically smaller target with fewer samples where the quality is lower
     * (Metal rasterization rate maps, OPEN-ITEMS A16); the logical texture is
     * resolved when the pass leaves the target. 0 = the option renders at full
     * rate. Past the 64-bit adapter masks: vio_adapters() never lists it. */
    VIO_FEATURE_RASTER_RATE_MAP = 64,
    /* Shader Model 6.9 (SM69-PLAN): vectors of 5..1024 components (GLSL
     * GL_EXT_long_vector on Vulkan; HLSL vector<T, N> through the compute 'hlsl'
     * override on D3D12 - SPIRV-Cross has no HLSL form for them), Shader
     * Execution Reordering (DXR 1.2 / VK_EXT_ray_tracing_invocation_reorder) and
     * Opacity Micromaps (DXR 1.2 / VK_EXT_opacity_micromap). */
    VIO_FEATURE_LONG_VECTOR = 65,
    VIO_FEATURE_SHADER_EXECUTION_REORDER = 66,
    VIO_FEATURE_OPACITY_MICROMAP = 67,
} vio_feature;

/* Zones per axis of a rate map. */
#define VIO_RATE_MAP_MAX 16

/* Component types of a cooperative-matrix shape. */
typedef enum _vio_coopmat_type {
    VIO_COOPMAT_FLOAT16 = 0,
    VIO_COOPMAT_FLOAT32,
    VIO_COOPMAT_FLOAT64,
    VIO_COOPMAT_SINT8,
    VIO_COOPMAT_SINT16,
    VIO_COOPMAT_SINT32,
    VIO_COOPMAT_SINT64,
    VIO_COOPMAT_UINT8,
    VIO_COOPMAT_UINT16,
    VIO_COOPMAT_UINT32,
    VIO_COOPMAT_UINT64,
    VIO_COOPMAT_BFLOAT16,
} vio_coopmat_type;

/* One shape vio_cooperative_matrix_shapes() reports: A is M x K, B is K x N,
 * C and the result are M x N. */
typedef struct _vio_coopmat_shape {
    int m, n, k;
    vio_coopmat_type a, b, c, result;
} vio_coopmat_shape;

#define VIO_COOPMAT_MAX_SHAPES 32

/* Slots of the vio_texture_index() table (Set 1 of the bindless contract). */
#define VIO_BINDLESS_MAX 1024
/* A released bindless slot (vio_texture_release_index) is reused after this many
 * vio_begin calls: more than any backend keeps frames in flight (D3D12 <= 3,
 * Vulkan 2, Metal 3), so no recorded frame can still read the old entry. */
#define VIO_BINDLESS_RETIRE_FRAMES 4
/* Kind of a bindless slot: which Set 1 array the shader reads it through
 * (vio_textures[] binding 0, vio_texture_arrays[] binding 6, vio_cubes[] binding 5). */
#define VIO_BINDLESS_KIND_2D    0
#define VIO_BINDLESS_KIND_ARRAY 1
#define VIO_BINDLESS_KIND_CUBE  2

#define VIO_MAX_VIEWPORTS 16

/* vio_bind_render_target() face / layer argument that binds every layer of a
 * cube or array target at once (VIO_FEATURE_LAYERED_RENDER). */
#define VIO_RT_ALL_LAYERS (-2)

/* Native device APIs a video encoder can share (VIDEO-ENCODE-PLAN.md) */
#define VIO_ENCODE_API_D3D11 1

/* vio_upscale modes (UPSCALE-PLAN.md) */
#define VIO_UPSCALE_SPATIAL  0
#define VIO_UPSCALE_TEMPORAL 1

/* vio_set_shading_rate() rates (GAP-PHASE5 Block 12). 4X4 needs the device's
 * additional-rates capability; the call returns false otherwise. */
typedef enum _vio_shading_rate {
    VIO_SHADING_RATE_1X1 = 0,   /* full rate (default) */
    VIO_SHADING_RATE_1X2 = 1,
    VIO_SHADING_RATE_2X1 = 2,
    VIO_SHADING_RATE_2X2 = 3,   /* a quarter of the fragment invocations */
    VIO_SHADING_RATE_4X4 = 4,   /* a sixteenth */
} vio_shading_rate;

/* ── Input actions ────────────────────────────────────────────────── */

typedef enum _vio_action {
    VIO_RELEASE = 0,
    VIO_PRESS   = 1,
    VIO_REPEAT  = 2,
} vio_action;

/* ── Mouse buttons ────────────────────────────────────────────────── */

typedef enum _vio_mouse_button {
    VIO_MOUSE_LEFT   = 0,
    VIO_MOUSE_RIGHT  = 1,
    VIO_MOUSE_MIDDLE = 2,
} vio_mouse_button;

/* ── Configuration ────────────────────────────────────────────────── */

typedef struct _vio_config {
    int         width;
    int         height;
    const char *title;
    int         vsync;
    int         samples;    /* MSAA, 0 = off */
    int         debug;      /* Validation Layers / Debug Output */
    int         headless;   /* Offscreen rendering, no visible window */
    int         headless_hardware; /* D3D11 / D3D12: headless on the GPU instead of WARP */
    /* Backbuffers == how many frames the CPU may run ahead of the GPU.
     * 0 = backend default. Currently only D3D12 honours it (2 or 3); other
     * backends ignore it. See VIO_D3D12_FRAME_COUNT_DEFAULT. */
    int         frame_count;
    /* Waitable swapchain (GAP-PHASE5 Block 5): maximum frames the CPU may queue
     * ahead of presentation; vio_begin blocks on the swapchain's waitable object
     * until a backbuffer is free. 1 = lowest input latency, 0 = driver default
     * (no waitable object). D3D11 / D3D12 (waitable object) and Metal (frames
     * in flight, 1..3). */
    int         frame_latency;
    /* HDR10 output (GAP-PHASE5 Block 6, D3D11 / D3D12 / Vulkan / Metal): 1 = 10-bit ST 2084
     * backbuffer when the window's display is in HDR mode, 2 = force it even on
     * an SDR display (tests), 0 = 8-bit sRGB. hdr_paper_white is the luminance
     * (nits) that display-referred white (1.0) maps to; 0 => 200. */
    int         hdr_output;
    float       hdr_paper_white;
    /* D3D12 shader model (GAP-PHASE5 Block 7): 6 compiles the SPIRV-Cross HLSL
     * with DXC to DXIL (dxcompiler.dll + dxil.dll must be loadable, the device
     * must report SM 6.0); 0 / 5 / 50..59 = FXC 5.1 as before. The profile is the
     * highest 6.x both the device and the loaded DXC / dxil.dll accept; 60..69
     * (major * 10 + minor) pins a lower one. 0 falls back to the environment
     * variable VIO_D3D12_SHADER_MODEL (same encoding). dxc_dir
     * optionally names the directory holding the two DLLs. */
    int         shader_model;
    char        dxc_dir[512];
    /* Metal: pin the version ladder to this Metal Shading Language version
     * (major * 10 + minor, e.g. 21 = MSL 2.1); 0 = the highest the OS accepts
     * (or VIO_METAL_MSL_VERSION from the environment). */
    int         msl_version;
    /* D3D12 Agility SDK: directory holding D3D12Core.dll (absolute, or relative
     * to the php executable's directory); the device then comes from
     * ID3D12SDKConfiguration1::CreateDeviceFactory instead of the OS runtime.
     * agility_sdk_version is the SDK version D3D12Core.dll exports
     * (D3D12SDKVersion); 0 = read it from the DLL. Empty = OS runtime. */
    char        agility_sdk[512];
    int         agility_sdk_version;
} vio_config;

/* vio_swapchain_info() — what the presentation path actually runs with. */
typedef struct _vio_swapchain_info {
    int buffer_count;    /* backbuffers (0 = unknown / no swapchain) */
    int frame_latency;   /* effective maximum frame latency (0 = driver default) */
    int waitable;        /* 1 = frame-latency waitable object in use */
    int hdr_output;      /* 1 = HDR10 (10-bit, ST 2084) output active (GAP-PHASE5 Block 6) */
    int format;          /* vio_pixel_format-like code of the backbuffer: 0 RGBA8, 8 RGB10A2 */
    int shader_model;    /* D3D: 6 = DXC / DXIL, 5 = FXC; 0 elsewhere (GAP-PHASE5 Block 7) */
    int shader_model_version; /* compile profile as major * 10 + minor: D3D12 60..69 (DXC) or 51,
                               * D3D11 50; 0 elsewhere */
    int agility_sdk;     /* D3D12: the Agility SDK version the device runs on, 0 = OS runtime */
} vio_swapchain_info;

/* vio_backend_info() — the API level a backend negotiated and what it can do
 * there (the OpenGL context ladder / Metal language ladder). Strings are
 * backend-owned and stay valid while the backend has its device. */
#define VIO_BACKEND_INFO_MAX_FAMILIES 32
#define VIO_BACKEND_INFO_MAX_CAPS     48
typedef struct _vio_backend_description {
    const char *api;                      /* e.g. "Metal 4" */
    const char *device;                   /* GPU / adapter name */
    const char *shading_language;         /* e.g. "MSL" */
    int         shading_language_version; /* in use, major * 10 + minor */
    int         shading_language_max;     /* highest the platform accepts */
    int         family_count;
    const char *families[VIO_BACKEND_INFO_MAX_FAMILIES];
    int         cap_count;
    const char *cap_names[VIO_BACKEND_INFO_MAX_CAPS];
    int         cap_values[VIO_BACKEND_INFO_MAX_CAPS];
    /* Adapter identity (OPEN-ITEMS-PLAN A4): PCI vendor id (0x10005 = Mesa's
     * software vendor, 0 = unknown), driver version, device type ("discrete",
     * "integrated", "software"; NULL = unknown) and dedicated video memory. */
    uint32_t    vendor_id;
    const char *driver;
    const char *device_type;
    uint64_t    vram_bytes;
} vio_backend_description;

/* vio_adapters() (OPEN-ITEMS-PLAN A6): one adapter as the backend sees it
 * without a context. features: bit VIO_FEATURE_* for each device capability
 * the backend can tell without opening its own device (hardware support; the
 * flag of a context may still depend on the shader toolchain). */
#define VIO_MAX_ADAPTERS 16
/* A set of VIO_FEATURE_* flags (adapter descriptions, 'require' / 'prefer').
 * Two words: the flags passed 63 with VIO_FEATURE_RASTER_RATE_MAP, and a plain
 * uint64_t mask could neither carry them nor let 'require' name them. */
#define VIO_FEATURE_SET_MAX 128
typedef struct _vio_feature_set { uint64_t w[VIO_FEATURE_SET_MAX / 64]; } vio_feature_set;
static inline void vio_featset_add(vio_feature_set *s, int f)
{
    if (f >= 0 && f < VIO_FEATURE_SET_MAX) s->w[f >> 6] |= 1ull << (f & 63);
}
static inline int vio_featset_has(const vio_feature_set *s, int f)
{
    return f >= 0 && f < VIO_FEATURE_SET_MAX && ((s->w[f >> 6] >> (f & 63)) & 1);
}
typedef struct _vio_adapter_info {
    char        name[256];
    uint32_t    vendor_id;
    uint32_t    device_id;
    char        driver[64];
    const char *device_type;   /* "discrete" / "integrated" / "software", NULL = unknown */
    uint64_t    vram_bytes;
    vio_feature_set features;
} vio_adapter_info;

/* Depth mip reduction (A26). */
#define VIO_DEPTH_REDUCE_MAX 0
#define VIO_DEPTH_REDUCE_MIN 1

/* Vendor name for a PCI vendor id (vio_backend_info / vio_adapters). */
static inline const char *vio_vendor_name(uint32_t id)
{
    switch (id) {
    case 0x10DE:  return "NVIDIA";
    case 0x1002:  return "AMD";
    case 0x1022:  return "AMD";
    case 0x8086:  return "Intel";
    case 0x106B:  return "Apple";
    case 0x1414:  return "Microsoft";
    case 0x13B5:  return "ARM";
    case 0x5143:  return "Qualcomm";
    case 0x10005: return "Mesa";
    default:      return "unknown";
    }
}

/* The capability set of backends without their own (D3D, Vulkan, GL): the
 * optional VIO_FEATURE_* flags the backend reports. */
static inline void vio_describe_feature_caps(vio_backend_description *out, int (*supports)(vio_feature))
{
#define VIO_DCAP(n, f) do { if (out->cap_count < VIO_BACKEND_INFO_MAX_CAPS) { \
        out->cap_names[out->cap_count] = n; out->cap_values[out->cap_count++] = supports(f) ? 1 : 0; } } while (0)
    VIO_DCAP("compute", VIO_FEATURE_COMPUTE);
    VIO_DCAP("geometry", VIO_FEATURE_GEOMETRY);
    VIO_DCAP("tessellation", VIO_FEATURE_TESSELLATION);
    VIO_DCAP("multiview", VIO_FEATURE_MULTIVIEW);
    VIO_DCAP("multi_viewport", VIO_FEATURE_MULTI_VIEWPORT);
    VIO_DCAP("layered_vertex", VIO_FEATURE_VERTEX_LAYER);
    VIO_DCAP("indirect_draw", VIO_FEATURE_INDIRECT_DRAW);
    VIO_DCAP("bc_texture_compression", VIO_FEATURE_TEXTURE_COMPRESSION_BC);
    VIO_DCAP("shading_rate", VIO_FEATURE_SHADING_RATE);
    VIO_DCAP("shading_rate_image", VIO_FEATURE_SHADING_RATE_IMAGE);
    VIO_DCAP("hdr_output", VIO_FEATURE_HDR_OUTPUT);
    VIO_DCAP("subgroup", VIO_FEATURE_SUBGROUP);
    VIO_DCAP("quad_group", VIO_FEATURE_SUBGROUP_QUAD);
    VIO_DCAP("barycentrics", VIO_FEATURE_BARYCENTRICS);
    VIO_DCAP("atomic64", VIO_FEATURE_ATOMIC64);
    VIO_DCAP("float16", VIO_FEATURE_SHADER_FLOAT16);
    VIO_DCAP("draw_parameters", VIO_FEATURE_BASE_VERTEX);
    VIO_DCAP("compute_derivatives", VIO_FEATURE_COMPUTE_DERIVATIVES);
    VIO_DCAP("bindless", VIO_FEATURE_BINDLESS);
    VIO_DCAP("mesh_shaders", VIO_FEATURE_MESH_SHADER);
    VIO_DCAP("raytracing", VIO_FEATURE_RAYTRACING);
    VIO_DCAP("ray_query", VIO_FEATURE_RAY_QUERY);
    VIO_DCAP("sampler_feedback", VIO_FEATURE_SAMPLER_FEEDBACK);
    VIO_DCAP("cooperative_matrix", VIO_FEATURE_COOPERATIVE_MATRIX);
    VIO_DCAP("work_graphs", VIO_FEATURE_WORK_GRAPHS);
    VIO_DCAP("gpu_timestamp", VIO_FEATURE_GPU_TIMESTAMP);
#undef VIO_DCAP
}

/* vio_acceleration_structure(): triangle geometry (positions xyz, uint32
 * indices; CPU copies the mesh keeps) and the instances placing it. The
 * backend builds one bottom-level structure per geometry and one top-level
 * structure over the instances, synchronously. */
typedef struct _vio_as_geometry {
    const float    *positions;     /* vertex_count * 3 floats */
    int             vertex_count;
    const uint32_t *indices;       /* index_count uint32 (NULL: non-indexed triangle list) */
    int             index_count;
    /* Opacity micromap (VIO_FEATURE_OPACITY_MICROMAP, SM69-PLAN Phase 4): one OMM
     * per triangle in the OC1 layout both APIs share - 4^subdivision micro-
     * triangles, 1 (2-state) or 2 (4-state) bits each, LSB first - packed by vio,
     * omm_bytes per OMM. The geometry is then non-opaque: transparent micro-
     * triangles are skipped, unknown ones run the any-hit shader. */
    int                  omm_format;       /* 0 = none, 2 = 2-state, 4 = 4-state */
    int                  omm_subdivision;  /* 0..12 */
    const unsigned char *omm_data;         /* omm_count * omm_bytes */
    int                  omm_count;        /* = the geometry's triangle count */
    int                  omm_bytes;
} vio_as_geometry;

typedef struct _vio_as_instance {
    int   geometry;                /* index into geometries */
    float transform[12];           /* row-major 3x4 object -> world */
    int   hit_group;               /* hit group of the RT pipeline (A13; SBT offset / contribution) */
    int   mask;                    /* instance mask, 0..255 (rays skip it when mask & cull mask == 0) */
} vio_as_instance;

typedef struct _vio_as_desc {
    const vio_as_geometry *geometries;
    int                    geometry_count;
    const vio_as_instance *instances;
    int                    instance_count;
} vio_as_desc;

/* vio_rt_pipeline() (VIO_FEATURE_RAYTRACING): the ray tracing stages as
 * SPIR-V (GL_EXT_ray_tracing, Vulkan) and/or an HLSL library (D3D12, DXR 1.0:
 * exports vio_raygen / vio_miss / vio_closest_hit / vio_any_hit). One raygen,
 * one miss shader, one triangle hit group (closest hit + optional any hit). */
typedef enum {
    VIO_RT_STAGE_RAYGEN      = 0,
    VIO_RT_STAGE_MISS        = 1,
    VIO_RT_STAGE_CLOSEST_HIT = 2,
    VIO_RT_STAGE_ANY_HIT     = 3,
    VIO_RT_STAGE_COUNT       = 4
} vio_rt_stage;

#define VIO_RT_STAGE_CALLABLE 4     /* not in spirv[]: vio_rt_pipeline_desc.callable */
#define VIO_RT_MAX_GROUPS     8     /* miss shaders, hit groups, callables (each) */
#define VIO_RT_MAX_RECORD     256   /* bytes of shader record data per group */

/* One shader group (A13): a general group (raygen / miss / callable) has its
 * shader in spirv[0]; a triangle hit group the closest hit in spirv[0] and the
 * optional any hit in spirv[1]. record = the group's shader record data. */
typedef struct _vio_rt_group_src {
    const uint32_t *spirv[2];
    size_t          spirv_size[2];
    unsigned char   record[VIO_RT_MAX_RECORD];
} vio_rt_group_src;

typedef struct _vio_rt_pipeline_desc {
    const uint32_t *spirv[VIO_RT_STAGE_COUNT];      /* raygen / miss 0 / hit group 0 (legacy view) */
    size_t          spirv_size[VIO_RT_STAGE_COUNT]; /* bytes */
    const char     *hlsl;                           /* D3D12 library source, NULL when not given */
    int             max_recursion;                  /* >= 1 */
    int             payload_size;                   /* bytes, D3D12 shader config */
    /* Every group (A13): traceRayEXT's missIndex picks a miss shader, the
     * instance's hit_group (+ sbtRecordOffset) a hit group, executeCallableEXT's
     * index a callable. record_size bytes of record data follow each handle
     * (GLSL shaderRecordEXT buffer; D3D12 root constants at b0, space1). */
    vio_rt_group_src raygen;
    vio_rt_group_src miss[VIO_RT_MAX_GROUPS];
    int              miss_count;
    vio_rt_group_src hit[VIO_RT_MAX_GROUPS];
    int              hit_count;
    vio_rt_group_src callable[VIO_RT_MAX_GROUPS];
    int              callable_count;
    int              record_size;                   /* multiple of 4, 0 = no records */
} vio_rt_pipeline_desc;

/* A storage buffer bound for vio_trace_rays (vio_rt_bind_buffer). */
typedef struct _vio_rt_buffer_binding {
    void *backend_buffer;          /* create_storage_buffer handle, or the backend texture (kind 1) */
    int   binding;                 /* GLSL binding / HLSL u register (t + s register for a texture) */
    int   kind;                    /* VIO_RT_BIND_BUFFER / VIO_RT_BIND_TEXTURE (A13) */
} vio_rt_buffer_binding;

#define VIO_RT_BIND_BUFFER  0
#define VIO_RT_BIND_TEXTURE 1
#define VIO_RT_MAX_BUFFERS 16       /* bound resources per pipeline, buffers and textures */

/* ── Descriptor structs ───────────────────────────────────────────── */

typedef struct _vio_vertex_attrib {
    int        location;
    vio_format format;
    vio_usage  usage;
    /* Matrix inputs (`layout(location = 3) in mat4 aModel`) are expanded into
     * one attribute per column at location + column. matrix_columns is the
     * column count of the source matrix (0 / 1 for plain vectors) and
     * matrix_column this attribute's column. HLSL from SPIRV-Cross names the
     * columns TEXCOORD{location}_{column}, so the D3D input layouts need both. */
    int        matrix_columns;
    int        matrix_column;
} vio_vertex_attrib;

/* Mesh-level per-attribute description used by the create_mesh vtable slot.
 * Simpler than vio_vertex_attrib (no semantic usage), since mesh upload only
 * needs to know where each float-N attribute sits in the vertex layout. */
/* Where each vertex location sits in a mesh's vertices (OPEN-ITEMS-PLAN A31).
 * The typed-layout backends (D3D11, D3D12, Vulkan, Metal) build their input
 * layout from the shader's inputs; with this map they read each input at the
 * mesh's own offset instead of packing the inputs densely in location order.
 * key 0 = the mesh declared no layout (dense packing as before); otherwise a
 * hash of the offsets, so pipeline variants can be cached per layout. */
#define VIO_MESH_MAX_LOCATIONS 16
typedef struct _vio_mesh_layout {
    uint32_t key;
    int16_t  offset[VIO_MESH_MAX_LOCATIONS];   /* bytes into the vertex, -1 = not in the mesh */
} vio_mesh_layout;

/* Byte offset of a shader input at `location`: the mesh's when it has one,
 * else `dense` (the backend's packed offset). */
static inline int vio_mesh_layout_offset(const vio_mesh_layout *l, int location, int dense)
{
    if (!l || !l->key || location < 0 || location >= VIO_MESH_MAX_LOCATIONS || l->offset[location] < 0) return dense;
    return l->offset[location];
}

typedef struct _vio_mesh_attrib {
    int location;     /* glsl `layout(location = N)` */
    int components;   /* 1..4 floats */
    int offset;       /* bytes into the vertex */
} vio_mesh_attrib;

typedef struct _vio_pipeline_desc {
    void            *shader;
    vio_vertex_attrib *vertex_layout;
    int              vertex_attrib_count;
    vio_topology     topology;
    vio_cull_mode    cull_mode;
    int              depth_test;
    vio_depth_func   depth_func;    /* VIO_DEPTH_LESS (default 0) or VIO_DEPTH_LEQUAL */
    vio_blend_mode   blend;
    int              depth_write;            /* 1 (default) => depth test writes; 0 => test only
                                                (sky, transparent geometry) */
    int              color_mask;             /* VIO_COLOR_* bits, default VIO_COLOR_RGBA */
    float            depth_bias;             /* constant depth bias (shadow mapping) */
    float            slope_scaled_depth_bias; /* slope-scaled bias (shadow mapping) */
    int              hdr_output;             /* 1 => render-target output format is
                                                FP16 (R16G16B16A16_FLOAT); 0 (default)
                                                => R8G8B8A8_UNORM. A PSO's RTV format
                                                must match the bound render target's
                                                format on D3D12, so a pipeline drawing
                                                into an hdr=true target must set this.
                                                Honored by D3D12 only this round; the
                                                other backends treat it as a no-op
                                                (they derive the color format from the
                                                bound target / render-pass instead). */
    int              color_count;            /* MRT: number of colour outputs the PSO
                                                writes (0 => legacy single target from
                                                hdr_output). D3D12 needs the formats at
                                                PSO creation; Metal/GL/D3D11 derive them
                                                from the bound render target. */
    int              color_formats[VIO_MAX_COLOR_ATTACHMENTS]; /* vio_pixel_format */
    int              per_attachment;         /* 1 => attachment_blend[] / attachment_mask[]
                                                override blend / color_mask per colour
                                                attachment (MRT: alpha-blend colour while a
                                                data attachment stays untouched). */
    int              attachment_blend[VIO_MAX_COLOR_ATTACHMENTS]; /* vio_blend_mode per attachment */
    int              attachment_mask[VIO_MAX_COLOR_ATTACHMENTS];  /* VIO_COLOR_* bits per attachment */
    /* Stencil test (vio_pipeline(['stencil' => [...]]), VIO_FEATURE_STENCIL). The
     * same function / operations apply to front and back faces. The depth
     * attachments of every backend that reports the feature carry 8 stencil bits
     * (D24S8 on D3D, DEPTH24_STENCIL8 on GL); vio_clear resets them to 0. */
    int              stencil_enable;
    int              stencil_func;           /* vio_compare_func */
    int              stencil_ref;            /* 0..255 */
    int              stencil_read_mask;      /* 0..255 */
    int              stencil_write_mask;     /* 0..255 */
    int              stencil_pass_op;        /* vio_stencil_op: stencil + depth passed */
    int              stencil_fail_op;        /* vio_stencil_op: stencil test failed */
    int              stencil_depth_fail_op;  /* vio_stencil_op: stencil passed, depth failed */
    int              patch_vertices;         /* control points per patch for
                                                VIO_PATCHES (1..32; 0 => 3). */
    int              view_count;             /* multiview: views per draw (the
                                                shader's 'view_count', 2..4); 0 = off */
} vio_pipeline_desc;

typedef struct _vio_buffer_desc {
    vio_buffer_type type;
    const void     *data;
    size_t          size;
    int             binding;
    int             stride;   /* element stride in bytes for STORAGE buffers
                                 (StructuredBuffer StructureByteStride). 0 (default)
                                 => treat as raw/4-byte elements. */
    int             indirect; /* STORAGE buffer that vio_draw_indirect() reads its
                                 draw arguments from (D3D11 needs the flag at
                                 creation; raw / stride <= 4 only). */
} vio_buffer_desc;

/* Colour attachment formats for render targets (vio_render_target
 * 'attachments' => [...]) and D3D12 PSO output formats. RGBA8 is the default
 * and the swapchain format; RGBA16F is what the legacy 'hdr' => true selects. */
typedef enum {
    VIO_FORMAT_RGBA8      = 0,
    VIO_FORMAT_RGBA16F    = 1,
    VIO_FORMAT_RGBA32F    = 2,
    VIO_FORMAT_R11G11B10F = 3,
    VIO_FORMAT_RG16F      = 4,
    VIO_FORMAT_R16F       = 5,
    VIO_FORMAT_R32F       = 6,
    VIO_FORMAT_R8         = 7,
    VIO_FORMAT_RGB10A2    = 8,   /* HDR10 backbuffer (R10G10B10A2_UNORM); readback expands to RGBA8 */
    /* Block-compressed texture data (vio_texture(['format' => …]), GAP-PHASE5
     * Block 9): 4x4 blocks, BC1 / BC4 8 bytes, BC3 / BC5 / BC7 16 bytes. Texture
     * only - never a render-target format. */
    VIO_FORMAT_BC1        = 9,   /* RGB(A) 1-bit alpha,   DXT1 */
    VIO_FORMAT_BC3        = 10,  /* RGBA,                 DXT5 */
    VIO_FORMAT_BC4        = 11,  /* single channel (R)   */
    VIO_FORMAT_BC5        = 12,  /* two channels (RG), normal maps */
    VIO_FORMAT_BC7        = 13,  /* high-quality RGBA    */
    /* ASTC LDR, 16-byte blocks of N x N texels (OPEN-ITEMS-PLAN A20); Apple GPUs,
     * Vulkan textureCompressionASTC_LDR, GL_KHR_texture_compression_astc_ldr. */
    VIO_FORMAT_ASTC_4x4   = 14,
    VIO_FORMAT_ASTC_5x5   = 15,
    VIO_FORMAT_ASTC_6x6   = 16,
    VIO_FORMAT_ASTC_8x8   = 17,
} vio_pixel_format;

typedef struct _vio_texture_desc {
    const void *data;
    size_t      data_size;
    int         width;
    int         height;
    int         depth;     /* 0 (default) => 2D texture; > 0 => 3D / volume texture
                              (data is width*height*depth*4 RGBA8, slices in +Z order) */
    vio_filter  filter;
    vio_wrap    wrap;
    int         mipmaps;
    int         single_channel; /* 1 => R8 (1 byte/px, e.g. font coverage atlas);
                                   0 => RGBA8. data must match the chosen format. */
    int         storage;        /* 1 => usable as a compute storage image
                                   (image2D / image3D): Metal ShaderWrite usage,
                                   D3D UAV, GL image unit. Sampling still works. */
    int         anisotropy;     /* max anisotropic filtering, 0/1 = off (default),
                                   clamped to 16 and to the device limit. Only
                                   meaningful with filter = LINEAR. */
    /* GAP-PHASE5 Block 9. format: VIO_FORMAT_RGBA8 (default; single_channel
     * selects R8) or a VIO_FORMAT_BC* block format. layers > 1 => 2D texture
     * array. mip_levels > 1 => `data` already holds that many levels, level-
     * major with the layers consecutive inside a level, images tightly packed
     * (rows of 4x4 blocks for BC) - the KTX2 layout; `mipmaps` is then moot. */
    int         format;
    int         layers;
    int         mip_levels;
} vio_texture_desc;

typedef struct _vio_shader_desc {
    const void       *vertex_data;
    size_t            vertex_size;
    const void       *fragment_data;
    size_t            fragment_size;
    vio_shader_format format;
    /* Optional stages (NULL = absent). Same encoding as vertex/fragment
     * (SPIR-V when format is GLSL/AUTO on the backend path, else source).
     * tess_control and tess_eval always come as a pair. Backends that report
     * VIO_FEATURE_GEOMETRY / VIO_FEATURE_TESSELLATION = 0 never see them:
     * vio_shader() refuses the stage up front. */
    const void       *geometry_data;
    size_t            geometry_size;
    const void       *tess_control_data;
    size_t            tess_control_size;
    const void       *tess_eval_data;
    size_t            tess_eval_size;
    /* HLSL overrides of the optional stages (NULL = transpile the GLSL stage).
     * Only backends with VIO_FEATURE_HLSL_STAGE_OVERRIDE read them; the source
     * outputs D3D clip space (z in [0, w]) - no depth fixup is added. */
    const char       *geometry_hlsl;
    const char       *tess_control_hlsl;
    const char       *tess_eval_hlsl;
    /* 'hlsl' => ['fragment' => src]: replaces the transpiled pixel shader (D3D12;
     * sampler feedback has no GLSL form). The GLSL fragment stage stays required
     * and defines the cbuffer layout. */
    const char       *fragment_hlsl;
    /* vio_compute_pipeline(['msl' => src]) (OPEN-ITEMS A17): Metal compiles this
     * kernel instead of the translated GLSL (MSL 4 tensors / Metal Performance
     * Primitives have no GLSL form). The GLSL kernel stays required: its
     * reflection gives local_size and the bindings (binding N = buffer / texture N). */
    const char       *compute_msl;
    /* vio_compute_pipeline(['hlsl' => ...]) (SM69-PLAN Phase 2): D3D11 / D3D12
     * compile this HLSL kernel (entry main) instead of the translated GLSL, with
     * the GLSL kernel's reflection: Params UBO binding N = bN, storage buffers
     * and images binding N = tN / uN. */
    const char       *compute_hlsl;
    /* vio_shader(['view_count' => N]) (VIO_FEATURE_MULTIVIEW): the stages use
     * gl_ViewIndex and every draw runs N times, view v into layer v of a target
     * bound with VIO_RT_ALL_LAYERS. 0 = not a multiview shader. */
    int               view_count;
    /* Mesh pipelines (VIO_FEATURE_MESH_SHADER): vertex_data then holds the MESH
     * stage's SPIR-V (execution model MeshEXT, see vio_spirv_execution_model) and
     * task_data the optional task stage. */
    const void       *task_data;
    size_t            task_size;
} vio_shader_desc;

/* Shader stage index shared by vio_shader_object's per-stage constant
 * buffers, the compiler and the bind_stage_constants vtable slot. */
typedef enum _vio_shader_stage {
    VIO_STAGE_VERTEX       = 0,
    VIO_STAGE_FRAGMENT     = 1,
    VIO_STAGE_GEOMETRY     = 2,
    VIO_STAGE_TESS_CONTROL = 3,
    VIO_STAGE_TESS_EVAL    = 4,
    VIO_STAGE_COUNT        = 5,
} vio_shader_stage;

/* Mesh-pipeline stages for vio_compile_glsl_stage_to_spirv (outside the
 * per-stage arrays VIO_STAGE_COUNT sizes: a mesh shader takes the vertex slot
 * of vio_shader_object, the task shader its own field). */
#define VIO_STAGE_MESH 16
#define VIO_STAGE_TASK 17

typedef struct _vio_draw_cmd {
    void *pipeline;
    void *vertex_buffer;
    int   vertex_count;
    int   first_vertex;
    int   instance_count;
    int   vertex_stride;    /* mesh stride in bytes (overrides pipeline stride if > 0) */
} vio_draw_cmd;

typedef struct _vio_draw_indexed_cmd {
    void *pipeline;
    void *vertex_buffer;
    void *index_buffer;
    int   index_count;
    int   first_index;
    int   vertex_offset;
    int   instance_count;
    int   vertex_stride;    /* mesh stride in bytes (overrides pipeline stride if > 0) */
    int   index_bytes;      /* 2 => uint16 indices, 4 (or 0) => uint32 (GAP-PHASE5 Block 2) */
} vio_draw_indexed_cmd;

typedef struct _vio_compute_cmd {
    void *pipeline;
    int   group_count_x;
    int   group_count_y;
    int   group_count_z;
    int   async;   /* 1 => record into the open frame's command stream instead of
                      a fenced standalone submission; completion is observed via
                      compute_wait / read_buffer. Backends without an in-frame
                      path (or outside vio_begin/vio_end) run synchronously. */
} vio_compute_cmd;

#endif /* VIO_TYPES_H */

/*
 * php-vio - Metal Backend implementation (macOS)
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "php.h"

#ifdef HAVE_METAL

#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>
#import <objc/message.h>
#include <os/lock.h>
#include <stdatomic.h>

#ifdef HAVE_GLFW
#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>
#define GLFW_EXPOSE_NATIVE_COCOA
#include <GLFW/glfw3native.h>
#endif

#include "vio_metal.h"

static void metal_marks_reset(void);
#include "../../shaders/shaders_2d.h"
#include "../../vio_render_target.h"
#include "../../vio_texfmt.h"
#include "../../vio_buffer.h"   /* vio_buffer_object — compute storage-buffer free path */

/* SPIRV-Cross C API — used by the compute path to transpile the SDF compute
 * SPIR-V to MSL with EXPLICIT MSL buffer indices (see metal_cs_spirv_to_msl).
 * Guarded the same way as src/vio_shader_reflect.c; when SPIRV-Cross is absent
 * the compute primitive reports unsupported and the engine falls back to CPU. */
#ifdef HAVE_SPIRV_CROSS
#include <spirv_cross/spirv_cross_c.h>
#endif

/* stb_image_write for screenshot PNG export (implementation in stb_image_write_impl.c) */
#include "../../../vendor/stb/stb_image_write.h"

/* ── Version ladder / capabilities ───────────────────────────────── */

/* What the device can do at the Metal Shading Language version the context
 * runs at - the Metal counterpart of vio_gl.caps. Filled once per context in
 * metal_detect_caps(); every version-gated flag is (device support AND
 * msl_version >= its minimum), so pinning a lower rung switches it off. */
typedef struct _vio_metal_caps {
    int msl_version;            /* rung in use, major * 10 + minor (20 .. 41) */
    int msl_max;                /* highest rung the OS compiles */
    int apple_family;           /* highest MTLGPUFamilyAppleN, 0 = none */
    int mac2, metal3, metal4;   /* MTLGPUFamilyMac2 / Metal3 / Metal4 */
    int tessellation;           /* MSL 2.1: [[patch]] + tessellation kernels */
    int layered_vertex;         /* [[render_target_array_index]] from the VS: Mac2 / Apple5 */
    int quad_group;             /* quad_* permutes, MSL 2.1 (SPIRV-Cross on macOS): Mac2 / Apple4 */
    int simd_group;             /* simd_* reductions / ballot, MSL 2.2 (threads_per_simdgroup in fragment functions): Mac2 / Apple7 */
    int barycentrics;           /* [[barycentric_coord]], MSL 2.2 */
    int vertex_amplification;   /* [[amplification_id]], MSL 2.2 */
    int argument_buffers_tier2; /* bindless-style argument buffers */
    int raytracing;             /* intersection queries, MSL 2.3 */
    int function_pointers;      /* visible / intersection function tables, MSL 2.3 */
    int raytracing_from_render; /* ray queries in render pipelines, MSL 2.4 */
    int cooperative_matrix;     /* GL_KHR_cooperative_matrix -> simdgroup_matrix 8x8; SPIRV-Cross needs MSL 3.1: Apple7+ */
    int mesh_shaders;           /* object / mesh stages, MSL 3.0: Metal3 + (Apple7 / Mac2) */
    int atomic64;               /* 64-bit atomic min / max, MSL 3.1: Apple9 */
    int tensors;                /* MTLTensor + Metal Performance Primitives, MSL 4.0: Metal4 */
    int bindless;               /* texture-handle argument buffers (gpuResourceID), MSL 3.0: Metal3 + tier 2 */
    int rasterization_rate_map;
    int bc_texture_compression;
    int unified_memory;
} vio_metal_caps;

/* ── Metal state ─────────────────────────────────────────────────── */

typedef struct _vio_metal_state {
    id<MTLDevice>              device;
    id<MTLCommandQueue>        command_queue;
    CAMetalLayer              *metal_layer;
    id<CAMetalDrawable>        current_drawable;
    id<MTLCommandBuffer>       current_cmd_buf;
    id<MTLRenderCommandEncoder> current_encoder;
    double                     last_gpu_ms;     /* GPUEndTime - GPUStartTime of the last completed frame */
    void                      *cur_marks;       /* metal_mark_frame of the open frame (vio_gpu_timestamp), NULL = no mark yet */
    MTLRenderPassDescriptor   *render_pass_desc;
    id<MTLTexture>             depth_texture;
    int                        width;
    int                        height;
    float                      clear_r, clear_g, clear_b, clear_a;
    int                        initialized;
    int                        vsync;
    id<MTLTexture>             offscreen_texture; /* for vsync-off rendering */
    /* Swapchain MSAA (vio_create 'samples'): rendered into this 2DMultisample
     * pair and resolved into the drawable / offscreen texture at pass end. 1 =
     * off. Clamped to what the device supports. */
    int                        samples;
    id<MTLTexture>             msaa_color;
    id<MTLTexture>             msaa_depth;
    int                        debug;            /* vio_create 'debug': per-command-buffer error reporting */
    int                        unified_memory;   /* Apple silicon: Shared textures OK; Intel: Managed */
    char                       gpu_name[256];
    vio_metal_caps             caps;             /* version ladder + device capabilities */
    char                       api_name[16];     /* "Metal 4" (vio_backend_info) */
    /* Swapchain colour format: BGRA8Unorm, or RGB10A2Unorm with the layer in
     * the ITU-R 2100 PQ colour space for vio_create(['hdr_output' => ...]). The
     * offscreen / MSAA swapchain textures, the 2D swapchain variant and the PSO
     * target description all follow it. */
    MTLPixelFormat             swap_format;
    int                        hdr_output;       /* 1 = HDR10 (RGB10A2 + ST 2084) active */
    float                      hdr_paper_white;  /* nits that 2D white (1.0) maps to */
    /* vio_create(['frame_latency' => n]): at most n frames in flight. begin_frame
     * waits on the semaphore, the frame's command buffer signals it on
     * completion - the Metal counterpart of the DXGI waitable object. */
    dispatch_semaphore_t       frame_semaphore;
    int                        frame_latency;
    int                        frame_semaphore_held; /* begin_frame took a slot present has not handed on yet */
#ifdef HAVE_GLFW
    /* When the backend was bootstrapped via vio_metal_setup_context() the
     * GLFW window is polled each frame to discover resizes. Pure-native
     * setups (iOS, headless) leave this NULL and call
     * vio_metal_handle_resize() externally instead. */
    GLFWwindow                *glfw_window;
#endif
} vio_metal_state;

static vio_metal_state vio_mtl = {0};
static void metal_bindless_release(void);
static void metal_bindless_bind(id<MTLRenderCommandEncoder> enc, int vertex, int fragment);

/* Keep reference to last presented frame for read_pixels/screenshot */
static id<MTLTexture>       last_presented_texture = nil;
static id<MTLCommandBuffer> last_presented_cmd_buf = nil;

/* Currently bound offscreen render target. When non-NULL, metal_begin_frame
 * builds its MTLRenderPassDescriptor from the RT's textures instead of the
 * swapchain drawable. Mirrors vio_d3d11.current_rtv. NULL means "draw to
 * swapchain". */
static vio_render_target_object *current_bound_rt = NULL;
/* Cube RT: face / mip level bound as the colour attachment (-1 = not a face bind). */
static int current_bound_face  = -1;
static int current_bound_level = 0;

/* ── Metal 2D pipeline state ─────────────────────────────────────── */

#define VIO_METAL_2D_VARIANTS 8

/* The 2D PSOs bake the colour format + sample count of their target, so the
 * batch keeps one (shapes, sprites) pair per (format, samples) it has drawn
 * into: swapchain BGRA8x1, HDR RTs, MSAA RTs. Index 0 is built eagerly. */
typedef struct _vio_metal_2d_variant {
    int                        pixel_format;  /* MTLPixelFormat of attachment 0 */
    int                        extra_fmts[VIO_MAX_COLOR_ATTACHMENTS - 1]; /* MRT attachments 1..n (unwritten, mask none) */
    int                        color_count;
    int                        samples;
    int                        has_depth;     /* 0 on cube-RT mip levels > 0 (no depth attachment) */
    int                        has_stencil;
    id<MTLRenderPipelineState> shapes;
    id<MTLRenderPipelineState> sprites;
} vio_metal_2d_variant;

typedef struct _vio_metal_2d {
    id<MTLRenderPipelineState> pipeline_shapes;   /* == variants[0], swapchain */
    id<MTLRenderPipelineState> pipeline_sprites;
    id<MTLFunction>            vertex_fn;
    id<MTLFunction>            frag_shapes_fn;
    id<MTLFunction>            frag_sprites_fn;
    MTLVertexDescriptor       *vertex_desc;
    vio_metal_2d_variant       variants[VIO_METAL_2D_VARIANTS];
    int                        variant_count;
    id<MTLDepthStencilState>   depth_disabled;
    id<MTLSamplerState>        sampler;
    id<MTLBuffer>              vertex_buffer;
    int                        vb_capacity;
    int                        initialized;
} vio_metal_2d;

static vio_metal_2d mtl_2d = {0};

/* ── Metal texture registry ──────────────────────────────────────── */

#define VIO_METAL_MAX_TEXTURES 8192

static id<MTLTexture> metal_textures[VIO_METAL_MAX_TEXTURES];
static unsigned int   metal_next_texture_id = 1;

static unsigned int metal_register_texture(id<MTLTexture> tex)
{
    /* Find a free slot starting from metal_next_texture_id */
    for (unsigned int i = metal_next_texture_id; i < VIO_METAL_MAX_TEXTURES; i++) {
        if (metal_textures[i] == nil) {
            metal_textures[i] = tex;
            metal_next_texture_id = i + 1;
            return i;
        }
    }
    /* Wrap around and search from beginning */
    for (unsigned int i = 1; i < metal_next_texture_id && i < VIO_METAL_MAX_TEXTURES; i++) {
        if (metal_textures[i] == nil) {
            metal_textures[i] = tex;
            metal_next_texture_id = i + 1;
            return i;
        }
    }
    return 0; /* Registry full */
}

/* Forward declarations for helpers used before their definition. */
static void metal_resize(int width, int height);
static void metal_open_encoder(int load_clear);
static id<MTLTexture> metal_current_color_texture(void);
/* Colour layout of whatever the open encoder renders into: every attachment's
 * pixel format (count 0 for depth-only targets), sample count, depth presence. */
typedef struct _vio_metal_target_desc {
    MTLPixelFormat fmts[VIO_MAX_COLOR_ATTACHMENTS];
    int            count;      /* colour attachments; 0 => depth-only */
    int            samples;
    int            has_depth;
    int            has_stencil; /* depth attachment is VIO_METAL_DEPTH_STENCIL */
} vio_metal_target_desc;

/* Depth format of the swapchain and of every render target (colour, depth_only,
 * cube, array): 8 stencil bits for vio_pipeline(['stencil' => ...]), like D3D's
 * D24S8 everywhere. Sampling reads the depth plane; the depth readback blits it
 * with MTLBlitOptionDepthFromDepthStencil. */
#define VIO_METAL_DEPTH_STENCIL MTLPixelFormatDepth32Float_Stencil8
static void metal_current_target(vio_metal_target_desc *t);
static MTLPixelFormat metal_pixel_format(int vio_fmt);
static int metal_rt_attachment_count(const vio_render_target_object *rt);
static void metal_destroy_texture(void *texture);

/* ── Pass state that outlives a reopened encoder ──────────────────────
 * metal_open_encoder starts every pass with the full-target viewport; a
 * tessellation draw that has to reopen the pass mid-draw restores the
 * viewports and the fragment textures bound for that draw from here. */
#define VIO_METAL_MAX_VIEWPORTS 16
static int            metal_target_w = 0, metal_target_h = 0;
static MTLViewport    metal_vp[VIO_METAL_MAX_VIEWPORTS];
static MTLScissorRect metal_sc[VIO_METAL_MAX_VIEWPORTS];
static int            metal_vp_count = 0;        /* 0 = full-target default */
static int            metal_vp_has_scissor = 0;  /* vio_viewports sets a scissor per viewport */
#define VIO_METAL_FS_SLOTS 31
static id<MTLTexture>      metal_fs_tex[VIO_METAL_FS_SLOTS];
static id<MTLSamplerState> metal_fs_smp[VIO_METAL_FS_SLOTS];

/* Textures of the compute kernels of an emulated geometry stage (A28) by MSL
 * index: [0] vertex kernel, [1] geometry kernel (metal_bind_gs_stage_texture,
 * set on the kernel encoders by metal_draw_gs). */
static id<MTLTexture>      metal_gs_tex[2][VIO_METAL_FS_SLOTS];
static id<MTLSamplerState> metal_gs_smp[2][VIO_METAL_FS_SLOTS];

static void metal_fs_shadow_reset(void)
{
    for (int i = 0; i < VIO_METAL_FS_SLOTS; i++) { metal_fs_tex[i] = nil; metal_fs_smp[i] = nil; }
}

static void metal_fs_shadow_set(int idx, id<MTLTexture> tex, id<MTLSamplerState> smp)
{
    if (idx < 0 || idx >= VIO_METAL_FS_SLOTS) return;
    metal_fs_tex[idx] = tex;
    metal_fs_smp[idx] = smp;
}

/* Viewport rect -> scissor clamped to the open pass (empty when outside). */
static MTLScissorRect metal_clamped_scissor(int x, int y, int w, int h)
{
    int x0 = x < 0 ? 0 : x, y0 = y < 0 ? 0 : y;
    int x1 = x + w, y1 = y + h;
    if (x1 > metal_target_w) x1 = metal_target_w;
    if (y1 > metal_target_h) y1 = metal_target_h;
    MTLScissorRect r = { (NSUInteger)(x0 < metal_target_w ? x0 : 0), (NSUInteger)(y0 < metal_target_h ? y0 : 0), 0, 0 };
    if (x1 > x0 && y1 > y0) { r.width = (NSUInteger)(x1 - x0); r.height = (NSUInteger)(y1 - y0); }
    return r;
}

static void metal_apply_viewports(void)
{
    if (!vio_mtl.current_encoder || metal_vp_count < 1) return;
    if (metal_vp_count == 1) {
        [vio_mtl.current_encoder setViewport:metal_vp[0]];
        if (metal_vp_has_scissor) [vio_mtl.current_encoder setScissorRect:metal_sc[0]];
        return;
    }
    [vio_mtl.current_encoder setViewports:metal_vp count:(NSUInteger)metal_vp_count];
    [vio_mtl.current_encoder setScissorRects:metal_sc count:(NSUInteger)metal_vp_count];
}

/* Vertex-stage [[render_target_array_index]] / [[viewport_array_index]] and
 * setViewports:count: (layered rendering, multiple viewports). */
static int metal_supports_layered_vertex(void)
{
    return vio_mtl.device ? vio_mtl.caps.layered_vertex : 0;
}
static void metal_ring_begin_frame(void);
static void metal_ring_end_frame(id<MTLCommandBuffer> cb);
static void metal_3d_shutdown(void);
static int  metal_clamp_sample_count(int requested);

/* Storage mode for CPU-uploaded textures: Shared needs unified memory (Apple
 * silicon); discrete Intel-Mac GPUs require Managed (replaceRegion: syncs). */
static MTLStorageMode metal_cpu_texture_storage(void)
{
    return vio_mtl.unified_memory ? MTLStorageModeShared : MTLStorageModeManaged;
}

/* Every command buffer goes through here so `debug` can attach execution-status
 * tracking + a completion handler that reports GPU faults (the Metal analogue
 * of the D3D debug layer / Vulkan validation output). */
static id<MTLCommandBuffer> metal_new_command_buffer(void)
{
    if (!vio_mtl.command_queue) return nil;
    if (!vio_mtl.debug) return [vio_mtl.command_queue commandBuffer];

    MTLCommandBufferDescriptor *d = [[MTLCommandBufferDescriptor alloc] init];
    d.errorOptions = MTLCommandBufferErrorOptionEncoderExecutionStatus;
    d.retainedReferences = YES;
    id<MTLCommandBuffer> cb = [vio_mtl.command_queue commandBufferWithDescriptor:d];
    [cb addCompletedHandler:^(id<MTLCommandBuffer> done) {
        if (done.error) {
            /* Completion runs on a Metal thread: no Zend calls here. */
            fprintf(stderr, "[vio metal debug] command buffer failed: %s\n",
                    [[done.error localizedDescription] UTF8String]);
            NSArray *infos = done.error.userInfo[MTLCommandBufferEncoderInfoErrorKey];
            for (id<MTLCommandBufferEncoderInfo> info in infos) {
                fprintf(stderr, "[vio metal debug]   encoder '%s' state %ld\n",
                        [info.label UTF8String], (long)info.errorState);
            }
            fflush(stderr);
        }
    }];
    return cb;
}

/* Swapchain MSAA attachments at the current drawable size (no-op when off). */
static void metal_create_swapchain_msaa(int w, int h)
{
    vio_mtl.msaa_color = nil;
    vio_mtl.msaa_depth = nil;
    if (vio_mtl.samples <= 1) return;

    MTLTextureDescriptor *cd = [MTLTextureDescriptor
        texture2DDescriptorWithPixelFormat:vio_mtl.swap_format width:w height:h mipmapped:NO];
    cd.textureType = MTLTextureType2DMultisample;
    cd.sampleCount = (NSUInteger)vio_mtl.samples;
    cd.usage = MTLTextureUsageRenderTarget;
    cd.storageMode = MTLStorageModePrivate;
    vio_mtl.msaa_color = [vio_mtl.device newTextureWithDescriptor:cd];

    MTLTextureDescriptor *dd = [MTLTextureDescriptor
        texture2DDescriptorWithPixelFormat:VIO_METAL_DEPTH_STENCIL width:w height:h mipmapped:NO];
    dd.textureType = MTLTextureType2DMultisample;
    dd.sampleCount = (NSUInteger)vio_mtl.samples;
    dd.usage = MTLTextureUsageRenderTarget;
    dd.storageMode = MTLStorageModePrivate;
    vio_mtl.msaa_depth = [vio_mtl.device newTextureWithDescriptor:dd];

    if (!vio_mtl.msaa_color || !vio_mtl.msaa_depth) {
        php_error_docref(NULL, E_WARNING, "Metal: %dx swapchain MSAA unavailable, falling back to 1x", vio_mtl.samples);
        vio_mtl.msaa_color = nil;
        vio_mtl.msaa_depth = nil;
        vio_mtl.samples = 1;
    }
}

/* ── Depth texture helper ────────────────────────────────────────── */

static void create_depth_texture(int w, int h)
{
    if (vio_mtl.depth_texture) {
        vio_mtl.depth_texture = nil;
    }

    MTLTextureDescriptor *desc = [MTLTextureDescriptor
        texture2DDescriptorWithPixelFormat:VIO_METAL_DEPTH_STENCIL
        width:w height:h mipmapped:NO];
    desc.usage = MTLTextureUsageRenderTarget;
    desc.storageMode = MTLStorageModePrivate;

    vio_mtl.depth_texture = [vio_mtl.device newTextureWithDescriptor:desc];
}

/* ── Setup / Teardown ────────────────────────────────────────────── */

/* Set by the platform wrapper before setup_context_native: 1 when the window's
 * display can show extended dynamic range (an HDR / XDR screen), which is what
 * hdr_output => 1 asks for. hdr_output => 2 ignores it. */
static int metal_display_edr = 0;

/* HDR10 swapchain: 10-bit RGB10A2 in the ITU-R BT.2100 PQ colour space. The
 * layer then expects ST 2084-encoded values - the 2D batch PQ-encodes its
 * display-referred colours (paper white), 3D shaders write what they write,
 * exactly as on the D3D / Vulkan HDR10 swapchains. */
static void metal_configure_layer_format(vio_config *cfg)
{
    vio_mtl.swap_format = MTLPixelFormatBGRA8Unorm;
    vio_mtl.hdr_output = 0;
    vio_mtl.hdr_paper_white = cfg->hdr_paper_white > 0.0f ? cfg->hdr_paper_white : 200.0f;
#if TARGET_OS_OSX
    if (cfg->hdr_output >= 2 || (cfg->hdr_output == 1 && metal_display_edr)) {
        if (@available(macOS 10.15, *)) {
            CGColorSpaceRef pq = CGColorSpaceCreateWithName(kCGColorSpaceITUR_2100_PQ);
            if (pq) {
                vio_mtl.metal_layer.pixelFormat = MTLPixelFormatRGB10A2Unorm;
                vio_mtl.metal_layer.colorspace = pq;
                vio_mtl.metal_layer.wantsExtendedDynamicRangeContent = YES;
                CGColorSpaceRelease(pq);
                vio_mtl.swap_format = MTLPixelFormatRGB10A2Unorm;
                vio_mtl.hdr_output = 1;
                return;
            }
        }
    }
#endif
    vio_mtl.metal_layer.pixelFormat = MTLPixelFormatBGRA8Unorm;
}

/* ── Version ladder ──────────────────────────────────────────────── */

static int  metal_supports_bc(void);
static void metal_msl_set_target(int version, int ios);   /* vio_metal_msl.h */

/* Metal Shading Language rungs, newest first - the counterpart of the OpenGL
 * context ladder (vio_window.c). The values are major * 10 + minor; the
 * MTLLanguageVersion is built numerically ((major << 16) | minor) so an older
 * SDK still compiles and the OS decides at runtime what it accepts. */
static const int metal_msl_ladder[] = { 41, 40, 32, 31, 30, 24, 23, 22, 21, 20 };
#define METAL_MSL_LADDER_N ((int)(sizeof(metal_msl_ladder) / sizeof(metal_msl_ladder[0])))
#define METAL_MSL_FLOOR 20

static MTLLanguageVersion metal_language_version(int v)
{
    return (MTLLanguageVersion)(((NSUInteger)(v / 10) << 16) | (NSUInteger)(v % 10));
}

/* MTLCompileOptions for every library of the context: the ladder rung. */
static MTLCompileOptions *metal_compile_options(void)
{
    MTLCompileOptions *opts = [MTLCompileOptions new];
    opts.languageVersion = metal_language_version(vio_mtl.caps.msl_version > 0 ? vio_mtl.caps.msl_version : 21);
    return opts;
}

/* 1 when the OS compiles a trivial kernel at MSL `v`. An unknown language
 * version fails the compile (older OS) or raises (older framework). */
static int metal_probe_msl(id<MTLDevice> device, int v)
{
    @autoreleasepool {
        MTLCompileOptions *opts = [MTLCompileOptions new];
        id<MTLLibrary> lib = nil;
        @try {
            opts.languageVersion = metal_language_version(v);
            NSError *err = nil;
            lib = [device newLibraryWithSource:@"kernel void vio_probe(device uint *b [[buffer(0)]]) { b[0] = 1u; }\n"
                                       options:opts error:&err];
        } @catch (NSException *e) {
            lib = nil;
        }
        return lib != nil;
    }
}

/* Highest rung the OS accepts (cached: it depends on the OS, not the context). */
static int metal_msl_max(id<MTLDevice> device)
{
    static int cached = 0;
    if (cached) return cached;
    cached = METAL_MSL_FLOOR;
    for (int i = 0; i < METAL_MSL_LADDER_N; i++) {
        if (metal_probe_msl(device, metal_msl_ladder[i])) { cached = metal_msl_ladder[i]; break; }
    }
    return cached;
}

/* Rung for a request (major * 10 + minor; 0 = the maximum): the highest rung
 * <= the request, clamped to [floor, max]. */
static int metal_select_msl_version(int requested, int max)
{
    if (requested <= 0 || requested >= max) return max;
    for (int i = 0; i < METAL_MSL_LADDER_N; i++) {
        if (metal_msl_ladder[i] <= requested && metal_msl_ladder[i] <= max) return metal_msl_ladder[i];
    }
    return METAL_MSL_FLOOR;
}

static int metal_has_family(long family)
{
    if (@available(macOS 10.15, iOS 13.0, *)) {
        @try {
            return [vio_mtl.device supportsFamily:(MTLGPUFamily)family] ? 1 : 0;
        } @catch (NSException *e) {
            return 0;
        }
    }
    return 0;
}

#define METAL_DEVICE_BOOL(sel) \
    ([vio_mtl.device respondsToSelector:@selector(sel)] && ((BOOL (*)(id, SEL))objc_msgSend)(vio_mtl.device, @selector(sel)))

/* Fill vio_mtl.caps for the rung `msl` (the device is open). */
static void metal_detect_caps(int msl, int max)
{
    vio_metal_caps *c = &vio_mtl.caps;
    memset(c, 0, sizeof(*c));
    c->msl_version = msl;
    c->msl_max = max;
    for (int a = 11; a >= 1; a--) {
        if (metal_has_family(1000 + a)) { c->apple_family = a; break; }
    }
    c->mac2   = metal_has_family(2002);
    c->metal3 = metal_has_family(5001);
    c->metal4 = metal_has_family(5002);
    snprintf(vio_mtl.api_name, sizeof(vio_mtl.api_name), "Metal %d", c->metal4 ? 4 : (c->metal3 ? 3 : 2));

    int spirv_cross = 0;
#ifdef HAVE_SPIRV_CROSS
    spirv_cross = 1;
#endif
    c->tessellation   = spirv_cross && msl >= 21;
    c->layered_vertex = c->mac2 || c->apple_family >= 5;
    c->quad_group     = msl >= 21 && (c->mac2 || c->apple_family >= 4);
    c->simd_group     = msl >= 22 && (c->mac2 || c->apple_family >= 7);   /* gl_SubgroupSize in a fragment shader needs 2.2 */
    c->barycentrics   = msl >= 22 && METAL_DEVICE_BOOL(supportsShaderBarycentricCoordinates);
    if (msl >= 22 && [vio_mtl.device respondsToSelector:@selector(supportsVertexAmplificationCount:)]) {
        if (@available(macOS 10.15.4, iOS 13.0, *)) c->vertex_amplification = [vio_mtl.device supportsVertexAmplificationCount:2] ? 1 : 0;
    }
    if ([vio_mtl.device respondsToSelector:@selector(argumentBuffersSupport)]) {
        c->argument_buffers_tier2 = vio_mtl.device.argumentBuffersSupport == MTLArgumentBuffersTier2;
    }
    c->raytracing             = msl >= 23 && METAL_DEVICE_BOOL(supportsRaytracing);
    c->function_pointers      = msl >= 23 && METAL_DEVICE_BOOL(supportsFunctionPointers);
    c->raytracing_from_render = msl >= 24 && METAL_DEVICE_BOOL(supportsRaytracingFromRender);
    c->cooperative_matrix     = spirv_cross && msl >= 31 && c->apple_family >= 7;
    c->mesh_shaders           = msl >= 30 && c->metal3 && (c->apple_family >= 7 || c->mac2);
    c->atomic64               = msl >= 31 && c->apple_family >= 9;
    c->tensors                = msl >= 40 && c->metal4;
    c->bindless               = msl >= 30 && c->metal3 && c->argument_buffers_tier2;
    if ([vio_mtl.device respondsToSelector:@selector(supportsRasterizationRateMapWithLayerCount:)]) {
        if (@available(macOS 10.15.4, iOS 13.0, *)) c->rasterization_rate_map = [vio_mtl.device supportsRasterizationRateMapWithLayerCount:1] ? 1 : 0;
    }
    c->bc_texture_compression = metal_supports_bc();
    c->unified_memory         = vio_mtl.unified_memory;

#if TARGET_OS_IPHONE
    metal_msl_set_target(msl, 1);
#else
    metal_msl_set_target(msl, 0);
#endif
}

/* Requested rung: vio_create(['msl_version' => n]), else VIO_METAL_MSL_VERSION. */
static int metal_requested_msl(const vio_config *cfg)
{
    if (cfg && cfg->msl_version > 0) return cfg->msl_version;
    const char *env = getenv("VIO_METAL_MSL_VERSION");
    return env && *env ? atoi(env) : 0;
}

int vio_metal_setup_context_native(void *cf_metal_layer, int width, int height,
                                   vio_config *cfg)
{
    @autoreleasepool {
        if (!cf_metal_layer) {
            php_error_docref(NULL, E_WARNING, "Metal: setup_context_native called with NULL layer");
            return -1;
        }

        /* Debug: Metal's API validation layer is loaded by the framework when
         * METAL_DEVICE_WRAPPER_TYPE=1 is in the environment at device creation
         * time, so set it (without clobbering an explicit user choice) before
         * the first device. Per-command-buffer fault reporting is wired in
         * metal_new_command_buffer. */
        vio_mtl.debug = cfg->debug ? 1 : 0;
        if (vio_mtl.debug) {
            setenv("METAL_DEVICE_WRAPPER_TYPE", "1", 0);
            setenv("METAL_ERROR_MODE", "3", 0);   /* log validation errors instead of aborting */
        }

        /* Create Metal device */
        vio_mtl.device = MTLCreateSystemDefaultDevice();
        if (!vio_mtl.device) {
            php_error_docref(NULL, E_WARNING, "Metal: no GPU device found");
            return -1;
        }
        vio_mtl.unified_memory = vio_mtl.device.hasUnifiedMemory ? 1 : 0;
        snprintf(vio_mtl.gpu_name, sizeof(vio_mtl.gpu_name), "%s", [vio_mtl.device.name UTF8String]);
        vio_mtl.samples = metal_clamp_sample_count(cfg->samples);

        /* Version ladder: the highest MSL the OS compiles, or the pinned rung;
         * every shader of the context is built for it. */
        {
            int max = metal_msl_max(vio_mtl.device);
            metal_detect_caps(metal_select_msl_version(metal_requested_msl(cfg), max), max);
        }

        /* Create command queue */
        vio_mtl.command_queue = [vio_mtl.device newCommandQueue];
        if (!vio_mtl.command_queue) {
            php_error_docref(NULL, E_WARNING, "Metal: failed to create command queue");
            return -1;
        }

        /* The layer is owned by the caller (NSView contentView on macOS,
         * UIView on iOS). We configure it for our pixel format / sync mode
         * and keep an ARC-strong reference for the context lifetime. */
        vio_mtl.metal_layer = (__bridge CAMetalLayer *)cf_metal_layer;
        vio_mtl.metal_layer.device = vio_mtl.device;
        metal_configure_layer_format(cfg);
        vio_mtl.metal_layer.framebufferOnly = NO; /* Need readable for screenshots */
        vio_mtl.metal_layer.opaque = YES;
        vio_mtl.vsync = cfg->vsync;
#if TARGET_OS_OSX
        /* displaySyncEnabled is a macOS-only CAMetalLayer property; on iOS
         * vsync is governed by CADisplayLink in the host view. */
        vio_mtl.metal_layer.displaySyncEnabled = cfg->vsync ? YES : NO;
#endif
        /* Use 3 drawables to avoid nextDrawable returning nil when PHP's GC
           causes occasional frame time spikes. Default of 2 is too tight. */
        vio_mtl.metal_layer.maximumDrawableCount = 3;

        /* frame_latency => n: cap the frames in flight (CPU run-ahead). The
         * per-frame ring has 3 slots, so 3 is also the natural maximum. */
        vio_mtl.frame_latency = cfg->frame_latency > 0 ? (cfg->frame_latency > 3 ? 3 : cfg->frame_latency) : 0;
        vio_mtl.frame_semaphore = vio_mtl.frame_latency > 0 ? dispatch_semaphore_create(vio_mtl.frame_latency) : nil;
        vio_mtl.frame_semaphore_held = 0;
        vio_mtl.metal_layer.drawableSize = CGSizeMake(width, height);

        vio_mtl.width  = width;
        vio_mtl.height = height;

        /* Create depth texture (+ the MSAA pair when 'samples' > 1) */
        create_depth_texture(width, height);
        metal_create_swapchain_msaa(width, height);

        /* Create offscreen render target for vsync-off mode */
        if (!cfg->vsync) {
            MTLTextureDescriptor *offDesc = [MTLTextureDescriptor
                texture2DDescriptorWithPixelFormat:vio_mtl.swap_format
                width:width height:height mipmapped:NO];
            offDesc.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
            offDesc.storageMode = MTLStorageModePrivate;
            vio_mtl.offscreen_texture = [vio_mtl.device newTextureWithDescriptor:offDesc];
        }

        /* Render pass descriptor template */
        vio_mtl.render_pass_desc = [MTLRenderPassDescriptor renderPassDescriptor];
        vio_mtl.render_pass_desc.depthAttachment.texture = vio_mtl.depth_texture;
        vio_mtl.render_pass_desc.depthAttachment.loadAction = MTLLoadActionClear;
        vio_mtl.render_pass_desc.depthAttachment.storeAction = MTLStoreActionDontCare;
        vio_mtl.render_pass_desc.depthAttachment.clearDepth = 1.0;

        vio_mtl.clear_r = 0.1f;
        vio_mtl.clear_g = 0.1f;
        vio_mtl.clear_b = 0.1f;
        vio_mtl.clear_a = 1.0f;

        metal_marks_reset();   /* a new context starts without named GPU sections */
        vio_mtl.initialized = 1;
    }

    return 0;
}

void vio_metal_handle_resize(int width, int height)
{
    if (!vio_mtl.initialized) return;
    if (width == vio_mtl.width && height == vio_mtl.height) return;
    metal_resize(width, height);
}

#ifdef HAVE_GLFW
int vio_metal_setup_context(void *glfw_window, vio_config *cfg)
{
    @autoreleasepool {
        GLFWwindow *win = (GLFWwindow *)glfw_window;
        if (!win) {
            php_error_docref(NULL, E_WARNING, "Metal: setup_context called with NULL GLFW window");
            return -1;
        }

        /* Get NSWindow from GLFW */
        NSWindow *ns_window = (NSWindow *)glfwGetCocoaWindow(win);
        if (!ns_window) {
            php_error_docref(NULL, E_WARNING, "Metal: failed to get Cocoa window");
            return -1;
        }

        /* Create a CAMetalLayer and attach to the content view. The native
         * setup function below configures it (device, pixel format, ...). */
        CAMetalLayer *layer = [CAMetalLayer layer];
        NSView *content_view = [ns_window contentView];
        [content_view setWantsLayer:YES];
        [content_view setLayer:layer];
        [content_view setLayerContentsRedrawPolicy:NSViewLayerContentsRedrawNever];

#if TARGET_OS_OSX
        /* hdr_output => 1 only switches to HDR10 on a display that can show it. */
        metal_display_edr = 0;
        if (@available(macOS 10.15, *)) {
            NSScreen *screen = ns_window.screen ? ns_window.screen : [NSScreen mainScreen];
            metal_display_edr = screen && screen.maximumPotentialExtendedDynamicRangeColorComponentValue > 1.0;
        }
#endif

        int fb_w, fb_h;
        if (cfg->headless) {
            /* Headless renders into an offscreen texture that vio_read_pixels
             * returns at the logical config size. On a Retina display the
             * window's framebuffer is 2x (e.g. 2560x1440 for a 1280x720
             * request), which would size the offscreen texture at 2x and make
             * readback return only the top-left (logical-sized) quadrant. There
             * is no display to match offscreen, so size it 1:1 with the request
             * using the logical window size. */
            glfwGetWindowSize(win, &fb_w, &fb_h);
        } else {
            glfwGetFramebufferSize(win, &fb_w, &fb_h);
        }

        if (vio_metal_setup_context_native((__bridge void *)layer, fb_w, fb_h, cfg) != 0) {
            return -1;
        }

        /* Remember the GLFW window for pull-based resize polling in
         * metal_begin_frame. iOS / headless setups skip this and push
         * resizes through vio_metal_handle_resize() instead.
         *
         * Headless renders to a fixed-size offscreen texture (sized 1:1 with the
         * logical request above). Polling the window each frame would read the
         * Retina framebuffer size and resize the offscreen back to 2x, so leave
         * glfw_window NULL for headless — matching the "headless leaves this
         * NULL" contract documented on the struct field. */
        vio_mtl.glfw_window = cfg->headless ? NULL : win;
    }

    return 0;
}
#endif /* HAVE_GLFW */

void vio_metal_shutdown_context(void)
{
    @autoreleasepool {
        if (!vio_mtl.initialized) return;
        metal_bindless_release();

        /* Shutdown 2D pipeline */
        if (mtl_2d.initialized) {
            mtl_2d.pipeline_shapes  = nil;
            mtl_2d.pipeline_sprites = nil;
            for (int i = 0; i < mtl_2d.variant_count; i++) {
                mtl_2d.variants[i].shapes  = nil;
                mtl_2d.variants[i].sprites = nil;
            }
            mtl_2d.variant_count    = 0;
            mtl_2d.vertex_fn        = nil;
            mtl_2d.frag_shapes_fn   = nil;
            mtl_2d.frag_sprites_fn  = nil;
            mtl_2d.vertex_desc      = nil;
            mtl_2d.sampler          = nil;
            mtl_2d.vertex_buffer    = nil;
            mtl_2d.initialized      = 0;
        }

        /* 3D path: transient rings, identity instance buffer, bound-state stash */
        metal_3d_shutdown();

        /* Release all registered textures */
        for (unsigned int i = 1; i < VIO_METAL_MAX_TEXTURES; i++) {
            metal_textures[i] = nil;
        }
        metal_next_texture_id = 1;

        /* Wait for GPU to finish */
        if (vio_mtl.command_queue) {
            id<MTLCommandBuffer> cmd = metal_new_command_buffer();
            [cmd commit];
            [cmd waitUntilCompleted];
        }

        /* libdispatch aborts when a semaphore is released below its initial
         * value: hand back a slot taken by an unpresented frame, then wait out
         * every in-flight frame's completion handler before dropping it. */
        if (vio_mtl.frame_semaphore) {
            if (vio_mtl.frame_semaphore_held) dispatch_semaphore_signal(vio_mtl.frame_semaphore);
            for (int i = 0; i < vio_mtl.frame_latency; i++) dispatch_semaphore_wait(vio_mtl.frame_semaphore, DISPATCH_TIME_FOREVER);
            for (int i = 0; i < vio_mtl.frame_latency; i++) dispatch_semaphore_signal(vio_mtl.frame_semaphore);
            vio_mtl.frame_semaphore = nil;
            vio_mtl.frame_semaphore_held = 0;
            vio_mtl.frame_latency = 0;
        }
        vio_mtl.offscreen_texture = nil;

        vio_mtl.depth_texture    = nil;
        vio_mtl.msaa_color       = nil;
        vio_mtl.msaa_depth       = nil;
        vio_mtl.render_pass_desc = nil;
        vio_mtl.command_queue    = nil;
        vio_mtl.metal_layer      = nil;
        vio_mtl.device           = nil;

        vio_mtl.initialized = 0;
    }
}

/* ── Metal 2D pipeline initialization ────────────────────────────── */

/* Find or build the (shapes, sprites) PSO pair for a target format + sample
 * count. Returns NULL (after a warning) when Metal rejects the descriptor or
 * the small cache is full. */
static vio_metal_2d_variant *metal_2d_variant(const vio_metal_target_desc *t)
{
    MTLPixelFormat fmt = t->fmts[0];
    int samples = t->samples < 1 ? 1 : t->samples;
    int has_depth = t->has_depth;
    int has_stencil = t->has_stencil;
    int count = t->count < 1 ? 1 : t->count;
    for (int i = 0; i < mtl_2d.variant_count; i++) {
        vio_metal_2d_variant *v = &mtl_2d.variants[i];
        if (v->pixel_format != (int)fmt || v->samples != samples || v->has_depth != has_depth ||
            v->has_stencil != has_stencil || v->color_count != count) continue;
        int same = 1;
        for (int k = 1; k < count; k++) if (v->extra_fmts[k - 1] != (int)t->fmts[k]) { same = 0; break; }
        if (same) return v;
    }
    if (mtl_2d.variant_count >= VIO_METAL_2D_VARIANTS || !mtl_2d.vertex_fn) {
        php_error_docref(NULL, E_WARNING, "Metal 2D: no pipeline variant for format %d x%d samples",
                         (int)fmt, samples);
        return NULL;
    }

    @autoreleasepool {
        NSError *error = nil;
        MTLRenderPipelineDescriptor *pipeDesc = [[MTLRenderPipelineDescriptor alloc] init];
        pipeDesc.vertexFunction = mtl_2d.vertex_fn;
        pipeDesc.fragmentFunction = mtl_2d.frag_shapes_fn;
        pipeDesc.vertexDescriptor = mtl_2d.vertex_desc;
        pipeDesc.rasterSampleCount = (NSUInteger)samples;
        pipeDesc.colorAttachments[0].pixelFormat = fmt;
        pipeDesc.colorAttachments[0].blendingEnabled = YES;
        pipeDesc.colorAttachments[0].sourceRGBBlendFactor = MTLBlendFactorSourceAlpha;
        pipeDesc.colorAttachments[0].destinationRGBBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
        pipeDesc.colorAttachments[0].sourceAlphaBlendFactor = MTLBlendFactorSourceAlpha;
        pipeDesc.colorAttachments[0].destinationAlphaBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
        /* MRT target: Metal requires every pass attachment to appear in the PSO
         * with a matching format; the 2D shaders only write output 0, so the
         * others are declared with an empty write mask. */
        for (int k = 1; k < count; k++) {
            pipeDesc.colorAttachments[k].pixelFormat = t->fmts[k];
            pipeDesc.colorAttachments[k].writeMask = MTLColorWriteMaskNone;
        }
        pipeDesc.depthAttachmentPixelFormat = has_depth ? (has_stencil ? VIO_METAL_DEPTH_STENCIL : MTLPixelFormatDepth32Float)
                                                        : MTLPixelFormatInvalid;
        pipeDesc.stencilAttachmentPixelFormat = (has_depth && has_stencil) ? VIO_METAL_DEPTH_STENCIL : MTLPixelFormatInvalid;

        id<MTLRenderPipelineState> shapes =
            [vio_mtl.device newRenderPipelineStateWithDescriptor:pipeDesc error:&error];
        if (!shapes) {
            php_error_docref(NULL, E_WARNING, "Metal 2D: shapes pipeline failed: %s",
                [[error localizedDescription] UTF8String]);
            return NULL;
        }
        pipeDesc.fragmentFunction = mtl_2d.frag_sprites_fn;
        id<MTLRenderPipelineState> sprites =
            [vio_mtl.device newRenderPipelineStateWithDescriptor:pipeDesc error:&error];
        if (!sprites) {
            php_error_docref(NULL, E_WARNING, "Metal 2D: sprites pipeline failed: %s",
                [[error localizedDescription] UTF8String]);
            return NULL;
        }

        vio_metal_2d_variant *v = &mtl_2d.variants[mtl_2d.variant_count++];
        v->pixel_format = (int)fmt;
        v->color_count  = count;
        for (int k = 1; k < count; k++) v->extra_fmts[k - 1] = (int)t->fmts[k];
        v->samples      = samples;
        v->has_depth    = has_depth;
        v->has_stencil  = has_stencil;
        v->shapes       = shapes;
        v->sprites      = sprites;
        return v;
    }
}

int vio_metal_2d_init(int width, int height)
{
    @autoreleasepool {
        if (!vio_mtl.initialized || !vio_mtl.device) return -1;

        /* Compile MSL shader library */
        NSError *error = nil;
        NSString *source = [NSString stringWithUTF8String:vio_2d_metal_shader_source];
        id<MTLLibrary> library = [vio_mtl.device newLibraryWithSource:source
                                                              options:metal_compile_options()
                                                                error:&error];
        if (!library) {
            php_error_docref(NULL, E_WARNING, "Metal 2D: shader compilation failed: %s",
                [[error localizedDescription] UTF8String]);
            return -1;
        }

        id<MTLFunction> vertexFunc   = [library newFunctionWithName:@"vio_2d_vertex_main"];
        id<MTLFunction> fragShapes   = [library newFunctionWithName:@"vio_2d_fragment_shapes"];
        id<MTLFunction> fragSprites  = [library newFunctionWithName:@"vio_2d_fragment_sprites"];

        if (!vertexFunc || !fragShapes || !fragSprites) {
            php_error_docref(NULL, E_WARNING, "Metal 2D: failed to find shader functions");
            return -1;
        }

        /* Vertex descriptor matching vio_2d_vertex: {x,y, u,v, r,g,b,a} = 32 bytes */
        MTLVertexDescriptor *vertDesc = [[MTLVertexDescriptor alloc] init];
        /* attribute 0: position (float2) at offset 0 */
        vertDesc.attributes[0].format = MTLVertexFormatFloat2;
        vertDesc.attributes[0].offset = 0;
        vertDesc.attributes[0].bufferIndex = 0;
        /* attribute 1: texcoord (float2) at offset 8 */
        vertDesc.attributes[1].format = MTLVertexFormatFloat2;
        vertDesc.attributes[1].offset = 8;
        vertDesc.attributes[1].bufferIndex = 0;
        /* attribute 2: color (float4) at offset 16 */
        vertDesc.attributes[2].format = MTLVertexFormatFloat4;
        vertDesc.attributes[2].offset = 16;
        vertDesc.attributes[2].bufferIndex = 0;
        /* layout 0: stride 32, per-vertex */
        vertDesc.layouts[0].stride = 32;
        vertDesc.layouts[0].stepRate = 1;
        vertDesc.layouts[0].stepFunction = MTLVertexStepFunctionPerVertex;

        mtl_2d.vertex_fn       = vertexFunc;
        mtl_2d.frag_shapes_fn  = fragShapes;
        mtl_2d.frag_sprites_fn = fragSprites;
        mtl_2d.vertex_desc     = vertDesc;
        mtl_2d.variant_count   = 0;

        /* Swapchain variant (BGRA8, single sample) is built eagerly so a
         * failure surfaces at init, not at the first flush. */
        vio_metal_target_desc swap = { { vio_mtl.swap_format, 0, 0, 0 }, 1, 1, 1, 1 };
        vio_metal_2d_variant *v0 = metal_2d_variant(&swap);
        if (!v0) {
            return -1;
        }
        mtl_2d.pipeline_shapes  = v0->shapes;
        mtl_2d.pipeline_sprites = v0->sprites;

        /* Sampler for sprites/text */
        MTLSamplerDescriptor *sampDesc = [[MTLSamplerDescriptor alloc] init];
        sampDesc.minFilter = MTLSamplerMinMagFilterLinear;
        sampDesc.magFilter = MTLSamplerMinMagFilterLinear;
        sampDesc.sAddressMode = MTLSamplerAddressModeClampToEdge;
        sampDesc.tAddressMode = MTLSamplerAddressModeClampToEdge;
        mtl_2d.sampler = [vio_mtl.device newSamplerStateWithDescriptor:sampDesc];

        /* Depth-disabled state for 2D rendering */
        MTLDepthStencilDescriptor *dsDesc = [[MTLDepthStencilDescriptor alloc] init];
        dsDesc.depthCompareFunction = MTLCompareFunctionAlways;
        dsDesc.depthWriteEnabled = NO;
        mtl_2d.depth_disabled = [vio_mtl.device newDepthStencilStateWithDescriptor:dsDesc];

        mtl_2d.vb_capacity = 0;
        mtl_2d.vertex_buffer = nil;
        mtl_2d.initialized = 1;
    }

    return 0;
}

int vio_metal_2d_is_active(void)
{
    return vio_mtl.initialized && mtl_2d.initialized;
}

void vio_metal_2d_set_size(int width, int height, int fb_width, int fb_height)
{
    (void)width; (void)height; (void)fb_width; (void)fb_height;
    /* Projection is set via state->projection in flush — nothing to do here */
}

/* ── Metal 2D flush ──────────────────────────────────────────────── */

void vio_metal_2d_flush(vio_2d_state *state)
{
    @autoreleasepool {
        if (!vio_mtl.current_encoder || !mtl_2d.initialized) return;
        if (state->item_count == 0) return;

        /* Upload vertex data — grow Metal buffer if needed */
        int needed = state->vertex_count;
        if (needed > mtl_2d.vb_capacity || !mtl_2d.vertex_buffer) {
            int new_cap = (needed > mtl_2d.vb_capacity * 2) ? needed : mtl_2d.vb_capacity * 2;
            if (new_cap < 4096) new_cap = 4096;
            mtl_2d.vertex_buffer = [vio_mtl.device
                newBufferWithLength:sizeof(vio_2d_vertex) * new_cap
                options:MTLResourceStorageModeShared];
            mtl_2d.vb_capacity = new_cap;
        }
        memcpy([mtl_2d.vertex_buffer contents], state->vertices,
               sizeof(vio_2d_vertex) * state->vertex_count);

        /* Projection + output control: PQ-encode only what lands on an HDR10
         * swapchain - a render target keeps display-referred values, so 2D drawn
         * into it and composited onto the swapchain later is encoded once. */
        float ub[20];
        memcpy(ub, state->projection, sizeof(float) * 16);
        ub[16] = (vio_mtl.hdr_output && !current_bound_rt) ? 1.0f : 0.0f;
        ub[17] = vio_mtl.hdr_paper_white > 0.0f ? vio_mtl.hdr_paper_white : 200.0f;
        ub[18] = 0.0f; ub[19] = 0.0f;

        /* Disable depth for 2D, and undo any 3D encoder state (a preceding
         * vio_draw may have left back-face culling / depth bias set — the 2D
         * quads are emitted in screen space with no fixed winding). */
        [vio_mtl.current_encoder setDepthStencilState:mtl_2d.depth_disabled];
        [vio_mtl.current_encoder setCullMode:MTLCullModeNone];
        [vio_mtl.current_encoder setDepthBias:0.0f slopeScale:0.0f clamp:0.0f];

        /* Bind vertex buffer and projection */
        [vio_mtl.current_encoder setVertexBuffer:mtl_2d.vertex_buffer offset:0 atIndex:0];
        [vio_mtl.current_encoder setVertexBytes:ub length:sizeof(ub) atIndex:1];
        [vio_mtl.current_encoder setFragmentBytes:ub length:sizeof(ub) atIndex:1];

        /* PSO pair matching the bound target (swapchain / HDR RT / MSAA RT). */
        vio_metal_target_desc target;
        metal_current_target(&target);
        if (target.count == 0) return;  /* depth-only RT: 2D has nothing to write */
        vio_metal_2d_variant *variant = metal_2d_variant(&target);
        if (!variant) return;

        /* Track current state to minimize redundant calls */
        id<MTLRenderPipelineState> current_pipeline = nil;
        unsigned int current_texture = 0;
        int scissor_active = 0;
        float sc_x = 0, sc_y = 0, sc_w = 0, sc_h = 0;

        /* Framebuffer scale for scissor (logical → pixel coords) */
        float sx = (state->width > 0)  ? (float)state->fb_width  / (float)state->width  : 1.0f;
        float sy = (state->height > 0) ? (float)state->fb_height / (float)state->height : 1.0f;

        for (int i = 0; i < state->item_count; i++) {
            vio_2d_item *item = &state->items[i];

            /* Update scissor state */
            if (item->scissor.enabled) {
                if (!scissor_active ||
                    item->scissor.x != sc_x || item->scissor.y != sc_y ||
                    item->scissor.w != sc_w || item->scissor.h != sc_h) {
                    sc_x = item->scissor.x; sc_y = item->scissor.y;
                    sc_w = item->scissor.w; sc_h = item->scissor.h;

                    NSUInteger px = (NSUInteger)(sc_x * sx);
                    NSUInteger py = (NSUInteger)(sc_y * sy);
                    NSUInteger pw = (NSUInteger)(sc_w * sx);
                    NSUInteger ph = (NSUInteger)(sc_h * sy);
                    /* Clamp to framebuffer bounds */
                    if (px + pw > (NSUInteger)vio_mtl.width) pw = (NSUInteger)vio_mtl.width - px;
                    if (py + ph > (NSUInteger)vio_mtl.height) ph = (NSUInteger)vio_mtl.height - py;

                    MTLScissorRect rect = {px, py, pw, ph};
                    [vio_mtl.current_encoder setScissorRect:rect];
                    scissor_active = 1;
                }
            } else if (scissor_active) {
                MTLScissorRect full = {0, 0, (NSUInteger)vio_mtl.width, (NSUInteger)vio_mtl.height};
                [vio_mtl.current_encoder setScissorRect:full];
                scissor_active = 0;
            }

            /* Select pipeline */
            id<MTLRenderPipelineState> wanted =
                (item->texture_id > 0) ? variant->sprites : variant->shapes;
            if (wanted != current_pipeline) {
                current_pipeline = wanted;
                [vio_mtl.current_encoder setRenderPipelineState:current_pipeline];
            }

            /* Bind texture if needed */
            if (item->texture_id != current_texture) {
                current_texture = item->texture_id;
                if (current_texture > 0 && current_texture < VIO_METAL_MAX_TEXTURES) {
                    id<MTLTexture> tex = metal_textures[current_texture];
                    if (tex) {
                        [vio_mtl.current_encoder setFragmentTexture:tex atIndex:0];
                        [vio_mtl.current_encoder setFragmentSamplerState:mtl_2d.sampler atIndex:0];
                    }
                }
            }

            /* Draw triangles */
            [vio_mtl.current_encoder drawPrimitives:MTLPrimitiveTypeTriangle
                                        vertexStart:item->vertex_start
                                        vertexCount:item->vertex_count];
        }

        /* Restore full scissor */
        if (scissor_active) {
            MTLScissorRect full = {0, 0, (NSUInteger)vio_mtl.width, (NSUInteger)vio_mtl.height};
            [vio_mtl.current_encoder setScissorRect:full];
        }
    }
}

/* ── Metal texture management ────────────────────────────────────── */

unsigned int vio_metal_create_texture_rgba(int width, int height,
    const unsigned char *pixels, int filter_linear, int wrap_clamp)
{
    @autoreleasepool {
        if (!vio_mtl.initialized || !vio_mtl.device) return 0;

        MTLTextureDescriptor *desc = [MTLTextureDescriptor
            texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
            width:width height:height mipmapped:NO];
        desc.usage = MTLTextureUsageShaderRead;
        desc.storageMode = metal_cpu_texture_storage();

        id<MTLTexture> tex = [vio_mtl.device newTextureWithDescriptor:desc];
        if (!tex) return 0;

        MTLRegion region = MTLRegionMake2D(0, 0, width, height);
        [tex replaceRegion:region mipmapLevel:0
               withBytes:pixels bytesPerRow:width * 4];

        return metal_register_texture(tex);
    }
}

unsigned int vio_metal_create_texture_3d_rgba(int width, int height, int depth,
    const unsigned char *pixels, int filter_linear, int wrap_clamp)
{
    (void)filter_linear; (void)wrap_clamp;  /* sampler state is separate in Metal */
    @autoreleasepool {
        if (!vio_mtl.initialized || !vio_mtl.device || width <= 0 || height <= 0 || depth <= 0) {
            return 0;
        }

        MTLTextureDescriptor *desc = [[MTLTextureDescriptor alloc] init];
        desc.textureType = MTLTextureType3D;
        desc.pixelFormat = MTLPixelFormatRGBA8Unorm;
        desc.width  = width;
        desc.height = height;
        desc.depth  = depth;
        desc.mipmapLevelCount = 1;
        desc.usage = MTLTextureUsageShaderRead;
        desc.storageMode = metal_cpu_texture_storage();

        id<MTLTexture> tex = [vio_mtl.device newTextureWithDescriptor:desc];
        if (!tex) return 0;

        MTLRegion region = MTLRegionMake3D(0, 0, 0, width, height, depth);
        [tex replaceRegion:region mipmapLevel:0 slice:0
               withBytes:pixels
             bytesPerRow:width * 4
           bytesPerImage:width * height * 4];

        return metal_register_texture(tex);
    }
}

unsigned int vio_metal_create_font_atlas(int width, int height,
    const unsigned char *bitmap)
{
    @autoreleasepool {
        if (!vio_mtl.initialized || !vio_mtl.device) return 0;

        /* Single-channel atlas with swizzle: sample as (1,1,1,R) for text blending */
        MTLTextureDescriptor *desc = [MTLTextureDescriptor
            texture2DDescriptorWithPixelFormat:MTLPixelFormatR8Unorm
            width:width height:height mipmapped:NO];
        desc.usage = MTLTextureUsageShaderRead;
        desc.storageMode = metal_cpu_texture_storage();
        desc.swizzle = MTLTextureSwizzleChannelsMake(
            MTLTextureSwizzleOne,   /* R -> 1.0 */
            MTLTextureSwizzleOne,   /* G -> 1.0 */
            MTLTextureSwizzleOne,   /* B -> 1.0 */
            MTLTextureSwizzleRed    /* A -> red channel (alpha) */
        );

        id<MTLTexture> tex = [vio_mtl.device newTextureWithDescriptor:desc];
        if (!tex) return 0;

        MTLRegion region = MTLRegionMake2D(0, 0, width, height);
        [tex replaceRegion:region mipmapLevel:0
               withBytes:bitmap bytesPerRow:width];

        return metal_register_texture(tex);
    }
}

void vio_metal_delete_texture(unsigned int texture_id)
{
    if (texture_id > 0 && texture_id < VIO_METAL_MAX_TEXTURES) {
        metal_textures[texture_id] = nil;
    }
}

unsigned int vio_metal_register_external_texture(void *cf_retained_texture)
{
    if (!cf_retained_texture) return 0;
    if (!vio_mtl.initialized) return 0;
    id<MTLTexture> tex = (__bridge id<MTLTexture>)cf_retained_texture;
    return metal_register_texture(tex);
}

/* ── Metal pixel readback ────────────────────────────────────────── */

int vio_metal_read_pixels(int width, int height, unsigned char *out_rgba)
{
    @autoreleasepool {
        if (!vio_mtl.initialized) return -1;

        id<MTLTexture> srcTexture = nil;

        if (vio_mtl.current_cmd_buf) {
            /* Mid-frame readback (vio_read_pixels between vio_begin and vio_end,
             * the OpenGL / D3D contract): flush what has been recorded so far —
             * close the encoder, commit, wait — read the current colour target,
             * then continue the frame on a fresh command buffer whose encoder
             * Loads the existing contents. The drawable (if any) is presented
             * by that later command buffer, which Metal permits. */
            srcTexture = metal_current_color_texture();
            if (vio_mtl.current_encoder) {
                [vio_mtl.current_encoder endEncoding];
                vio_mtl.current_encoder = nil;
            }
            [vio_mtl.current_cmd_buf commit];
            [vio_mtl.current_cmd_buf waitUntilCompleted];
            vio_mtl.current_cmd_buf = metal_new_command_buffer();
            metal_open_encoder(/*load_clear=*/0);
        } else {
            /* Between frames: the last presented / committed frame. */
            if (last_presented_cmd_buf) {
                [last_presented_cmd_buf waitUntilCompleted];
            }
            srcTexture = last_presented_texture;
        }
        if (!srcTexture) return -1;

        int tw = (int)srcTexture.width;
        int th = (int)srcTexture.height;
        int use_w = (width < tw) ? width : tw;
        int use_h = (height < th) ? height : th;
        if (use_w <= 0 || use_h <= 0) return -1;

        /* getBytes is invalid on MTLStorageModePrivate textures (the swapchain
         * drawable and the headless offscreen texture are both Private), which
         * returned garbage/uniform data and crashed the offscreen path. Blit the
         * source into a Shared buffer first — the portable readback path across
         * Apple Silicon and Intel — then copy out. */
        NSUInteger bytesPerRow = (NSUInteger)use_w * 4;
        NSUInteger bufLen = bytesPerRow * (NSUInteger)use_h;
        id<MTLBuffer> staging = [vio_mtl.device newBufferWithLength:bufLen
                                                            options:MTLResourceStorageModeShared];
        if (!staging) return -1;

        id<MTLCommandBuffer> cb = metal_new_command_buffer();
        id<MTLBlitCommandEncoder> blit = [cb blitCommandEncoder];
        [blit copyFromTexture:srcTexture
                  sourceSlice:0
                  sourceLevel:0
                 sourceOrigin:MTLOriginMake(0, 0, 0)
                   sourceSize:MTLSizeMake(use_w, use_h, 1)
                     toBuffer:staging
            destinationOffset:0
       destinationBytesPerRow:bytesPerRow
     destinationBytesPerImage:bufLen];
        [blit endEncoding];
        [cb commit];
        [cb waitUntilCompleted];

        const unsigned char *bgra = (const unsigned char *)[staging contents];
        if (!bgra) return -1;

        /* Convert BGRA -> RGBA. The caller's buffer is width x height; when the
         * texture is smaller (a drawable that has not caught up with a resize)
         * the rows must still land at the caller's stride, or every row after
         * the first is shifted and the image tears diagonally. */
        if (srcTexture.pixelFormat == MTLPixelFormatRGB10A2Unorm) {
            /* HDR10 swapchain: R bits 0-9, G 10-19, B 20-29, A 30-31; the top 8
             * bits of each channel, as the D3D / Vulkan 10-bit readbacks do. */
            for (int y = 0; y < use_h; y++) {
                const uint32_t *row = (const uint32_t *)(bgra + (size_t)y * bytesPerRow);
                for (int x = 0; x < use_w; x++) {
                    uint32_t v = row[x];
                    size_t dst = ((size_t)y * width + x) * 4;
                    out_rgba[dst + 0] = (unsigned char)((v >> 2) & 0xFF);
                    out_rgba[dst + 1] = (unsigned char)((v >> 12) & 0xFF);
                    out_rgba[dst + 2] = (unsigned char)((v >> 22) & 0xFF);
                    out_rgba[dst + 3] = (unsigned char)(((v >> 30) & 0x3) * 85);
                }
            }
            return 0;
        }
        for (int y = 0; y < use_h; y++) {
            for (int x = 0; x < use_w; x++) {
                size_t src = ((size_t)y * use_w + x) * 4;
                size_t dst = ((size_t)y * width + x) * 4;
                out_rgba[dst + 0] = bgra[src + 2]; /* R */
                out_rgba[dst + 1] = bgra[src + 1]; /* G */
                out_rgba[dst + 2] = bgra[src + 0]; /* B */
                out_rgba[dst + 3] = bgra[src + 3]; /* A */
            }
        }

        return 0;
    }
}

void vio_metal_gpu_info(const char **name, uint64_t *vram_bytes)
{
    if (!vio_mtl.initialized || !vio_mtl.device) return;
    if (name) *name = vio_mtl.gpu_name;
    if (vram_bytes) {
        /* Unified-memory GPUs have no dedicated VRAM; recommendedMaxWorkingSetSize
         * is Apple's figure for how much this GPU can keep resident. */
        *vram_bytes = (uint64_t)vio_mtl.device.recommendedMaxWorkingSetSize;
    }
}

/* Vtable read_pixels: fbo is the OpenGL FBO handle and is ignored here. */
static int metal_read_pixels_slot(unsigned int fbo, int width, int height, void *out_rgba)
{
    (void)fbo;
    return vio_metal_read_pixels(width, height, (unsigned char *)out_rgba);
}

int vio_metal_save_screenshot(const char *path, int width, int height)
{
    unsigned char *rgba = ecalloc((size_t)width * height * 4, 1);
    if (vio_metal_read_pixels(width, height, rgba) != 0) {
        efree(rgba);
        return -1;
    }

    int result = stbi_write_png(path, width, height, 4, rgba, width * 4);
    efree(rgba);
    return result ? 0 : -1;
}

/* ── Vtable implementations ──────────────────────────────────────── */

static int metal_init(vio_config *cfg)
{
    (void)cfg;
    return 0;
}

static void metal_shutdown(void)
{
    vio_metal_shutdown_context();
}

static void *metal_create_surface(vio_config *cfg)
{
    (void)cfg;
    return NULL;
}

static void metal_destroy_surface(void *surface)
{
    (void)surface;
}

static void metal_resize(int width, int height)
{
    @autoreleasepool {
        if (!vio_mtl.initialized) return;

        vio_mtl.width  = width;
        vio_mtl.height = height;
        vio_mtl.metal_layer.drawableSize = CGSizeMake(width, height);
        create_depth_texture(width, height);
        metal_create_swapchain_msaa(width, height);
        vio_mtl.render_pass_desc.depthAttachment.texture = vio_mtl.depth_texture;

        /* Recreate offscreen texture for vsync-off mode */
        if (!vio_mtl.vsync) {
            MTLTextureDescriptor *offDesc = [MTLTextureDescriptor
                texture2DDescriptorWithPixelFormat:vio_mtl.swap_format
                width:width height:height mipmapped:NO];
            offDesc.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
            offDesc.storageMode = MTLStorageModePrivate;
            vio_mtl.offscreen_texture = [vio_mtl.device newTextureWithDescriptor:offDesc];
        }
    }
}

/* The colour texture the open frame renders into: bound RT > vsync-off
 * offscreen > swapchain drawable. nil for a depth-only RT / no drawable. */
static id<MTLTexture> metal_current_color_texture(void)
{
    if (current_bound_rt) {
        if (current_bound_rt->depth_only) return nil;
        return (__bridge id<MTLTexture>)current_bound_rt->metal_color_texture;
    }
    if (!vio_mtl.vsync && vio_mtl.offscreen_texture) return vio_mtl.offscreen_texture;
    if (vio_mtl.current_drawable) return vio_mtl.current_drawable.texture;
    return nil;
}

/* Open a render-pass encoder against either the currently bound offscreen RT
 * (current_bound_rt) or the swapchain drawable. Assumes cmd buffer exists.
 * load_clear=1 uses Clear (and clear_r/g/b/a); 0 uses Load (preserves existing
 * contents). Sets viewport + scissor to the target's dimensions. */
static void metal_open_encoder(int load_clear)
{
    @autoreleasepool {
        if (!vio_mtl.current_cmd_buf) return;

        id<MTLTexture> depth_target = vio_mtl.depth_texture; /* default swapchain depth */
        int target_w = vio_mtl.width;
        int target_h = vio_mtl.height;

        /* Colour attachments (one for the swapchain / classic RT, up to
         * VIO_MAX_COLOR_ATTACHMENTS for an MRT target) plus their MSAA resolve
         * partners. */
        id<MTLTexture> color_targets[VIO_MAX_COLOR_ATTACHMENTS]  = {nil, nil, nil, nil};
        id<MTLTexture> resolve_targets[VIO_MAX_COLOR_ATTACHMENTS] = {nil, nil, nil, nil};
        int n_color = 0;
        NSUInteger cube_slice = 0, cube_level = 0, all_layers = 0;
        /* A layered target multisamples at level 0 only (A24). */
        int rt_ms = current_bound_rt && current_bound_rt->samples > 1 &&
                    !((current_bound_rt->is_cube || current_bound_rt->layers > 1) && current_bound_level > 0);
        if (current_bound_rt) {
            depth_target = (__bridge id<MTLTexture>)current_bound_rt->metal_depth_texture;
            target_w = current_bound_rt->width;
            target_h = current_bound_rt->height;
            if (!current_bound_rt->depth_only) {
                n_color = metal_rt_attachment_count(current_bound_rt);
                for (int i = 0; i < n_color; i++) {
                    id<MTLTexture> c = i == 0 ? metal_current_color_texture()
                                              : (__bridge id<MTLTexture>)current_bound_rt->metal_color_textures[i];
                    if (rt_ms) {
                        /* MSAA: render into the multisample pair, resolve into the
                         * single-sample colour texture at every pass end. */
                        resolve_targets[i] = c;
                        color_targets[i] = (__bridge id<MTLTexture>)current_bound_rt->metal_msaa_color_textures[i];
                    } else {
                        color_targets[i] = c;
                    }
                }
            }
            if (current_bound_rt->is_cube || current_bound_rt->layers > 1) {
                /* Cube face / array layer, or every layer (VIO_RT_ALL_LAYERS: the
                 * vertex stage picks it with [[render_target_array_index]]). */
                if (current_bound_face == VIO_RT_ALL_LAYERS) all_layers = (NSUInteger)vio_rt_layer_count(current_bound_rt);
                cube_slice = (NSUInteger)(current_bound_face >= 0 ? current_bound_face : 0);
                cube_level = (NSUInteger)current_bound_level;
                target_w >>= cube_level; if (target_w < 1) target_w = 1;
                target_h >>= cube_level; if (target_h < 1) target_h = 1;
                /* The shared depth texture only matches level 0. */
                if (cube_level > 0) depth_target = nil;
            }
            if (rt_ms) {
                depth_target = (__bridge id<MTLTexture>)current_bound_rt->metal_msaa_depth_texture;
            }
        } else {
            id<MTLTexture> color_target = metal_current_color_texture();
            if (color_target) {
                n_color = 1;
                if (vio_mtl.samples > 1 && vio_mtl.msaa_color) {
                    /* Swapchain MSAA (vio_create 'samples'): same scheme, resolving into
                     * the drawable / vsync-off offscreen texture. */
                    resolve_targets[0] = color_target;
                    color_targets[0] = vio_mtl.msaa_color;
                    depth_target = vio_mtl.msaa_depth;
                } else {
                    color_targets[0] = color_target;
                }
            }
        }

        MTLRenderPassDescriptor *desc = [MTLRenderPassDescriptor renderPassDescriptor];

        for (int i = 0; i < n_color; i++) {
            if (!color_targets[i]) continue;
            MTLRenderPassColorAttachmentDescriptor *ca = desc.colorAttachments[i];
            ca.texture = color_targets[i];
            ca.slice = cube_slice;
            ca.level = cube_level;
            ca.loadAction = load_clear ? MTLLoadActionClear : MTLLoadActionLoad;
            if (resolve_targets[i]) {
                ca.resolveTexture = resolve_targets[i];
                ca.resolveSlice = cube_slice;
                ca.resolveLevel = cube_level;
                /* Keep the MSAA contents too, so a reopened pass (RT bind /
                 * unbind / eager clear / mid-frame readback) can Load them. */
                ca.storeAction = MTLStoreActionStoreAndMultisampleResolve;
            } else {
                ca.storeAction = MTLStoreActionStore;
            }
            ca.clearColor = MTLClearColorMake(vio_mtl.clear_r, vio_mtl.clear_g, vio_mtl.clear_b, vio_mtl.clear_a);
        }

        if (all_layers > 1) desc.renderTargetArrayLength = all_layers;
        if (depth_target) {
            int depth_layered = depth_target.textureType == MTLTextureTypeCube ||
                                depth_target.textureType == MTLTextureType2DArray ||
                                depth_target.textureType == MTLTextureType2DMultisampleArray;
            desc.depthAttachment.texture = depth_target;
            if (depth_layered) desc.depthAttachment.slice = cube_slice;
            if (depth_target.pixelFormat == VIO_METAL_DEPTH_STENCIL) {
                desc.stencilAttachment.texture = depth_target;
                if (depth_layered) desc.stencilAttachment.slice = cube_slice;
                desc.stencilAttachment.loadAction = load_clear ? MTLLoadActionClear : MTLLoadActionLoad;
                desc.stencilAttachment.storeAction = MTLStoreActionStore;
                desc.stencilAttachment.clearStencil = 0;
            }
            desc.depthAttachment.loadAction = load_clear ? MTLLoadActionClear : MTLLoadActionLoad;
            /* Always Store: a mid-frame RT bind/unbind closes this encoder and
             * reopens one with Load — DontCare would hand that Load undefined
             * depth and 3D draws after the switch would fail the depth test. */
            desc.depthAttachment.storeAction = MTLStoreActionStore;
            desc.depthAttachment.clearDepth = 1.0;
            if (rt_ms && current_bound_rt->depth_only && current_bound_rt->metal_depth_texture) {
                /* depth_only MSAA (A24): resolve into the sampled depth at every pass end. */
                desc.depthAttachment.resolveTexture = (__bridge id<MTLTexture>)current_bound_rt->metal_depth_texture;
                desc.depthAttachment.depthResolveFilter = current_bound_rt->depth_reduction == VIO_DEPTH_REDUCE_MIN
                    ? MTLMultisampleDepthResolveFilterMin : MTLMultisampleDepthResolveFilterMax;
                desc.depthAttachment.storeAction = MTLStoreActionStoreAndMultisampleResolve;
            }
        }

        vio_mtl.current_encoder = [vio_mtl.current_cmd_buf
            renderCommandEncoderWithDescriptor:desc];

        MTLViewport viewport = {0, 0, (double)target_w, (double)target_h, 0.0, 1.0};
        [vio_mtl.current_encoder setViewport:viewport];
        metal_target_w = target_w;
        metal_target_h = target_h;
        metal_vp_count = 0;   /* a new pass starts with the full-target viewport */
        metal_fs_shadow_reset();

        MTLScissorRect scissor = {0, 0, (NSUInteger)target_w, (NSUInteger)target_h};
        [vio_mtl.current_encoder setScissorRect:scissor];
    }
}

static void metal_begin_frame(void)
{
    @autoreleasepool {
        if (!vio_mtl.initialized) return;

#ifdef HAVE_GLFW
        /* GLFW path: poll the window for resize each frame. Native callers
         * (iOS UIView) push resizes through vio_metal_handle_resize() and
         * leave glfw_window NULL, so we skip the poll in that case. */
        if (vio_mtl.glfw_window) {
            int fb_w, fb_h;
            glfwGetFramebufferSize(vio_mtl.glfw_window, &fb_w, &fb_h);
            if (fb_w != vio_mtl.width || fb_h != vio_mtl.height) {
                metal_resize(fb_w, fb_h);
            }
        }
#endif

        /* DO NOT reset current_bound_rt here. The persistent-bind contract
         * mirrored from D3D11/D3D12 (vio_d3d11.current_rtv survives across
         * frames) requires that an explicit vio_bind_render_target stays in
         * effect until vio_unbind_render_target is called - otherwise
         * Engine::warmRender's repeated beginFrame/endFrame loop would
         * silently render to the swapchain on every frame after the first. */

        /* In vsync-off mode, render to offscreen texture to avoid display-rate
           throttling from nextDrawable. This gives accurate GPU-only frame timing
           for benchmarks. In vsync mode, render directly to the drawable.
           Only fetch a drawable when no explicit RT is bound. */
        /* frame_latency: block until one of the n in-flight frames retired. */
        if (vio_mtl.frame_semaphore && !vio_mtl.frame_semaphore_held) {
            dispatch_semaphore_wait(vio_mtl.frame_semaphore, DISPATCH_TIME_FOREVER);
            vio_mtl.frame_semaphore_held = 1;
        }

        if (current_bound_rt) {
            vio_mtl.current_drawable = nil;
        } else if (!vio_mtl.vsync && vio_mtl.offscreen_texture) {
            vio_mtl.current_drawable = nil;
        } else {
            vio_mtl.current_drawable = [vio_mtl.metal_layer nextDrawable];
            if (!vio_mtl.current_drawable) {
                if (vio_mtl.frame_semaphore_held) {
                    dispatch_semaphore_signal(vio_mtl.frame_semaphore);
                    vio_mtl.frame_semaphore_held = 0;
                }
                return;
            }
        }

        vio_mtl.current_cmd_buf = metal_new_command_buffer();
        metal_ring_begin_frame();
        metal_open_encoder(/*load_clear=*/1);
    }
}

static void metal_end_frame(void)
{
    @autoreleasepool {
        if (!vio_mtl.current_encoder) return;

        [vio_mtl.current_encoder endEncoding];
        vio_mtl.current_encoder = nil;
    }
}

/* ── Named GPU timestamps (vio_gpu_timestamp, OPEN-ITEMS-PLAN A19) ──────
 * Metal has no timestamp at an arbitrary point of a command buffer, so a mark
 * ends a section: the frame's command buffer so far is committed (no CPU
 * wait) and the frame continues on a new one, reopened with Load like the
 * mid-frame readback. A section lasts from the previous section's GPUEndTime
 * (the first one from its buffer's GPUStartTime) to its buffer's GPUEndTime.
 * The completion handlers run on Metal threads; whichever finishes last
 * publishes the frame. The upload ring fences on the frame's last buffer
 * (metal_ring_end_frame), so splitting the frame keeps its memory alive. */
typedef struct {
    vio_gpu_mark_names names;
    double             start;
    double             end[VIO_GPU_MARKS_MAX];
    atomic_int         remaining;   /* section buffers + the frame's last buffer */
} metal_mark_frame;

static os_unfair_lock      metal_marks_lock = OS_UNFAIR_LOCK_INIT;
static vio_gpu_mark_result metal_marks_result;
static vio_gpu_mark_result metal_marks_snapshot;
static int                 metal_marks_valid;

static void metal_marks_publish(const metal_mark_frame *mf)
{
    vio_gpu_mark_result r;
    r.count = mf ? mf->names.count : 0;
    double prev = mf ? mf->start : 0.0;
    for (int i = 0; i < r.count; i++) {
        memcpy(r.name[i], mf->names.name[i], VIO_GPU_MARK_NAME_MAX);
        double t = mf->end[i];
        r.ms[i] = t > prev ? (t - prev) * 1000.0 : 0.0;
        if (t > prev) prev = t;
    }
    os_unfair_lock_lock(&metal_marks_lock);
    metal_marks_result = r;
    metal_marks_valid = 1;
    os_unfair_lock_unlock(&metal_marks_lock);
}

static void metal_marks_reset(void)
{
    os_unfair_lock_lock(&metal_marks_lock);
    metal_marks_valid = 0;
    os_unfair_lock_unlock(&metal_marks_lock);
    vio_mtl.cur_marks = NULL;   /* an unfinished frame's set is owned by its handlers */
}

/* The section buffer or the frame's last buffer completed. */
static void metal_marks_done(metal_mark_frame *mf)
{
    if (atomic_fetch_sub(&mf->remaining, 1) == 1) {
        metal_marks_publish(mf);
        free(mf);
    }
}

static void metal_present(void)
{
    @autoreleasepool {
        if (!vio_mtl.current_cmd_buf) return;

        /* GPU timestamps (GAP-PHASE5 Block 3) and named sections: the frame
         * completes with its last buffer. vio_gpu_timestamp splits the frame
         * into several buffers (the queue runs them in order), so the frame
         * spans from the first section's start to this buffer's end - before,
         * only the last buffer counted and the sections outgrew the frame (test
         * 172 on the CI). A frame without marks publishes an empty set. */
        metal_mark_frame *mf = (metal_mark_frame *)vio_mtl.cur_marks;
        vio_mtl.cur_marks = NULL;
        [vio_mtl.current_cmd_buf addCompletedHandler:^(id<MTLCommandBuffer> done) {
            double start = (mf && mf->start > 0.0) ? mf->start : done.GPUStartTime;
            double ms = (done.GPUEndTime - start) * 1000.0;
            if (ms >= 0.0) vio_mtl.last_gpu_ms = ms;
            if (mf) metal_marks_done(mf);
            else metal_marks_publish(NULL);
        }];
        if (vio_mtl.frame_semaphore_held) {
            /* The frame's slot frees up when its command buffer retires. */
            dispatch_semaphore_t sem = vio_mtl.frame_semaphore;
            [vio_mtl.current_cmd_buf addCompletedHandler:^(id<MTLCommandBuffer> done) {
                (void)done;
                dispatch_semaphore_signal(sem);
            }];
            vio_mtl.frame_semaphore_held = 0;
        }

        if (vio_mtl.current_drawable) {
            /* Vsync path: present drawable to screen */
            last_presented_texture = vio_mtl.current_drawable.texture;
            last_presented_cmd_buf = vio_mtl.current_cmd_buf;

            [vio_mtl.current_cmd_buf presentDrawable:vio_mtl.current_drawable];
            [vio_mtl.current_cmd_buf commit];
        } else {
            /* Vsync-off/offscreen path: commit without presenting.
               Use vio_gpu_flush() afterwards for accurate timing. */
            last_presented_texture = vio_mtl.offscreen_texture;
            last_presented_cmd_buf = vio_mtl.current_cmd_buf;

            [vio_mtl.current_cmd_buf commit];
        }

        /* The ring slices this frame's draws read stay reserved until the
         * command buffer retires (waited in metal_ring_begin_frame). */
        metal_ring_end_frame(vio_mtl.current_cmd_buf);

        vio_mtl.current_cmd_buf  = nil;
        vio_mtl.current_drawable = nil;
    }
}

static void metal_gpu_flush(void)
{
    @autoreleasepool {
        if (last_presented_cmd_buf) {
            [last_presented_cmd_buf waitUntilCompleted];
        }
    }
}

static void metal_clear(float r, float g, float b, float a)
{
    vio_mtl.clear_r = r;
    vio_mtl.clear_g = g;
    vio_mtl.clear_b = b;
    vio_mtl.clear_a = a;

    /* Eager clear, D3D11 semantics: inside a frame the currently bound target
     * (swapchain or RT) is cleared NOW — the pass is closed and reopened with
     * Clear load actions for colour + depth. Outside a frame the colour is
     * latched and applied when metal_begin_frame opens the first pass, so the
     * portable "vio_clear before vio_begin" pattern keeps working too. */
    if (vio_mtl.current_cmd_buf && vio_mtl.current_encoder) {
        @autoreleasepool {
            [vio_mtl.current_encoder endEncoding];
            vio_mtl.current_encoder = nil;
            metal_open_encoder(/*load_clear=*/1);
        }
    }
}

/* ── Shader compilation: SPIR-V → MSL → MTLLibrary ────────────────── */

extern uint32_t *vio_compile_glsl_to_spirv(const char *source, int stage,
                                            size_t *out_size, char **error_msg);

#include "../../vio_mesh.h"
#include "../../vio_shader.h"
#include "../../vio_shader_reflect.h"   /* vio_spirv_execution_model */
#include "../../vio_cubemap.h"
#include "../../vio_texture.h"

/* Vertex-buffer indices reserved for [[stage_in]] data. Every shader resource
 * (UBO / SSBO / push constant) is renumbered to MSL buffer index 0..N-1 at
 * compile time (metal_gfx_spirv_to_msl), so these two can never collide with
 * them — Metal exposes 31 vertex-stage buffer slots (0..30). */
#define VIO_METAL_VB_MESH      30
#define VIO_METAL_VB_INSTANCE  29

/* Resource tables, stage-input layout and the SPIR-V -> MSL transpiler
 * (metal_gfx_spirv_to_msl, vio_metal_tess_reflect) live in plain C. */
#include "vio_metal_msl.h"

/* Vertex + geometry stage as compute kernels and the pass-through vertex
 * function (METAL-GEOMETRY-PLAN.md). */
extern uint32_t *vio_compile_glsl_compute_to_spirv(const char *source, size_t *out_size, char **error_msg);
extern uint32_t *vio_compile_glsl_stage_to_spirv(const char *source, int stage, size_t *out_size, char **error_msg);
#include "vio_metal_kernel.h"

/* Compute pipelines of a tessellation shader's vertex kernel, one per mesh
 * stride (the stage-input descriptor bakes the stride in). */
#define VIO_METAL_TESS_VS_VARIANTS 4

/* ARC does not track strong references stored in C structs, so we keep
 * Metal objects as opaque `void *` and bridge across this boundary
 * manually with __bridge_retained / CFRelease. */
typedef struct _vio_metal_shader {
    void *vert_fn;        /* id<MTLFunction>, +1 retained */
    void *frag_fn;        /* id<MTLFunction>, +1 retained */
    /* Fragment variant with every colour output masked away (SPIRV-Cross
     * frag_output_mask = 0) for depth-only render targets: keeps `discard` /
     * alpha-test in shadow passes alive where a PSO without a fragment stage
     * would not. NULL when the mask compile failed — the PSO then rasterizes
     * depth only. */
    void *frag_fn_noout;  /* id<MTLFunction>, +1 retained */
    vio_metal_stage_res vs;
    vio_metal_stage_res fs;
    vio_metal_vs_layout vl;
    /* Tessellation (tess control + eval present). vert_fn is then the TES as a
     * post-tessellation vertex function; the vertex and control stages run as
     * compute kernels before every draw (metal_draw_tess). */
    int                 tess;
    vio_metal_tess_info tess_info;
    vio_metal_stage_res tcs;
    vio_metal_stage_res tes;
    void               *vs_kernel_fn;  /* id<MTLFunction> kernel, +1 retained */
    void               *tcs_pso;       /* id<MTLComputePipelineState>, +1 retained */
    struct {
        int      stride;
        uint32_t layout;               /* vio_mesh_layout.key of the mesh */
        void    *pso;                  /* id<MTLComputePipelineState>, +1 retained */
    } vs_kernel_variants[VIO_METAL_TESS_VS_VARIANTS];
    int                 vs_kernel_variant_count;
    /* Isolines / point_mode (metal_tess_emul_msl): vert_fn is vio_pass, the
     * TES runs as the capture function of an extra pass without rasterization. */
    int                 tess_emul;
    int                 view_count;      /* multiview views (instancing emulation), 0 = off */
    /* Mesh pipeline (VIO_FEATURE_MESH_SHADER): vert_fn is the [[mesh]] function
     * (the mesh stage takes the vertex slot), object_fn the optional [[object]]
     * function; threads per threadgroup from each stage's LocalSize. */
    int                 mesh;
    void               *object_fn;       /* id<MTLFunction>, +1 retained, NULL without a task stage */
    unsigned            mesh_tpg[3], object_tpg[3];
    vio_metal_stage_res obj;             /* task stage resources (its uniform block shares the mesh stage's) */
    int                 tess_emul_domain;  /* VIO_MSL_DOMAIN_* of the GLSL (isolines compiled as quads) */
    void               *tess_cap_fn;       /* id<MTLFunction>, +1 retained */
    void               *tess_cap_pso;      /* id<MTLRenderPipelineState>, +1 retained, built lazily */
    /* Geometry stage, emulated: vert_fn is the pass-through vertex function
     * (vs / vl its tables), the vertex and geometry stages run as kernels
     * before every draw (metal_draw_gs). */
    int                 gs;
    int                 gs_in_vertices;  /* 1 / 2 / 3 / 4 / 6 per input primitive */
    int                 gs_out_prim;     /* MK_PRIM_* */
    int                 gs_max_vertices, gs_invocations, gs_idx_per;
    int                 gs_vs_slots, gs_slots;  /* vec4 slots per vertex / geometry record */
    int                 gs_vs_inst;      /* the vertex stage reads locations 3..6 */
    int                 gs_mesh_stride;  /* default mesh stride of the vertex stage */
    void               *gs_vs_pso;       /* id<MTLComputePipelineState>, +1 retained */
    void               *gs_pso;          /* id<MTLComputePipelineState>, +1 retained */
    vio_metal_stage_res gs_vsk, gs_gsk;  /* resource tables of the two kernels */
    int                 gs_vs_cb, gs_gs_cb;   /* MSL index of each stage's own uniform block, -1 */
    int                 gs_rec_index;    /* pass-through: MSL index of the record buffer */
} vio_metal_shader;

static id<MTLLibrary> metal_build_library(const char *msl, char **error_out)
{
    NSError *err = nil;
    MTLCompileOptions *opts = metal_compile_options();
    id<MTLLibrary> lib = [vio_mtl.device newLibraryWithSource:[NSString stringWithUTF8String:msl] options:opts error:&err];
    if (!lib && error_out) *error_out = strdup(err ? [[err localizedDescription] UTF8String] : "unknown MSL compile error");
    return lib;
}

static id<MTLFunction> metal_build_function(const char *msl, char **error_out)
{
    @autoreleasepool {
        NSString *src = [NSString stringWithUTF8String:msl];
        NSError *err = nil;
        MTLCompileOptions *opts = metal_compile_options();   /* the ladder rung (tessellation needs 2.1) */
        id<MTLLibrary> lib = [vio_mtl.device newLibraryWithSource:src options:opts error:&err];
        if (!lib) {
            if (error_out) {
                NSString *msg = err ? [err localizedDescription] : @"unknown MSL compile error";
                *error_out = strdup([msg UTF8String]);
            }
            return nil;
        }
        /* SPIRV-Cross emits the entry point as `main0` (it cannot use `main` in MSL) */
        id<MTLFunction> fn = [lib newFunctionWithName:@"main0"];
        if (!fn) {
            fn = [lib newFunctionWithName:@"main"];
        }
        if (!fn && error_out) {
            *error_out = strdup("MSL entry point 'main0' not found");
        }
        /* Library is retained transitively by the function; we don't keep
         * a separate strong reference here — caller stores `fn`. */
        (void)lib;
        return fn;
    }
}

static void metal_destroy_shader(void *s)
{
    if (!s) return;
    vio_metal_shader *sh = (vio_metal_shader *)s;
    if (sh->vert_fn) { CFRelease((CFTypeRef)sh->vert_fn); sh->vert_fn = NULL; }
    if (sh->object_fn) { CFRelease((CFTypeRef)sh->object_fn); sh->object_fn = NULL; }
    if (sh->frag_fn) { CFRelease((CFTypeRef)sh->frag_fn); sh->frag_fn = NULL; }
    if (sh->frag_fn_noout) { CFRelease((CFTypeRef)sh->frag_fn_noout); sh->frag_fn_noout = NULL; }
    if (sh->vs_kernel_fn) { CFRelease((CFTypeRef)sh->vs_kernel_fn); sh->vs_kernel_fn = NULL; }
    if (sh->tcs_pso) { CFRelease((CFTypeRef)sh->tcs_pso); sh->tcs_pso = NULL; }
    if (sh->gs_vs_pso) { CFRelease((CFTypeRef)sh->gs_vs_pso); sh->gs_vs_pso = NULL; }
    if (sh->tess_cap_fn) { CFRelease((CFTypeRef)sh->tess_cap_fn); sh->tess_cap_fn = NULL; }
    if (sh->tess_cap_pso) { CFRelease((CFTypeRef)sh->tess_cap_pso); sh->tess_cap_pso = NULL; }
    if (sh->gs_pso) { CFRelease((CFTypeRef)sh->gs_pso); sh->gs_pso = NULL; }
    for (int i = 0; i < sh->vs_kernel_variant_count; i++) {
        if (sh->vs_kernel_variants[i].pso) CFRelease((CFTypeRef)sh->vs_kernel_variants[i].pso);
    }
    free(sh);
}

#ifdef HAVE_SPIRV_CROSS
/* SPIR-V of the vertex / fragment stage: php_vio.c hands over SPIR-V, older
 * callers raw GLSL. Sets *owned when the result must be freed. */
static uint32_t *metal_stage_spirv(const void *data, size_t size, int is_fragment,
                                   size_t *out_size, int *owned, char **err)
{
    *owned = 0;
    if (size >= 4 && *(const uint32_t *)data == 0x07230203) {
        *out_size = size;
        return (uint32_t *)data;
    }
    uint32_t *spv = vio_compile_glsl_to_spirv((const char *)data, is_fragment, out_size, err);
    if (spv) *owned = 1;
    return spv;
}
#endif

#ifdef HAVE_SPIRV_CROSS
/* GLSL compute kernel -> SPIR-V -> MSL (renumbered like every stage) -> PSO. */
static id<MTLComputePipelineState> metal_kernel_pso(const char *glsl, vio_metal_stage_res *res, char **err)
{
    size_t size = 0;
    char *warn = NULL;
    uint32_t *spv = vio_compile_glsl_compute_to_spirv(glsl, &size, &warn);
    if (!spv) { *err = warn; return nil; }
    free(warn);
    char *msl = metal_gfx_spirv_to_msl(spv, size, VIO_MSL_KERNEL, res, NULL, 0xFFFFFFFFu, NULL, err);
    free(spv);
    if (!msl) return nil;
    id<MTLFunction> fn = metal_build_function(msl, err);
    free(msl);
    if (!fn) return nil;
    NSError *nserr = nil;
    id<MTLComputePipelineState> pso = [vio_mtl.device newComputePipelineStateWithFunction:fn error:&nserr];
    if (!pso) *err = strdup(nserr ? [[nserr localizedDescription] UTF8String] : "compute pipeline failed");
    return pso;
}

/* Geometry stage (METAL-GEOMETRY-PLAN.md): vertex and geometry kernels, and
 * the pass-through vertex stage as the shader's MSL vertex source (returned,
 * malloc'd; sh->vs / sh->vl describe it). */
static char *metal_gs_prepare(vio_metal_shader *sh, const uint32_t *vs_spirv, size_t vs_size,
                              const void *gs_data, size_t gs_size, char **err)
{
    mk_module mv, mg;
    char *vk = NULL, *gk = NULL, *pt = NULL, *msl = NULL;
    uint32_t *pt_spv = NULL;
    size_t pt_size = 0;
    char *warn = NULL;
    id<MTLComputePipelineState> vs_pso = nil, gs_pso = nil;
    /* Interface blocks (A28): one varying per member, linked by location like
     * the fragment stage (metal_compile_shader flattens it too). The parsed
     * modules point into these copies, freed after the kernels are built. */
    size_t vs_flat_words = 0, gs_flat_words = 0;
    uint32_t *vs_flat = vio_spirv_flatten_io_blocks(vs_spirv, vs_size / 4, &vs_flat_words);
    uint32_t *gs_flat = vio_spirv_flatten_io_blocks((const uint32_t *)gs_data, gs_size / 4, &gs_flat_words);
    if (vs_flat) { vs_spirv = vs_flat; vs_size = vs_flat_words * 4; }
    if (gs_flat) { gs_data = gs_flat; gs_size = gs_flat_words * 4; }
    if (mk_parse(&mv, vs_spirv, vs_size, err) != 0) { free(vs_flat); free(gs_flat); return NULL; }
    if (mk_parse(&mg, (const uint32_t *)gs_data, gs_size, err) != 0) { mk_free(&mv); free(vs_flat); free(gs_flat); return NULL; }
    if (mv.model != MK_MODEL_VERTEX || mg.model != MK_MODEL_GEOMETRY) {
        *err = strdup("expected a vertex and a geometry stage");
        goto done;
    }
    vk = mk_vs_kernel_glsl(&mv, err);
    if (!vk) goto done;
    gk = mk_gs_kernel_glsl(&mg, &mv, err);
    if (!gk) goto done;
    pt = mk_passthrough_vs_glsl(&mg);
    if (!pt) { *err = strdup("out of memory"); goto done; }
    if (getenv("VIO_DUMP_GS_GLSL")) {
        fprintf(stderr, "==== Metal vertex kernel GLSL ====\n%s\n==== Metal geometry kernel GLSL ====\n%s\n"
                        "==== Metal pass-through vertex GLSL ====\n%s\n==== end ====\n", vk, gk, pt);
        fflush(stderr);
    }

    vs_pso = metal_kernel_pso(vk, &sh->gs_vsk, err);
    if (!vs_pso) goto done;
    gs_pso = metal_kernel_pso(gk, &sh->gs_gsk, err);
    if (!gs_pso) goto done;
    pt_spv = vio_compile_glsl_stage_to_spirv(pt, VIO_STAGE_VERTEX, &pt_size, &warn);
    if (!pt_spv) { *err = warn; goto done; }
    free(warn);
    msl = metal_gfx_spirv_to_msl(pt_spv, pt_size, VIO_MSL_VERTEX, &sh->vs, &sh->vl, 0xFFFFFFFFu, NULL, err);
    if (!msl) goto done;

    sh->gs = 1;
    sh->gs_in_vertices  = mg.in_vertices;
    sh->gs_out_prim     = mg.out_prim;
    sh->gs_max_vertices = mg.max_vertices;
    sh->gs_invocations  = mg.invocations;
    sh->gs_idx_per      = mk_gs_index_count(&mg);
    sh->gs_vs_slots     = mv.out_slots;
    sh->gs_slots        = mg.out_slots;
    sh->gs_vs_inst      = mk_vs_uses_instance(&mv);
    sh->gs_mesh_stride  = mk_vs_mesh_stride(&mv);
    sh->gs_vs_cb        = mk_user_cbuffer(&sh->gs_vsk);
    sh->gs_gs_cb        = mk_user_cbuffer(&sh->gs_gsk);
    sh->gs_rec_index    = mk_res_index(&sh->vs, VIO_MK_BIND_IN);
    sh->gs_vs_pso = (void *)CFBridgingRetain(vs_pso);
    sh->gs_pso    = (void *)CFBridgingRetain(gs_pso);
done:
    mk_free(&mv);
    mk_free(&mg);
    free(vs_flat); free(gs_flat);
    free(vk); free(gk); free(pt); free(pt_spv);
    if (!sh->gs) { free(msl); return NULL; }
    return msl;
}
#endif

static void *metal_compile_shader(vio_shader_desc *desc)
{
    if (!desc || !desc->vertex_data || !desc->fragment_data) {
        php_error_docref(NULL, E_WARNING, "Metal: compile_shader called with NULL data");
        return NULL;
    }
    if (!vio_mtl.device) {
        php_error_docref(NULL, E_WARNING, "Metal: device not initialized");
        return NULL;
    }
#ifndef HAVE_SPIRV_CROSS
    php_error_docref(NULL, E_WARNING, "Metal: shaders require SPIRV-Cross (build with --with-spirv-cross)");
    return NULL;
#else
    int tess = desc->tess_control_data && desc->tess_eval_data;
    if (desc->geometry_data) {
        /* Emulated with compute kernels (metal_draw_gs). */
        if (tess) {
            php_error_docref(NULL, E_WARNING, "Metal: a geometry stage behind tessellation is not supported");
            return NULL;
        }
        if (desc->geometry_size < 20 || *(const uint32_t *)desc->geometry_data != 0x07230203) {
            php_error_docref(NULL, E_WARNING, "Metal: the geometry stage must arrive as SPIR-V");
            return NULL;
        }
    }
    if (tess && !(desc->tess_control_size >= 4 && *(const uint32_t *)desc->tess_control_data == 0x07230203 &&
                  desc->tess_eval_size >= 4 && *(const uint32_t *)desc->tess_eval_data == 0x07230203)) {
        php_error_docref(NULL, E_WARNING, "Metal: tessellation stages must arrive as SPIR-V");
        return NULL;
    }

    char *err = NULL;
    const char *what = NULL;
    metal_msl_multiview = desc->view_count > 1;   /* reset at the end and on failure */
    size_t vs_size = 0, fs_size = 0;
    int vs_owned = 0, fs_owned = 0;
    uint32_t *vs_spirv = NULL, *fs_spirv = NULL;
    char *vs_msl = NULL, *fs_msl = NULL, *fs_msl_noout = NULL, *tcs_msl = NULL, *tes_msl = NULL, *task_msl = NULL;
    id<MTLFunction> vfn = nil, ffn = nil, ffn_noout = nil, kfn = nil, tcsfn = nil, cap_fn = nil;
    id<MTLComputePipelineState> tcs_pso = nil;

    vio_metal_shader *sh = calloc(1, sizeof(vio_metal_shader));
    if (!sh) return NULL;

    vs_spirv = metal_stage_spirv(desc->vertex_data, desc->vertex_size, 0, &vs_size, &vs_owned, &err);
    if (!vs_spirv) { what = "VS GLSL→SPIR-V"; goto fail; }
    fs_spirv = metal_stage_spirv(desc->fragment_data, desc->fragment_size, 1, &fs_size, &fs_owned, &err);
    if (!fs_spirv) { what = "FS GLSL→SPIR-V"; goto fail; }

    if (tess) {
        /* Metal has no hull / domain stages: the vertex and control stages
         * become compute kernels, the evaluation stage the post-tessellation
         * vertex function (see metal_draw_tess). */
        sh->tess = 1;
        if (vio_metal_tess_reflect((const uint32_t *)desc->tess_control_data, desc->tess_control_size,
                                   (const uint32_t *)desc->tess_eval_data, desc->tess_eval_size,
                                   &sh->tess_info, &err) != 0) { what = "tessellation reflection"; goto fail; }
        /* Isolines and point_mode: Metal's tessellator has neither (see
         * metal_tess_emul_msl). The control stage writes quad factors for
         * isolines, the evaluation stage is compiled as a quad domain. */
        uint32_t *tes_words = (uint32_t *)desc->tess_eval_data;
        size_t tes_bytes = desc->tess_eval_size;
        uint32_t *tes_copy = NULL;
        if (sh->tess_info.domain == VIO_MSL_DOMAIN_ISOLINES || sh->tess_info.point_mode) {
            sh->tess_emul = 1;
            sh->tess_emul_domain = sh->tess_info.domain;
            if (sh->tess_info.domain == VIO_MSL_DOMAIN_ISOLINES) {
                sh->tess_info.domain = VIO_MSL_DOMAIN_QUADS;
                tes_copy = (uint32_t *)malloc(tes_bytes);
                if (!tes_copy) { what = "tessellation"; goto fail; }
                memcpy(tes_copy, tes_words, tes_bytes);
                for (size_t i = 5; i < tes_bytes / 4 && (tes_copy[i] >> 16); i += tes_copy[i] >> 16) {
                    /* OpExecutionMode Isolines -> Quads */
                    if ((tes_copy[i] & 0xFFFF) == 16 && (tes_copy[i] >> 16) >= 3 && tes_copy[i + 2] == 25) tes_copy[i + 2] = 24;
                }
                tes_words = tes_copy;
            }
        }
        /* The stages hand their varyings over in buffers whose structs are
         * built per stage: interface blocks become one varying per member on
         * every stage (a patch block's member read through the block came out
         * as a thread -> device cast, test 198 B), and the control and
         * evaluation stages use every input they declare (test 198 C). */
        size_t nw = 0;
        uint32_t *vs_flat = vio_spirv_flatten_io_blocks(vs_spirv, vs_size / 4, &nw);
        size_t vs_flat_bytes = vs_flat ? nw * 4 : vs_size;
        uint32_t *tcs_words = (uint32_t *)desc->tess_control_data, *tcs_own = NULL, *t;
        size_t tcs_bytes = desc->tess_control_size;
        if ((t = vio_spirv_flatten_io_blocks(tcs_words, tcs_bytes / 4, &nw)) != NULL) { tcs_own = t; tcs_words = t; tcs_bytes = nw * 4; }
        if ((t = metal_spirv_use_all_inputs(tcs_words, tcs_bytes / 4, &nw)) != NULL) { free(tcs_own); tcs_own = t; tcs_words = t; tcs_bytes = nw * 4; }
        uint32_t *tes_own = NULL;
        if ((t = vio_spirv_flatten_io_blocks(tes_words, tes_bytes / 4, &nw)) != NULL) { tes_own = t; tes_words = t; tes_bytes = nw * 4; }
        if ((t = metal_spirv_use_all_inputs(tes_words, tes_bytes / 4, &nw)) != NULL) { free(tes_own); tes_own = t; tes_words = t; tes_bytes = nw * 4; }

        vs_msl = metal_gfx_spirv_to_msl(vs_flat ? vs_flat : vs_spirv, vs_flat_bytes, VIO_MSL_VERTEX_TESS, &sh->vs, &sh->vl,
                                        0xFFFFFFFFu, &sh->tess_info, &err);
        free(vs_flat);
        if (!vs_msl) { free(tcs_own); free(tes_own); free(tes_copy); what = "VS SPIR-V→MSL kernel"; goto fail; }
        tcs_msl = metal_gfx_spirv_to_msl(tcs_words, tcs_bytes,
                                         VIO_MSL_TESS_CONTROL, &sh->tcs, NULL, 0xFFFFFFFFu, &sh->tess_info, &err);
        free(tcs_own);
        if (!tcs_msl) { free(tes_own); free(tes_copy); what = "TCS SPIR-V→MSL"; goto fail; }
        tes_msl = metal_gfx_spirv_to_msl(tes_words, tes_bytes,
                                         VIO_MSL_TESS_EVAL, &sh->tes, NULL, 0xFFFFFFFFu, &sh->tess_info, &err);
        free(tes_own);
        free(tes_copy);
        if (!tes_msl) { what = "TES SPIR-V→MSL"; goto fail; }
        if (sh->tess_emul) {
            char *emul = metal_tess_emul_msl(tes_msl, sh->tess_emul_domain, sh->tess_info.spacing, &err);
            free(tes_msl);
            tes_msl = emul;
            if (!tes_msl) { what = "TES capture MSL"; goto fail; }
        }
    } else if (vio_spirv_execution_model(vs_spirv, vs_size) == VIO_SPIRV_MODEL_MESH_EXT) {
        /* Mesh pipeline: the mesh stage arrives in the vertex slot. */
        sh->mesh = 1;
        metal_spirv_local_size(vs_spirv, vs_size, sh->mesh_tpg);
        vs_msl = metal_gfx_spirv_to_msl(vs_spirv, vs_size, VIO_MSL_MESH, &sh->vs, NULL, 0xFFFFFFFFu, NULL, &err);
        if (!vs_msl) { what = "mesh SPIR-V→MSL"; goto fail; }
        if (desc->task_data) {
            if (desc->task_size < 20 || *(const uint32_t *)desc->task_data != 0x07230203) { err = strdup("not SPIR-V"); what = "task stage"; goto fail; }
            metal_spirv_local_size((const uint32_t *)desc->task_data, desc->task_size, sh->object_tpg);
            task_msl = metal_gfx_spirv_to_msl((const uint32_t *)desc->task_data, desc->task_size, VIO_MSL_TASK,
                                              &sh->obj, NULL, 0xFFFFFFFFu, NULL, &err);
            if (!task_msl) { what = "task SPIR-V→MSL"; goto fail; }
        }
    } else if (desc->geometry_data) {
        vs_msl = metal_gs_prepare(sh, vs_spirv, vs_size, desc->geometry_data, desc->geometry_size, &err);
        if (!vs_msl) { what = "geometry stage"; goto fail; }
        /* Fragment inputs as the pass-through stage writes them (A28): blocks
         * split per member, gl_PrimitiveID from the geometry stage as a flat
         * int at VIO_PRIMID_LOCATION. */
        size_t nw = 0;
        uint32_t *nf = vio_spirv_flatten_io_blocks(fs_spirv, fs_size / 4, &nw);
        if (nf) {
            if (fs_owned) free(fs_spirv);
            fs_spirv = nf; fs_size = nw * 4; fs_owned = 1;
        }
        if (vio_spirv_writes_builtin((const uint32_t *)desc->geometry_data, desc->geometry_size, 7) &&
            (nf = vio_spirv_builtin_to_location(fs_spirv, fs_size / 4, 7, 1, VIO_PRIMID_LOCATION, 1, &nw)) != NULL) {
            if (fs_owned) free(fs_spirv);
            fs_spirv = nf; fs_size = nw * 4; fs_owned = 1;
        }
    } else {
        vs_msl = metal_gfx_spirv_to_msl(vs_spirv, vs_size, VIO_MSL_VERTEX, &sh->vs, &sh->vl,
                                        0xFFFFFFFFu, NULL, &err);
        if (!vs_msl) { what = "VS SPIR-V→MSL"; goto fail; }
    }

    fs_msl = metal_gfx_spirv_to_msl(fs_spirv, fs_size, VIO_MSL_FRAGMENT, &sh->fs, NULL, 0xFFFFFFFFu, NULL, &err);
    if (!fs_msl) { what = "FS SPIR-V→MSL"; goto fail; }
    /* Depth-only variant: same resource renumbering (identical inputs, identical
     * algorithm => identical tables), colour outputs masked. Best effort. */
    {
        vio_metal_stage_res scratch;
        char *err2 = NULL;
        fs_msl_noout = metal_gfx_spirv_to_msl(fs_spirv, fs_size, VIO_MSL_FRAGMENT, &scratch, NULL, 0u, NULL, &err2);
        free(err2);
    }
    metal_msl_multiview = 0;
    sh->view_count = desc->view_count > 1 ? desc->view_count : 0;

    if (tess) {
        kfn = metal_build_function(vs_msl, &err);
        if (!kfn) { what = "VS kernel MSL→MTLFunction"; goto fail; }
        tcsfn = metal_build_function(tcs_msl, &err);
        if (!tcsfn) { what = "TCS MSL→MTLFunction"; goto fail; }
        NSError *nserr = nil;
        tcs_pso = [vio_mtl.device newComputePipelineStateWithFunction:tcsfn error:&nserr];
        if (!tcs_pso) {
            err = strdup(nserr ? [[nserr localizedDescription] UTF8String] : "unknown");
            what = "TCS compute pipeline";
            goto fail;
        }
        if (sh->tess_emul) {
            /* main0 is the capture function; the draw's vertex function is vio_pass. */
            id<MTLLibrary> lib = metal_build_library(tes_msl, &err);
            if (!lib) { what = "TES capture MSL→MTLLibrary"; goto fail; }
            cap_fn = [lib newFunctionWithName:@"main0"];
            vfn = [lib newFunctionWithName:@"vio_pass"];
            if (!cap_fn || !vfn) { err = strdup("capture / pass functions missing"); what = "TES capture"; goto fail; }
        } else {
            vfn = metal_build_function(tes_msl, &err);
            if (!vfn) { what = "TES MSL→MTLFunction"; goto fail; }
        }
    } else {
        vfn = metal_build_function(vs_msl, &err);
        if (!vfn) { what = sh->mesh ? "mesh MSL→MTLFunction" : "VS MSL→MTLFunction"; goto fail; }
        if (task_msl) {
            id<MTLFunction> ofn = metal_build_function(task_msl, &err);
            if (!ofn) { what = "task MSL→MTLFunction"; goto fail; }
            sh->object_fn = (void *)CFBridgingRetain(ofn);
        }
    }
    ffn = metal_build_function(fs_msl, &err);
    if (!ffn) { what = "FS MSL→MTLFunction"; goto fail; }
    if (fs_msl_noout) {
        char *err2 = NULL;
        ffn_noout = metal_build_function(fs_msl_noout, &err2);
        free(err2);
    }

    sh->vert_fn = (void *)CFBridgingRetain(vfn);
    sh->frag_fn = (void *)CFBridgingRetain(ffn);
    sh->frag_fn_noout = ffn_noout ? (void *)CFBridgingRetain(ffn_noout) : NULL;
    if (tess) {
        sh->vs_kernel_fn = (void *)CFBridgingRetain(kfn);
        sh->tcs_pso = (void *)CFBridgingRetain(tcs_pso);
        if (cap_fn) sh->tess_cap_fn = (void *)CFBridgingRetain(cap_fn);
    }
    if (vs_owned) free(vs_spirv);
    if (fs_owned) free(fs_spirv);
    free(vs_msl); free(fs_msl); free(fs_msl_noout); free(tcs_msl); free(tes_msl); free(task_msl);
    return sh;

fail:
    metal_msl_multiview = 0;
    php_error_docref(NULL, E_WARNING, "Metal: %s failed: %s", what ? what : "shader compile", err ? err : "unknown");
    free(err);
    if (vs_owned) free(vs_spirv);
    if (fs_owned) free(fs_spirv);
    free(vs_msl); free(fs_msl); free(fs_msl_noout); free(tcs_msl); free(tes_msl); free(task_msl);
    metal_destroy_shader(sh);   /* kernel PSOs of a geometry stage */
    return NULL;
#endif /* HAVE_SPIRV_CROSS */
}

/* ── Render-target lifecycle ──────────────────────────────────────
 *
 * Offscreen MTLTexture-backed render target. Used by consumer code that
 * wants to redirect a frame's draws into an offscreen color buffer (e.g.
 * PHPolygon's Engine::warmRender pre-warming a splash texture without
 * touching the swapchain). Independent of the 3D pipeline — the 2D batch
 * renderer is what actually emits draws into the RT.
 *
 * Ownership: each MTLTexture is created with newTextureWithDescriptor:
 * (+1 retain) and bridged into the RT object with CFBridgingRetain so the
 * strong reference outlives this autoreleasepool. Released in destroy via
 * CFBridgingRelease (which ARC drops on scope exit). */

/* Give a freshly created RT defined contents (colour 0, depth 1.0) with one
 * empty Clear pass per slice, so a target that is bound and drawn into without
 * an explicit vio_clear still depth-tests (Private textures start undefined). */
/* Number of colour attachments of an RT (1 for the classic single target). */
static int metal_rt_attachment_count(const vio_render_target_object *rt)
{
    int n = rt->attachment_count > 0 ? rt->attachment_count : 1;
    return n > VIO_MAX_COLOR_ATTACHMENTS ? VIO_MAX_COLOR_ATTACHMENTS : n;
}

/* MTLPixelFormat of a vio_pixel_format colour attachment. RGBA8 maps to
 * BGRA8Unorm like the swapchain (readback swizzles it back). */
static MTLPixelFormat metal_pixel_format(int vio_fmt)
{
    switch (vio_fmt) {
        case VIO_FORMAT_RGBA16F:    return MTLPixelFormatRGBA16Float;
        case VIO_FORMAT_RGBA32F:    return MTLPixelFormatRGBA32Float;
        case VIO_FORMAT_R11G11B10F: return MTLPixelFormatRG11B10Float;
        case VIO_FORMAT_RG16F:      return MTLPixelFormatRG16Float;
        case VIO_FORMAT_R16F:       return MTLPixelFormatR16Float;
        case VIO_FORMAT_R32F:       return MTLPixelFormatR32Float;
        case VIO_FORMAT_R8:         return MTLPixelFormatR8Unorm;
        case VIO_FORMAT_RGBA8:
        default:                    return MTLPixelFormatBGRA8Unorm;
    }
}

/* vio_pixel_format of an MTLTexture (for the shared RGBA8 readback converter). */
static int metal_vio_format(MTLPixelFormat f, int *bgra)
{
    *bgra = 0;
    switch (f) {
        case MTLPixelFormatBGRA8Unorm:  *bgra = 1; return VIO_FORMAT_RGBA8;
        case MTLPixelFormatRGBA8Unorm:  return VIO_FORMAT_RGBA8;
        case MTLPixelFormatRGBA16Float: return VIO_FORMAT_RGBA16F;
        case MTLPixelFormatRGBA32Float: return VIO_FORMAT_RGBA32F;
        case MTLPixelFormatRG11B10Float:return VIO_FORMAT_R11G11B10F;
        case MTLPixelFormatRG16Float:   return VIO_FORMAT_RG16F;
        case MTLPixelFormatR16Float:    return VIO_FORMAT_R16F;
        case MTLPixelFormatR32Float:    return VIO_FORMAT_R32F;
        case MTLPixelFormatR8Unorm:     return VIO_FORMAT_R8;
        default:                        return -1;
    }
}

static void metal_rt_initial_clear(vio_render_target_object *rt)
{
    int n_color = rt->depth_only ? 0 : metal_rt_attachment_count(rt);
    id<MTLTexture> depth = rt->metal_msaa_depth_texture ? (__bridge id<MTLTexture>)rt->metal_msaa_depth_texture
                         : (rt->metal_depth_texture ? (__bridge id<MTLTexture>)rt->metal_depth_texture : nil);
    if (!rt->metal_color_texture && !depth) return;
    id<MTLCommandBuffer> cb = metal_new_command_buffer();
    int slices = vio_rt_layer_count(rt);
    int depth_layered = depth && (depth.textureType == MTLTextureTypeCube || depth.textureType == MTLTextureType2DArray ||
                                  depth.textureType == MTLTextureType2DMultisampleArray);
    for (int s = 0; s < slices; s++) {
        MTLRenderPassDescriptor *d = [MTLRenderPassDescriptor renderPassDescriptor];
        for (int i = 0; i < n_color; i++) {
            id<MTLTexture> color = rt->metal_color_textures[i] ? (__bridge id<MTLTexture>)rt->metal_color_textures[i] : nil;
            id<MTLTexture> msaa  = rt->metal_msaa_color_textures[i] ? (__bridge id<MTLTexture>)rt->metal_msaa_color_textures[i] : nil;
            if (!color) continue;
            d.colorAttachments[i].texture = msaa ? msaa : color;
            d.colorAttachments[i].slice = (NSUInteger)s;
            d.colorAttachments[i].loadAction = MTLLoadActionClear;
            d.colorAttachments[i].clearColor = MTLClearColorMake(0, 0, 0, 0);
            if (msaa) {
                d.colorAttachments[i].resolveTexture = color;
                d.colorAttachments[i].resolveSlice = (NSUInteger)s;
                d.colorAttachments[i].storeAction = MTLStoreActionStoreAndMultisampleResolve;
            } else {
                d.colorAttachments[i].storeAction = MTLStoreActionStore;
            }
        }
        if (depth && (depth_layered || s == 0)) {
            d.depthAttachment.texture = depth;
            if (depth_layered) d.depthAttachment.slice = (NSUInteger)s;
            d.depthAttachment.loadAction = MTLLoadActionClear;
            d.depthAttachment.storeAction = MTLStoreActionStore;
            d.depthAttachment.clearDepth = 1.0;
            if (rt->depth_only && rt->metal_msaa_depth_texture && rt->metal_depth_texture) {
                d.depthAttachment.resolveTexture = (__bridge id<MTLTexture>)rt->metal_depth_texture;
                d.depthAttachment.storeAction = MTLStoreActionStoreAndMultisampleResolve;
            }
            if (depth.pixelFormat == VIO_METAL_DEPTH_STENCIL) {
                d.stencilAttachment.texture = depth;
                d.stencilAttachment.loadAction = MTLLoadActionClear;
                d.stencilAttachment.storeAction = MTLStoreActionStore;
                d.stencilAttachment.clearStencil = 0;
            }
        }
        id<MTLRenderCommandEncoder> enc = [cb renderCommandEncoderWithDescriptor:d];
        [enc endEncoding];
    }
    [cb commit];
}

/* Largest sample count <= requested that the device supports (1 when MSAA is
 * off or the request is nonsense). Metal requires the exact count to be
 * supported, so walk down 8 -> 4 -> 2. */
static int metal_clamp_sample_count(int requested)
{
    if (requested <= 1 || !vio_mtl.device) return 1;
    int candidates[] = {8, 4, 2};
    for (int i = 0; i < 3; i++) {
        if (candidates[i] <= requested &&
            [vio_mtl.device supportsTextureSampleCount:(NSUInteger)candidates[i]]) {
            return candidates[i];
        }
    }
    return 1;
}

/* Cube / array MSAA (A24, blind - macOS CI): a 2DMultisampleArray colour + depth
 * pair; level-0 passes render into the face's slice and resolve into the same
 * slice of the cube / array (StoreAndMultisampleResolve), levels > 0 render
 * single-sampled into the face. */
static void metal_rt_layered_msaa(vio_render_target_object *rt, MTLPixelFormat fmt, int w, int h)
{
    int layers = vio_rt_layer_count(rt);
    if (rt->samples <= 1) return;
    MTLTextureDescriptor *cd = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:fmt
                                    width:(NSUInteger)w height:(NSUInteger)h mipmapped:NO];
    cd.textureType = MTLTextureType2DMultisampleArray;
    cd.sampleCount = (NSUInteger)rt->samples;
    cd.arrayLength = (NSUInteger)layers;
    cd.usage = MTLTextureUsageRenderTarget;
    cd.storageMode = MTLStorageModePrivate;
    MTLTextureDescriptor *dd = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:VIO_METAL_DEPTH_STENCIL
                                    width:(NSUInteger)w height:(NSUInteger)h mipmapped:NO];
    dd.textureType = MTLTextureType2DMultisampleArray;
    dd.sampleCount = (NSUInteger)rt->samples;
    dd.arrayLength = (NSUInteger)layers;
    dd.usage = MTLTextureUsageRenderTarget;
    dd.storageMode = MTLStorageModePrivate;
    id<MTLTexture> c = [vio_mtl.device newTextureWithDescriptor:cd];
    id<MTLTexture> d = [vio_mtl.device newTextureWithDescriptor:dd];
    if (!c || !d) { rt->samples = 1; return; }
    rt->metal_msaa_color_texture = (void *)CFBridgingRetain(c);
    rt->metal_msaa_color_textures[0] = rt->metal_msaa_color_texture;
    rt->metal_msaa_depth_texture = (void *)CFBridgingRetain(d);
}

static int metal_create_render_target(void *rt_ptr, int width, int height, int hdr, int depth_only)
{
    @autoreleasepool {
        vio_render_target_object *rt = (vio_render_target_object *)rt_ptr;
        if (!vio_mtl.initialized || !vio_mtl.device) return -1;

        /* MSAA: rt->samples was staged by vio_render_target(). A depth-only
         * (shadow-map) target keeps a single sample — its depth is SAMPLED by
         * later passes and a multisampled depth texture would need a
         * depth2d_ms in the shader. Colour targets render into a 2DMultisample
         * pair and resolve into the plain textures every pass end
         * (StoreAndMultisampleResolve), so metal_color_texture stays the one
         * thing wrappers / the 2D registry / readback see. */
        /* depth_only targets multisample as plain 2D targets (A24): Metal resolves
         * the depth itself (depthResolveFilter Max / Min) into the sampled texture. */
        int samples = (depth_only && (rt->is_cube || rt->layers > 1 || rt->mip_levels > 1)) ? 1 : metal_clamp_sample_count(rt->samples);
        rt->samples = samples;

        int n_color = metal_rt_attachment_count(rt);
        if (rt->attachment_count <= 0) {
            rt->attachment_count = 1;
            rt->formats[0] = hdr ? VIO_FORMAT_RGBA16F : VIO_FORMAT_RGBA8;
        }
        MTLPixelFormat color_fmt = metal_pixel_format(rt->formats[0]);

        if (rt->is_cube && depth_only) {
            /* Depth-only cube (point-light shadows): one depth-stencil cube, a
             * face per bind or every face with VIO_RT_ALL_LAYERS. */
            MTLTextureDescriptor *dd = [MTLTextureDescriptor
                textureCubeDescriptorWithPixelFormat:VIO_METAL_DEPTH_STENCIL size:(NSUInteger)width mipmapped:NO];
            dd.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
            dd.storageMode = MTLStorageModePrivate;
            id<MTLTexture> dcube = [vio_mtl.device newTextureWithDescriptor:dd];
            if (!dcube) {
                php_error_docref(NULL, E_WARNING, "Metal: failed to create depth cube render target (%d)", width);
                return -1;
            }
            rt->mip_levels = 1;
            rt->metal_depth_texture = (void *)CFBridgingRetain(dcube);
            rt->backend_type = VIO_RT_BACKEND_METAL;
            metal_rt_initial_clear(rt);
            return 0;
        }
        if (rt->layers > 1) {
            /* Array target ('layers' => N): colour and depth both 2DArray, a
             * layer per bind or all of them with VIO_RT_ALL_LAYERS. */
            MTLTextureDescriptor *dd = [MTLTextureDescriptor
                texture2DDescriptorWithPixelFormat:VIO_METAL_DEPTH_STENCIL width:(NSUInteger)width height:(NSUInteger)height mipmapped:NO];
            dd.textureType = MTLTextureType2DArray;
            dd.arrayLength = (NSUInteger)rt->layers;
            dd.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
            dd.storageMode = MTLStorageModePrivate;
            id<MTLTexture> darr = [vio_mtl.device newTextureWithDescriptor:dd];
            id<MTLTexture> carr = nil;
            if (!depth_only) {
                MTLTextureDescriptor *cd = [MTLTextureDescriptor
                    texture2DDescriptorWithPixelFormat:color_fmt width:(NSUInteger)width height:(NSUInteger)height mipmapped:NO];
                cd.textureType = MTLTextureType2DArray;
                cd.arrayLength = (NSUInteger)rt->layers;
                cd.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
                cd.storageMode = MTLStorageModePrivate;
                carr = [vio_mtl.device newTextureWithDescriptor:cd];
            }
            if (!darr || (!depth_only && !carr)) {
                php_error_docref(NULL, E_WARNING, "Metal: failed to create array render target (%dx%d, %d layers)",
                                 width, height, rt->layers);
                return -1;
            }
            rt->mip_levels = 1;
            if (carr) {
                rt->metal_color_textures[0] = (void *)CFBridgingRetain(carr);
                rt->metal_color_texture = rt->metal_color_textures[0];
                metal_rt_layered_msaa(rt, color_fmt, width, height);
            }
            rt->metal_depth_texture = (void *)CFBridgingRetain(darr);
            rt->backend_type = VIO_RT_BACKEND_METAL;
            metal_rt_initial_clear(rt);
            return 0;
        }
        if (rt->is_cube) {
            /* Cubemap colour attachment: one MTLTextureTypeCube with the full
             * mip chain when requested (generate_mipmaps fills it), plus ONE
             * shared 2D depth texture at face size — faces are rendered one at
             * a time (bind_render_target_face selects the slice). */
            if (rt->mip_levels < 1) rt->mip_levels = 1;
            MTLTextureDescriptor *cd = [MTLTextureDescriptor
                textureCubeDescriptorWithPixelFormat:color_fmt size:(NSUInteger)width
                                           mipmapped:(rt->mip_levels > 1 ? YES : NO)];
            cd.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
            cd.storageMode = MTLStorageModePrivate;
            id<MTLTexture> cube = [vio_mtl.device newTextureWithDescriptor:cd];
            /* Depth is a cube too, so VIO_RT_ALL_LAYERS can depth-test every
             * face in one pass; a face bind selects its slice. */
            MTLTextureDescriptor *dd = [MTLTextureDescriptor
                textureCubeDescriptorWithPixelFormat:VIO_METAL_DEPTH_STENCIL size:(NSUInteger)width mipmapped:NO];
            dd.usage = MTLTextureUsageRenderTarget;
            dd.storageMode = MTLStorageModePrivate;
            id<MTLTexture> cube_depth = [vio_mtl.device newTextureWithDescriptor:dd];
            if (!cube || !cube_depth) {
                php_error_docref(NULL, E_WARNING, "Metal: failed to create cube render target (%d, %d mips)",
                                 width, rt->mip_levels);
                return -1;
            }
            rt->mip_levels = (int)cube.mipmapLevelCount;
            rt->metal_color_texture = (void *)CFBridgingRetain(cube);
            rt->metal_color_textures[0] = rt->metal_color_texture;
            rt->metal_depth_texture = (void *)CFBridgingRetain(cube_depth);
            metal_rt_layered_msaa(rt, color_fmt, width, width);
            rt->backend_type = VIO_RT_BACKEND_METAL;
            metal_rt_initial_clear(rt);
            return 0;
        }

        /* Depth texture — always created (parallel to OpenGL's "always create
         * depth attachment" pattern so shadow-map RTs work uniformly). */
        /* depth_only + 'mipmaps' (A26): a Depth32Float chain (no stencil plane). */
        int depth_mips = depth_only && rt->mip_levels > 1 && samples <= 1;
        MTLTextureDescriptor *depth_desc = [MTLTextureDescriptor
            texture2DDescriptorWithPixelFormat:(depth_mips ? MTLPixelFormatDepth32Float : VIO_METAL_DEPTH_STENCIL)
            width:width height:height mipmapped:(depth_mips ? YES : NO)];
        depth_desc.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
        depth_desc.storageMode = MTLStorageModePrivate;
        if (samples > 1) {
            depth_desc.textureType = MTLTextureType2DMultisample;
            depth_desc.sampleCount = (NSUInteger)samples;
            depth_desc.usage = MTLTextureUsageRenderTarget;
        }

        id<MTLTexture> depth_tex = [vio_mtl.device newTextureWithDescriptor:depth_desc];
        if (!depth_tex) {
            php_error_docref(NULL, E_WARNING,
                "Metal: failed to create render-target depth texture (%dx%d)", width, height);
            return -1;
        }

        /* One colour texture per attachment (MRT), each with its MSAA partner
         * when samples > 1. Index 0 also lands in the legacy scalar fields. */
        if (!depth_only) {
            for (int i = 0; i < n_color; i++) {
                MTLPixelFormat fmt_i = metal_pixel_format(rt->formats[i]);
                MTLTextureDescriptor *color_desc = [MTLTextureDescriptor
                    texture2DDescriptorWithPixelFormat:fmt_i
                    width:width height:height mipmapped:NO];
                color_desc.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
                color_desc.storageMode = MTLStorageModePrivate;

                id<MTLTexture> color_tex = [vio_mtl.device newTextureWithDescriptor:color_desc];
                if (!color_tex) {
                    php_error_docref(NULL, E_WARNING,
                        "Metal: failed to create render-target color texture %d (%dx%d, format %d)",
                        i, width, height, rt->formats[i]);
                    return -1;
                }
                rt->metal_color_textures[i] = (void *)CFBridgingRetain(color_tex);
                if (samples > 1) {
                    MTLTextureDescriptor *ms_desc = [MTLTextureDescriptor
                        texture2DDescriptorWithPixelFormat:fmt_i
                        width:width height:height mipmapped:NO];
                    ms_desc.textureType = MTLTextureType2DMultisample;
                    ms_desc.sampleCount = (NSUInteger)samples;
                    ms_desc.usage = MTLTextureUsageRenderTarget;
                    ms_desc.storageMode = MTLStorageModePrivate;
                    id<MTLTexture> msaa_color = [vio_mtl.device newTextureWithDescriptor:ms_desc];
                    if (!msaa_color) {
                        php_error_docref(NULL, E_WARNING,
                            "Metal: failed to create %dx MSAA render-target texture (%dx%d)",
                            samples, width, height);
                        return -1;
                    }
                    rt->metal_msaa_color_textures[i] = (void *)CFBridgingRetain(msaa_color);
                }
            }
        }

        /* Index 0 doubles as the legacy scalar slot. */
        rt->metal_color_texture = rt->metal_color_textures[0];
        if (samples > 1) {
            rt->metal_msaa_color_texture = rt->metal_msaa_color_textures[0];
            rt->metal_msaa_depth_texture = (void *)CFBridgingRetain(depth_tex);
            rt->metal_depth_texture = NULL;  /* no single-sample depth to sample */
            if (depth_only) {
                /* The resolve target of the multisampled depth: what sampling and readback see. */
                MTLTextureDescriptor *rd = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:VIO_METAL_DEPTH_STENCIL
                                                width:(NSUInteger)width height:(NSUInteger)height mipmapped:NO];
                rd.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
                rd.storageMode = MTLStorageModePrivate;
                id<MTLTexture> resolved = [vio_mtl.device newTextureWithDescriptor:rd];
                if (!resolved) return -1;
                rt->metal_depth_texture = (void *)CFBridgingRetain(resolved);
            }
        } else {
            rt->metal_depth_texture = (void *)CFBridgingRetain(depth_tex);
        }
        rt->backend_type = VIO_RT_BACKEND_METAL;
        metal_rt_initial_clear(rt);

        return 0;
    }
}

static void metal_bind_render_target_at(vio_render_target_object *rt, int face, int level)
{
    @autoreleasepool {
        if (rt->backend_type != VIO_RT_BACKEND_METAL || !vio_mtl.initialized) return;

        current_bound_rt    = rt;
        current_bound_face  = face;
        current_bound_level = level;
        rt->bound_face  = face;
        rt->bound_level = level;

        /* If a frame is already in progress (caller bound mid-frame, e.g. 3D
         * pipeline doing HDR + bloom passes), close the encoder and reopen on
         * the new target with Load semantics so contents are preserved. The
         * common bind-before-vio_begin case (Engine::warmRender on the 2D
         * pipeline) skips this branch and lets metal_begin_frame pick up the
         * stash naturally on the next vio_begin. */
        if (vio_mtl.current_cmd_buf) {
            if (vio_mtl.current_encoder) {
                [vio_mtl.current_encoder endEncoding];
                vio_mtl.current_encoder = nil;
            }
            metal_open_encoder(/*load_clear=*/0);
        }
    }
}

static void metal_bind_render_target(void *rt_ptr)
{
    vio_render_target_object *rt = (vio_render_target_object *)rt_ptr;
    /* A cube / array RT bound without an explicit face renders into +X /
     * layer 0, level 0. */
    metal_bind_render_target_at(rt, (rt->is_cube || rt->layers > 1) ? 0 : -1, 0);
}

static int metal_bind_render_target_face(void *rt_ptr, int face, int level)
{
    vio_render_target_object *rt = (vio_render_target_object *)rt_ptr;
    if (!rt || (!rt->is_cube && rt->layers <= 1)) return -1;
    if (face == VIO_RT_ALL_LAYERS) {
        if (level != 0) return -1;
    } else if (face < 0 || face >= vio_rt_layer_count(rt) || level < 0 || level >= rt->mip_levels) {
        return -1;
    }
    metal_bind_render_target_at(rt, face, level);
    return 0;
}

static void metal_unbind_render_target(unsigned int default_fbo, int width, int height)
{
    (void)default_fbo; (void)width; (void)height;
    @autoreleasepool {
        if (!vio_mtl.initialized) return;

        /* State mutation must run regardless of frame state, otherwise the
         * stash keeps a dangling pointer to a release()'d RT object and the
         * next metal_begin_frame opens an encoder on freed Metal textures.
         * Symptom: every-few-frames flicker after a warmRender pass. */
        current_bound_rt    = NULL;
        current_bound_face  = -1;
        current_bound_level = 0;

        /* Encoder swap is only meaningful inside an active frame. Outside one
         * (the common case after warmRender, where vio_end already committed
         * the cmd buffer), there's nothing to reopen — the next vio_begin
         * will pick up the cleared stash and open on the swapchain. */
        if (!vio_mtl.current_cmd_buf) return;

        if (vio_mtl.current_encoder) {
            [vio_mtl.current_encoder endEncoding];
            vio_mtl.current_encoder = nil;
        }

        /* Restore swapchain — load existing contents so previously drawn
         * geometry isn't wiped when the consumer bounces between RT and
         * swapchain inside a single frame. current_bound_rt is already NULL
         * from the early state-mutation at the top of this function. */
        metal_open_encoder(/*load_clear=*/0);
    }
}

static void metal_destroy_render_target(void *rt_ptr)
{
    vio_render_target_object *rt = (vio_render_target_object *)rt_ptr;
    if (rt->backend_type != VIO_RT_BACKEND_METAL) return;

    /* If this RT was bound at destroy time, drop the stash so begin_frame
     * doesn't reach into a freed texture. */
    if (current_bound_rt == rt) {
        current_bound_rt = NULL;
    }

    /* MRT attachments 1..n; index 0 is released through the scalar below. */
    for (int i = 1; i < VIO_MAX_COLOR_ATTACHMENTS; i++) {
        if (rt->metal_color_textures[i])         { CFBridgingRelease(rt->metal_color_textures[i]);         rt->metal_color_textures[i] = NULL; }
        if (rt->metal_msaa_color_textures[i])    { CFBridgingRelease(rt->metal_msaa_color_textures[i]);    rt->metal_msaa_color_textures[i] = NULL; }
        if (rt->metal_color_backend_textures[i]) { metal_destroy_texture(rt->metal_color_backend_textures[i]); rt->metal_color_backend_textures[i] = NULL; }
    }
    rt->metal_color_textures[0] = NULL;
    rt->metal_msaa_color_textures[0] = NULL;
    rt->metal_color_backend_textures[0] = NULL;
    if (rt->metal_color_texture) {
        CFBridgingRelease(rt->metal_color_texture);
        rt->metal_color_texture = NULL;
    }
    if (rt->metal_depth_texture) {
        CFBridgingRelease(rt->metal_depth_texture);
        rt->metal_depth_texture = NULL;
    }
    if (rt->metal_msaa_color_texture) {
        CFBridgingRelease(rt->metal_msaa_color_texture);
        rt->metal_msaa_color_texture = NULL;
    }
    if (rt->metal_msaa_depth_texture) {
        CFBridgingRelease(rt->metal_msaa_depth_texture);
        rt->metal_msaa_depth_texture = NULL;
    }
    /* vio_render_target_texture() wrappers (each holds its own +1 on the
     * MTLTexture, so the order relative to the releases above is irrelevant). */
    if (rt->metal_color_backend_texture) {
        metal_destroy_texture(rt->metal_color_backend_texture);
        rt->metal_color_backend_texture = NULL;
    }
    if (rt->metal_depth_backend_texture) {
        metal_destroy_texture(rt->metal_depth_backend_texture);
        rt->metal_depth_backend_texture = NULL;
    }
}

/* ═══════════════════════════════════════════════════════════════════
 * 3D graphics pipeline
 *
 * Mirrors the D3D11 / D3D12 contract: the PHP layer keeps mesh data in
 * backend_vb / backend_ib (create_buffer), compiles shaders through
 * compile_shader, reflects the default uniform block into sh->cbuffer_data and
 * issues draw / draw_indexed with the currently bound pipeline.
 *
 * Metal specifics that shape this code:
 *   - MTLRenderPipelineState is monolithic: it bakes vertex layout (incl. the
 *     mesh stride), color/depth attachment formats and blend state. One
 *     vio_pipeline therefore owns a small cache of PSO variants keyed by
 *     (mesh stride, target color format) — swapchain BGRA8, HDR RGBA16F and
 *     depth-only targets each get their own PSO on first use.
 *   - There is no buffer renaming (D3D11 MAP_WRITE_DISCARD). Draws inside one
 *     command buffer all read a buffer at EXECUTION time, so uniform data
 *     rewritten between two draws would feed both draws the last write. Every
 *     draw therefore copies the shader's cbuffer shadow — and instance matrices
 *     — into a fresh slice of a per-frame transient ring, exactly the linear
 *     allocator vio_d3d12 uses for its cbuffer / instance heaps.
 *   - Shader resources are pinned to MSL indices at compile time (see
 *     metal_gfx_spirv_to_msl); the vertex-data buffers live at the reserved
 *     indices 29 (instances) / 30 (mesh) so they can never collide.
 * ═══════════════════════════════════════════════════════════════════ */

/* ── Per-frame transient ring ─────────────────────────────────────── */

#define VIO_METAL_RING_FRAMES     3          /* == CAMetalLayer maximumDrawableCount */
#define VIO_METAL_RING_MIN_BYTES  (1u << 20)
#define VIO_METAL_RING_ALIGN      256        /* setVertexBuffer:offset: alignment on macOS */

typedef struct _vio_metal_ring {
    id<MTLBuffer>        buf;        /* current chunk */
    NSMutableArray      *retired;    /* chunks outgrown mid-frame; still referenced by recorded draws */
    NSUInteger           capacity;
    NSUInteger           offset;
    NSUInteger           used_total; /* bytes handed out this frame (drives the next grow) */
    id<MTLCommandBuffer> fence;      /* command buffer that read this ring — wait before reuse */
} vio_metal_ring;

static vio_metal_ring  metal_rings[VIO_METAL_RING_FRAMES];
static int             metal_ring_index = 0;
static vio_metal_ring *metal_ring_cur = NULL;

static void metal_ring_begin_frame(void)
{
    metal_ring_index = (metal_ring_index + 1) % VIO_METAL_RING_FRAMES;
    vio_metal_ring *r = &metal_rings[metal_ring_index];

    /* The GPU may still be reading this ring's slices from three frames ago. */
    if (r->fence) {
        [r->fence waitUntilCompleted];
        r->fence = nil;
    }
    if (r->retired) {
        [r->retired removeAllObjects];
    }
    /* Grow up-front to last time's demand so a busy frame allocates once. */
    NSUInteger want = r->used_total > VIO_METAL_RING_MIN_BYTES ? r->used_total : VIO_METAL_RING_MIN_BYTES;
    if (!r->buf || r->capacity < want) {
        r->buf = [vio_mtl.device newBufferWithLength:want options:MTLResourceStorageModeShared];
        r->capacity = r->buf ? want : 0;
    }
    r->offset = 0;
    r->used_total = 0;
    metal_ring_cur = r;
}

static void metal_ring_end_frame(id<MTLCommandBuffer> cb)
{
    if (metal_ring_cur) {
        metal_ring_cur->fence = cb;
        metal_ring_cur = NULL;
    }
}

static void metal_ring_shutdown(void)
{
    for (int i = 0; i < VIO_METAL_RING_FRAMES; i++) {
        vio_metal_ring *r = &metal_rings[i];
        if (r->fence) [r->fence waitUntilCompleted];
        r->fence = nil;
        r->buf = nil;
        r->retired = nil;
        r->capacity = r->offset = r->used_total = 0;
    }
    metal_ring_cur = NULL;
}

/* Carve `size` bytes out of the current frame's ring. Returns the chunk and the
 * 256-aligned byte offset to bind; nil when no frame is open or on OOM. */
static id<MTLBuffer> metal_ring_alloc(NSUInteger size, NSUInteger *out_offset)
{
    vio_metal_ring *r = metal_ring_cur;
    if (!r || size == 0 || !vio_mtl.device) return nil;

    NSUInteger off = (r->offset + VIO_METAL_RING_ALIGN - 1) & ~(NSUInteger)(VIO_METAL_RING_ALIGN - 1);
    if (!r->buf || off + size > r->capacity) {
        /* Outgrown: retire the chunk — draws already recorded still point into
         * it, so it must survive until this ring's fence — and start bigger. */
        NSUInteger cap = r->capacity * 2;
        if (cap < size) cap = size;
        if (cap < VIO_METAL_RING_MIN_BYTES) cap = VIO_METAL_RING_MIN_BYTES;
        id<MTLBuffer> nb = [vio_mtl.device newBufferWithLength:cap options:MTLResourceStorageModeShared];
        if (!nb) return nil;
        if (r->buf) {
            if (!r->retired) r->retired = [NSMutableArray new];
            [r->retired addObject:r->buf];
        }
        r->buf = nb;
        r->capacity = cap;
        off = 0;
    }
    r->offset = off + size;
    r->used_total += size + VIO_METAL_RING_ALIGN;
    if (out_offset) *out_offset = off;
    return r->buf;
}

/* ── Buffers (vertex / index / uniform / storage) ─────────────────── */

typedef struct _vio_metal_buffer {
    void  *buffer;      /* id<MTLBuffer>, +1 retained via CFBridgingRetain; NULL for UNIFORM */
    size_t size;        /* bytes */
    int    stride;      /* element stride (informational; raw float access ignores it) */
    int    type;        /* vio_buffer_type */
    /* UNIFORM buffers keep their contents CPU-side only and are uploaded into a
     * fresh ring slice on every bind (see file header: no buffer renaming). */
    unsigned char *shadow;
    size_t         shadow_size;
} vio_metal_buffer;

/* The compute path predates the generalised buffer; keep its name as an alias. */
typedef vio_metal_buffer vio_metal_compute_buffer;

static void *metal_create_buffer(vio_buffer_desc *desc)
{
    if (!desc || !vio_mtl.device || desc->size == 0) return NULL;

    vio_metal_buffer *buf = calloc(1, sizeof(vio_metal_buffer));
    if (!buf) return NULL;
    buf->size   = desc->size;
    buf->stride = desc->stride;
    buf->type   = (int)desc->type;

    if (desc->type == VIO_BUFFER_UNIFORM) {
        buf->shadow_size = (desc->size + 15) & ~(size_t)15;
        buf->shadow = calloc(1, buf->shadow_size);
        if (!buf->shadow) {
            free(buf);
            return NULL;
        }
        if (desc->data) memcpy(buf->shadow, desc->data, desc->size);
        return buf;
    }

    @autoreleasepool {
        /* Shared: CPU-visible for seeding (input) and readback (storage output)
         * without a blit; coherent on Apple silicon. */
        id<MTLBuffer> mbuf;
        if (desc->data) {
            mbuf = [vio_mtl.device newBufferWithBytes:desc->data
                                               length:desc->size
                                              options:MTLResourceStorageModeShared];
        } else {
            mbuf = [vio_mtl.device newBufferWithLength:desc->size
                                              options:MTLResourceStorageModeShared];
            /* newBufferWithLength does not guarantee zeroed contents. */
            if (mbuf) memset([mbuf contents], 0, desc->size);
        }
        if (!mbuf) {
            php_error_docref(NULL, E_WARNING, "Metal: buffer allocation failed (%zu bytes)", desc->size);
            free(buf);
            return NULL;
        }
        buf->buffer = (void *)CFBridgingRetain(mbuf);
    }
    return buf;
}

static void metal_update_buffer(void *buffer_ptr, const void *data, size_t size, size_t offset)
{
    vio_metal_buffer *buf = (vio_metal_buffer *)buffer_ptr;
    if (!buf || !data || size == 0) return;

    if (buf->type == VIO_BUFFER_UNIFORM) {
        if (!buf->shadow) return;
        if (offset >= buf->shadow_size) return;
        memcpy(buf->shadow + offset, data, size < buf->shadow_size - offset ? size : buf->shadow_size - offset);
        return;
    }
    if (!buf->buffer) return;
    @autoreleasepool {
        id<MTLBuffer> mb = (__bridge id<MTLBuffer>)buf->buffer;
        if (offset >= buf->size) return;
        size_t n = size < buf->size - offset ? size : buf->size - offset;
        memcpy((char *)[mb contents] + offset, data, n);
    }
}

static void metal_destroy_buffer(void *buffer_ptr)
{
    vio_metal_buffer *buf = (vio_metal_buffer *)buffer_ptr;
    if (!buf) return;
    if (buf->buffer) { CFRelease((CFTypeRef)buf->buffer); buf->buffer = NULL; }
    if (buf->shadow) { free(buf->shadow); buf->shadow = NULL; }
    free(buf);
}

/* VioBuffer free handler path (vio_buffer.c -> destroy_buffer_obj). */
static void metal_destroy_buffer_obj(void *buf_ptr)
{
    vio_buffer_object *bo = (vio_buffer_object *)buf_ptr;
    if (!bo || !bo->backend_buffer) return;
    metal_destroy_buffer(bo->backend_buffer);
    bo->backend_buffer = NULL;
}

/* Upload a UNIFORM buffer's shadow into a fresh ring slice and bind it to the
 * given stage indices (-1 = skip that stage). */
static void metal_bind_uniform_slice(vio_metal_buffer *ub, int vs_index, int fs_index)
{
    if (!ub || !ub->shadow || !vio_mtl.current_encoder) return;
    if (vs_index < 0 && fs_index < 0) return;

    NSUInteger off = 0;
    id<MTLBuffer> rb = metal_ring_alloc(ub->shadow_size, &off);
    if (!rb) return;
    memcpy((char *)[rb contents] + off, ub->shadow, ub->shadow_size);
    if (vs_index >= 0) [vio_mtl.current_encoder setVertexBuffer:rb offset:off atIndex:(NSUInteger)vs_index];
    if (fs_index >= 0) [vio_mtl.current_encoder setFragmentBuffer:rb offset:off atIndex:(NSUInteger)fs_index];
}

/* ── Textures ─────────────────────────────────────────────────────── */

typedef struct _vio_metal_texture {
    void        *tex;          /* id<MTLTexture>, +1 retained */
    void        *sampler;      /* id<MTLSamplerState>, +1 retained */
    void        *sampler_cmp;  /* id<MTLSamplerState> with compare func; built lazily for sampler2DShadow */
    unsigned int registry_id;  /* slot in metal_textures[] so the 2D batch can bind it too; 0 = none */
    int          width, height, depth;
    int          filter, wrap, mipmaps;
    int          anisotropy;   /* vio_texture(['anisotropy' => N]), 1 = off (GAP-PHASE5 Block 11) */
} vio_metal_texture;

#define VIO_METAL_WRAP_BORDER_WHITE 100  /* internal: clamp to opaque-white border (shadow maps) */

static id<MTLSamplerState> metal_make_sampler(int filter, int wrap, int mipmaps, int compare, int anisotropy)
{
    MTLSamplerDescriptor *sd = [[MTLSamplerDescriptor alloc] init];
    /* vio_texture(['anisotropy' => 1..16]): like D3D / GL only with a linear
     * filter and never on comparison samplers. */
    if (anisotropy > 1 && filter != VIO_FILTER_NEAREST && !compare) {
        sd.maxAnisotropy = (NSUInteger)(anisotropy > 16 ? 16 : anisotropy);
    }
    MTLSamplerMinMagFilter f = (filter == VIO_FILTER_NEAREST) ? MTLSamplerMinMagFilterNearest
                                                              : MTLSamplerMinMagFilterLinear;
    sd.minFilter = f;
    sd.magFilter = f;
    sd.mipFilter = mipmaps ? MTLSamplerMipFilterLinear : MTLSamplerMipFilterNotMipmapped;
    MTLSamplerAddressMode am;
    switch (wrap) {
        case VIO_WRAP_CLAMP:  am = MTLSamplerAddressModeClampToEdge; break;
        case VIO_WRAP_MIRROR: am = MTLSamplerAddressModeMirrorRepeat; break;
        case VIO_METAL_WRAP_BORDER_WHITE:
            /* Texels outside the shadow map compare as "lit" (depth 1.0) —
             * the D3D11 RT wrapper uses ADDRESS_BORDER with a white border. */
            am = MTLSamplerAddressModeClampToBorderColor;
            sd.borderColor = MTLSamplerBorderColorOpaqueWhite;
            break;
        default:              am = MTLSamplerAddressModeRepeat; break;
    }
    sd.sAddressMode = am;
    sd.tAddressMode = am;
    sd.rAddressMode = am;
    if (compare) {
        /* sampler2DShadow: GL's default GL_LEQUAL comparison. */
        sd.compareFunction = MTLCompareFunctionLessEqual;
    }
    return [vio_mtl.device newSamplerStateWithDescriptor:sd];
}

static vio_metal_texture *metal_wrap_texture_aniso(id<MTLTexture> tex, int filter, int wrap, int mipmaps, int register_2d, int anisotropy);

static vio_metal_texture *metal_wrap_texture(id<MTLTexture> tex, int filter, int wrap, int mipmaps, int register_2d)
{
    return metal_wrap_texture_aniso(tex, filter, wrap, mipmaps, register_2d, 1);
}

static vio_metal_texture *metal_wrap_texture_aniso(id<MTLTexture> tex, int filter, int wrap, int mipmaps, int register_2d, int anisotropy)
{
    vio_metal_texture *t = calloc(1, sizeof(vio_metal_texture));
    if (!t) return NULL;
    t->tex     = (void *)CFBridgingRetain(tex);
    t->width   = (int)tex.width;
    t->height  = (int)tex.height;
    t->depth   = tex.textureType == MTLTextureType3D ? (int)tex.depth : 0;
    t->filter  = filter;
    t->wrap    = wrap;
    t->mipmaps = mipmaps;
    t->anisotropy = anisotropy;
    id<MTLSamplerState> s = metal_make_sampler(filter, wrap, mipmaps, 0, anisotropy);
    t->sampler = s ? (void *)CFBridgingRetain(s) : NULL;
    if (register_2d) {
        t->registry_id = metal_register_texture(tex);
    }
    return t;
}

static id<MTLSamplerState> metal_texture_cmp_sampler(vio_metal_texture *t)
{
    if (!t->sampler_cmp) {
        id<MTLSamplerState> s = metal_make_sampler(t->filter, t->wrap, 0, 1, 1);
        t->sampler_cmp = s ? (void *)CFBridgingRetain(s) : NULL;
    }
    return (__bridge id<MTLSamplerState>)t->sampler_cmp;
}

static MTLPixelFormat metal_texfmt(int fmt)
{
    switch (fmt) {
        case VIO_FORMAT_BC1: return MTLPixelFormatBC1_RGBA;
        case VIO_FORMAT_BC3: return MTLPixelFormatBC3_RGBA;
        case VIO_FORMAT_BC4: return MTLPixelFormatBC4_RUnorm;
        case VIO_FORMAT_BC5: return MTLPixelFormatBC5_RGUnorm;
        case VIO_FORMAT_BC7: return MTLPixelFormatBC7_RGBAUnorm;
        case VIO_FORMAT_ASTC_4x4: return MTLPixelFormatASTC_4x4_LDR;
        case VIO_FORMAT_ASTC_5x5: return MTLPixelFormatASTC_5x5_LDR;
        case VIO_FORMAT_ASTC_6x6: return MTLPixelFormatASTC_6x6_LDR;
        case VIO_FORMAT_ASTC_8x8: return MTLPixelFormatASTC_8x8_LDR;
        case VIO_FORMAT_R8:  return MTLPixelFormatR8Unorm;
        default:             return MTLPixelFormatRGBA8Unorm;
    }
}

/* BC formats exist on every Mac GPU; the macOS 11 query confirms it where available. */
static int metal_supports_bc(void)
{
    if (!vio_mtl.device) return 0;
    if ([vio_mtl.device respondsToSelector:@selector(supportsBCTextureCompression)]) {
        return [vio_mtl.device supportsBCTextureCompression] ? 1 : 0;
    }
    return 1;
}

/* Texture arrays / block-compressed data / explicit mip chains (GAP-PHASE5
 * Block 9): MTLTextureType2DArray for layers > 1, one replaceRegion per
 * (level, layer) from the level-major payload; a single uncompressed level with
 * `mipmaps` is completed by the blit encoder. Arrays stay out of the 2D sprite
 * registry (the 2D batch samples texture2d only). */
static void *metal_create_texture_ex(vio_texture_desc *desc)
{
    int layers = desc->layers > 1 ? desc->layers : 1;
    int levels = desc->mip_levels > 1 ? desc->mip_levels : 1;
    int compressed = vio_texfmt_is_compressed(desc->format);
    int gen = !compressed && desc->mipmaps && levels == 1;
    if (vio_texfmt_is_astc(desc->format) ? vio_mtl.caps.apple_family < 2 : (compressed && !metal_supports_bc())) return NULL;
    @autoreleasepool {
        MTLTextureDescriptor *td = [[MTLTextureDescriptor alloc] init];
        td.textureType = layers > 1 ? MTLTextureType2DArray : MTLTextureType2D;
        td.pixelFormat = metal_texfmt(desc->format);
        td.width = (NSUInteger)desc->width;
        td.height = (NSUInteger)desc->height;
        td.arrayLength = (NSUInteger)layers;
        td.mipmapLevelCount = (NSUInteger)(gen ? vio_texfmt_full_mip_count(desc->width, desc->height) : levels);
        td.usage = MTLTextureUsageShaderRead;
        td.storageMode = metal_cpu_texture_storage();
        id<MTLTexture> tex = [vio_mtl.device newTextureWithDescriptor:td];
        if (!tex) return NULL;

        const uint8_t *p = (const uint8_t *)desc->data;
        int lw = desc->width, lh = desc->height;
        for (int l = 0; l < levels; l++) {
            NSUInteger pitch = (NSUInteger)vio_texfmt_row_pitch(desc->format, lw);
            size_t image = vio_texfmt_image_size(desc->format, lw, lh);
            for (int a = 0; a < layers; a++) {
                [tex replaceRegion:MTLRegionMake2D(0, 0, (NSUInteger)lw, (NSUInteger)lh)
                       mipmapLevel:(NSUInteger)l slice:(NSUInteger)a withBytes:p bytesPerRow:pitch bytesPerImage:0];
                p += image;
            }
            lw = lw > 1 ? lw / 2 : 1;
            lh = lh > 1 ? lh / 2 : 1;
        }
        if (gen && tex.mipmapLevelCount > 1) {
            id<MTLCommandBuffer> cb = metal_new_command_buffer();
            id<MTLBlitCommandEncoder> blit = [cb blitCommandEncoder];
            [blit generateMipmapsForTexture:tex];
            [blit endEncoding];
            [cb commit];
            [cb waitUntilCompleted];
        }
        return metal_wrap_texture_aniso(tex, (int)desc->filter, (int)desc->wrap, (levels > 1 || gen) ? 1 : 0,
                                        layers > 1 ? 0 : 1, desc->anisotropy);
    }
}

static void *metal_create_texture(vio_texture_desc *desc)
{
    if (!desc || !desc->data || !vio_mtl.device || desc->width <= 0 || desc->height <= 0) return NULL;
    if (desc->layers > 1 || desc->mip_levels > 1 || vio_texfmt_is_compressed(desc->format)) {
        return metal_create_texture_ex(desc);
    }

    @autoreleasepool {
        MTLPixelFormat fmt = desc->single_channel ? MTLPixelFormatR8Unorm : MTLPixelFormatRGBA8Unorm;
        MTLTextureDescriptor *td = [MTLTextureDescriptor
            texture2DDescriptorWithPixelFormat:fmt
            width:desc->width height:desc->height mipmapped:desc->mipmaps ? YES : NO];
        td.usage = MTLTextureUsageShaderRead | (desc->storage ? MTLTextureUsageShaderWrite : 0);
        td.storageMode = metal_cpu_texture_storage();
        if (desc->single_channel) {
            /* R8 coverage atlas sampled as (1,1,1,R), same as vio_metal_create_font_atlas. */
            td.swizzle = MTLTextureSwizzleChannelsMake(MTLTextureSwizzleOne, MTLTextureSwizzleOne,
                                                       MTLTextureSwizzleOne, MTLTextureSwizzleRed);
        }
        id<MTLTexture> tex = [vio_mtl.device newTextureWithDescriptor:td];
        if (!tex) return NULL;

        NSUInteger bpr = (NSUInteger)desc->width * (desc->single_channel ? 1 : 4);
        [tex replaceRegion:MTLRegionMake2D(0, 0, desc->width, desc->height)
               mipmapLevel:0 withBytes:desc->data bytesPerRow:bpr];

        if (desc->mipmaps && tex.mipmapLevelCount > 1) {
            /* Own command buffer: committed before any frame that samples it. */
            id<MTLCommandBuffer> cb = metal_new_command_buffer();
            id<MTLBlitCommandEncoder> blit = [cb blitCommandEncoder];
            [blit generateMipmapsForTexture:tex];
            [blit endEncoding];
            [cb commit];
            [cb waitUntilCompleted];
        }

        return metal_wrap_texture_aniso(tex, (int)desc->filter, (int)desc->wrap, desc->mipmaps, 1, desc->anisotropy);
    }
}

static void *metal_create_texture_3d(vio_texture_desc *desc)
{
    if (!desc || !desc->data || !vio_mtl.device ||
        desc->width <= 0 || desc->height <= 0 || desc->depth <= 0) return NULL;

    @autoreleasepool {
        MTLTextureDescriptor *td = [[MTLTextureDescriptor alloc] init];
        td.textureType = MTLTextureType3D;
        td.pixelFormat = MTLPixelFormatRGBA8Unorm;
        td.width  = desc->width;
        td.height = desc->height;
        td.depth  = desc->depth;
        td.mipmapLevelCount = 1;
        td.usage = MTLTextureUsageShaderRead | (desc->storage ? MTLTextureUsageShaderWrite : 0);
        td.storageMode = metal_cpu_texture_storage();
        id<MTLTexture> tex = [vio_mtl.device newTextureWithDescriptor:td];
        if (!tex) return NULL;

        [tex replaceRegion:MTLRegionMake3D(0, 0, 0, desc->width, desc->height, desc->depth)
               mipmapLevel:0 slice:0 withBytes:desc->data
               bytesPerRow:(NSUInteger)desc->width * 4
             bytesPerImage:(NSUInteger)desc->width * desc->height * 4];

        /* Volumes are never drawn by the 2D batch — skip the registry. */
        return metal_wrap_texture_aniso(tex, (int)desc->filter, (int)desc->wrap, 0, 0, desc->anisotropy);
    }
}

static void metal_destroy_texture(void *texture)
{
    vio_metal_texture *t = (vio_metal_texture *)texture;
    if (!t) return;
    if (t->registry_id) vio_metal_delete_texture(t->registry_id);
    if (t->tex)         CFRelease((CFTypeRef)t->tex);
    if (t->sampler)     CFRelease((CFTypeRef)t->sampler);
    if (t->sampler_cmp) CFRelease((CFTypeRef)t->sampler_cmp);
    free(t);
}

unsigned int vio_metal_texture_registry_id(void *backend_texture)
{
    vio_metal_texture *t = (vio_metal_texture *)backend_texture;
    return t ? t->registry_id : 0;
}

void *vio_metal_wrap_rt_texture(void *cf_retained_texture, int depth_only)
{
    if (!cf_retained_texture || !vio_mtl.device) return NULL;
    @autoreleasepool {
        id<MTLTexture> tex = (__bridge id<MTLTexture>)cf_retained_texture;
        /* Nearest + clamp, like the D3D11 RT wrapper: a shadow map must not
         * bilinear-blend depth, and postprocess blits sample 1:1. The wrapper
         * takes its own +1 on the texture (CFBridgingRetain in metal_wrap_texture)
         * so the RT and the wrapper can be torn down in either order. */
        vio_metal_texture *t = metal_wrap_texture(tex, VIO_FILTER_NEAREST,
                                                  depth_only ? VIO_METAL_WRAP_BORDER_WHITE : VIO_WRAP_CLAMP,
                                                  0, 0);
        if (t && depth_only) {
            (void)metal_texture_cmp_sampler(t);
        }
        return t;
    }
}

/* ── Pipeline (PSO variant cache + fixed-function state) ──────────── */

#define VIO_METAL_PSO_VARIANTS 8

typedef struct _vio_metal_pso_variant {
    int   color_fmts[VIO_MAX_COLOR_ATTACHMENTS]; /* MTLPixelFormat per attachment */
    int   color_count; /* 0 for depth-only targets */
    int   stride;      /* mesh vertex stride baked into the vertex descriptor */
    uint32_t layout;   /* vio_mesh_layout.key whose offsets the descriptor uses (0 = dense) */
    int   samples;     /* raster sample count of the target (1 or the RT's MSAA count) */
    int   has_depth;   /* 0 when the target has no depth attachment (cube-RT mip > 0) */
    int   has_stencil; /* depth attachment carries stencil */
    void *pso;         /* id<MTLRenderPipelineState>, +1 retained */
} vio_metal_pso_variant;

/* Layout of the mesh being drawn (apply_mesh_layout, OPEN-ITEMS-PLAN A31): the
 * vertex descriptors read each input at the mesh's offset. The emulated
 * geometry stage bakes dense offsets into its vertex kernel and keeps them. */
static vio_mesh_layout metal_mesh_layout;

static void metal_apply_mesh_layout(const vio_mesh_layout *ml)
{
    if (ml) metal_mesh_layout = *ml;
    else metal_mesh_layout.key = 0;
}

typedef struct _vio_metal_pipeline {
    vio_metal_shader *shader;       /* borrowed — the VioPipeline holds a strong ref to its VioShader */
    void            *vert_fn;       /* +1 retained copies so a PSO can be built after the shader is gone */
    void            *frag_fn;
    void            *frag_fn_noout; /* depth-only variant, may be NULL */
    void            *depth_state;   /* id<MTLDepthStencilState>, +1 retained (with the stencil test) */
    void            *depth_state_nostencil; /* same depth test, stencil off: targets without a stencil plane */
    uint32_t         stencil_ref;
    MTLPrimitiveType primitive;
    MTLCullMode      cull;
    vio_blend_mode   blend;
    int              color_mask;    /* VIO_COLOR_* bits */
    int              per_attachment; /* attachment_blend[] / attachment_mask[] override blend / color_mask per attachment */
    int              attachment_blend[VIO_MAX_COLOR_ATTACHMENTS];
    int              attachment_mask[VIO_MAX_COLOR_ATTACHMENTS];
    float            depth_bias;
    float            slope_scaled_depth_bias;
    int              patch_vertices; /* input control points per patch (tessellation shaders) */
    vio_topology     topology;       /* primitive assembly of an emulated geometry stage */
    vio_metal_pso_variant variants[VIO_METAL_PSO_VARIANTS];
    int                   variant_count;
} vio_metal_pipeline;

static vio_metal_pipeline *metal_current_pipeline = NULL;

/* The bound shader's default cbuffers, staged by vio_bind_pipeline through
 * vio_metal_set_shader_cbuffers(); uploaded per draw in metal_prepare_draw. */
static vio_metal_buffer *metal_current_vs_cb = NULL;
static vio_metal_buffer *metal_current_fs_cb = NULL;

/* Vertex-stage storage buffer staged by bind_storage_buffer (Path B). */
static vio_metal_buffer *metal_pending_storage = NULL;
static int               metal_pending_storage_binding = -1;

/* One identity mat4 bound at VIO_METAL_VB_INSTANCE for non-instanced draws of
 * shaders that declare location 3..6 — otherwise the vertex fetch reads an
 * unbound slot (D3D11 has the same identity_instance_buf). */
static id<MTLBuffer> metal_identity_instance = nil;

/* Constant blocks of the tessellation stages for the next draw, copied by
 * bind_stage_constants (vio_push_extra_stage_constants runs right before every
 * draw). The TCS kernel takes them via setBytes, the TES as a ring slice. */
static unsigned char metal_stage_const[VIO_EXTRA_STAGE_COUNT][VIO_CBUFFER_SIZE];
static size_t        metal_stage_const_size[VIO_EXTRA_STAGE_COUNT];

static void metal_bind_stage_constants(int stage, void *backend_buffer, const void *data, size_t size)
{
    (void)backend_buffer;
    int i = VIO_EXTRA_STAGE_INDEX(stage);
    if (i < 0 || i >= VIO_EXTRA_STAGE_COUNT || !data) return;
    if (size > VIO_CBUFFER_SIZE) size = VIO_CBUFFER_SIZE;
    memcpy(metal_stage_const[i], data, size);
    /* MSL structs round up to 16 bytes; the tail stays zero. */
    if (size < VIO_CBUFFER_SIZE) memset(metal_stage_const[i] + size, 0, ((size + 15) & ~(size_t)15) - size);
    metal_stage_const_size[i] = (size + 15) & ~(size_t)15;
}

static MTLPrimitiveTopologyClass metal_topology_class(MTLPrimitiveType t)
{
    switch (t) {
        case MTLPrimitiveTypePoint:     return MTLPrimitiveTopologyClassPoint;
        case MTLPrimitiveTypeLine:
        case MTLPrimitiveTypeLineStrip: return MTLPrimitiveTopologyClassLine;
        default:                        return MTLPrimitiveTopologyClassTriangle;
    }
}

static MTLCompareFunction metal_compare(int f)
{
    switch (f) {
        case VIO_CMP_NEVER:    return MTLCompareFunctionNever;
        case VIO_CMP_LESS:     return MTLCompareFunctionLess;
        case VIO_CMP_EQUAL:    return MTLCompareFunctionEqual;
        case VIO_CMP_LEQUAL:   return MTLCompareFunctionLessEqual;
        case VIO_CMP_GREATER:  return MTLCompareFunctionGreater;
        case VIO_CMP_NOTEQUAL: return MTLCompareFunctionNotEqual;
        case VIO_CMP_GEQUAL:   return MTLCompareFunctionGreaterEqual;
        default:               return MTLCompareFunctionAlways;
    }
}

static MTLStencilOperation metal_stencil_op(int op)
{
    switch (op) {
        case VIO_STENCIL_ZERO:      return MTLStencilOperationZero;
        case VIO_STENCIL_REPLACE:   return MTLStencilOperationReplace;
        case VIO_STENCIL_INCR:      return MTLStencilOperationIncrementClamp;
        case VIO_STENCIL_DECR:      return MTLStencilOperationDecrementClamp;
        case VIO_STENCIL_INVERT:    return MTLStencilOperationInvert;
        case VIO_STENCIL_INCR_WRAP: return MTLStencilOperationIncrementWrap;
        case VIO_STENCIL_DECR_WRAP: return MTLStencilOperationDecrementWrap;
        default:                    return MTLStencilOperationKeep;
    }
}

static MTLPrimitiveType metal_topology(vio_topology t)
{
    switch (t) {
        case VIO_TRIANGLE_STRIP: return MTLPrimitiveTypeTriangleStrip;
        case VIO_LINES:          return MTLPrimitiveTypeLine;
        case VIO_LINE_STRIP:     return MTLPrimitiveTypeLineStrip;
        case VIO_POINTS:         return MTLPrimitiveTypePoint;
        case VIO_TRIANGLE_FAN:   /* no native fan, same fallback as D3D */
        case VIO_TRIANGLES:
        default:                 return MTLPrimitiveTypeTriangle;
    }
}

static MTLVertexFormat metal_vertex_format(int components)
{
    switch (components) {
        case 1:  return MTLVertexFormatFloat;
        case 2:  return MTLVertexFormatFloat2;
        case 4:  return MTLVertexFormatFloat4;
        default: return MTLVertexFormatFloat3;
    }
}

static void *metal_create_pipeline(vio_pipeline_desc *desc)
{
    if (!desc || !desc->shader || !vio_mtl.device) return NULL;
    vio_metal_shader *sh = (vio_metal_shader *)desc->shader;

    vio_metal_pipeline *p = calloc(1, sizeof(vio_metal_pipeline));
    if (!p) return NULL;

    @autoreleasepool {
        p->shader  = sh;
        if (sh->vert_fn) p->vert_fn = (void *)CFRetain((CFTypeRef)sh->vert_fn);
        if (sh->frag_fn) p->frag_fn = (void *)CFRetain((CFTypeRef)sh->frag_fn);
        if (sh->frag_fn_noout) p->frag_fn_noout = (void *)CFRetain((CFTypeRef)sh->frag_fn_noout);
        p->primitive = metal_topology(desc->topology);
        p->topology = desc->topology;
        if (sh->tess_emul) {
            p->primitive = sh->tess_emul_domain == VIO_MSL_DOMAIN_ISOLINES ? MTLPrimitiveTypeLine : MTLPrimitiveTypePoint;
        }
        if (sh->gs) {
            /* The draw rasterizes what the geometry kernel emitted. */
            p->primitive = sh->gs_out_prim == MK_PRIM_POINTS ? MTLPrimitiveTypePoint
                         : sh->gs_out_prim == MK_PRIM_LINES  ? MTLPrimitiveTypeLine : MTLPrimitiveTypeTriangle;
        }
        switch (desc->cull_mode) {
            case VIO_CULL_BACK:  p->cull = MTLCullModeBack;  break;
            case VIO_CULL_FRONT: p->cull = MTLCullModeFront; break;
            default:             p->cull = MTLCullModeNone;  break;
        }
        p->blend = desc->blend;
        p->color_mask = desc->color_mask ? desc->color_mask : VIO_COLOR_RGBA;
        p->per_attachment = desc->per_attachment;
        for (int ai = 0; ai < VIO_MAX_COLOR_ATTACHMENTS; ai++) {
            p->attachment_blend[ai] = desc->attachment_blend[ai];
            p->attachment_mask[ai]  = desc->attachment_mask[ai]; /* literal: 0 = masked off */
        }
        p->depth_bias = desc->depth_bias;
        p->slope_scaled_depth_bias = desc->slope_scaled_depth_bias;
        p->patch_vertices = desc->patch_vertices > 0 ? desc->patch_vertices : 3;
        if (p->patch_vertices > 32) p->patch_vertices = 32;

        MTLDepthStencilDescriptor *ds = [[MTLDepthStencilDescriptor alloc] init];
        if (desc->depth_test) {
            ds.depthCompareFunction = (desc->depth_func == VIO_DEPTH_LEQUAL)
                ? MTLCompareFunctionLessEqual : MTLCompareFunctionLess;
        } else {
            ds.depthCompareFunction = MTLCompareFunctionAlways;
        }
        ds.depthWriteEnabled = (desc->depth_test && desc->depth_write) ? YES : NO;
        id<MTLDepthStencilState> plain = [vio_mtl.device newDepthStencilStateWithDescriptor:ds];
        p->depth_state_nostencil = plain ? (void *)CFBridgingRetain(plain) : NULL;
        if (desc->stencil_enable) {
            /* Same test on both faces, like D3D's FrontFace / BackFace pair. */
            MTLStencilDescriptor *sd = [[MTLStencilDescriptor alloc] init];
            sd.stencilCompareFunction    = metal_compare(desc->stencil_func);
            sd.stencilFailureOperation   = metal_stencil_op(desc->stencil_fail_op);
            sd.depthFailureOperation     = metal_stencil_op(desc->stencil_depth_fail_op);
            sd.depthStencilPassOperation = metal_stencil_op(desc->stencil_pass_op);
            sd.readMask  = (uint32_t)desc->stencil_read_mask & 0xFF;
            sd.writeMask = (uint32_t)desc->stencil_write_mask & 0xFF;
            ds.frontFaceStencil = sd;
            ds.backFaceStencil  = sd;
            p->stencil_ref = (uint32_t)desc->stencil_ref & 0xFF;
            id<MTLDepthStencilState> dss = [vio_mtl.device newDepthStencilStateWithDescriptor:ds];
            p->depth_state = dss ? (void *)CFBridgingRetain(dss) : NULL;
        } else if (p->depth_state_nostencil) {
            p->depth_state = (void *)CFRetain((CFTypeRef)p->depth_state_nostencil);
        }

        if (!metal_identity_instance) {
            static const float identity[16] = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
            metal_identity_instance = [vio_mtl.device newBufferWithBytes:identity length:sizeof(identity)
                                                                 options:MTLResourceStorageModeShared];
        }
    }
    return p;
}

static void metal_destroy_pipeline(void *pipeline_ptr)
{
    vio_metal_pipeline *p = (vio_metal_pipeline *)pipeline_ptr;
    if (!p) return;
    if (metal_current_pipeline == p) metal_current_pipeline = NULL;
    for (int i = 0; i < p->variant_count; i++) {
        if (p->variants[i].pso) CFRelease((CFTypeRef)p->variants[i].pso);
    }
    if (p->depth_state) CFRelease((CFTypeRef)p->depth_state);
    if (p->depth_state_nostencil) CFRelease((CFTypeRef)p->depth_state_nostencil);
    if (p->vert_fn)     CFRelease((CFTypeRef)p->vert_fn);
    if (p->frag_fn)     CFRelease((CFTypeRef)p->frag_fn);
    if (p->frag_fn_noout) CFRelease((CFTypeRef)p->frag_fn_noout);
    free(p);
}

static void metal_bind_pipeline(void *pipeline_ptr)
{
    metal_current_pipeline = (vio_metal_pipeline *)pipeline_ptr;
    /* Fixed-function + PSO are (re)applied per draw by metal_prepare_draw —
     * the target format may change between bind and draw (RT bind). Stage
     * constants of the previous shader must not leak into this one. */
    memset(metal_stage_const_size, 0, sizeof(metal_stage_const_size));
}

void vio_metal_set_shader_cbuffers(void *vs_cbuffer, void *fs_cbuffer)
{
    metal_current_vs_cb = (vio_metal_buffer *)vs_cbuffer;
    metal_current_fs_cb = (vio_metal_buffer *)fs_cbuffer;
}

/* Color format of whatever the open encoder renders into. has_color = 0 for a
 * depth-only RT (no color attachment => PSO without fragment output). */
static void metal_current_target(vio_metal_target_desc *t)
{
    memset(t, 0, sizeof(*t));
    t->samples = 1;
    t->has_depth = 1;
    if (current_bound_rt) {
        if ((current_bound_rt->is_cube || current_bound_rt->layers > 1) && current_bound_level > 0) t->has_depth = 0;
        int rt_ms = current_bound_rt->samples > 1 && t->has_depth;   /* layered levels > 0: single-sample (A24) */
        if (current_bound_rt->samples > 1 && !(current_bound_rt->is_cube || current_bound_rt->layers > 1)) rt_ms = 1;
        void *dt = rt_ms ? current_bound_rt->metal_msaa_depth_texture : current_bound_rt->metal_depth_texture;
        t->has_stencil = t->has_depth && dt && ((__bridge id<MTLTexture>)dt).pixelFormat == VIO_METAL_DEPTH_STENCIL;
        if (rt_ms) t->samples = current_bound_rt->samples;
        if (current_bound_rt->depth_only || !current_bound_rt->metal_color_texture) {
            t->count = 0;
            return;
        }
        t->count = metal_rt_attachment_count(current_bound_rt);
        for (int i = 0; i < t->count; i++) {
            id<MTLTexture> tex = (__bridge id<MTLTexture>)current_bound_rt->metal_color_textures[i];
            t->fmts[i] = tex ? tex.pixelFormat : MTLPixelFormatInvalid;
        }
        return;
    }
    t->fmts[0] = vio_mtl.swap_format;
    t->count = 1;
    if (vio_mtl.samples > 1 && vio_mtl.msaa_color) t->samples = vio_mtl.samples;
    id<MTLTexture> sdt = (vio_mtl.samples > 1 && vio_mtl.msaa_color) ? vio_mtl.msaa_depth : vio_mtl.depth_texture;
    t->has_stencil = sdt && sdt.pixelFormat == VIO_METAL_DEPTH_STENCIL;
}

/* Find or build the PSO variant for (target colour formats, mesh stride, samples, depth). */
static id<MTLRenderPipelineState> metal_pipeline_pso(vio_metal_pipeline *p, const vio_metal_target_desc *t, int stride)
{
    vio_metal_shader *sh = p->shader;
    if (stride <= 0 || stride < sh->vl.vertex_stride) stride = sh->vl.vertex_stride;
    int samples = t->samples < 1 ? 1 : t->samples;
    int has_depth = t->has_depth;
    int has_stencil = t->has_stencil;
    int has_color = t->count > 0;
    const vio_mesh_layout *ml = &metal_mesh_layout;
    uint32_t want_layout = ml->key;

    for (int i = 0; i < p->variant_count; i++) {
        vio_metal_pso_variant *v = &p->variants[i];
        if (v->color_count != t->count || v->stride != stride || v->samples != samples || v->has_depth != has_depth ||
            v->has_stencil != has_stencil || v->layout != want_layout) continue;
        int same = 1;
        for (int k = 0; k < t->count; k++) if (v->color_fmts[k] != (int)t->fmts[k]) { same = 0; break; }
        if (same) return (__bridge id<MTLRenderPipelineState>)v->pso;
    }
    if (p->variant_count >= VIO_METAL_PSO_VARIANTS) {
        php_error_docref(NULL, E_WARNING,
            "Metal: pipeline exceeded %d (target format, stride, samples) variants", VIO_METAL_PSO_VARIANTS);
        return nil;
    }

    @autoreleasepool {
        MTLRenderPipelineDescriptor *d = [[MTLRenderPipelineDescriptor alloc] init];
        d.vertexFunction = (__bridge id<MTLFunction>)p->vert_fn;
        /* Depth-only targets have no colour attachment: use the fragment
         * variant with masked outputs so `discard` (alpha-tested shadows)
         * still runs; without one, rasterize depth only. */
        d.fragmentFunction = has_color ? (__bridge id<MTLFunction>)p->frag_fn
                                       : (p->frag_fn_noout ? (__bridge id<MTLFunction>)p->frag_fn_noout : nil);
        d.rasterizationEnabled = YES;
        d.rasterSampleCount = (NSUInteger)samples;

        /* Vertex descriptor from the shader's own [[stage_in]] reflection.
         * Per-vertex attributes are packed in ascending location order (the
         * convention every backend and vio_mesh share); locations 3..6 are
         * the per-instance mat4 columns in the instance buffer. */
        MTLVertexDescriptor *vd = [[MTLVertexDescriptor alloc] init];
        vio_metal_vs_input sorted[VIO_METAL_MAX_RES];
        int n = sh->vl.input_count;
        memcpy(sorted, sh->vl.inputs, sizeof(vio_metal_vs_input) * (size_t)n);
        for (int a = 0; a < n - 1; a++) {
            for (int b = 0; b < n - 1 - a; b++) {
                if (sorted[b].location > sorted[b + 1].location) {
                    vio_metal_vs_input tmp = sorted[b]; sorted[b] = sorted[b + 1]; sorted[b + 1] = tmp;
                }
            }
        }
        NSUInteger mesh_offset = 0;
        int has_mesh_attr = 0, has_inst_attr = 0;
        for (int i = 0; i < n; i++) {
            vio_metal_vs_input *in = &sorted[i];
            for (int c = 0; c < in->columns; c++) {
                int loc = in->location + c;
                if (loc < 0 || loc > 30) continue;
                if (in->location >= 3 && in->location <= 6) {
                    vd.attributes[loc].format = metal_vertex_format(in->components);
                    vd.attributes[loc].offset = (NSUInteger)(loc - 3) * 16;
                    vd.attributes[loc].bufferIndex = VIO_METAL_VB_INSTANCE;
                    has_inst_attr = 1;
                } else {
                    vd.attributes[loc].format = metal_vertex_format(in->components);
                    vd.attributes[loc].offset = (NSUInteger)vio_mesh_layout_offset(ml, loc, (int)mesh_offset);
                    vd.attributes[loc].bufferIndex = VIO_METAL_VB_MESH;
                    mesh_offset += (NSUInteger)in->components * sizeof(float);
                    has_mesh_attr = 1;
                }
            }
        }
        if (has_mesh_attr) {
            vd.layouts[VIO_METAL_VB_MESH].stride = (NSUInteger)stride;
            vd.layouts[VIO_METAL_VB_MESH].stepFunction = MTLVertexStepFunctionPerVertex;
            vd.layouts[VIO_METAL_VB_MESH].stepRate = 1;
        }
        if (has_inst_attr) {
            vd.layouts[VIO_METAL_VB_INSTANCE].stride = 64;
            vd.layouts[VIO_METAL_VB_INSTANCE].stepFunction = MTLVertexStepFunctionPerInstance;
            /* Multiview draws N emulated instances per real one. */
            vd.layouts[VIO_METAL_VB_INSTANCE].stepRate = sh->view_count > 1 ? (NSUInteger)sh->view_count : 1;
        }
        if (sh->tess && !sh->tess_emul) {
            /* vert_fn is the TES: it reads the control points from the TCS
             * output buffers itself, so no vertex descriptor. Metal's winding
             * label matches GL's for tessellated triangles and quads (test 144:
             * the reversed order culled every GL-ccw patch under CULL_BACK). */
            const vio_metal_tess_info *ti = &sh->tess_info;
            d.tessellationPartitionMode =
                ti->spacing == VIO_MSL_SPACING_FRACTIONAL_EVEN ? MTLTessellationPartitionModeFractionalEven :
                ti->spacing == VIO_MSL_SPACING_FRACTIONAL_ODD  ? MTLTessellationPartitionModeFractionalOdd :
                                                                 MTLTessellationPartitionModeInteger;
            d.tessellationFactorFormat = MTLTessellationFactorFormatHalf;
            d.tessellationFactorStepFunction = MTLTessellationFactorStepFunctionPerPatch;
            d.tessellationControlPointIndexType = MTLTessellationControlPointIndexTypeNone;
            d.tessellationOutputWindingOrder = ti->ccw ? MTLWindingCounterClockwise : MTLWindingClockwise;
            d.tessellationFactorScaleEnabled = NO;
            d.maxTessellationFactor = 64;
        } else if (!sh->tess && (has_mesh_attr || has_inst_attr)) {
            /* (an emulated isoline / point_mode draw reads the capture buffer) */
            d.vertexDescriptor = vd;
        }

        for (int att = 0; att < t->count; att++) {
            /* Blend / write mask per attachment: the pipeline's single state on every
             * attachment, or - 'attachment_blend' / 'attachment_color_mask' - its own. */
            int attMask  = p->per_attachment ? p->attachment_mask[att]  : p->color_mask;
            int attBlend = p->per_attachment ? p->attachment_blend[att] : (int)p->blend;
            MTLRenderPipelineColorAttachmentDescriptor *ca = d.colorAttachments[att];
            ca.pixelFormat = t->fmts[att];
            MTLColorWriteMask wm = MTLColorWriteMaskNone;
            if (attMask & VIO_COLOR_R) wm |= MTLColorWriteMaskRed;
            if (attMask & VIO_COLOR_G) wm |= MTLColorWriteMaskGreen;
            if (attMask & VIO_COLOR_B) wm |= MTLColorWriteMaskBlue;
            if (attMask & VIO_COLOR_A) wm |= MTLColorWriteMaskAlpha;
            ca.writeMask = wm;
            if (attBlend != VIO_BLEND_NONE) {
                ca.blendingEnabled = YES;
                ca.rgbBlendOperation = MTLBlendOperationAdd;
                ca.alphaBlendOperation = MTLBlendOperationAdd;
            }
            switch (attBlend) {
                case VIO_BLEND_ALPHA:
                    ca.sourceRGBBlendFactor = MTLBlendFactorSourceAlpha;
                    ca.destinationRGBBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
                    ca.sourceAlphaBlendFactor = MTLBlendFactorOne;
                    ca.destinationAlphaBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
                    break;
                case VIO_BLEND_ADDITIVE:
                    ca.sourceRGBBlendFactor = MTLBlendFactorSourceAlpha;
                    ca.destinationRGBBlendFactor = MTLBlendFactorOne;
                    ca.sourceAlphaBlendFactor = MTLBlendFactorOne;
                    ca.destinationAlphaBlendFactor = MTLBlendFactorOne;
                    break;
                case VIO_BLEND_PREMULTIPLIED:
                    ca.sourceRGBBlendFactor = MTLBlendFactorOne;
                    ca.destinationRGBBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
                    ca.sourceAlphaBlendFactor = MTLBlendFactorOne;
                    ca.destinationAlphaBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
                    break;
                case VIO_BLEND_MULTIPLY:
                    ca.sourceRGBBlendFactor = MTLBlendFactorDestinationColor;
                    ca.destinationRGBBlendFactor = MTLBlendFactorZero;
                    ca.sourceAlphaBlendFactor = MTLBlendFactorDestinationAlpha;
                    ca.destinationAlphaBlendFactor = MTLBlendFactorZero;
                    break;
                case VIO_BLEND_SCREEN:
                    ca.sourceRGBBlendFactor = MTLBlendFactorOne;
                    ca.destinationRGBBlendFactor = MTLBlendFactorOneMinusSourceColor;
                    ca.sourceAlphaBlendFactor = MTLBlendFactorOne;
                    ca.destinationAlphaBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
                    break;
                case VIO_BLEND_MIN:
                    ca.rgbBlendOperation = MTLBlendOperationMin;
                    ca.alphaBlendOperation = MTLBlendOperationMin;
                    ca.sourceRGBBlendFactor = ca.destinationRGBBlendFactor = MTLBlendFactorOne;
                    ca.sourceAlphaBlendFactor = ca.destinationAlphaBlendFactor = MTLBlendFactorOne;
                    break;
                case VIO_BLEND_MAX:
                    ca.rgbBlendOperation = MTLBlendOperationMax;
                    ca.alphaBlendOperation = MTLBlendOperationMax;
                    ca.sourceRGBBlendFactor = ca.destinationRGBBlendFactor = MTLBlendFactorOne;
                    ca.sourceAlphaBlendFactor = ca.destinationAlphaBlendFactor = MTLBlendFactorOne;
                    break;
                default:
                    break;
            }
        }
        d.depthAttachmentPixelFormat = has_depth ? (has_stencil ? VIO_METAL_DEPTH_STENCIL : MTLPixelFormatDepth32Float)
                                                 : MTLPixelFormatInvalid;
        d.stencilAttachmentPixelFormat = (has_depth && has_stencil) ? VIO_METAL_DEPTH_STENCIL : MTLPixelFormatInvalid;
        if (!sh->tess || sh->tess_emul) {
            /* Required on macOS when the vertex stage writes gl_Layer /
             * gl_ViewportIndex ([[render_target_array_index]] /
             * [[viewport_array_index]]); harmless otherwise. */
            d.inputPrimitiveTopology = metal_topology_class(p->primitive);
        }

        NSError *err = nil;
        id<MTLRenderPipelineState> pso = nil;
        if (sh->mesh) {
            /* Mesh pipeline: same attachments / blend state, object + mesh
             * functions instead of the vertex stage. */
            MTLMeshRenderPipelineDescriptor *md = [[MTLMeshRenderPipelineDescriptor alloc] init];
            md.objectFunction = sh->object_fn ? (__bridge id<MTLFunction>)sh->object_fn : nil;
            md.meshFunction = (__bridge id<MTLFunction>)p->vert_fn;
            md.fragmentFunction = d.fragmentFunction;
            md.rasterSampleCount = d.rasterSampleCount;
            md.rasterizationEnabled = YES;
            if (sh->object_fn) md.payloadMemoryLength = 16384;
            for (int k = 0; k < 8; k++) {
                MTLRenderPipelineColorAttachmentDescriptor *src = d.colorAttachments[k];
                if (src.pixelFormat == MTLPixelFormatInvalid) continue;
                MTLRenderPipelineColorAttachmentDescriptor *dst = md.colorAttachments[k];
                dst.pixelFormat = src.pixelFormat;
                dst.writeMask = src.writeMask;
                dst.blendingEnabled = src.blendingEnabled;
                dst.rgbBlendOperation = src.rgbBlendOperation;
                dst.alphaBlendOperation = src.alphaBlendOperation;
                dst.sourceRGBBlendFactor = src.sourceRGBBlendFactor;
                dst.destinationRGBBlendFactor = src.destinationRGBBlendFactor;
                dst.sourceAlphaBlendFactor = src.sourceAlphaBlendFactor;
                dst.destinationAlphaBlendFactor = src.destinationAlphaBlendFactor;
            }
            md.depthAttachmentPixelFormat = d.depthAttachmentPixelFormat;
            md.stencilAttachmentPixelFormat = d.stencilAttachmentPixelFormat;
            pso = [vio_mtl.device newRenderPipelineStateWithMeshDescriptor:md options:MTLPipelineOptionNone
                                                                reflection:nil error:&err];
        } else {
            pso = [vio_mtl.device newRenderPipelineStateWithDescriptor:d error:&err];
        }
        if (!pso) {
            php_error_docref(NULL, E_WARNING, "Metal: render pipeline creation failed: %s",
                err ? [[err localizedDescription] UTF8String] : "unknown");
            return nil;
        }
        vio_metal_pso_variant *v = &p->variants[p->variant_count++];
        v->color_count = t->count;
        for (int k = 0; k < t->count; k++) v->color_fmts[k] = (int)t->fmts[k];
        v->stride    = stride;
        v->layout    = want_layout;
        v->samples   = samples;
        v->has_depth = has_depth;
        v->has_stencil = has_stencil;
        v->pso       = (void *)CFBridgingRetain(pso);
        return pso;
    }
}

/* Apply everything a draw needs on the open encoder: PSO variant for the
 * current target, depth/cull/bias state, this draw's cbuffer slices and the
 * identity instance buffer. Returns 0 when there is nothing to draw into. */
/* ── Inline ray tracing (VIO_FEATURE_RAY_QUERY) ─────────────────────
 * One primitive acceleration structure per geometry, one instance structure
 * over the instances, built synchronously on their own command buffer. The
 * structure bound with vio_bind_acceleration_structure reaches the shader at
 * the [[buffer(n)]] index its acceleration_structure parameter was renumbered
 * to (vertex / fragment), or at the GLSL binding (compute); the instance
 * structure only references the primitive ones, so every draw / dispatch that
 * binds it declares them with useResource. */
typedef struct _vio_metal_as {
    void  *tlas;          /* id<MTLAccelerationStructure>, +1 */
    void **blas;          /* id<MTLAccelerationStructure>[blas_count], +1 each */
    int    blas_count;
    void  *instances;     /* id<MTLBuffer> of instance descriptors, +1 */
    int    instance_count;
} vio_metal_as;

static vio_metal_as *metal_bound_as = NULL;
static int           metal_bound_as_binding = 0;

static void metal_use_as(id<MTLRenderCommandEncoder> renc, id<MTLComputeCommandEncoder> cenc, vio_metal_as *as)
{
    if (!as) return;
    for (int i = 0; i < as->blas_count; i++) {
        id<MTLResource> r = (__bridge id<MTLResource>)as->blas[i];
        if (renc) {
            if (@available(macOS 13.0, iOS 16.0, *)) [renc useResource:r usage:MTLResourceUsageRead stages:MTLRenderStageVertex | MTLRenderStageFragment];
            else [renc useResource:r usage:MTLResourceUsageRead];
        }
        if (cenc) [cenc useResource:r usage:MTLResourceUsageRead];
    }
}

/* Instance descriptors: the row-major 3x4 transform as four packed columns. */
static id<MTLBuffer> metal_as_instance_buffer(id<MTLDevice> dev, const vio_as_instance *inst, int count, int blas_count)
{
    id<MTLBuffer> ibuf = [dev newBufferWithLength:sizeof(MTLAccelerationStructureInstanceDescriptor) * (NSUInteger)count
                                          options:MTLResourceStorageModeShared];
    if (!ibuf) return nil;
    MTLAccelerationStructureInstanceDescriptor *ids = (MTLAccelerationStructureInstanceDescriptor *)[ibuf contents];
    for (int i = 0; i < count; i++) {
        const vio_as_instance *in = &inst[i];
        if (in->geometry < 0 || in->geometry >= blas_count) return nil;
        memset(&ids[i], 0, sizeof(ids[i]));
        for (int c = 0; c < 4; c++) {
            ids[i].transformationMatrix.columns[c].x = in->transform[0 * 4 + c];
            ids[i].transformationMatrix.columns[c].y = in->transform[1 * 4 + c];
            ids[i].transformationMatrix.columns[c].z = in->transform[2 * 4 + c];
        }
        ids[i].options = MTLAccelerationStructureInstanceOptionOpaque;
        ids[i].mask = (uint32_t)(in->mask & 0xFF);
        ids[i].intersectionFunctionTableOffset = 0;
        ids[i].accelerationStructureIndex = (uint32_t)in->geometry;
    }
    return ibuf;
}

/* The instance-structure descriptor over the kept primitive structures; refit
 * usage so vio_acceleration_structure_update can update it in place (A14). */
static MTLInstanceAccelerationStructureDescriptor *metal_as_instance_desc(vio_metal_as *as, id<MTLBuffer> ibuf, int count)
    API_AVAILABLE(macos(11.0), ios(14.0))
{
    NSMutableArray *blases = [NSMutableArray arrayWithCapacity:(NSUInteger)as->blas_count];
    for (int i = 0; i < as->blas_count; i++) [blases addObject:(__bridge id<MTLAccelerationStructure>)as->blas[i]];
    MTLInstanceAccelerationStructureDescriptor *idesc = [MTLInstanceAccelerationStructureDescriptor descriptor];
    idesc.instancedAccelerationStructures = blases;
    idesc.instanceCount = (NSUInteger)count;
    idesc.instanceDescriptorBuffer = ibuf;
    idesc.usage = MTLAccelerationStructureUsageRefit;
    return idesc;
}

static void *metal_create_acceleration_structure(const vio_as_desc *desc)
{
    if (!desc || desc->geometry_count < 1 || desc->instance_count < 1 || !vio_mtl.device) return NULL;
    if (!vio_mtl.caps.raytracing) return NULL;
    if (@available(macOS 11.0, iOS 14.0, *)) {
        vio_metal_as *as = calloc(1, sizeof(vio_metal_as));
        if (!as) return NULL;
        as->blas = calloc((size_t)desc->geometry_count, sizeof(void *));
        if (!as->blas) { free(as); return NULL; }
        @autoreleasepool {
            id<MTLDevice> dev = vio_mtl.device;
            id<MTLCommandBuffer> cb = [vio_mtl.command_queue commandBuffer];
            id<MTLAccelerationStructureCommandEncoder> enc = [cb accelerationStructureCommandEncoder];
            NSMutableArray *keep = [NSMutableArray array];   /* geometry + scratch buffers live until the wait */
            NSMutableArray *blases = [NSMutableArray array];
            for (int g = 0; g < desc->geometry_count; g++) {
                const vio_as_geometry *geo = &desc->geometries[g];
                id<MTLBuffer> vb = [dev newBufferWithBytes:geo->positions length:(NSUInteger)geo->vertex_count * 12
                                                   options:MTLResourceStorageModeShared];
                MTLAccelerationStructureTriangleGeometryDescriptor *tri = [MTLAccelerationStructureTriangleGeometryDescriptor descriptor];
                tri.vertexBuffer = vb;
                tri.vertexStride = 12;
                tri.opaque = YES;
                if (geo->indices && geo->index_count >= 3) {
                    id<MTLBuffer> ib = [dev newBufferWithBytes:geo->indices length:(NSUInteger)geo->index_count * 4
                                                       options:MTLResourceStorageModeShared];
                    tri.indexBuffer = ib;
                    tri.indexType = MTLIndexTypeUInt32;
                    tri.triangleCount = (NSUInteger)geo->index_count / 3;
                    [keep addObject:ib];
                } else {
                    tri.triangleCount = (NSUInteger)geo->vertex_count / 3;
                }
                [keep addObject:vb];
                MTLPrimitiveAccelerationStructureDescriptor *pd = [MTLPrimitiveAccelerationStructureDescriptor descriptor];
                pd.geometryDescriptors = @[ tri ];
                MTLAccelerationStructureSizes sz = [dev accelerationStructureSizesWithDescriptor:pd];
                id<MTLAccelerationStructure> blas = [dev newAccelerationStructureWithSize:sz.accelerationStructureSize];
                id<MTLBuffer> scratch = [dev newBufferWithLength:sz.buildScratchBufferSize > 0 ? sz.buildScratchBufferSize : 16
                                                         options:MTLResourceStorageModePrivate];
                if (!blas || !scratch) { [enc endEncoding]; goto fail; }
                [enc buildAccelerationStructure:blas descriptor:pd scratchBuffer:scratch scratchBufferOffset:0];
                [keep addObject:scratch];
                [blases addObject:blas];
                as->blas[as->blas_count++] = (void *)CFBridgingRetain(blas);
            }
            [enc endEncoding];

            id<MTLBuffer> ibuf = metal_as_instance_buffer(dev, desc->instances, desc->instance_count, as->blas_count);
            if (!ibuf) goto fail;
            MTLInstanceAccelerationStructureDescriptor *idesc = metal_as_instance_desc(as, ibuf, desc->instance_count);
            (void)blases;
            MTLAccelerationStructureSizes isz = [dev accelerationStructureSizesWithDescriptor:idesc];
            id<MTLAccelerationStructure> tlas = [dev newAccelerationStructureWithSize:isz.accelerationStructureSize];
            id<MTLBuffer> iscratch = [dev newBufferWithLength:isz.buildScratchBufferSize > 0 ? isz.buildScratchBufferSize : 16
                                                      options:MTLResourceStorageModePrivate];
            if (!tlas || !iscratch) goto fail;
            /* A second encoder: the instance build reads the finished primitive structures. */
            id<MTLAccelerationStructureCommandEncoder> ienc = [cb accelerationStructureCommandEncoder];
            [ienc buildAccelerationStructure:tlas descriptor:idesc scratchBuffer:iscratch scratchBufferOffset:0];
            [ienc endEncoding];
            [cb commit];
            [cb waitUntilCompleted];
            if (cb.status != MTLCommandBufferStatusCompleted) goto fail;
            as->tlas = (void *)CFBridgingRetain(tlas);
            as->instances = (void *)CFBridgingRetain(ibuf);
            as->instance_count = desc->instance_count;
            (void)keep;
            return as;
        }
fail:
        for (int i = 0; i < as->blas_count; i++) CFRelease(as->blas[i]);
        free(as->blas);
        free(as);
        return NULL;
    }
    return NULL;
}

static int metal_update_acceleration_structure(void *ptr, const vio_as_instance *inst, int count, int refit)
{
    vio_metal_as *as = (vio_metal_as *)ptr;
    if (!as || !inst || count < 1 || !vio_mtl.device || !as->tlas) return -1;
    if (refit && count != as->instance_count) return -1;
    if (@available(macOS 11.0, iOS 14.0, *)) {
        @autoreleasepool {
            id<MTLDevice> dev = vio_mtl.device;
            id<MTLBuffer> ibuf = metal_as_instance_buffer(dev, inst, count, as->blas_count);
            if (!ibuf) return -1;
            MTLInstanceAccelerationStructureDescriptor *idesc = metal_as_instance_desc(as, ibuf, count);
            MTLAccelerationStructureSizes sz = [dev accelerationStructureSizesWithDescriptor:idesc];
            id<MTLCommandBuffer> cb = [vio_mtl.command_queue commandBuffer];
            id<MTLAccelerationStructureCommandEncoder> enc = [cb accelerationStructureCommandEncoder];
            id<MTLAccelerationStructure> fresh = nil;
            if (refit) {
                /* In place (destination nil); the queue orders it after frames that read it. */
                id<MTLBuffer> s = [dev newBufferWithLength:sz.refitScratchBufferSize > 0 ? sz.refitScratchBufferSize : 16
                                                   options:MTLResourceStorageModePrivate];
                if (!s) { [enc endEncoding]; return -1; }
                [enc refitAccelerationStructure:(__bridge id<MTLAccelerationStructure>)as->tlas descriptor:idesc
                                    destination:nil scratchBuffer:s scratchBufferOffset:0];
            } else {
                fresh = [dev newAccelerationStructureWithSize:sz.accelerationStructureSize];
                id<MTLBuffer> s = [dev newBufferWithLength:sz.buildScratchBufferSize > 0 ? sz.buildScratchBufferSize : 16
                                                   options:MTLResourceStorageModePrivate];
                if (!fresh || !s) { [enc endEncoding]; return -1; }
                [enc buildAccelerationStructure:fresh descriptor:idesc scratchBuffer:s scratchBufferOffset:0];
            }
            [enc endEncoding];
            [cb commit];
            [cb waitUntilCompleted];
            if (cb.status != MTLCommandBufferStatusCompleted) return -1;
            if (fresh) {
                CFRelease(as->tlas);   /* command buffers that used it retain it */
                as->tlas = (void *)CFBridgingRetain(fresh);
            }
            if (as->instances) CFRelease(as->instances);
            as->instances = (void *)CFBridgingRetain(ibuf);
            as->instance_count = count;
            return 0;
        }
    }
    return -1;
}

static void metal_destroy_acceleration_structure(void *ptr)
{
    vio_metal_as *as = (vio_metal_as *)ptr;
    if (!as) return;
    if (metal_bound_as == as) metal_bound_as = NULL;
    /* Command buffers retain what they reference: releasing now is safe. */
    if (as->tlas) CFRelease(as->tlas);
    if (as->instances) CFRelease(as->instances);
    for (int i = 0; i < as->blas_count; i++) if (as->blas[i]) CFRelease(as->blas[i]);
    free(as->blas);
    free(as);
}

static void metal_bind_acceleration_structure(void *ptr, int binding)
{
    metal_bound_as = (vio_metal_as *)ptr;
    metal_bound_as_binding = binding;
}

/* Bind the bound acceleration structure to the stages of the current draw. */
static void metal_bind_as_for_draw(id<MTLRenderCommandEncoder> enc, vio_metal_shader *sh)
{
    if (!metal_bound_as || !metal_bound_as->tlas || !sh) return;
    int used = 0;
    if (@available(macOS 12.0, iOS 15.0, *)) {
        id<MTLAccelerationStructure> tlas = (__bridge id<MTLAccelerationStructure>)metal_bound_as->tlas;
        for (int i = 0; i < sh->vs.buffer_count; i++) {
            if (sh->vs.buffers[i].kind != 3) continue;
            [enc setVertexAccelerationStructure:tlas atBufferIndex:(NSUInteger)sh->vs.buffers[i].msl_index];
            used = 1;
        }
        for (int i = 0; i < sh->fs.buffer_count; i++) {
            if (sh->fs.buffers[i].kind != 3) continue;
            [enc setFragmentAccelerationStructure:tlas atBufferIndex:(NSUInteger)sh->fs.buffers[i].msl_index];
            used = 1;
        }
    }
    if (used) metal_use_as(enc, nil, metal_bound_as);
}

/* Fragment storage buffers (A15): bound per draw at the fragment stage's
 * pinned [[buffer(n)]] of the SSBO with that GLSL binding. */
static vio_metal_compute_buffer *metal_fs_storage[VIO_MAX_FRAGMENT_STORAGE];
static int metal_fs_storage_used;

static int metal_bind_fragment_storage(void *backend_buffer, int binding)
{
    if (binding < 0 || binding >= VIO_MAX_FRAGMENT_STORAGE) return -1;
    metal_fs_storage[binding] = (vio_metal_compute_buffer *)backend_buffer;
    return 0;
}

static void metal_bind_fs_storage(id<MTLRenderCommandEncoder> enc, vio_metal_shader *sh)
{
    for (int i = 0; i < sh->fs.buffer_count; i++) {
        const vio_metal_res_buffer *rb = &sh->fs.buffers[i];
        if (rb->kind != 1 || rb->binding < 0 || rb->binding >= VIO_MAX_FRAGMENT_STORAGE) continue;
        vio_metal_compute_buffer *b = metal_fs_storage[rb->binding];
        if (!b || !b->buffer) {
            /* A declared but unbound block writes into a scratch buffer, never off the end of nothing. */
            static id<MTLBuffer> scratch = nil;
            if (!scratch) scratch = [vio_mtl.device newBufferWithLength:65536 options:MTLResourceStorageModeShared];
            if (scratch) [enc setFragmentBuffer:scratch offset:0 atIndex:(NSUInteger)rb->msl_index];
            continue;
        }
        [enc setFragmentBuffer:(__bridge id<MTLBuffer>)b->buffer offset:0 atIndex:(NSUInteger)rb->msl_index];
        metal_fs_storage_used = 1;
    }
}

static int metal_prepare_draw(int stride)
{
    vio_metal_pipeline *p = metal_current_pipeline;
    if (!p || !p->shader || !vio_mtl.current_encoder) return 0;

    vio_metal_target_desc target;
    metal_current_target(&target);
    int has_depth = target.has_depth;

    id<MTLRenderPipelineState> pso = metal_pipeline_pso(p, &target, stride);
    if (!pso) return 0;

    id<MTLRenderCommandEncoder> enc = vio_mtl.current_encoder;
    [enc setRenderPipelineState:pso];
    if (!has_depth) {
        /* No depth attachment on this target: a depth-writing state is invalid. */
        [enc setDepthStencilState:mtl_2d.depth_disabled];
    } else if (target.has_stencil && p->depth_state) {
        [enc setDepthStencilState:(__bridge id<MTLDepthStencilState>)p->depth_state];
        [enc setStencilReferenceValue:p->stencil_ref];
    } else if (p->depth_state_nostencil) {
        [enc setDepthStencilState:(__bridge id<MTLDepthStencilState>)p->depth_state_nostencil];
    }
    [enc setCullMode:p->cull];
    [enc setFrontFacingWinding:MTLWindingCounterClockwise];  /* match GL / D3D FrontCounterClockwise */
    [enc setDepthBias:p->depth_bias slopeScale:p->slope_scaled_depth_bias clamp:0.0f];

    /* Per-draw uniform slices. A fragment stage that reflects a default block
     * but got no separate cbuffer object shares the vertex data (D3D11 binds
     * the VS cbuffer to PS b0 in that case). */
    vio_metal_shader *sh = p->shader;
    if (sh->mesh) {
        /* The mesh stage owns the "vertex" uniform block; a task stage that
         * declares the same block reads the same slice. */
        vio_metal_buffer *mcb = metal_current_vs_cb;
        if (mcb && mcb->shadow && mcb->shadow_size > 0) {
            NSUInteger off = 0;
            id<MTLBuffer> rb = metal_ring_alloc(mcb->shadow_size, &off);
            if (rb) {
                memcpy((char *)[rb contents] + off, mcb->shadow, mcb->shadow_size);
                if (sh->vs.cbuffer_index >= 0) [enc setMeshBuffer:rb offset:off atIndex:(NSUInteger)sh->vs.cbuffer_index];
                if (sh->object_fn && sh->obj.cbuffer_index >= 0) [enc setObjectBuffer:rb offset:off atIndex:(NSUInteger)sh->obj.cbuffer_index];
            }
        }
        vio_metal_buffer *fcb = metal_current_fs_cb ? metal_current_fs_cb : NULL;
        if (fcb) metal_bind_uniform_slice(fcb, -1, sh->fs.cbuffer_index);
        else if (mcb && sh->fs.cbuffer_index >= 0) metal_bind_uniform_slice(mcb, -1, sh->fs.cbuffer_index);
        return 1;
    }
    if (sh->tess) {
        /* The vertex function is the TES (its own block); the vertex stage's
         * block goes to the vertex kernel in metal_draw_tess. */
        int ti = VIO_EXTRA_STAGE_INDEX(VIO_STAGE_TESS_EVAL);
        if (sh->tes.cbuffer_index >= 0 && metal_stage_const_size[ti] > 0) {
            NSUInteger off = 0;
            id<MTLBuffer> rb = metal_ring_alloc(metal_stage_const_size[ti], &off);
            if (rb) {
                memcpy((char *)[rb contents] + off, metal_stage_const[ti], metal_stage_const_size[ti]);
                [enc setVertexBuffer:rb offset:off atIndex:(NSUInteger)sh->tes.cbuffer_index];
            }
        }
        vio_metal_buffer *fcb = metal_current_fs_cb ? metal_current_fs_cb : metal_current_vs_cb;
        if (fcb) metal_bind_uniform_slice(fcb, -1, sh->fs.cbuffer_index);
        return 1;
    }
    if (metal_current_vs_cb) {
        metal_bind_uniform_slice(metal_current_vs_cb, sh->vs.cbuffer_index,
                                 metal_current_fs_cb ? -1 : sh->fs.cbuffer_index);
    }
    if (metal_current_fs_cb) {
        metal_bind_uniform_slice(metal_current_fs_cb, -1, sh->fs.cbuffer_index);
    }

    if (sh->vl.uses_instance_attribs && metal_identity_instance) {
        [enc setVertexBuffer:metal_identity_instance offset:0 atIndex:VIO_METAL_VB_INSTANCE];
    }
    metal_bind_as_for_draw(enc, sh);
    metal_bind_fs_storage(enc, sh);
    metal_bindless_bind(enc, sh->vs.uses_bindless, sh->fs.uses_bindless);
    if (sh->view_count > 1) {
        /* SPIRV-Cross's multiview view mask: {base view, view count}. */
        uint32_t mask[2] = { 0u, (uint32_t)sh->view_count };
        [enc setVertexBytes:mask length:sizeof(mask) atIndex:VIO_METAL_VIEW_MASK_INDEX];
        [enc setFragmentBytes:mask length:sizeof(mask) atIndex:VIO_METAL_VIEW_MASK_INDEX];
    }
    return 1;
}

/* ── Bindless table (vio_texture_index, BINDLESS-PLAN.md) ──────────────
 * One Shared buffer of MTLResourceIDs per kind - vio_textures (2D), vio_texture_arrays
 * and vio_cubes - that the shader's Set 1 / 2 / 3 argument buffers read (the MSL
 * translation moves vio_cubes / vio_texture_arrays to sets 2 / 3, each an unsized
 * array at offset 0), plus the textures themselves so every draw can make them
 * resident (useResources). The slot space is shared: a slot has one kind. */
static id<MTLBuffer>   metal_bindless_bufs[3] = { nil, nil, nil };   /* per VIO_BINDLESS_KIND_* */
static const NSUInteger metal_bindless_index[3] = {
    VIO_METAL_BINDLESS_INDEX, VIO_METAL_BINDLESS_ARRAY_INDEX, VIO_METAL_BINDLESS_CUBE_INDEX };
static id<MTLResource> metal_bindless_res[VIO_BINDLESS_MAX];
static int             metal_bindless_count = 0;
/* The live entries, packed for useResources (released slots are nil). */
static id<MTLResource> metal_bindless_live[VIO_BINDLESS_MAX];
static int             metal_bindless_live_count = 0;

static void metal_bindless_pack(void)
{
    metal_bindless_live_count = 0;
    for (int i = 0; i < metal_bindless_count; i++)
        if (metal_bindless_res[i]) metal_bindless_live[metal_bindless_live_count++] = metal_bindless_res[i];
    for (int i = metal_bindless_live_count; i < VIO_BINDLESS_MAX && metal_bindless_live[i]; i++) metal_bindless_live[i] = nil;
}

static int metal_bindless_set(int slot, void *backend_texture, int kind)
{
    if (slot < 0 || slot >= VIO_BINDLESS_MAX || kind < VIO_BINDLESS_KIND_2D || kind > VIO_BINDLESS_KIND_CUBE) return -1;
    if (!backend_texture) {
        /* Released: a zero resource ID, and no longer resident. */
        if (metal_bindless_bufs[kind]) memset((char *)[metal_bindless_bufs[kind] contents] + (size_t)slot * 8, 0, 8);
        metal_bindless_res[slot] = nil;
        metal_bindless_pack();
        return 0;
    }
    if (!vio_mtl.device || !vio_mtl.caps.bindless) return -1;
    id<MTLTexture> tex = nil;
    if (kind == VIO_BINDLESS_KIND_CUBE) {
        vio_cubemap_object *cm = (vio_cubemap_object *)backend_texture;
        if (cm->metal_texture) tex = (__bridge id<MTLTexture>)cm->metal_texture;
    } else {
        vio_metal_texture *mt = (vio_metal_texture *)backend_texture;
        if (mt->tex) tex = (__bridge id<MTLTexture>)mt->tex;
    }
    if (!tex) return -1;
    if (@available(macOS 13.0, iOS 16.0, *)) {
        if (!metal_bindless_bufs[kind]) {
            metal_bindless_bufs[kind] = [vio_mtl.device newBufferWithLength:VIO_BINDLESS_MAX * sizeof(MTLResourceID)
                                                                    options:MTLResourceStorageModeShared];
            if (!metal_bindless_bufs[kind]) return -1;
            memset([metal_bindless_bufs[kind] contents], 0, VIO_BINDLESS_MAX * sizeof(MTLResourceID));
        }
        MTLResourceID rid = tex.gpuResourceID;
        memcpy((char *)[metal_bindless_bufs[kind] contents] + (size_t)slot * sizeof(MTLResourceID), &rid, sizeof(rid));
        metal_bindless_res[slot] = tex;
        if (slot + 1 > metal_bindless_count) metal_bindless_count = slot + 1;
        metal_bindless_pack();
        return 0;
    }
    return -1;
}

static void metal_bindless_release(void)
{
    for (int i = 0; i < metal_bindless_count; i++) metal_bindless_res[i] = nil;
    metal_bindless_count = 0;
    metal_bindless_pack();
    for (int k = 0; k < 3; k++) metal_bindless_bufs[k] = nil;
}

/* Bind the tables' argument buffers to the stages that read them and make
 * their textures resident for this draw. */
static void metal_bindless_bind(id<MTLRenderCommandEncoder> enc, int vertex, int fragment)
{
    if (!vertex && !fragment) return;
    for (int k = 0; k < 3; k++) {
        if (!metal_bindless_bufs[k]) continue;
        if (vertex)   [enc setVertexBuffer:metal_bindless_bufs[k] offset:0 atIndex:metal_bindless_index[k]];
        if (fragment) [enc setFragmentBuffer:metal_bindless_bufs[k] offset:0 atIndex:metal_bindless_index[k]];
    }
    if (metal_bindless_live_count > 0) {
        MTLRenderStages stages = (vertex ? MTLRenderStageVertex : 0) | (fragment ? MTLRenderStageFragment : 0);
        [enc useResources:metal_bindless_live count:(NSUInteger)metal_bindless_live_count usage:MTLResourceUsageRead stages:stages];
    }
}

/* The same for a compute kernel. */
static void metal_bindless_bind_compute(id<MTLComputeCommandEncoder> enc)
{
    for (int k = 0; k < 3; k++) {
        if (metal_bindless_bufs[k]) [enc setBuffer:metal_bindless_bufs[k] offset:0 atIndex:metal_bindless_index[k]];
    }
    if (metal_bindless_live_count > 0)
        [enc useResources:metal_bindless_live count:(NSUInteger)metal_bindless_live_count usage:MTLResourceUsageRead];
}

/* Instances to issue for `n` user instances: x views under multiview. */
static NSUInteger metal_mv_instances(NSUInteger n)
{
    vio_metal_pipeline *p = metal_current_pipeline;
    int v = (p && p->shader) ? p->shader->view_count : 0;
    return v > 1 ? n * (NSUInteger)v : n;
}

static void metal_bind_mesh_vb(void *vertex_buffer)
{
    vio_metal_buffer *vb = (vio_metal_buffer *)vertex_buffer;
    if (vb && vb->buffer) {
        [vio_mtl.current_encoder setVertexBuffer:(__bridge id<MTLBuffer>)vb->buffer
                                          offset:0 atIndex:VIO_METAL_VB_MESH];
    }
}

/* ── Tessellation draws ───────────────────────────────────────────
 *
 * Metal has no hull / domain stages. Every draw of a tessellation shader runs
 *   1. the vertex kernel: one thread per (vertex, instance), outputs into a
 *      ring slice;
 *   2. the control kernel: one thread per output control point (a threadgroup
 *      per patch), writing control points, patch constants and the
 *      tessellation factors into ring slices;
 *   3. drawPatches on the frame's render encoder: the fixed-function
 *      tessellator reads the factors, the TES (vertex function) the control
 *      points and patch constants.
 * The kernels go into their own command buffer, committed at once: it is
 * enqueued before the frame's command buffer (committed in vio_end), and
 * Metal's hazard tracking orders the ring writes before the draw reads them.
 * The render encoder stays open, so the textures, viewport and scissor of the
 * draw survive. Consequence: an async compute dispatch recorded earlier in the
 * same frame runs after these kernels. */

static MTLAttributeFormat metal_attribute_format(int components)
{
    switch (components) {
        case 1:  return MTLAttributeFormatFloat;
        case 2:  return MTLAttributeFormatFloat2;
        case 4:  return MTLAttributeFormatFloat4;
        default: return MTLAttributeFormatFloat3;
    }
}

/* Compute pipeline of the vertex kernel for one mesh stride: the stage-input
 * descriptor mirrors the render path's vertex descriptor, stepping per vertex
 * along grid X and per instance along grid Y. */
static id<MTLComputePipelineState> metal_tess_vs_pso(vio_metal_shader *sh, int stride)
{
    const vio_mesh_layout *ml = &metal_mesh_layout;
    for (int i = 0; i < sh->vs_kernel_variant_count; i++) {
        if (sh->vs_kernel_variants[i].stride == stride && sh->vs_kernel_variants[i].layout == ml->key) {
            return (__bridge id<MTLComputePipelineState>)sh->vs_kernel_variants[i].pso;
        }
    }
    if (sh->vs_kernel_variant_count >= VIO_METAL_TESS_VS_VARIANTS) {
        php_error_docref(NULL, E_WARNING,
            "Metal: tessellation shader drawn with more than %d mesh strides", VIO_METAL_TESS_VS_VARIANTS);
        return nil;
    }
    @autoreleasepool {
        MTLStageInputOutputDescriptor *sd = [MTLStageInputOutputDescriptor stageInputOutputDescriptor];
        vio_metal_vs_input sorted[VIO_METAL_MAX_RES];
        int n = sh->vl.input_count;
        memcpy(sorted, sh->vl.inputs, sizeof(vio_metal_vs_input) * (size_t)n);
        for (int a = 0; a < n - 1; a++) {
            for (int b = 0; b < n - 1 - a; b++) {
                if (sorted[b].location > sorted[b + 1].location) {
                    vio_metal_vs_input tmp = sorted[b]; sorted[b] = sorted[b + 1]; sorted[b + 1] = tmp;
                }
            }
        }
        NSUInteger mesh_offset = 0;
        int has_mesh_attr = 0, has_inst_attr = 0;
        for (int i = 0; i < n; i++) {
            vio_metal_vs_input *in = &sorted[i];
            for (int c = 0; c < in->columns; c++) {
                int loc = in->location + c;
                if (loc < 0 || loc > 30) continue;
                sd.attributes[loc].format = metal_attribute_format(in->components);
                if (in->location >= 3 && in->location <= 6) {
                    sd.attributes[loc].offset = (NSUInteger)(loc - 3) * 16;
                    sd.attributes[loc].bufferIndex = VIO_METAL_VB_INSTANCE;
                    has_inst_attr = 1;
                } else {
                    sd.attributes[loc].offset = (NSUInteger)vio_mesh_layout_offset(ml, loc, (int)mesh_offset);
                    sd.attributes[loc].bufferIndex = VIO_METAL_VB_MESH;
                    mesh_offset += (NSUInteger)in->components * sizeof(float);
                    has_mesh_attr = 1;
                }
            }
        }
        if (has_mesh_attr) {
            sd.layouts[VIO_METAL_VB_MESH].stride = (NSUInteger)stride;
            sd.layouts[VIO_METAL_VB_MESH].stepFunction = MTLStepFunctionThreadPositionInGridX;
            sd.layouts[VIO_METAL_VB_MESH].stepRate = 1;
        }
        if (has_inst_attr) {
            sd.layouts[VIO_METAL_VB_INSTANCE].stride = 64;
            sd.layouts[VIO_METAL_VB_INSTANCE].stepFunction = MTLStepFunctionThreadPositionInGridY;
            sd.layouts[VIO_METAL_VB_INSTANCE].stepRate = 1;
        }

        MTLComputePipelineDescriptor *cd = [[MTLComputePipelineDescriptor alloc] init];
        cd.computeFunction = (__bridge id<MTLFunction>)sh->vs_kernel_fn;
        cd.stageInputDescriptor = sd;
        NSError *err = nil;
        id<MTLComputePipelineState> pso = [vio_mtl.device newComputePipelineStateWithDescriptor:cd
                                                                                        options:MTLPipelineOptionNone
                                                                                     reflection:nil
                                                                                          error:&err];
        if (!pso) {
            php_error_docref(NULL, E_WARNING, "Metal: tessellation vertex kernel pipeline failed: %s",
                err ? [[err localizedDescription] UTF8String] : "unknown");
            return nil;
        }
        sh->vs_kernel_variants[sh->vs_kernel_variant_count].stride = stride;
        sh->vs_kernel_variants[sh->vs_kernel_variant_count].layout = ml->key;
        sh->vs_kernel_variants[sh->vs_kernel_variant_count].pso = (void *)CFBridgingRetain(pso);
        sh->vs_kernel_variant_count++;
        return pso;
    }
}

/* Command buffer that carries async dispatches not yet known to be complete:
 * the open frame buffer while recording, the committed one after present. */
static id<MTLCommandBuffer> metal_async_cb = nil;
static void metal_compute_wait(void);

static void metal_fs_shadow_copy(id<MTLTexture> __strong *tex, id<MTLSamplerState> __strong *smp)
{
    for (int i = 0; i < VIO_METAL_FS_SLOTS; i++) { tex[i] = metal_fs_tex[i]; smp[i] = metal_fs_smp[i]; }
}

/* Re-bind the fragment textures of the draw on a reopened encoder. */
static void metal_fs_shadow_restore(id<MTLTexture> __strong *tex, id<MTLSamplerState> __strong *smp)
{
    if (!vio_mtl.current_encoder) return;
    for (int i = 0; i < VIO_METAL_FS_SLOTS; i++) {
        if (!tex[i]) continue;
        [vio_mtl.current_encoder setFragmentTexture:tex[i] atIndex:(NSUInteger)i];
        if (smp[i]) [vio_mtl.current_encoder setFragmentSamplerState:smp[i] atIndex:(NSUInteger)i];
        metal_fs_shadow_set(i, tex[i], smp[i]);
    }
}

/* Async dispatches of this frame finish before what follows, the open pass
 * keeps its textures and viewports (indirect / emulated tessellation draws). */
static void metal_flush_async_keep_pass(void)
{
    if (!(metal_async_cb != nil && metal_async_cb == vio_mtl.current_cmd_buf)) return;
    id<MTLTexture> saved_tex[VIO_METAL_FS_SLOTS];
    id<MTLSamplerState> saved_smp[VIO_METAL_FS_SLOTS];
    MTLViewport saved_vp[VIO_METAL_MAX_VIEWPORTS];
    MTLScissorRect saved_sc[VIO_METAL_MAX_VIEWPORTS];
    int saved_vp_count = metal_vp_count, saved_has_sc = metal_vp_has_scissor;
    metal_fs_shadow_copy(saved_tex, saved_smp);
    memcpy(saved_vp, metal_vp, sizeof(saved_vp));
    memcpy(saved_sc, metal_sc, sizeof(saved_sc));
    metal_compute_wait();
    metal_fs_shadow_restore(saved_tex, saved_smp);
    memcpy(metal_vp, saved_vp, sizeof(saved_vp));
    memcpy(metal_sc, saved_sc, sizeof(saved_sc));
    metal_vp_count = saved_vp_count;
    metal_vp_has_scissor = saved_has_sc;
    metal_apply_viewports();
}

/* GL rounding of a tessellation level (11.2.2): 0 when the level culls. */
static int metal_tess_round(float f, int spacing, float *clamped)
{
    if (!(f > 0.0f)) return 0;
    int n;
    if (spacing == VIO_MSL_SPACING_FRACTIONAL_ODD) {
        f = f < 1.0f ? 1.0f : (f > 63.0f ? 63.0f : f);
        n = 2 * (int)ceilf((f - 1.0f) * 0.5f) + 1;
    } else if (spacing == VIO_MSL_SPACING_FRACTIONAL_EVEN) {
        f = f < 2.0f ? 2.0f : (f > 64.0f ? 64.0f : f);
        n = 2 * (int)ceilf(f * 0.5f);
    } else {
        f = f < 1.0f ? 1.0f : (f > 64.0f ? 64.0f : f);
        n = (int)ceilf(f);
    }
    if (clamped) *clamped = f;
    return n;
}

/* Inner level: never culls (clamped to 1), and 1 counts as 1 + epsilon unless
 * every level of the patch is 1. */
static int metal_tess_inner(float f, int spacing, int all_one, float *clamped)
{
    int n = metal_tess_round(f > 0.0f ? f : 1.0f, spacing, clamped);
    if (n <= 1 && !all_one) {
        n = spacing == VIO_MSL_SPACING_FRACTIONAL_ODD ? 3 : 2;
        *clamped = (float)n;
    }
    return n;
}

static uint32_t metal_f2u(float f) { uint32_t u; memcpy(&u, &f, 4); return u; }
static void metal_put_half(uint16_t *dst, float f) { __fp16 h = (__fp16)f; memcpy(dst, &h, 2); }

/* Isolines / point_mode (metal_tess_emul_msl): the kernels have run in `kcb`;
 * read the levels back, tessellate with exact integer factors in a pass
 * without rasterization that captures every domain point, then draw the
 * captured points as lines / points in the frame's pass. Synchronous - these
 * draws wait for their kernels. */
static void metal_tess_emul_draw(vio_metal_shader *sh, id<MTLCommandBuffer> kcb, NSUInteger out_cp,
                                 id<MTLBuffer> cp_out, NSUInteger cp_off, id<MTLBuffer> patch_out, NSUInteger patch_off,
                                 id<MTLBuffer> factors, NSUInteger fac_off, NSUInteger patches)
{
    [kcb commit];
    [kcb waitUntilCompleted];
    const int dom = sh->tess_emul_domain, sp = sh->tess_info.spacing;
    const int quadlike = dom != VIO_MSL_DOMAIN_TRIANGLES;
    const size_t fstride = quadlike ? 6 : 4;   /* halfs per MTL{Quad,Triangle}TessellationFactorsHalf */
    NSUInteger hw_off = 0, info_off = 0;
    id<MTLBuffer> hw = metal_ring_alloc(patches * fstride * 2, &hw_off);
    id<MTLBuffer> info = metal_ring_alloc(patches * 64, &info_off);
    if (!hw || !info) return;
    const __fp16 *fh = (const __fp16 *)((const char *)[factors contents] + fac_off);
    uint16_t *hwp = (uint16_t *)((char *)[hw contents] + hw_off);
    uint32_t *ip = (uint32_t *)((char *)[info contents] + info_off);
    uint32_t total = 0;
    uint32_t *lines = NULL;
    size_t line_count = 0, line_cap = 0;

    for (NSUInteger pi = 0; pi < patches; pi++) {
        const __fp16 *f = fh + pi * fstride;
        uint16_t *h = hwp + pi * fstride;
        uint32_t *t = ip + pi * 16;
        memset(t, 0, 64);
        memset(h, 0, fstride * 2);
        t[0] = total;
        if (dom == VIO_MSL_DOMAIN_ISOLINES) {
            float fl;
            int lines_n = metal_tess_round((float)f[0], VIO_MSL_SPACING_EQUAL, NULL);
            int seg = metal_tess_round((float)f[1], sp, &fl);
            if (!lines_n || !seg) continue;
            t[1] = (uint32_t)lines_n; t[2] = (uint32_t)seg; t[3] = metal_f2u(fl);
            metal_put_half(h + 0, (float)lines_n); metal_put_half(h + 1, (float)seg);
            metal_put_half(h + 2, (float)lines_n); metal_put_half(h + 3, (float)seg);
            metal_put_half(h + 4, (float)seg);     metal_put_half(h + 5, (float)lines_n);
            for (int i = 0; i < lines_n; i++) {
                for (int j = 0; j < seg; j++) {
                    if (line_count + 2 > line_cap) {
                        line_cap = line_cap ? line_cap * 2 : 1024;
                        uint32_t *g = (uint32_t *)realloc(lines, line_cap * 4);
                        if (!g) { free(lines); return; }
                        lines = g;
                    }
                    uint32_t v = total + (uint32_t)(i * (seg + 1) + j);
                    lines[line_count++] = v;
                    lines[line_count++] = v + 1;
                }
            }
            total += (uint32_t)(lines_n * (seg + 1));
        } else if (dom == VIO_MSL_DOMAIN_QUADS) {
            float lv[4], li[2];
            int m[4], ok = 1, ones = 1;
            for (int e = 0; e < 4; e++) { m[e] = metal_tess_round((float)f[e], sp, &lv[e]); ok = ok && m[e]; ones = ones && m[e] == 1; }
            if (!ok) continue;
            int in0r = metal_tess_round((float)f[4] > 0 ? (float)f[4] : 1.0f, sp, NULL);
            int in1r = metal_tess_round((float)f[5] > 0 ? (float)f[5] : 1.0f, sp, NULL);
            ones = ones && in0r == 1 && in1r == 1;
            int n0 = metal_tess_inner((float)f[4], sp, ones, &li[0]);
            int n1 = metal_tess_inner((float)f[5], sp, ones, &li[1]);
            t[1] = (uint32_t)m[0]; t[2] = (uint32_t)m[1]; t[3] = (uint32_t)m[2];
            t[4] = (uint32_t)m[3]; t[5] = (uint32_t)n0; t[6] = (uint32_t)n1;
            for (int e = 0; e < 4; e++) { t[8 + e] = metal_f2u(lv[e]); metal_put_half(h + e, (float)m[e]); }
            t[12] = metal_f2u(li[0]); t[13] = metal_f2u(li[1]);
            metal_put_half(h + 4, (float)n0); metal_put_half(h + 5, (float)n1);
            total += 4 + (uint32_t)(m[0] - 1 + m[1] - 1 + m[2] - 1 + m[3] - 1) + (uint32_t)((n0 - 1) * (n1 - 1));
        } else {
            float lv[3], li;
            int m[3], ok = 1, ones = 1;
            for (int e = 0; e < 3; e++) { m[e] = metal_tess_round((float)f[e], sp, &lv[e]); ok = ok && m[e]; ones = ones && m[e] == 1; }
            if (!ok) continue;
            ones = ones && metal_tess_round((float)f[3] > 0 ? (float)f[3] : 1.0f, sp, NULL) == 1;
            int n = metal_tess_inner((float)f[3], sp, ones, &li);
            t[1] = (uint32_t)m[0]; t[2] = (uint32_t)m[1]; t[3] = (uint32_t)m[2]; t[4] = (uint32_t)n;
            for (int e = 0; e < 3; e++) { t[8 + e] = metal_f2u(lv[e]); metal_put_half(h + e, (float)m[e]); }
            t[12] = metal_f2u(li);
            metal_put_half(h + 3, (float)n);
            uint32_t pts = 3 + (uint32_t)(m[0] - 1 + m[1] - 1 + m[2] - 1);
            if (!ones) for (int k = 1; 2 * k <= n; k++) pts += (n == 2 * k) ? 1 : 3 * (uint32_t)(n - 2 * k);
            total += pts;
        }
    }
    if (total == 0) { free(lines); return; }

    NSUInteger cap_off = 0, idx_off = 0;
    id<MTLBuffer> cap = metal_ring_alloc((NSUInteger)total * sh->tess_info.tes_out_stride, &cap_off);
    id<MTLBuffer> idx = nil;
    if (line_count) {
        idx = metal_ring_alloc(line_count * 4, &idx_off);
        if (idx) memcpy((char *)[idx contents] + idx_off, lines, line_count * 4);
    }
    free(lines);
    if (!cap || (dom == VIO_MSL_DOMAIN_ISOLINES && !idx)) return;

    if (!sh->tess_cap_pso) {
        MTLRenderPipelineDescriptor *d = [[MTLRenderPipelineDescriptor alloc] init];
        d.vertexFunction = (__bridge id<MTLFunction>)sh->tess_cap_fn;
        d.rasterizationEnabled = NO;
        d.tessellationPartitionMode = MTLTessellationPartitionModeInteger;
        d.tessellationFactorFormat = MTLTessellationFactorFormatHalf;
        d.tessellationFactorStepFunction = MTLTessellationFactorStepFunctionPerPatch;
        d.tessellationControlPointIndexType = MTLTessellationControlPointIndexTypeNone;
        d.tessellationOutputWindingOrder = MTLWindingCounterClockwise;
        d.tessellationFactorScaleEnabled = NO;
        d.maxTessellationFactor = 64;
        NSError *err = nil;
        id<MTLRenderPipelineState> pso = [vio_mtl.device newRenderPipelineStateWithDescriptor:d error:&err];
        if (!pso) {
            php_error_docref(NULL, E_WARNING, "Metal: tessellation capture pipeline failed: %s",
                             err ? [[err localizedDescription] UTF8String] : "unknown");
            return;
        }
        sh->tess_cap_pso = (void *)CFBridgingRetain(pso);
    }

    /* capture pass: no attachments, no rasterization */
    id<MTLCommandBuffer> cb = metal_new_command_buffer();
    MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
    rp.renderTargetWidth = 1;
    rp.renderTargetHeight = 1;
    rp.defaultRasterSampleCount = 1;
    id<MTLRenderCommandEncoder> ce = [cb renderCommandEncoderWithDescriptor:rp];
    [ce setRenderPipelineState:(__bridge id<MTLRenderPipelineState>)sh->tess_cap_pso];
    [ce setVertexBuffer:cp_out offset:cp_off atIndex:VIO_METAL_TESS_IN_INDEX];
    [ce setVertexBuffer:patch_out offset:patch_off atIndex:VIO_METAL_TESS_PATCH_IN_INDEX];
    [ce setVertexBuffer:factors offset:fac_off atIndex:VIO_METAL_TESS_FACTOR_INDEX];   /* gl_TessLevel* as written */
    [ce setVertexBuffer:info offset:info_off atIndex:VIO_METAL_TESS_EMUL_INFO_INDEX];
    [ce setVertexBuffer:cap offset:cap_off atIndex:VIO_METAL_TESS_EMUL_CAP_INDEX];
    int ti = VIO_EXTRA_STAGE_INDEX(VIO_STAGE_TESS_EVAL);
    if (sh->tes.cbuffer_index >= 0 && metal_stage_const_size[ti] > 0) {
        [ce setVertexBytes:metal_stage_const[ti] length:metal_stage_const_size[ti] atIndex:(NSUInteger)sh->tes.cbuffer_index];
    }
    [ce setTessellationFactorBuffer:hw offset:hw_off instanceStride:0];
    [ce drawPatches:out_cp patchStart:0 patchCount:patches patchIndexBuffer:nil patchIndexBufferOffset:0
      instanceCount:1 baseInstance:0];
    [ce endEncoding];
    [cb commit];

    /* the draw: captured points through vio_pass */
    if (!metal_prepare_draw(0)) return;
    id<MTLRenderCommandEncoder> enc = vio_mtl.current_encoder;
    [enc setVertexBuffer:cap offset:cap_off atIndex:VIO_METAL_TESS_EMUL_CAP_INDEX];
    if (dom == VIO_MSL_DOMAIN_ISOLINES) {
        [enc drawIndexedPrimitives:MTLPrimitiveTypeLine indexCount:line_count indexType:MTLIndexTypeUInt32
                       indexBuffer:idx indexBufferOffset:idx_off];
    } else {
        [enc drawPrimitives:MTLPrimitiveTypePoint vertexStart:0 vertexCount:total];
    }
}

/* Draw the current tessellation pipeline. An indexed draw is de-indexed into a
 * ring slice first (the kernel's stage-input fetch reads vertices in grid
 * order; mesh buffers are Shared, so the CPU can gather them). Every
 * patch_vertices input vertices form one patch; a remainder is dropped like on
 * the other backends. instance_mats: per-instance mat4s (locations 3..6) or
 * NULL for identity. */
static void metal_draw_tess(vio_metal_buffer *vb, int stride, int first_vertex, int vertex_count,
                            vio_metal_buffer *ib, int index_bytes, int first_index, int index_count,
                            int base_vertex, const float *instance_mats, int instances, int with_storage)
{
    vio_metal_pipeline *p = metal_current_pipeline;
    vio_metal_shader *sh = p ? p->shader : NULL;
    if (!sh || !sh->tess || !vb || !vb->buffer || !sh->tcs_pso || !vio_mtl.current_encoder) return;
    if (stride <= 0) stride = sh->vl.vertex_stride;
    if (stride <= 0 || stride % 4 != 0) return;
    if (instances < 1) instances = 1;

    @autoreleasepool {
        const vio_metal_tess_info *ti = &sh->tess_info;
        int pv = p->patch_vertices;
        id<MTLBuffer> in_buf = (__bridge id<MTLBuffer>)vb->buffer;
        NSUInteger in_off = 0;
        size_t vb_count = vb->size / (size_t)stride;
        int n;

        if (ib && ib->buffer && index_count > 0) {
            n = index_count;
            NSUInteger off = 0;
            id<MTLBuffer> rb = metal_ring_alloc((NSUInteger)n * (NSUInteger)stride, &off);
            if (!rb) return;
            const unsigned char *src = (const unsigned char *)[in_buf contents];
            const void *idx = [(__bridge id<MTLBuffer>)ib->buffer contents];
            unsigned char *dst = (unsigned char *)[rb contents] + off;
            size_t ib_count = ib->size / (index_bytes == 2 ? 2 : 4);
            for (int i = 0; i < n; i++) {
                size_t k = (size_t)first_index + (size_t)i;
                long long v = 0;
                if (k < ib_count) v = index_bytes == 2 ? ((const uint16_t *)idx)[k] : ((const uint32_t *)idx)[k];
                v += base_vertex;
                if (v < 0 || (size_t)v >= vb_count) v = 0;
                memcpy(dst + (size_t)i * (size_t)stride, src + (size_t)v * (size_t)stride, (size_t)stride);
            }
            in_buf = rb;
            in_off = off;
        } else {
            if (first_vertex < 0 || (size_t)first_vertex >= vb_count) return;
            n = vertex_count;
            if ((size_t)first_vertex + (size_t)n > vb_count) n = (int)(vb_count - (size_t)first_vertex);
            in_off = (NSUInteger)first_vertex * (NSUInteger)stride;
        }

        int per_instance = n / pv;
        if (per_instance <= 0) return;
        NSUInteger used = (NSUInteger)per_instance * (NSUInteger)pv;
        NSUInteger patches = (NSUInteger)per_instance * (NSUInteger)instances;
        NSUInteger out_cp = (NSUInteger)ti->output_vertices;
        /* MTLQuadTessellationFactorsHalf / MTLTriangleTessellationFactorsHalf */
        NSUInteger factor_bytes = ti->domain == VIO_MSL_DOMAIN_QUADS ? 12 : 8;

        NSUInteger vs_off = 0, cp_off = 0, patch_off = 0, fac_off = 0, inst_off = 0;
        id<MTLBuffer> vs_out    = metal_ring_alloc(used * (NSUInteger)instances * ti->vs_out_stride, &vs_off);
        id<MTLBuffer> cp_out    = metal_ring_alloc(patches * out_cp * ti->cp_stride, &cp_off);
        id<MTLBuffer> patch_out = metal_ring_alloc(patches * ti->patch_stride, &patch_off);
        id<MTLBuffer> factors   = metal_ring_alloc(patches * factor_bytes, &fac_off);
        id<MTLBuffer> inst      = nil;
        if (sh->vl.uses_instance_attribs) {
            inst = metal_ring_alloc((NSUInteger)instances * 64, &inst_off);
            if (inst) {
                float *m = (float *)((char *)[inst contents] + inst_off);
                for (int i = 0; i < instances; i++) {
                    if (instance_mats) {
                        memcpy(m + i * 16, instance_mats + i * 16, 64);
                    } else {
                        static const float identity[16] = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
                        memcpy(m + i * 16, identity, 64);
                    }
                }
            }
        }
        if (!vs_out || !cp_out || !patch_out || !factors || (sh->vl.uses_instance_attribs && !inst)) return;

        id<MTLComputePipelineState> vs_pso = metal_tess_vs_pso(sh, stride);
        id<MTLComputePipelineState> tcs_pso = (__bridge id<MTLComputePipelineState>)sh->tcs_pso;
        if (!vs_pso) return;

        /* An async dispatch recorded earlier in this frame may write what the
         * kernels read: then they go into the frame command buffer behind it
         * (the pass is closed and reopened, the draw's textures and viewports
         * restored). Otherwise they run in their own command buffer, committed
         * ahead of the frame's, and the open pass is left alone. */
        /* Emulated isolines / point_mode wait for their kernels: async work
         * of the frame is flushed first so they can run in their own buffer. */
        if (sh->tess_emul) metal_flush_async_keep_pass();
        int in_frame = metal_async_cb != nil && metal_async_cb == vio_mtl.current_cmd_buf;
        id<MTLTexture> saved_tex[VIO_METAL_FS_SLOTS];
        id<MTLSamplerState> saved_smp[VIO_METAL_FS_SLOTS];
        MTLViewport saved_vp[VIO_METAL_MAX_VIEWPORTS];
        MTLScissorRect saved_sc[VIO_METAL_MAX_VIEWPORTS];
        int saved_vp_count = metal_vp_count, saved_has_sc = metal_vp_has_scissor;
        id<MTLCommandBuffer> cb = nil;
        if (in_frame) {
            metal_fs_shadow_copy(saved_tex, saved_smp);
            memcpy(saved_vp, metal_vp, sizeof(saved_vp));
            memcpy(saved_sc, metal_sc, sizeof(saved_sc));
            [vio_mtl.current_encoder endEncoding];
            vio_mtl.current_encoder = nil;
            cb = vio_mtl.current_cmd_buf;
        } else {
            cb = metal_new_command_buffer();
        }
        if (!cb) return;

        /* 1. vertex kernel */
        id<MTLComputeCommandEncoder> ce = [cb computeCommandEncoder];
        [ce setComputePipelineState:vs_pso];
        [ce setBuffer:in_buf offset:in_off atIndex:VIO_METAL_VB_MESH];
        if (inst) [ce setBuffer:inst offset:inst_off atIndex:VIO_METAL_VB_INSTANCE];
        if (with_storage && metal_pending_storage && metal_pending_storage->buffer) {
            /* vio_draw_instanced_from_buffer: the vertex stage's SSBO. */
            int sidx = -1;
            for (int i = 0; i < sh->vs.buffer_count; i++) {
                if (sh->vs.buffers[i].kind == 1 && sh->vs.buffers[i].binding == metal_pending_storage_binding) { sidx = sh->vs.buffers[i].msl_index; break; }
            }
            for (int i = 0; sidx < 0 && i < sh->vs.buffer_count; i++) {
                if (sh->vs.buffers[i].kind == 1) sidx = sh->vs.buffers[i].msl_index;
            }
            if (sidx >= 0) [ce setBuffer:(__bridge id<MTLBuffer>)metal_pending_storage->buffer offset:0 atIndex:(NSUInteger)sidx];
        }
        if (metal_current_vs_cb && metal_current_vs_cb->shadow && sh->vs.cbuffer_index >= 0) {
            [ce setBytes:metal_current_vs_cb->shadow length:metal_current_vs_cb->shadow_size
                 atIndex:(NSUInteger)sh->vs.cbuffer_index];
        }
        [ce setBuffer:vs_out offset:vs_off atIndex:VIO_METAL_TESS_OUT_INDEX];
        [ce setStageInRegion:MTLRegionMake2D(0, 0, used, (NSUInteger)instances)];
        NSUInteger w = vs_pso.maxTotalThreadsPerThreadgroup < 64 ? vs_pso.maxTotalThreadsPerThreadgroup : 64;
        [ce dispatchThreads:MTLSizeMake(used, (NSUInteger)instances, 1)
            threadsPerThreadgroup:MTLSizeMake(w, 1, 1)];
        [ce endEncoding];

        /* 2. control kernel: a threadgroup per patch, so barrier() in the TCS
         * only spans the patch's own control points. */
        ce = [cb computeCommandEncoder];
        [ce setComputePipelineState:tcs_pso];
        [ce setBuffer:vs_out offset:vs_off atIndex:VIO_METAL_TESS_IN_INDEX];
        [ce setBuffer:cp_out offset:cp_off atIndex:VIO_METAL_TESS_OUT_INDEX];
        [ce setBuffer:patch_out offset:patch_off atIndex:VIO_METAL_TESS_PATCH_OUT_INDEX];
        [ce setBuffer:factors offset:fac_off atIndex:VIO_METAL_TESS_FACTOR_INDEX];
        uint32_t params[2] = { (uint32_t)pv, (uint32_t)patches };
        [ce setBytes:params length:sizeof(params) atIndex:VIO_METAL_TESS_PARAMS_INDEX];
        int tc = VIO_EXTRA_STAGE_INDEX(VIO_STAGE_TESS_CONTROL);
        if (sh->tcs.cbuffer_index >= 0 && metal_stage_const_size[tc] > 0) {
            [ce setBytes:metal_stage_const[tc] length:metal_stage_const_size[tc]
                 atIndex:(NSUInteger)sh->tcs.cbuffer_index];
        }
        [ce dispatchThreadgroups:MTLSizeMake(patches, 1, 1) threadsPerThreadgroup:MTLSizeMake(out_cp, 1, 1)];
        [ce endEncoding];
        if (sh->tess_emul && !in_frame) {
            metal_tess_emul_draw(sh, cb, out_cp, cp_out, cp_off, patch_out, patch_off, factors, fac_off, patches);
            return;
        }
        if (in_frame) {
            metal_open_encoder(/*load_clear=*/0);
            metal_fs_shadow_restore(saved_tex, saved_smp);
            memcpy(metal_vp, saved_vp, sizeof(saved_vp));
            memcpy(metal_sc, saved_sc, sizeof(saved_sc));
            metal_vp_count = saved_vp_count;
            metal_vp_has_scissor = saved_has_sc;
            metal_apply_viewports();
        } else {
            [cb commit];
        }

        /* 3. tessellate + rasterize in the frame's render pass */
        if (!metal_prepare_draw(stride)) return;
        id<MTLRenderCommandEncoder> enc = vio_mtl.current_encoder;
        [enc setVertexBuffer:cp_out offset:cp_off atIndex:VIO_METAL_TESS_IN_INDEX];
        [enc setVertexBuffer:patch_out offset:patch_off atIndex:VIO_METAL_TESS_PATCH_IN_INDEX];
        [enc setVertexBuffer:factors offset:fac_off atIndex:VIO_METAL_TESS_FACTOR_INDEX];   /* gl_TessLevel* in the TES */
        [enc setTessellationFactorBuffer:factors offset:fac_off instanceStride:0];
        [enc drawPatches:out_cp
              patchStart:0
              patchCount:patches
        patchIndexBuffer:nil
  patchIndexBufferOffset:0
           instanceCount:1
            baseInstance:0];
    }
}

/* ── Geometry-stage draws (METAL-GEOMETRY-PLAN.md) ─────────────────
 *
 * 1. CPU: primitive assembly into a stream of mesh vertex indices, gs_in_vertices
 *    per primitive (lists as they are, strips / fans expanded in GL order);
 * 2. vertex kernel: one thread per (stream vertex, instance) -> vertex records;
 * 3. geometry kernel: one thread per (primitive x invocation, instance) ->
 *    geometry records + an index list (unused entries point at record 0, a
 *    vertex outside the clip volume);
 * 4. indexed draw of that list through the pass-through vertex function.
 * Kernels are encoded like the tessellation kernels: own command buffer ahead
 * of the frame, or inside the frame behind an async dispatch. */

/* Source position of vertex k of primitive `prim` for the pipeline topology,
 * -1 when the topology cannot feed this input primitive. */
static long metal_gs_assemble(vio_topology topo, int n_in, long prim, long n_prims, int k)
{
    if (topo == VIO_TRIANGLE_STRIP_ADJACENCY && n_in == 6) {
        /* GL 10.1.12, table 10.1 (1-based), in GS order v1, a12, v2, a23, v3, a31 */
        long i = prim, t[6];
        int last = i == n_prims - 1;
        if (n_prims == 1) { t[0] = 1; t[1] = 2; t[2] = 3; t[3] = 6; t[4] = 5; t[5] = 4; }
        else if (i == 0)  { t[0] = 1; t[1] = 2; t[2] = 3; t[3] = 7; t[4] = 5; t[5] = 4; }
        else if (i & 1)   { t[0] = 2*i+3; t[1] = 2*i-1; t[2] = 2*i+1; t[3] = 2*i+4; t[4] = 2*i+5; t[5] = last ? 2*i+6 : 2*i+7; }
        else              { t[0] = 2*i+1; t[1] = 2*i-1; t[2] = 2*i+3; t[3] = last ? 2*i+6 : 2*i+7; t[4] = 2*i+5; t[5] = 2*i+4; }
        return t[k] - 1;
    }
    switch (topo) {
        case VIO_LINE_STRIP:
            if (n_in == 2) return prim + k;
            break;
        case VIO_TRIANGLE_STRIP:
            if (n_in == 3) {
                /* odd triangles swap their first two vertices (GL 10.1.4) */
                if ((prim & 1) && k < 2) return prim + (1 - k);
                return prim + k;
            }
            break;
        case VIO_TRIANGLE_FAN:
            if (n_in == 3) return k == 0 ? prim + 1 : (k == 1 ? prim + 2 : 0);
            break;
        case VIO_LINE_STRIP_ADJACENCY:
            if (n_in == 4) return prim + k;
            break;
        default:
            break;
    }
    return prim * n_in + k;
}

static long metal_gs_prim_count(vio_topology topo, int n_in, long n_src)
{
    switch (topo) {
        case VIO_LINE_STRIP:            if (n_in == 2) return n_src - 1; break;
        case VIO_TRIANGLE_STRIP:
        case VIO_TRIANGLE_FAN:          if (n_in == 3) return n_src - 2; break;
        case VIO_LINE_STRIP_ADJACENCY:  if (n_in == 4) return n_src - 3; break;
        case VIO_TRIANGLE_STRIP_ADJACENCY: if (n_in == 6) return n_src >= 6 ? (n_src - 4) / 2 : 0; break;
        default: break;
    }
    return n_src / n_in;
}

static void metal_gs_kernel_textures(id<MTLComputeCommandEncoder> ce, const vio_metal_stage_res *res, int k)
{
    for (int i = 0; i < res->texture_count; i++) {
        int ix = res->textures[i].msl_index;
        if (ix < 0 || ix >= VIO_METAL_FS_SLOTS || !metal_gs_tex[k][ix]) continue;
        [ce setTexture:metal_gs_tex[k][ix] atIndex:(NSUInteger)ix];
        if (metal_gs_smp[k][ix]) [ce setSamplerState:metal_gs_smp[k][ix] atIndex:(NSUInteger)ix];
    }
}

static void metal_draw_gs(vio_metal_buffer *vb, int stride, int first_vertex, int vertex_count,
                          vio_metal_buffer *ib, int index_bytes, int first_index, int index_count,
                          int base_vertex, const float *instance_mats, int instances, int with_storage)
{
    vio_metal_pipeline *p = metal_current_pipeline;
    vio_metal_shader *sh = p ? p->shader : NULL;
    if (!sh || !sh->gs || !vb || !vb->buffer || !sh->gs_vs_pso || !sh->gs_pso || !vio_mtl.current_encoder) return;
    if (stride <= 0) stride = sh->gs_mesh_stride;
    if (stride < 0 || stride % 4 != 0) return;
    if (instances < 1) instances = 1;

    @autoreleasepool {
        int indexed = ib && ib->buffer && index_count > 0;
        long n_src = indexed ? index_count : vertex_count;
        size_t vb_count = stride > 0 ? vb->size / (size_t)stride : 1;
        long prims = metal_gs_prim_count(p->topology, sh->gs_in_vertices, n_src);
        if (prims <= 0 || sh->gs_idx_per == 0) return;
        NSUInteger stream = (NSUInteger)prims * (NSUInteger)sh->gs_in_vertices;
        NSUInteger total_inv = (NSUInteger)prims * (NSUInteger)sh->gs_invocations * (NSUInteger)instances;

        NSUInteger vid_off = 0, vrec_off = 0, grec_off = 0, idx_off = 0, inst_off = 0;
        id<MTLBuffer> vid  = metal_ring_alloc(stream * 4, &vid_off);
        id<MTLBuffer> vrec = metal_ring_alloc(stream * (NSUInteger)instances * (NSUInteger)sh->gs_vs_slots * 16, &vrec_off);
        id<MTLBuffer> grec = metal_ring_alloc((1 + total_inv * (NSUInteger)sh->gs_max_vertices) * (NSUInteger)sh->gs_slots * 16, &grec_off);
        id<MTLBuffer> idx  = metal_ring_alloc(total_inv * (NSUInteger)sh->gs_idx_per * 4, &idx_off);
        id<MTLBuffer> inst = nil;
        if (sh->gs_vs_inst) {
            inst = metal_ring_alloc((NSUInteger)instances * 64, &inst_off);
            if (inst) {
                float *m = (float *)((char *)[inst contents] + inst_off);
                static const float identity[16] = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
                for (int i = 0; i < instances; i++) memcpy(m + i * 16, instance_mats ? instance_mats + i * 16 : identity, 64);
            }
        }
        if (!vid || !vrec || !grec || !idx || (sh->gs_vs_inst && !inst)) return;

        /* 1. assembly: mesh vertex of every stream position */
        {
            uint32_t *dst = (uint32_t *)((char *)[vid contents] + vid_off);
            const void *ix = indexed ? [(__bridge id<MTLBuffer>)ib->buffer contents] : NULL;
            size_t ib_count = indexed ? ib->size / (index_bytes == 2 ? 2 : 4) : 0;
            for (long pr = 0; pr < prims; pr++) {
                for (int k = 0; k < sh->gs_in_vertices; k++) {
                    long s = metal_gs_assemble(p->topology, sh->gs_in_vertices, pr, prims, k);
                    long long v;
                    if (indexed) {
                        size_t at = (size_t)first_index + (size_t)s;
                        v = at < ib_count ? (index_bytes == 2 ? ((const uint16_t *)ix)[at] : ((const uint32_t *)ix)[at]) : 0;
                        v += base_vertex;
                    } else {
                        v = (long long)first_vertex + s;
                    }
                    if (v < 0 || (size_t)v >= vb_count) v = 0;
                    dst[pr * sh->gs_in_vertices + k] = (uint32_t)v;
                }
            }
        }
        /* record 0: the culled vertex unused indices point at */
        {
            float *r0 = (float *)((char *)[grec contents] + grec_off);
            memset(r0, 0, (size_t)sh->gs_slots * 16);
            r0[0] = 4.0f; r0[1] = 4.0f; r0[2] = 4.0f; r0[3] = 1.0f;
            r0[6] = 1.0f;   /* point size */
        }

        int in_frame = metal_async_cb != nil && metal_async_cb == vio_mtl.current_cmd_buf;
        id<MTLTexture> saved_tex[VIO_METAL_FS_SLOTS];
        id<MTLSamplerState> saved_smp[VIO_METAL_FS_SLOTS];
        MTLViewport saved_vp[VIO_METAL_MAX_VIEWPORTS];
        MTLScissorRect saved_sc[VIO_METAL_MAX_VIEWPORTS];
        int saved_vp_count = metal_vp_count, saved_has_sc = metal_vp_has_scissor;
        id<MTLCommandBuffer> cb = nil;
        if (in_frame) {
            metal_fs_shadow_copy(saved_tex, saved_smp);
            memcpy(saved_vp, metal_vp, sizeof(saved_vp));
            memcpy(saved_sc, metal_sc, sizeof(saved_sc));
            [vio_mtl.current_encoder endEncoding];
            vio_mtl.current_encoder = nil;
            cb = vio_mtl.current_cmd_buf;
        } else {
            cb = metal_new_command_buffer();
        }
        if (!cb) return;
        static const unsigned char zero_block[256] = {0};

        /* 2. vertex kernel */
        id<MTLComputePipelineState> vs_pso = (__bridge id<MTLComputePipelineState>)sh->gs_vs_pso;
        id<MTLComputeCommandEncoder> ce = [cb computeCommandEncoder];
        [ce setComputePipelineState:vs_pso];
        int ix;
        if ((ix = mk_res_index(&sh->gs_vsk, VIO_MK_BIND_IN)) >= 0)  [ce setBuffer:(__bridge id<MTLBuffer>)vb->buffer offset:0 atIndex:(NSUInteger)ix];
        if ((ix = mk_res_index(&sh->gs_vsk, VIO_MK_BIND_OUT)) >= 0) [ce setBuffer:vrec offset:vrec_off atIndex:(NSUInteger)ix];
        if ((ix = mk_res_index(&sh->gs_vsk, VIO_MK_BIND_VID)) >= 0) [ce setBuffer:vid offset:vid_off atIndex:(NSUInteger)ix];
        if ((ix = mk_res_index(&sh->gs_vsk, VIO_MK_BIND_INST)) >= 0) {
            if (inst) [ce setBuffer:inst offset:inst_off atIndex:(NSUInteger)ix];
            else if (metal_identity_instance) [ce setBuffer:metal_identity_instance offset:0 atIndex:(NSUInteger)ix];
        }
        if ((ix = mk_res_index(&sh->gs_vsk, VIO_MK_BIND_PARAMS)) >= 0) {
            uint32_t params[4] = { (uint32_t)stream, (uint32_t)instances, (uint32_t)(stride / 4), 0 };
            [ce setBytes:params length:sizeof(params) atIndex:(NSUInteger)ix];
        }
        if (sh->gs_vs_cb >= 0) {
            if (metal_current_vs_cb && metal_current_vs_cb->shadow) {
                [ce setBytes:metal_current_vs_cb->shadow length:metal_current_vs_cb->shadow_size atIndex:(NSUInteger)sh->gs_vs_cb];
            } else {
                [ce setBytes:zero_block length:sizeof(zero_block) atIndex:(NSUInteger)sh->gs_vs_cb];
            }
        }
        if (with_storage && metal_pending_storage && metal_pending_storage->buffer &&
            (ix = mk_user_storage(&sh->gs_vsk, metal_pending_storage_binding)) >= 0) {
            [ce setBuffer:(__bridge id<MTLBuffer>)metal_pending_storage->buffer offset:0 atIndex:(NSUInteger)ix];
        }
        metal_gs_kernel_textures(ce, &sh->gs_vsk, 0);
        NSUInteger tw = vs_pso.maxTotalThreadsPerThreadgroup < VIO_MK_LOCAL_SIZE ? vs_pso.maxTotalThreadsPerThreadgroup : VIO_MK_LOCAL_SIZE;
        [ce dispatchThreads:MTLSizeMake(stream, (NSUInteger)instances, 1) threadsPerThreadgroup:MTLSizeMake(tw, 1, 1)];
        [ce endEncoding];

        /* 3. geometry kernel */
        id<MTLComputePipelineState> gs_pso = (__bridge id<MTLComputePipelineState>)sh->gs_pso;
        ce = [cb computeCommandEncoder];
        [ce setComputePipelineState:gs_pso];
        if ((ix = mk_res_index(&sh->gs_gsk, VIO_MK_BIND_IN)) >= 0)  [ce setBuffer:vrec offset:vrec_off atIndex:(NSUInteger)ix];
        if ((ix = mk_res_index(&sh->gs_gsk, VIO_MK_BIND_OUT)) >= 0) [ce setBuffer:grec offset:grec_off atIndex:(NSUInteger)ix];
        if ((ix = mk_res_index(&sh->gs_gsk, VIO_MK_BIND_IDX)) >= 0) [ce setBuffer:idx offset:idx_off atIndex:(NSUInteger)ix];
        if ((ix = mk_res_index(&sh->gs_gsk, VIO_MK_BIND_PARAMS)) >= 0) {
            uint32_t params[4] = { (uint32_t)prims, (uint32_t)stream, (uint32_t)instances, 0 };
            [ce setBytes:params length:sizeof(params) atIndex:(NSUInteger)ix];
        }
        if (sh->gs_gs_cb >= 0) {
            int gi = VIO_EXTRA_STAGE_INDEX(VIO_STAGE_GEOMETRY);
            if (metal_stage_const_size[gi] > 0) {
                [ce setBytes:metal_stage_const[gi] length:metal_stage_const_size[gi] atIndex:(NSUInteger)sh->gs_gs_cb];
            } else {
                [ce setBytes:zero_block length:sizeof(zero_block) atIndex:(NSUInteger)sh->gs_gs_cb];
            }
        }
        metal_gs_kernel_textures(ce, &sh->gs_gsk, 1);
        NSUInteger gx = (NSUInteger)prims * (NSUInteger)sh->gs_invocations;
        tw = gs_pso.maxTotalThreadsPerThreadgroup < VIO_MK_LOCAL_SIZE ? gs_pso.maxTotalThreadsPerThreadgroup : VIO_MK_LOCAL_SIZE;
        [ce dispatchThreads:MTLSizeMake(gx, (NSUInteger)instances, 1) threadsPerThreadgroup:MTLSizeMake(tw, 1, 1)];
        [ce endEncoding];

        if (in_frame) {
            metal_open_encoder(/*load_clear=*/0);
            metal_fs_shadow_restore(saved_tex, saved_smp);
            memcpy(metal_vp, saved_vp, sizeof(saved_vp));
            memcpy(metal_sc, saved_sc, sizeof(saved_sc));
            metal_vp_count = saved_vp_count;
            metal_vp_has_scissor = saved_has_sc;
            metal_apply_viewports();
        } else {
            [cb commit];
        }

        /* 4. rasterize the emitted primitives */
        if (!metal_prepare_draw(0)) return;
        id<MTLRenderCommandEncoder> enc = vio_mtl.current_encoder;
        if (sh->gs_rec_index >= 0) [enc setVertexBuffer:grec offset:grec_off atIndex:(NSUInteger)sh->gs_rec_index];
        [enc drawIndexedPrimitives:p->primitive
                        indexCount:total_inv * (NSUInteger)sh->gs_idx_per
                         indexType:MTLIndexTypeUInt32
                       indexBuffer:idx
                 indexBufferOffset:idx_off];
    }
}

static int metal_gs_bound(void)
{
    return metal_current_pipeline && metal_current_pipeline->shader && metal_current_pipeline->shader->gs;
}

static int metal_tess_bound(void)
{
    return metal_current_pipeline && metal_current_pipeline->shader && metal_current_pipeline->shader->tess;
}

static void metal_draw(vio_draw_cmd *cmd)
{
    if (!cmd || cmd->vertex_count <= 0) return;
    if (metal_gs_bound()) {
        metal_draw_gs((vio_metal_buffer *)cmd->vertex_buffer, cmd->vertex_stride, cmd->first_vertex,
                      cmd->vertex_count, NULL, 0, 0, 0, 0, NULL, cmd->instance_count, 0);
        return;
    }
    if (metal_tess_bound()) {
        metal_draw_tess((vio_metal_buffer *)cmd->vertex_buffer, cmd->vertex_stride, cmd->first_vertex,
                        cmd->vertex_count, NULL, 0, 0, 0, 0, NULL, cmd->instance_count, 0);
        return;
    }
    @autoreleasepool {
        if (!metal_prepare_draw(cmd->vertex_stride)) return;
        metal_bind_mesh_vb(cmd->vertex_buffer);
        NSUInteger instances = metal_mv_instances(cmd->instance_count > 0 ? (NSUInteger)cmd->instance_count : 1);
        [vio_mtl.current_encoder drawPrimitives:metal_current_pipeline->primitive
                                    vertexStart:(NSUInteger)cmd->first_vertex
                                    vertexCount:(NSUInteger)cmd->vertex_count
                                  instanceCount:instances];
    }
}

static void metal_draw_indexed(vio_draw_indexed_cmd *cmd)
{
    if (!cmd || cmd->index_count <= 0) return;
    vio_metal_buffer *ib = (vio_metal_buffer *)cmd->index_buffer;
    if (!ib || !ib->buffer) return;
    if (metal_gs_bound()) {
        metal_draw_gs((vio_metal_buffer *)cmd->vertex_buffer, cmd->vertex_stride, 0, 0,
                      ib, cmd->index_bytes, cmd->first_index, cmd->index_count, cmd->vertex_offset,
                      NULL, cmd->instance_count, 0);
        return;
    }
    if (metal_tess_bound()) {
        metal_draw_tess((vio_metal_buffer *)cmd->vertex_buffer, cmd->vertex_stride, 0, 0,
                        ib, cmd->index_bytes, cmd->first_index, cmd->index_count, cmd->vertex_offset,
                        NULL, cmd->instance_count, 0);
        return;
    }
    @autoreleasepool {
        if (!metal_prepare_draw(cmd->vertex_stride)) return;
        metal_bind_mesh_vb(cmd->vertex_buffer);
        NSUInteger instances = metal_mv_instances(cmd->instance_count > 0 ? (NSUInteger)cmd->instance_count : 1);
        [vio_mtl.current_encoder drawIndexedPrimitives:metal_current_pipeline->primitive
                                            indexCount:(NSUInteger)cmd->index_count
                                             indexType:(cmd->index_bytes == 2 ? MTLIndexTypeUInt16 : MTLIndexTypeUInt32)
                                           indexBuffer:(__bridge id<MTLBuffer>)ib->buffer
                                     indexBufferOffset:(NSUInteger)cmd->first_index * (cmd->index_bytes == 2 ? 2 : 4)
                                         instanceCount:instances
                                            baseVertex:(NSInteger)cmd->vertex_offset
                                          baseInstance:0];
    }
}

/* Issue the bound mesh with instance_count instances, index buffer if present. */
static void metal_draw_mesh_instances(vio_mesh_object *mesh, int instance_count)
{
    metal_bind_mesh_vb(mesh->backend_vb);
    vio_metal_buffer *ib = (vio_metal_buffer *)mesh->backend_ib;
    if (mesh->index_count > 0 && ib && ib->buffer) {
        [vio_mtl.current_encoder drawIndexedPrimitives:metal_current_pipeline->primitive
                                            indexCount:(NSUInteger)mesh->index_count
                                             indexType:(mesh->index_bytes == 2 ? MTLIndexTypeUInt16 : MTLIndexTypeUInt32)
                                           indexBuffer:(__bridge id<MTLBuffer>)ib->buffer
                                     indexBufferOffset:0
                                         instanceCount:metal_mv_instances((NSUInteger)instance_count)
                                            baseVertex:0
                                          baseInstance:0];
    } else if (mesh->vertex_count > 0) {
        [vio_mtl.current_encoder drawPrimitives:metal_current_pipeline->primitive
                                    vertexStart:0
                                    vertexCount:(NSUInteger)mesh->vertex_count
                                  instanceCount:metal_mv_instances((NSUInteger)instance_count)];
    }
}

void vio_metal_draw_instanced(void *mesh_obj, const float *matrices_4x4, int instance_count)
{
    vio_mesh_object *mesh = (vio_mesh_object *)mesh_obj;
    if (!mesh || !matrices_4x4 || instance_count <= 0) return;
    if (metal_gs_bound()) {
        metal_draw_gs((vio_metal_buffer *)mesh->backend_vb, mesh->stride, 0, mesh->vertex_count,
                      mesh->index_count > 0 ? (vio_metal_buffer *)mesh->backend_ib : NULL,
                      mesh->index_bytes, 0, mesh->index_count, 0, matrices_4x4, instance_count, 0);
        return;
    }
    if (metal_tess_bound()) {
        metal_draw_tess((vio_metal_buffer *)mesh->backend_vb, mesh->stride, 0, mesh->vertex_count,
                        mesh->index_count > 0 ? (vio_metal_buffer *)mesh->backend_ib : NULL,
                        mesh->index_bytes, 0, mesh->index_count, 0, matrices_4x4, instance_count, 0);
        return;
    }
    @autoreleasepool {
        if (!metal_prepare_draw(mesh->stride)) return;

        /* This draw's OWN instance slice (see file header: no renaming). */
        NSUInteger bytes = (NSUInteger)instance_count * 64;
        NSUInteger off = 0;
        id<MTLBuffer> rb = metal_ring_alloc(bytes, &off);
        if (!rb) return;
        memcpy((char *)[rb contents] + off, matrices_4x4, bytes);
        [vio_mtl.current_encoder setVertexBuffer:rb offset:off atIndex:VIO_METAL_VB_INSTANCE];

        metal_draw_mesh_instances(mesh, instance_count);
    }
}

/* ── Graphics-stage storage buffers (Path B: readback-free instancing) ── */

static void metal_bind_storage_buffer(void *backend_buffer, int binding, int access,
                                      int element_count, int stride)
{
    (void)access; (void)element_count; (void)stride;
    metal_pending_storage = (vio_metal_buffer *)backend_buffer;
    metal_pending_storage_binding = binding;
}

/* Indirect draw (GAP-PHASE5 Block 8): Metal's indirect argument structs share the
 * 5 / 4 uint32 layout, one draw per record. Per-instance storage binding as in
 * metal_draw_instanced_from_storage. */
/* vio_bind_storage_buffer's buffer at the vertex stage's SSBO slot (its GLSL
 * binding, else the first SSBO) - the indirect draws read per-instance data there. */
static void metal_bind_pending_storage_vs(void)
{
    vio_metal_buffer *sb = metal_pending_storage;
    if (!sb || !sb->buffer || !metal_current_pipeline || !metal_current_pipeline->shader) return;
    vio_metal_stage_res *vs = &metal_current_pipeline->shader->vs;
    int idx = -1;
    for (int i = 0; i < vs->buffer_count; i++) {
        if (vs->buffers[i].kind == 1 && vs->buffers[i].binding == metal_pending_storage_binding) { idx = vs->buffers[i].msl_index; break; }
    }
    if (idx < 0) {
        for (int i = 0; i < vs->buffer_count; i++) { if (vs->buffers[i].kind == 1) { idx = vs->buffers[i].msl_index; break; } }
    }
    if (idx >= 0) {
        [vio_mtl.current_encoder setVertexBuffer:(__bridge id<MTLBuffer>)sb->buffer offset:0 atIndex:(NSUInteger)idx];
    }
}

static void metal_draw_indirect(void *mesh_obj, void *args_buffer, int max_draws, size_t offset)
{
    vio_mesh_object *mesh = (vio_mesh_object *)mesh_obj;
    vio_metal_buffer *args = (vio_metal_buffer *)args_buffer;
    if (!mesh || !args || !args->buffer || max_draws <= 0) return;
    int mv = metal_current_pipeline && metal_current_pipeline->shader && metal_current_pipeline->shader->view_count > 1;
    if (metal_tess_bound() || metal_gs_bound() || mv) {
        int gs = metal_gs_bound();
        /* The tessellation / geometry kernels need the vertex and instance counts when they
         * are encoded, so the argument records are read on the CPU (the buffer
         * is Shared). A dispatch of this frame that writes them runs first; the
         * draw's textures and viewports survive that flush. */
        @autoreleasepool {
            id<MTLTexture> saved_tex[VIO_METAL_FS_SLOTS];
            id<MTLSamplerState> saved_smp[VIO_METAL_FS_SLOTS];
            MTLViewport saved_vp[VIO_METAL_MAX_VIEWPORTS];
            MTLScissorRect saved_sc[VIO_METAL_MAX_VIEWPORTS];
            int saved_vp_count = metal_vp_count, saved_has_sc = metal_vp_has_scissor;
            int flush = metal_async_cb != nil && metal_async_cb == vio_mtl.current_cmd_buf;
            if (flush) {
                metal_fs_shadow_copy(saved_tex, saved_smp);
                memcpy(saved_vp, metal_vp, sizeof(saved_vp));
                memcpy(saved_sc, metal_sc, sizeof(saved_sc));
            }
            metal_compute_wait();
            if (flush) {
                metal_fs_shadow_restore(saved_tex, saved_smp);
                memcpy(metal_vp, saved_vp, sizeof(saved_vp));
                memcpy(metal_sc, saved_sc, sizeof(saved_sc));
                metal_vp_count = saved_vp_count;
                metal_vp_has_scissor = saved_has_sc;
                metal_apply_viewports();
            }
            vio_metal_buffer *tib = mesh->index_count > 0 ? (vio_metal_buffer *)mesh->backend_ib : NULL;
            int indexed = tib && tib->buffer;
            size_t rec = indexed ? 20 : 16;
            const unsigned char *a = (const unsigned char *)[(__bridge id<MTLBuffer>)args->buffer contents];
            for (int i = 0; a && i < max_draws; i++) {
                size_t o = offset + (size_t)i * rec;
                if (o + rec > args->size) break;
                uint32_t r[5];
                memcpy(r, a + o, rec);
                if (r[0] == 0 || r[1] == 0) continue;
                if (mv) {
                    /* Multiview: the GPU records hold the user's instance count;
                     * issue the draw with it multiplied by the views. */
                    if (!metal_prepare_draw(mesh->stride)) break;
                    metal_bind_pending_storage_vs();
                    metal_bind_mesh_vb(mesh->backend_vb);
                    if (indexed) {
                        [vio_mtl.current_encoder drawIndexedPrimitives:metal_current_pipeline->primitive
                                                            indexCount:r[0]
                                                             indexType:(mesh->index_bytes == 2 ? MTLIndexTypeUInt16 : MTLIndexTypeUInt32)
                                                           indexBuffer:(__bridge id<MTLBuffer>)tib->buffer
                                                     indexBufferOffset:(NSUInteger)r[2] * (mesh->index_bytes == 2 ? 2 : 4)
                                                         instanceCount:metal_mv_instances(r[1])
                                                            baseVertex:(NSInteger)(int32_t)r[3]
                                                          baseInstance:r[4]];
                    } else {
                        [vio_mtl.current_encoder drawPrimitives:metal_current_pipeline->primitive
                                                    vertexStart:r[2]
                                                    vertexCount:r[0]
                                                  instanceCount:metal_mv_instances(r[1])
                                                   baseInstance:r[3]];
                    }
                    continue;
                }
                void (*draw)(vio_metal_buffer *, int, int, int, vio_metal_buffer *, int, int, int, int,
                             const float *, int, int) = gs ? metal_draw_gs : metal_draw_tess;
                if (indexed) {
                    draw((vio_metal_buffer *)mesh->backend_vb, mesh->stride, 0, 0, tib, mesh->index_bytes,
                         (int)r[2], (int)r[0], (int)(int32_t)r[3], NULL, (int)r[1], 0);
                } else {
                    draw((vio_metal_buffer *)mesh->backend_vb, mesh->stride, (int)r[2], (int)r[0],
                         NULL, 0, 0, 0, 0, NULL, (int)r[1], 0);
                }
            }
        }
        return;
    }
    @autoreleasepool {
        if (!metal_prepare_draw(mesh->stride)) return;
        metal_bind_pending_storage_vs();
        metal_bind_mesh_vb(mesh->backend_vb);
        vio_metal_buffer *ib = (vio_metal_buffer *)mesh->backend_ib;
        id<MTLBuffer> argbuf = (__bridge id<MTLBuffer>)args->buffer;
        if (mesh->index_count > 0 && ib && ib->buffer) {
            for (int i = 0; i < max_draws; i++) {
                [vio_mtl.current_encoder drawIndexedPrimitives:metal_current_pipeline->primitive
                                                     indexType:(mesh->index_bytes == 2 ? MTLIndexTypeUInt16 : MTLIndexTypeUInt32)
                                                   indexBuffer:(__bridge id<MTLBuffer>)ib->buffer
                                             indexBufferOffset:0
                                                indirectBuffer:argbuf
                                          indirectBufferOffset:(NSUInteger)(offset + (size_t)i * 20)];
            }
        } else {
            for (int i = 0; i < max_draws; i++) {
                [vio_mtl.current_encoder drawPrimitives:metal_current_pipeline->primitive
                                         indirectBuffer:argbuf
                                   indirectBufferOffset:(NSUInteger)(offset + (size_t)i * 16)];
            }
        }
    }
}

/* Mesh pipelines: x * y * z object (task) threadgroups, or mesh threadgroups
 * when the pipeline has no task stage. */
static int metal_mesh_bound(void)
{
    return metal_current_pipeline && metal_current_pipeline->shader && metal_current_pipeline->shader->mesh;
}

static void metal_draw_mesh_tasks(uint32_t x, uint32_t y, uint32_t z)
{
    if (!metal_mesh_bound() || !x || !y || !z) return;
    @autoreleasepool {
        if (!metal_prepare_draw(0)) return;
        vio_metal_shader *sh = metal_current_pipeline->shader;
        const unsigned *ot = sh->object_tpg, *mt = sh->mesh_tpg;
        [vio_mtl.current_encoder drawMeshThreadgroups:MTLSizeMake(x, y, z)
                          threadsPerObjectThreadgroup:MTLSizeMake(ot[0], ot[1], ot[2])
                            threadsPerMeshThreadgroup:MTLSizeMake(mt[0], mt[1], mt[2])];
    }
}

static void metal_draw_mesh_tasks_indirect(void *args_buffer, int max_draws, size_t offset)
{
    vio_metal_buffer *args = (vio_metal_buffer *)args_buffer;
    if (!metal_mesh_bound() || !args || !args->buffer || max_draws <= 0) return;
    @autoreleasepool {
        if (!metal_prepare_draw(0)) return;
        vio_metal_shader *sh = metal_current_pipeline->shader;
        const unsigned *ot = sh->object_tpg, *mt = sh->mesh_tpg;
        id<MTLBuffer> argbuf = (__bridge id<MTLBuffer>)args->buffer;
        for (int i = 0; i < max_draws; i++) {
            size_t o = offset + (size_t)i * 12;   /* MTLDispatchThreadgroupsIndirectArguments */
            if (o + 12 > args->size) break;
            [vio_mtl.current_encoder drawMeshThreadgroupsWithIndirectBuffer:argbuf
                                                       indirectBufferOffset:(NSUInteger)o
                                                threadsPerObjectThreadgroup:MTLSizeMake(ot[0], ot[1], ot[2])
                                                  threadsPerMeshThreadgroup:MTLSizeMake(mt[0], mt[1], mt[2])];
        }
    }
}

static void metal_draw_instanced_from_storage(void *mesh_obj, int instance_count)
{
    vio_mesh_object *mesh = (vio_mesh_object *)mesh_obj;
    if (!mesh || instance_count <= 0) return;
    if (metal_gs_bound()) {
        /* The vertex kernel reads the SSBO (gl_InstanceIndex = grid row). */
        metal_draw_gs((vio_metal_buffer *)mesh->backend_vb, mesh->stride, 0, mesh->vertex_count,
                      mesh->index_count > 0 ? (vio_metal_buffer *)mesh->backend_ib : NULL,
                      mesh->index_bytes, 0, mesh->index_count, 0, NULL, instance_count, 1);
        return;
    }
    if (metal_tess_bound()) {
        /* The vertex kernel reads the SSBO (gl_InstanceIndex = grid row). */
        metal_draw_tess((vio_metal_buffer *)mesh->backend_vb, mesh->stride, 0, mesh->vertex_count,
                        mesh->index_count > 0 ? (vio_metal_buffer *)mesh->backend_ib : NULL,
                        mesh->index_bytes, 0, mesh->index_count, 0, NULL, instance_count, 1);
        return;
    }
    @autoreleasepool {
        if (!metal_prepare_draw(mesh->stride)) return;

        vio_metal_buffer *sb = metal_pending_storage;
        if (sb && sb->buffer) {
            /* The SSBO the vertex stage indexes with gl_InstanceIndex: resolve
             * the GLSL binding to its pinned MSL index; fall back to the stage's
             * only SSBO when the binding is not found. */
            vio_metal_stage_res *vs = &metal_current_pipeline->shader->vs;
            int idx = -1;
            for (int i = 0; i < vs->buffer_count; i++) {
                if (vs->buffers[i].kind == 1 && vs->buffers[i].binding == metal_pending_storage_binding) {
                    idx = vs->buffers[i].msl_index;
                    break;
                }
            }
            if (idx < 0) {
                for (int i = 0; i < vs->buffer_count; i++) {
                    if (vs->buffers[i].kind == 1) { idx = vs->buffers[i].msl_index; break; }
                }
            }
            if (idx >= 0) {
                [vio_mtl.current_encoder setVertexBuffer:(__bridge id<MTLBuffer>)sb->buffer
                                                  offset:0 atIndex:(NSUInteger)idx];
            }
        }
        metal_draw_mesh_instances(mesh, instance_count);
    }
}

/* ── Texture / cubemap / uniform-buffer binds ─────────────────────── */

/* Resolve a PHP-side sampler slot to the fragment stage's pinned MSL index.
 * `slot` is the sampled_images reflection index (after the PHP layer's
 * gl_to_hlsl_sampler remap) or, when nothing was remapped, the raw GL unit. */
static int metal_resolve_fs_texture(int slot, int *is_depth)
{
    *is_depth = 0;
    vio_metal_pipeline *p = metal_current_pipeline;
    if (p && p->shader && slot >= 0 && slot < p->shader->fs.texture_count) {
        *is_depth = p->shader->fs.textures[slot].is_depth;
        return p->shader->fs.textures[slot].msl_index;
    }
    return slot;
}

/* Mesh pipelines (OPEN-ITEMS-PLAN A30): the PHP sampler map lists the fragment
 * samplers, then the mesh stage's new names, then the task stage's
 * (vio_shader_merge_stage_samplers). Resolve `slot` to its name and bind the
 * texture wherever the mesh ([[mesh]], vs table) or object stage declares it. */
static void metal_bind_mesh_stage_texture(int slot, id<MTLTexture> tex, id<MTLSamplerState> smp, id<MTLSamplerState> cmp)
{
    vio_metal_pipeline *p = metal_current_pipeline;
    if (!p || !p->shader || !p->shader->mesh || slot < 0) return;
    const vio_metal_stage_res *stages[3] = { &p->shader->fs, &p->shader->vs, p->shader->object_fn ? &p->shader->obj : NULL };
    const char *names[3 * VIO_METAL_MAX_RES];
    int n = 0;
    for (int s = 0; s < 3; s++) {
        if (!stages[s]) continue;
        for (int i = 0; i < stages[s]->texture_count; i++) {
            const char *nm = stages[s]->textures[i].name;
            int known = 0;
            for (int k = 0; k < n && !known; k++) known = strcmp(names[k], nm) == 0;
            if (!known) names[n++] = nm;
        }
    }
    if (slot >= n) return;
    for (int s = 1; s < 3; s++) {
        if (!stages[s]) continue;
        for (int i = 0; i < stages[s]->texture_count; i++) {
            const vio_metal_res_texture *rt = &stages[s]->textures[i];
            if (strcmp(rt->name, names[slot]) != 0 || rt->msl_index < 0 || rt->msl_index > 30) continue;
            id<MTLSamplerState> use = rt->is_depth && cmp ? cmp : smp;
            if (s == 1) {
                [vio_mtl.current_encoder setMeshTexture:tex atIndex:(NSUInteger)rt->msl_index];
                if (use) [vio_mtl.current_encoder setMeshSamplerState:use atIndex:(NSUInteger)rt->msl_index];
            } else {
                [vio_mtl.current_encoder setObjectTexture:tex atIndex:(NSUInteger)rt->msl_index];
                if (use) [vio_mtl.current_encoder setObjectSamplerState:use atIndex:(NSUInteger)rt->msl_index];
            }
        }
    }
}

/* Emulated geometry stage (A28): the sampler map lists the fragment samplers,
 * then the geometry stage's new names, then the vertex stage's. Samplers the
 * kernels declare are remembered by MSL index for metal_draw_gs. */
static void metal_bind_gs_stage_texture(int slot, id<MTLTexture> tex, id<MTLSamplerState> smp, id<MTLSamplerState> cmp)
{
    vio_metal_pipeline *p = metal_current_pipeline;
    if (!p || !p->shader || !p->shader->gs || slot < 0) return;
    const vio_metal_stage_res *stages[3] = { &p->shader->fs, &p->shader->gs_gsk, &p->shader->gs_vsk };
    const char *names[3 * VIO_METAL_MAX_RES];
    int n = 0;
    for (int s = 0; s < 3; s++) {
        for (int i = 0; i < stages[s]->texture_count; i++) {
            const char *nm = stages[s]->textures[i].name;
            int known = 0;
            for (int k = 0; k < n && !known; k++) known = strcmp(names[k], nm) == 0;
            if (!known) names[n++] = nm;
        }
    }
    if (slot >= n) return;
    for (int s = 1; s < 3; s++) {
        for (int i = 0; i < stages[s]->texture_count; i++) {
            const vio_metal_res_texture *rt = &stages[s]->textures[i];
            if (strcmp(rt->name, names[slot]) != 0 || rt->msl_index < 0 || rt->msl_index >= VIO_METAL_FS_SLOTS) continue;
            int k = s == 2 ? 0 : 1;
            metal_gs_tex[k][rt->msl_index] = tex;
            metal_gs_smp[k][rt->msl_index] = rt->is_depth && cmp ? cmp : smp;
        }
    }
}

static void metal_bind_texture(void *texture, int slot)
{
    vio_metal_texture *t = (vio_metal_texture *)texture;
    if (!t || !t->tex || !vio_mtl.current_encoder || slot < 0) return;
    @autoreleasepool {
        if (metal_current_pipeline && metal_current_pipeline->shader && metal_current_pipeline->shader->gs)
            metal_bind_gs_stage_texture(slot, (__bridge id<MTLTexture>)t->tex,
                                        (__bridge id<MTLSamplerState>)t->sampler, metal_texture_cmp_sampler(t));
        int is_depth = 0;
        int idx = metal_resolve_fs_texture(slot, &is_depth);
        if (idx < 0 || idx > 30) return;
        id<MTLSamplerState> s = is_depth ? metal_texture_cmp_sampler(t)
                                         : (__bridge id<MTLSamplerState>)t->sampler;
        [vio_mtl.current_encoder setFragmentTexture:(__bridge id<MTLTexture>)t->tex atIndex:(NSUInteger)idx];
        if (s) [vio_mtl.current_encoder setFragmentSamplerState:s atIndex:(NSUInteger)idx];
        metal_fs_shadow_set(idx, (__bridge id<MTLTexture>)t->tex, s);
        if (metal_current_pipeline && metal_current_pipeline->shader && metal_current_pipeline->shader->mesh)
            metal_bind_mesh_stage_texture(slot, (__bridge id<MTLTexture>)t->tex,
                                          (__bridge id<MTLSamplerState>)t->sampler, metal_texture_cmp_sampler(t));
    }
}

static int metal_upload_cubemap(void *cm_obj, int width, int height, const void *face_rgba[6])
{
    vio_cubemap_object *cm = (vio_cubemap_object *)cm_obj;
    if (!cm || !vio_mtl.device || width <= 0 || height != width) {
        php_error_docref(NULL, E_WARNING, "Metal: cubemap faces must be square (%dx%d)", width, height);
        return -1;
    }
    @autoreleasepool {
        MTLTextureDescriptor *td = [MTLTextureDescriptor
            textureCubeDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm size:(NSUInteger)width
                                       mipmapped:(cm->mipmaps ? YES : NO)];
        td.usage = MTLTextureUsageShaderRead;
        td.storageMode = metal_cpu_texture_storage();
        id<MTLTexture> tex = [vio_mtl.device newTextureWithDescriptor:td];
        if (!tex) return -1;

        /* Face order +X,-X,+Y,-Y,+Z,-Z == Metal cube slices 0..5. */
        for (int f = 0; f < 6; f++) {
            if (!face_rgba[f]) continue;
            [tex replaceRegion:MTLRegionMake2D(0, 0, width, height)
                   mipmapLevel:0 slice:(NSUInteger)f withBytes:face_rgba[f]
                   bytesPerRow:(NSUInteger)width * 4
                 bytesPerImage:(NSUInteger)width * height * 4];
        }
        if (cm->mipmaps && tex.mipmapLevelCount > 1) {
            id<MTLCommandBuffer> cb = metal_new_command_buffer();
            id<MTLBlitCommandEncoder> blit = [cb blitCommandEncoder];
            [blit generateMipmapsForTexture:tex];
            [blit endEncoding];
            [cb commit];
            [cb waitUntilCompleted];
        }
        id<MTLSamplerState> s = metal_make_sampler(VIO_FILTER_LINEAR, VIO_WRAP_CLAMP, cm->mipmaps ? 1 : 0, 0, 1);
        cm->metal_texture = (void *)CFBridgingRetain(tex);
        cm->metal_sampler = s ? (void *)CFBridgingRetain(s) : NULL;
        cm->backend_type  = 4;  /* VIO_CM_BACKEND_METAL — see vio_cubemap.h */
    }
    return 0;
}

/* vio_render_target_cubemap: wrap the cube RT's colour texture. The wrapper
 * takes its own +1 so RT and cubemap can be released in either order. */
static int metal_render_target_cubemap(void *rt_ptr, void *cm_obj)
{
    vio_render_target_object *rt = (vio_render_target_object *)rt_ptr;
    vio_cubemap_object *cm = (vio_cubemap_object *)cm_obj;
    /* A depth_only cube hands out its depth cube (samplerCube .r, or
     * samplerCubeShadow through the compare sampler in vio_metal_bind_cubemap). */
    void *src = rt && rt->depth_only ? rt->metal_depth_texture : (rt ? rt->metal_color_texture : NULL);
    if (!rt || !cm || !rt->is_cube || !src || !vio_mtl.device) return -1;
    @autoreleasepool {
        id<MTLSamplerState> s = metal_make_sampler(VIO_FILTER_LINEAR, VIO_WRAP_CLAMP, rt->mip_levels > 1 ? 1 : 0, 0, 1);
        cm->metal_texture = (void *)CFRetain((CFTypeRef)src);
        cm->metal_sampler = s ? (void *)CFBridgingRetain(s) : NULL;
        cm->mipmaps       = rt->mip_levels > 1;
        cm->borrowed      = 1;
        cm->resolution    = rt->width;
        cm->backend_type  = 4;
    }
    return 0;
}

/* half -> float for RGBA16F readback. */
static float metal_half_to_float(uint16_t h)
{
    uint32_t sign = (uint32_t)(h & 0x8000) << 16;
    uint32_t exp  = (h >> 10) & 0x1F;
    uint32_t mant = h & 0x3FF;
    uint32_t bits;
    if (exp == 0) {
        if (mant == 0) { bits = sign; }
        else {  /* subnormal */
            exp = 127 - 15 + 1;
            while (!(mant & 0x400)) { mant <<= 1; exp--; }
            mant &= 0x3FF;
            bits = sign | (exp << 23) | (mant << 13);
        }
    } else if (exp == 31) {
        bits = sign | 0x7F800000 | (mant << 13);
    } else {
        bits = sign | ((exp + 127 - 15) << 23) | (mant << 13);
    }
    float f; memcpy(&f, &bits, 4); return f;
}

static inline unsigned char metal_unit_to_byte(float v)
{
    if (v < 0.0f) v = 0.0f; if (v > 1.0f) v = 1.0f;
    return (unsigned char)(v * 255.0f + 0.5f);
}

/* Flush everything recorded so far so a readback sees this frame's draws,
 * then continue the frame on a fresh command buffer (Load semantics). */
static void metal_flush_for_readback(void)
{
    if (!vio_mtl.current_cmd_buf) return;
    if (vio_mtl.current_encoder) {
        [vio_mtl.current_encoder endEncoding];
        vio_mtl.current_encoder = nil;
    }
    [vio_mtl.current_cmd_buf commit];
    [vio_mtl.current_cmd_buf waitUntilCompleted];
    vio_mtl.current_cmd_buf = metal_new_command_buffer();
    metal_open_encoder(/*load_clear=*/0);
}

/* vio_read_render_target: blit one slice of the RT's (resolved) colour or
 * depth texture into a Shared buffer and convert to top-down RGBA8. */
static int metal_read_render_target(void *rt_ptr, int face, int attachment, void *out_rgba)
{
    vio_render_target_object *rt = (vio_render_target_object *)rt_ptr;
    if (!rt || rt->backend_type != VIO_RT_BACKEND_METAL || !vio_mtl.device) return -1;
    if (attachment < 0 || attachment >= metal_rt_attachment_count(rt)) return -1;
    @autoreleasepool {
        id<MTLTexture> src = rt->depth_only
            ? (__bridge id<MTLTexture>)rt->metal_depth_texture
            : (__bridge id<MTLTexture>)rt->metal_color_textures[attachment];   /* resolve texture when MSAA */
        if (!src) return -1;
        NSUInteger slice = 0;
        if (rt->is_cube || rt->layers > 1) slice = (NSUInteger)(face >= 0 ? face : (rt->bound_face >= 0 ? rt->bound_face : 0));
        if ((int)slice >= vio_rt_layer_count(rt)) return -1;

        metal_flush_for_readback();

        int w = rt->width, h = rt->height;
        MTLPixelFormat fmt = src.pixelFormat;
        int is_depth = fmt == MTLPixelFormatDepth32Float || fmt == VIO_METAL_DEPTH_STENCIL;
        int bgra = 0;
        int vfmt = is_depth ? -1 : metal_vio_format(fmt, &bgra);
        if (vfmt < -1) return -1;
        NSUInteger bpp = is_depth ? 4 : (NSUInteger)vio_rt_format_bpp(vfmt);
        NSUInteger bpr = (NSUInteger)w * bpp;
        id<MTLBuffer> staging = [vio_mtl.device newBufferWithLength:bpr * h options:MTLResourceStorageModeShared];
        if (!staging) return -1;

        id<MTLCommandBuffer> cb = metal_new_command_buffer();
        id<MTLBlitCommandEncoder> blit = [cb blitCommandEncoder];
        [blit copyFromTexture:src sourceSlice:slice sourceLevel:0
                 sourceOrigin:MTLOriginMake(0, 0, 0) sourceSize:MTLSizeMake(w, h, 1)
                     toBuffer:staging destinationOffset:0
       destinationBytesPerRow:bpr destinationBytesPerImage:bpr * h
                      options:(fmt == VIO_METAL_DEPTH_STENCIL ? MTLBlitOptionDepthFromDepthStencil : MTLBlitOptionNone)];
        [blit endEncoding];
        [cb commit];
        [cb waitUntilCompleted];

        const unsigned char *s = (const unsigned char *)[staging contents];
        unsigned char *out = (unsigned char *)out_rgba;
        size_t n = (size_t)w * h;
        if (is_depth) {
            const float *d = (const float *)s;
            for (size_t i = 0; i < n; i++) {
                unsigned char g = metal_unit_to_byte(d[i]);
                out[i*4+0] = out[i*4+1] = out[i*4+2] = g; out[i*4+3] = 255;
            }
        } else {
            /* Shared converter: every colour format -> RGBA8 (BGRA swizzled). */
            vio_rt_convert_to_rgba8(vfmt, bgra, s, (size_t)bpr, w, h, out);
        }
    }
    return 0;
}

static int metal_update_texture(void *tex_obj, const void *pixels, int x, int y, int w, int h)
{
    vio_texture_object *t = (vio_texture_object *)tex_obj;
    vio_metal_texture *mt = t ? (vio_metal_texture *)t->backend_texture : NULL;
    if (!mt || !mt->tex || t->is_3d) return -1;
    @autoreleasepool {
        id<MTLTexture> tex = (__bridge id<MTLTexture>)mt->tex;
        NSUInteger bpp = (tex.pixelFormat == MTLPixelFormatR8Unorm) ? 1 : 4;
        /* Shared / Managed textures accept replaceRegion at any time; the GPU
         * sees the new bytes at the next command-buffer commit. */
        [tex replaceRegion:MTLRegionMake2D(x, y, w, h) mipmapLevel:0
                 withBytes:pixels bytesPerRow:(NSUInteger)w * bpp];
    }
    return 0;
}

/* Build the mip chain of an RT colour texture / texture / cubemap. Inside a
 * frame the open pass is closed first so the blit is ordered after the draws
 * that produced level 0, then reopened with Load. */
/* Depth mip chain (A26, blind - the macOS CI is the check): each level is the
 * max / min of the 2x2 texels below (odd sizes fold the extra column / row
 * into the last texel), written as [[depth(any)]] by a full-screen triangle in
 * one render pass per level; the source is a single-level view of the level
 * below. Mipmapped depth targets are Depth32Float (no stencil plane). */
static const char *metal_dmip_msl =
    "#include <metal_stdlib>\n"
    "using namespace metal;\n"
    "struct VOut { float4 pos [[position]]; };\n"
    "struct P { int4 sizes; int4 mode; };\n"
    "struct FOut { float depth [[depth(any)]]; };\n"
    "vertex VOut vio_dmip_vs(uint vid [[vertex_id]]) {\n"
    "    float2 p = float2((vid << 1) & 2, vid & 2);\n"
    "    VOut o; o.pos = float4(p * 2.0 - 1.0, 0.0, 1.0); return o;\n"
    "}\n"
    "fragment FOut vio_dmip_fs(VOut in [[stage_in]], depth2d<float> src [[texture(0)]], constant P& p [[buffer(0)]]) {\n"
    "    int2 o = int2(in.pos.xy);\n"
    "    int2 n = int2((o.x == p.sizes.z - 1 && (p.sizes.x & 1) == 1 && p.sizes.x > 1) ? 3 : 2,\n"
    "                  (o.y == p.sizes.w - 1 && (p.sizes.y & 1) == 1 && p.sizes.y > 1) ? 3 : 2);\n"
    "    float d = p.mode.x == 0 ? 0.0 : 1.0;\n"
    "    for (int y = 0; y < 3; y++) for (int x = 0; x < 3; x++) {\n"
    "        if (x >= n.x || y >= n.y) continue;\n"
    "        int2 c = min(o * 2 + int2(x, y), p.sizes.xy - 1);\n"
    "        float s = src.read(uint2(c), 0);\n"
    "        d = p.mode.x == 0 ? max(d, s) : min(d, s);\n"
    "    }\n"
    "    FOut r; r.depth = d; return r;\n"
    "}\n";

static id<MTLRenderPipelineState> metal_dmip_pso = nil;
static id<MTLDepthStencilState>   metal_dmip_dss = nil;
static id<MTLDevice>              metal_dmip_device = nil;

static int metal_generate_depth_mips(vio_render_target_object *rt)
{
    if (!rt->metal_depth_texture || rt->mip_levels < 2) return -1;
    id<MTLTexture> tex = (__bridge id<MTLTexture>)rt->metal_depth_texture;
    if (tex.mipmapLevelCount < 2) return -1;
    if (metal_dmip_device != vio_mtl.device || !metal_dmip_pso) {
        metal_dmip_pso = nil;
        metal_dmip_dss = nil;
        metal_dmip_device = vio_mtl.device;
        char *err = NULL;
        id<MTLLibrary> lib = metal_build_library(metal_dmip_msl, &err);
        if (!lib) {
            php_error_docref(NULL, E_WARNING, "Metal: depth mip shader: %s", err ? err : "compile failed");
            free(err);
            return -1;
        }
        MTLRenderPipelineDescriptor *pd = [[MTLRenderPipelineDescriptor alloc] init];
        pd.vertexFunction = [lib newFunctionWithName:@"vio_dmip_vs"];
        pd.fragmentFunction = [lib newFunctionWithName:@"vio_dmip_fs"];
        pd.depthAttachmentPixelFormat = tex.pixelFormat;
        if (tex.pixelFormat == MTLPixelFormatDepth32Float_Stencil8) pd.stencilAttachmentPixelFormat = tex.pixelFormat;
        NSError *e = nil;
        metal_dmip_pso = [vio_mtl.device newRenderPipelineStateWithDescriptor:pd error:&e];
        MTLDepthStencilDescriptor *dd = [[MTLDepthStencilDescriptor alloc] init];
        dd.depthCompareFunction = MTLCompareFunctionAlways;
        dd.depthWriteEnabled = YES;
        metal_dmip_dss = [vio_mtl.device newDepthStencilStateWithDescriptor:dd];
        if (!metal_dmip_pso || !metal_dmip_dss) {
            php_error_docref(NULL, E_WARNING, "Metal: depth mip pipeline: %s", e ? e.localizedDescription.UTF8String : "failed");
            metal_dmip_pso = nil;
            return -1;
        }
    }
    int in_frame = vio_mtl.current_cmd_buf != nil;
    id<MTLCommandBuffer> cb = in_frame ? vio_mtl.current_cmd_buf : metal_new_command_buffer();
    if (in_frame && vio_mtl.current_encoder) {
        [vio_mtl.current_encoder endEncoding];
        vio_mtl.current_encoder = nil;
    }
    for (int l = 1; l < rt->mip_levels; l++) {
        int sw = rt->width >> (l - 1), sh = rt->height >> (l - 1), dw = rt->width >> l, dh = rt->height >> l;
        if (sw < 1) sw = 1;
        if (sh < 1) sh = 1;
        if (dw < 1) dw = 1;
        if (dh < 1) dh = 1;
        id<MTLTexture> src = [tex newTextureViewWithPixelFormat:tex.pixelFormat textureType:MTLTextureType2D
                                                         levels:NSMakeRange((NSUInteger)(l - 1), 1) slices:NSMakeRange(0, 1)];
        MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
        rp.depthAttachment.texture = tex;
        rp.depthAttachment.level = (NSUInteger)l;
        rp.depthAttachment.loadAction = MTLLoadActionDontCare;
        rp.depthAttachment.storeAction = MTLStoreActionStore;
        if (tex.pixelFormat == MTLPixelFormatDepth32Float_Stencil8) {
            rp.stencilAttachment.texture = tex;
            rp.stencilAttachment.level = (NSUInteger)l;
            rp.stencilAttachment.loadAction = MTLLoadActionDontCare;
            rp.stencilAttachment.storeAction = MTLStoreActionStore;
        }
        id<MTLRenderCommandEncoder> enc = [cb renderCommandEncoderWithDescriptor:rp];
        if (!src || !enc) {
            if (enc) [enc endEncoding];
            break;
        }
        MTLViewport vp = { 0.0, 0.0, (double)dw, (double)dh, 0.0, 1.0 };
        [enc setViewport:vp];
        [enc setRenderPipelineState:metal_dmip_pso];
        [enc setDepthStencilState:metal_dmip_dss];
        [enc setCullMode:MTLCullModeNone];
        int32_t pc[8] = { sw, sh, dw, dh, rt->depth_reduction == VIO_DEPTH_REDUCE_MIN ? 1 : 0, 0, 0, 0 };
        [enc setFragmentBytes:pc length:sizeof(pc) atIndex:0];
        [enc setFragmentTexture:src atIndex:0];
        [enc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
        [enc endEncoding];
    }
    if (in_frame) {
        metal_open_encoder(/*load_clear=*/0);
    } else {
        [cb commit];
        [cb waitUntilCompleted];
    }
    return 0;
}

static int metal_generate_mipmaps(void *obj, int kind)
{
    if (!obj || !vio_mtl.device) return -1;
    @autoreleasepool {
        id<MTLTexture> tex = nil;
        switch (kind) {
            case 0: {
                vio_render_target_object *rt = (vio_render_target_object *)obj;
                if (rt->backend_type == VIO_RT_BACKEND_METAL && rt->depth_only) return metal_generate_depth_mips(rt);
                if (rt->backend_type != VIO_RT_BACKEND_METAL || !rt->metal_color_texture || rt->layers > 1) return -1;
                tex = (__bridge id<MTLTexture>)rt->metal_color_texture;
                break;
            }
            case 1: {
                vio_texture_object *t = (vio_texture_object *)obj;
                vio_metal_texture *mt = (vio_metal_texture *)t->backend_texture;
                if (!mt || !mt->tex) return -1;
                tex = (__bridge id<MTLTexture>)mt->tex;
                break;
            }
            case 2: {
                vio_cubemap_object *cm = (vio_cubemap_object *)obj;
                if (!cm->metal_texture) return -1;
                tex = (__bridge id<MTLTexture>)cm->metal_texture;
                break;
            }
            default: return -1;
        }
        if (tex.mipmapLevelCount <= 1) return 0;  /* nothing to build */

        if (vio_mtl.current_cmd_buf) {
            if (vio_mtl.current_encoder) {
                [vio_mtl.current_encoder endEncoding];
                vio_mtl.current_encoder = nil;
            }
            id<MTLBlitCommandEncoder> blit = [vio_mtl.current_cmd_buf blitCommandEncoder];
            [blit generateMipmapsForTexture:tex];
            [blit endEncoding];
            metal_open_encoder(/*load_clear=*/0);
        } else {
            id<MTLCommandBuffer> cb = metal_new_command_buffer();
            id<MTLBlitCommandEncoder> blit = [cb blitCommandEncoder];
            [blit generateMipmapsForTexture:tex];
            [blit endEncoding];
            [cb commit];
            [cb waitUntilCompleted];
        }
    }
    return 0;
}

static void metal_destroy_cubemap(void *cm_ptr)
{
    vio_cubemap_object *cm = (vio_cubemap_object *)cm_ptr;
    if (!cm) return;
    if (cm->metal_texture) { CFRelease((CFTypeRef)cm->metal_texture); cm->metal_texture = NULL; }
    if (cm->metal_sampler) { CFRelease((CFTypeRef)cm->metal_sampler); cm->metal_sampler = NULL; }
}

void vio_metal_bind_cubemap(void *cubemap_obj, int slot)
{
    vio_cubemap_object *cm = (vio_cubemap_object *)cubemap_obj;
    if (!cm || !cm->metal_texture || !vio_mtl.current_encoder || slot < 0) return;
    @autoreleasepool {
        int is_depth = 0;
        int idx = metal_resolve_fs_texture(slot, &is_depth);
        if (idx < 0 || idx > 30) return;
        id<MTLTexture> ctex = (__bridge id<MTLTexture>)cm->metal_texture;
        id<MTLSamplerState> csmp = cm->metal_sampler ? (__bridge id<MTLSamplerState>)cm->metal_sampler : nil;
        if (is_depth) {
            static id<MTLSamplerState> cube_cmp = nil;
            if (!cube_cmp) cube_cmp = metal_make_sampler(VIO_FILTER_LINEAR, VIO_WRAP_CLAMP, 0, 1, 1);
            csmp = cube_cmp;
        }
        [vio_mtl.current_encoder setFragmentTexture:ctex atIndex:(NSUInteger)idx];
        if (csmp) [vio_mtl.current_encoder setFragmentSamplerState:csmp atIndex:(NSUInteger)idx];
        metal_fs_shadow_set(idx, ctex, csmp);
        metal_bind_mesh_stage_texture(slot, ctex, csmp, csmp);
    }
}

void vio_metal_bind_uniform_buffer(void *backend_buffer, int binding)
{
    vio_metal_buffer *ub = (vio_metal_buffer *)backend_buffer;
    vio_metal_pipeline *p = metal_current_pipeline;
    if (!ub || !p || !p->shader || !vio_mtl.current_encoder) return;
    @autoreleasepool {
        /* A user UBO `layout(binding = N) uniform Block` — find N in each stage. */
        int vs_idx = -1, fs_idx = -1;
        for (int i = 0; i < p->shader->vs.buffer_count; i++) {
            vio_metal_res_buffer *b = &p->shader->vs.buffers[i];
            if (b->kind == 0 && b->binding == binding) { vs_idx = b->msl_index; break; }
        }
        for (int i = 0; i < p->shader->fs.buffer_count; i++) {
            vio_metal_res_buffer *b = &p->shader->fs.buffers[i];
            if (b->kind == 0 && b->binding == binding) { fs_idx = b->msl_index; break; }
        }
        metal_bind_uniform_slice(ub, vs_idx, fs_idx);
    }
}

static void metal_set_viewport(int x, int y, int width, int height)
{
    if (!vio_mtl.current_encoder || width <= 0 || height <= 0) return;
    @autoreleasepool {
        MTLViewport vp = {(double)x, (double)y, (double)width, (double)height, 0.0, 1.0};
        [vio_mtl.current_encoder setViewport:vp];
        if (metal_vp_has_scissor) {
            /* vio_viewports clipped viewport 0 to its own rect: a single
             * viewport draws without a scissor again (full target). */
            MTLScissorRect full = {0, 0, (NSUInteger)metal_target_w, (NSUInteger)metal_target_h};
            [vio_mtl.current_encoder setScissorRect:full];
        }
        metal_vp[0] = vp;
        metal_sc[0] = metal_clamped_scissor(x, y, width, height);
        metal_vp_count = 1;
        metal_vp_has_scissor = 0;
    }
}

/* vio_viewports: viewport i (and its scissor, clamped to the target) for
 * gl_ViewportIndex = i ([[viewport_array_index]] in the vertex stage). */
static int metal_set_viewports(const int *rects, int count)
{
    if (!vio_mtl.current_encoder || count < 1 || count > VIO_METAL_MAX_VIEWPORTS) return -1;
    @autoreleasepool {
        for (int i = 0; i < count; i++) {
            int x = rects[i * 4], y = rects[i * 4 + 1], w = rects[i * 4 + 2], h = rects[i * 4 + 3];
            MTLViewport vp = {(double)x, (double)y, (double)w, (double)h, 0.0, 1.0};
            metal_vp[i] = vp;
            metal_sc[i] = metal_clamped_scissor(x, y, w, h);
        }
        metal_vp_count = count;
        metal_vp_has_scissor = 1;
        metal_apply_viewports();
    }
    return 0;
}

static void metal_destroy_mesh(void *mesh_ptr)
{
    vio_mesh_object *mesh = (vio_mesh_object *)mesh_ptr;
    if (!mesh) return;
    if (mesh->backend_vb) { metal_destroy_buffer(mesh->backend_vb); mesh->backend_vb = NULL; }
    if (mesh->backend_ib) { metal_destroy_buffer(mesh->backend_ib); mesh->backend_ib = NULL; }
}

/* VioShader free handler: the compiled functions plus the two default-block
 * cbuffers the PHP layer created through create_buffer(VIO_BUFFER_UNIFORM). */
static void metal_destroy_shader_obj(void *shader_ptr)
{
    vio_shader_object *sh = (vio_shader_object *)shader_ptr;
    if (!sh) return;
    if (sh->backend_shader) {
        metal_destroy_shader(sh->backend_shader);
        sh->backend_shader = NULL;
    }
    if (sh->cbuffer_backend) {
        if (metal_current_vs_cb == sh->cbuffer_backend) metal_current_vs_cb = NULL;
        metal_destroy_buffer(sh->cbuffer_backend);
        sh->cbuffer_backend = NULL;
    }
    if (sh->frag_cbuffer_backend) {
        if (metal_current_fs_cb == sh->frag_cbuffer_backend) metal_current_fs_cb = NULL;
        metal_destroy_buffer(sh->frag_cbuffer_backend);
        sh->frag_cbuffer_backend = NULL;
    }
}

static void metal_3d_shutdown(void)
{
    metal_ring_shutdown();
    metal_identity_instance = nil;
    metal_current_pipeline = NULL;
    metal_current_vs_cb = NULL;
    metal_current_fs_cb = NULL;
    metal_pending_storage = NULL;
    metal_pending_storage_binding = -1;
}

/* ── GPU compute primitive (SDF voxelization) ─────────────────────────
 *
 * Mirrors the D3D12 / Vulkan / OpenGL compute contract. Canonical shader
 * (PHPolygon\Fieldtracing\GpuSdfBaker::SHADER, GLSL #version 450,
 * local_size_x = 64):
 *   binding 0 = readonly  SSBO Boxes  (raw float[])
 *   binding 1 = writeonly SSBO OutD   (raw float[])
 *   binding 2 = UBO       Params {int nx,ny,nz,boxCount; float minx,miny,minz,cell;}
 *
 * GLSL -> SPIR-V (glslang, compute stage) -> MSL (SPIRV-Cross). The KEY
 * Metal-specific risk is the buffer-index remap: MSL has no separate UBO/SSBO
 * register spaces, so SPIRV-Cross assigns every buffer (boxes/dist/Params) a
 * single `[[buffer(N)]]` index, and those N's do NOT in general equal the GLSL
 * bindings 0/1/2. We make the mapping DETERMINISTIC by installing EXPLICIT MSL
 * resource bindings (spvc_compiler_msl_add_resource_binding) for (set 0,
 * binding 0/1/2) -> msl_buffer 0/1/2 before compiling. Thereafter the GLSL
 * binding == the MSL buffer index, so vio_compute_bind_buffer's `slot` (the GLSL
 * binding) is used directly as the setBuffer:atIndex: index, and the Params UBO
 * is bound at index 2. (Installing an explicit binding that the shader does not
 * actually use is harmless — SPIRV-Cross only honours it if the resource exists.)
 *
 * Buffers are MTLResourceStorageModeShared so .contents is CPU-readable for
 * seeding (input) and readback (output) with no blit. Each dispatch builds a
 * fresh command buffer on vio_mtl.command_queue and waitUntilCompleted — fully
 * synchronous, like the other backends — so by the time read_buffer or destroy
 * runs the GPU is idle and Shared memory is coherent on Apple silicon.
 *
 * Lifetime: Metal objects are stored as CFBridgingRetain'd `void *` in C structs
 * (the file's established convention; ARC does not track strong refs inside C
 * structs) and released with CFRelease in the destroy hooks — matching
 * vio_metal_shader (vert_fn/frag_fn) and the render-target textures. There is no
 * global device-teardown sweep to worry about (unlike Vulkan's vkDestroyDevice):
 * MTLBuffer / MTLComputePipelineState are reference-counted, so a PHP VioBuffer /
 * VioComputePipeline that outlives vio_destroy() still releases cleanly when its
 * Zend free handler runs and drops the last reference. */

#define VIO_METAL_COMPUTE_MAX_BINDINGS 8

typedef struct _vio_metal_compute_binding {
    vio_metal_compute_buffer *buffer;
    int slot;     /* GLSL binding == MSL buffer index (explicit remap above) */
    int access;   /* VIO_COMPUTE_READ (0) / VIO_COMPUTE_WRITE (1) */
} vio_metal_compute_binding;

typedef struct _vio_metal_compute_pipeline {
    void *pso;             /* id<MTLComputePipelineState>, +1 retained */

    /* Params constant block staged by compute_set_uniforms; bound at the Params
     * UBO's MSL buffer index (params_index, canonical 2). */
    void  *params_buffer;  /* id<MTLBuffer> (Shared), +1 retained; NULL until set */
    size_t params_capacity;
    size_t params_size;
    int    params_index;   /* MSL buffer index for the Params UBO (== GLSL binding) */

    vio_metal_compute_binding bindings[VIO_METAL_COMPUTE_MAX_BINDINGS];
    int                       binding_count;

    /* Storage images (GLSL image2D / image3D): MSL texture index == GLSL
     * binding, pinned by the same explicit remap as the buffers. */
    struct { vio_metal_texture *tex; int slot; int access; } images[VIO_METAL_COMPUTE_MAX_BINDINGS];
    int                       image_count;

    /* local_size_{x,y,z} reflected from the SPIR-V ExecutionMode so 2D/3D
     * kernels dispatch with the right threadsPerThreadgroup (0 => 64,1,1). */
    unsigned                  local_size[3];
    int                       uses_as;   /* the kernel declares an acceleration structure (ray query) */
    int                       uses_bindless;   /* reads the bindless table (metal_bindless_bind_compute) */
} vio_metal_compute_pipeline;

#ifdef HAVE_SPIRV_CROSS
/* Transpile a COMPUTE SPIR-V module to MSL, pinning the SSBO/UBO bindings 0/1/2
 * (set 0) to MSL buffer indices 0/1/2 so the PHP-side GLSL binding == the Metal
 * buffer index. Returns a malloc'd MSL string (caller frees) or NULL on failure.
 * On success *params_index_out receives the MSL buffer index of the Params UBO
 * (the explicit binding we installed for it, canonical 2). */
static char *metal_cs_spirv_to_msl(const uint32_t *spirv, size_t spirv_size,
                                   int *params_index_out, unsigned local_size_out[3],
                                   int *uses_bindless_out, char **error_msg)
{
    spvc_context  ctx = NULL;
    spvc_parsed_ir ir = NULL;
    spvc_compiler compiler = NULL;
    const char   *result = NULL;
    char         *output = NULL;

    if (params_index_out) *params_index_out = 2; /* canonical default */
    if (uses_bindless_out) *uses_bindless_out = 0;

    if (spvc_context_create(&ctx) != SPVC_SUCCESS) {
        if (error_msg) *error_msg = strdup("Failed to create SPIRV-Cross context");
        return NULL;
    }

    size_t word_count = spirv_size / sizeof(uint32_t);
    if (spvc_context_parse_spirv(ctx, spirv, word_count, &ir) != SPVC_SUCCESS) {
        if (error_msg) *error_msg = strdup(spvc_context_get_last_error_string(ctx));
        spvc_context_destroy(ctx);
        return NULL;
    }

    if (spvc_context_create_compiler(ctx, SPVC_BACKEND_MSL, ir,
                                     SPVC_CAPTURE_MODE_TAKE_OWNERSHIP,
                                     &compiler) != SPVC_SUCCESS) {
        if (error_msg) *error_msg = strdup(spvc_context_get_last_error_string(ctx));
        spvc_context_destroy(ctx);
        return NULL;
    }

    if (local_size_out) {
        for (unsigned i = 0; i < 3; i++) {
            local_size_out[i] = spvc_compiler_get_execution_mode_argument_by_index(
                compiler, SpvExecutionModeLocalSize, i);
        }
    }

    {
        spvc_compiler_options cs_opts = NULL;
        if (spvc_compiler_create_compiler_options(compiler, &cs_opts) == SPVC_SUCCESS) {
            metal_msl_apply_target(cs_opts, METAL_MSL_FLOOR);   /* the ladder rung (was SPIRV-Cross's default 1.2) */
            spvc_compiler_install_compiler_options(compiler, cs_opts);
        }
    }

    /* Install explicit MSL resource bindings for (set 0, binding N) ->
     * msl_buffer N / msl_texture N. This removes the dependency on SPIRV-Cross's
     * automatic index assignment entirely: whatever the shader declares at GLSL
     * binding N is emitted as [[buffer(N)]] (SSBO/UBO) or [[texture(N)]]
     * (storage image). The params UBO keeps its canonical index 2. Buffers and
     * textures live in separate Metal tables, so a buffer and an image at the
     * same binding never collide. */
    for (unsigned b = 0; b < 31; b++) {
        spvc_msl_resource_binding rb;
        spvc_msl_resource_binding_init(&rb);
        rb.stage       = SpvExecutionModelGLCompute;
        rb.desc_set    = 0;
        rb.binding     = b;
        rb.msl_buffer  = b;   /* GLSL binding == MSL buffer index */
        rb.msl_texture = b;   /* unused for buffers; set for completeness */
        rb.msl_sampler = b;
        spvc_compiler_msl_add_resource_binding(compiler, &rb);
    }

    /* The bindless table (Set 1..3 argument buffers), as in the graphics stages. */
    {
        spvc_resources resources = NULL;
        if (spvc_compiler_create_shader_resources(compiler, &resources) == SPVC_SUCCESS) {
            int uses = metal_msl_bindless(compiler, resources, SpvExecutionModelGLCompute);
            if (uses_bindless_out) *uses_bindless_out = uses;
        }
    }

    if (spvc_compiler_compile(compiler, &result) != SPVC_SUCCESS) {
        if (error_msg) *error_msg = strdup(spvc_context_get_last_error_string(ctx));
        spvc_context_destroy(ctx);
        return NULL;
    }

    if (getenv("VIO_DUMP_CS_MSL")) {
        fprintf(stderr, "==== Metal compute MSL (buffers boxes=0, dist=1, Params=2) ====\n%s\n==== end ====\n",
                result);
        fflush(stderr);
    }

    output = strdup(result);
    spvc_context_destroy(ctx);
    return output;
}

/* Find the kernel entry-point name in a compiled MTLLibrary. SPIRV-Cross renames
 * GLSL `main` -> `main0` for MSL (it cannot use the reserved `main`), but we
 * resolve it robustly: prefer the library's single declared function, then fall
 * back to the well-known names. Returns a +1 retained id<MTLFunction> or nil. */
static id<MTLFunction> metal_cs_kernel_function(id<MTLLibrary> lib)
{
    if (!lib) return nil;
    /* A compute MSL module emitted by SPIRV-Cross declares exactly one kernel
     * function; functionNames lists it regardless of the renamed entry point. */
    NSArray<NSString *> *names = [lib functionNames];
    if (names.count == 1) {
        id<MTLFunction> fn = [lib newFunctionWithName:names.firstObject];
        if (fn) return fn;
    }
    id<MTLFunction> fn = [lib newFunctionWithName:@"main0"];
    if (!fn) fn = [lib newFunctionWithName:@"main"];
    if (!fn && names.count >= 1) {
        fn = [lib newFunctionWithName:names.firstObject];
    }
    return fn;
}
#endif /* HAVE_SPIRV_CROSS */

extern uint32_t *vio_compile_glsl_compute_to_spirv(const char *source,
                                                   size_t *out_size, char **error_msg);

static void *metal_create_compute_pipeline(vio_shader_desc *desc)
{
#ifndef HAVE_SPIRV_CROSS
    (void)desc;
    php_error_docref(NULL, E_WARNING, "Metal: compute requires SPIRV-Cross (not built)");
    return NULL;
#else
    if (!vio_mtl.device) {
        php_error_docref(NULL, E_WARNING, "Metal: compute pipeline before device init");
        return NULL;
    }

    /* GLSL compute source arrives in fragment_data (see vio_compute_pipeline).
     * It may also already be SPIR-V (0x07230203 magic). */
    const char *src = (const char *)desc->fragment_data;
    if (!src && desc->vertex_data) src = (const char *)desc->vertex_data;
    if (!src) {
        php_error_docref(NULL, E_WARNING, "Metal: compute pipeline missing source");
        return NULL;
    }
    size_t src_size = desc->fragment_size ? desc->fragment_size : desc->vertex_size;

    char     *err   = NULL;
    uint32_t *spirv = NULL;
    size_t    spirv_size = 0;
    int       free_spirv = 0;

    int is_spirv = (src_size >= 4 && *(const uint32_t *)src == 0x07230203);
    if (is_spirv) {
        spirv = (uint32_t *)src;
        spirv_size = src_size;
    } else {
        spirv = vio_compile_glsl_compute_to_spirv(src, &spirv_size, &err);
        if (!spirv) {
            php_error_docref(NULL, E_WARNING, "Metal: CS GLSL->SPIR-V failed: %s",
                             err ? err : "unknown");
            free(err);
            return NULL;
        }
        free_spirv = 1;
    }

    int params_index = 2;
    unsigned local_size[3] = {0, 0, 0};
    int uses_bindless = 0;
    char *msl = metal_cs_spirv_to_msl(spirv, spirv_size, &params_index, local_size, &uses_bindless, &err);
    if (free_spirv) free(spirv);
    if (!msl) {
        php_error_docref(NULL, E_WARNING, "Metal: CS SPIR-V->MSL failed: %s",
                         err ? err : "unknown");
        free(err);
        return NULL;
    }

    vio_metal_compute_pipeline *cp = NULL;

    int uses_as = msl && strstr(msl, "acceleration_structure") != NULL;
    @autoreleasepool {
        NSString *msl_src = [NSString stringWithUTF8String:msl];
        free(msl);
        msl = NULL;

        NSError *nerr = nil;
        MTLCompileOptions *opts = metal_compile_options();
        id<MTLLibrary> lib = [vio_mtl.device newLibraryWithSource:msl_src options:opts error:&nerr];
        if (!lib) {
            php_error_docref(NULL, E_WARNING, "Metal: CS MSL compile failed: %s",
                             nerr ? [[nerr localizedDescription] UTF8String] : "unknown");
            return NULL;
        }

        id<MTLFunction> fn = metal_cs_kernel_function(lib);
        if (!fn) {
            php_error_docref(NULL, E_WARNING, "Metal: CS kernel entry point not found");
            return NULL;
        }

        NSError *perr = nil;
        id<MTLComputePipelineState> pso =
            [vio_mtl.device newComputePipelineStateWithFunction:fn error:&perr];
        if (!pso) {
            php_error_docref(NULL, E_WARNING, "Metal: newComputePipelineStateWithFunction failed: %s",
                             perr ? [[perr localizedDescription] UTF8String] : "unknown");
            return NULL;
        }

        cp = calloc(1, sizeof(vio_metal_compute_pipeline));
        if (!cp) {
            php_error_docref(NULL, E_WARNING, "Metal: compute pipeline alloc failed");
            return NULL;
        }
        cp->pso          = (void *)CFBridgingRetain(pso);
        cp->params_index = params_index;
        cp->uses_as      = uses_as;
        cp->uses_bindless = uses_bindless;
        memcpy(cp->local_size, local_size, sizeof(local_size));
    }

    return cp;
#endif /* HAVE_SPIRV_CROSS */
}

static void metal_destroy_compute_pipeline(void *pipeline_ptr)
{
    vio_metal_compute_pipeline *cp = (vio_metal_compute_pipeline *)pipeline_ptr;
    if (!cp) return;
    /* All dispatches are fully fenced (waitUntilCompleted), so the GPU is idle
     * w.r.t. this pipeline by the time PHP drops it. Bound storage buffers are
     * owned by their own VioBuffer objects — we only release what WE retained:
     * the PSO and the params buffer. */
    if (cp->params_buffer) { CFRelease((CFTypeRef)cp->params_buffer); cp->params_buffer = NULL; }
    if (cp->pso)           { CFRelease((CFTypeRef)cp->pso);           cp->pso = NULL; }
    free(cp);
}

static void metal_compute_bind_buffer(void *pipeline_ptr, void *backend_buffer,
                                      int slot, int access, int element_count, int stride)
{
    (void)element_count; (void)stride; /* full-range buffer view; metadata unused */
    vio_metal_compute_pipeline *cp = (vio_metal_compute_pipeline *)pipeline_ptr;
    vio_metal_compute_buffer  *buf = (vio_metal_compute_buffer *)backend_buffer;
    if (!cp || !buf) return;
    /* One buffer per slot: rebinding a slot replaces its binding. */
    for (int i = 0; i < cp->binding_count; i++) {
        if (cp->bindings[i].slot == slot) { cp->bindings[i].buffer = buf; cp->bindings[i].access = access; return; }
    }
    if (cp->binding_count >= VIO_METAL_COMPUTE_MAX_BINDINGS) return;

    vio_metal_compute_binding *b = &cp->bindings[cp->binding_count++];
    b->buffer = buf;
    b->slot   = slot;     /* GLSL binding == MSL [[buffer(slot)]] (explicit remap) */
    b->access = access;
}

static void metal_compute_bind_image(void *pipeline_ptr, void *tex_obj, int slot, int access)
{
    vio_metal_compute_pipeline *cp = (vio_metal_compute_pipeline *)pipeline_ptr;
    vio_texture_object *t = (vio_texture_object *)tex_obj;
    vio_metal_texture *mt = t ? (vio_metal_texture *)t->backend_texture : NULL;
    if (!cp || !mt || !mt->tex) return;
    /* Re-binding a slot replaces the previous image (sticky like the buffers). */
    for (int i = 0; i < cp->image_count; i++) {
        if (cp->images[i].slot == slot) { cp->images[i].tex = mt; cp->images[i].access = access; return; }
    }
    if (cp->image_count >= VIO_METAL_COMPUTE_MAX_BINDINGS) return;
    cp->images[cp->image_count].tex    = mt;
    cp->images[cp->image_count].slot   = slot;
    cp->images[cp->image_count].access = access;
    cp->image_count++;
}

static void metal_compute_set_uniforms(void *pipeline_ptr, const void *data, int size)
{
    vio_metal_compute_pipeline *cp = (vio_metal_compute_pipeline *)pipeline_ptr;
    if (!cp || !data || size <= 0 || !vio_mtl.device) return;

    @autoreleasepool {
        if (!cp->params_buffer || cp->params_capacity < (size_t)size) {
            if (cp->params_buffer) {
                CFRelease((CFTypeRef)cp->params_buffer);
                cp->params_buffer = NULL;
            }
            id<MTLBuffer> pbuf = [vio_mtl.device newBufferWithLength:(NSUInteger)size
                                                            options:MTLResourceStorageModeShared];
            if (!pbuf) {
                php_error_docref(NULL, E_WARNING, "Metal: compute params buffer create failed");
                cp->params_capacity = 0;
                return;
            }
            cp->params_buffer   = (void *)CFBridgingRetain(pbuf);
            cp->params_capacity = (size_t)size;
        }
        id<MTLBuffer> pbuf = (__bridge id<MTLBuffer>)cp->params_buffer;
        memcpy([pbuf contents], data, (size_t)size);
        cp->params_size = (size_t)size;
    }
}

/* metal_async_cb (async dispatches not yet known to be complete) is declared
 * with the tessellation draws, which have to order themselves after it. */

static void metal_compute_wait(void)
{
    @autoreleasepool {
        if (!metal_async_cb) return;
        if (metal_async_cb == vio_mtl.current_cmd_buf) {
            /* Still recording: commit + wait + reopen (same as mid-frame readback). */
            metal_flush_for_readback();
        } else {
            [metal_async_cb waitUntilCompleted];
        }
        metal_async_cb = nil;
    }
}

static void metal_dispatch_compute(vio_compute_cmd *cmd)
{
    if (!cmd) return;
    vio_metal_compute_pipeline *cp = (vio_metal_compute_pipeline *)cmd->pipeline;
    if (!cp || !cp->pso) {
        php_error_docref(NULL, E_WARNING, "Metal: dispatch_compute with invalid pipeline");
        return;
    }
    if (!vio_mtl.command_queue) {
        php_error_docref(NULL, E_WARNING, "Metal: dispatch_compute without command queue");
        return;
    }

    @autoreleasepool {
        id<MTLComputePipelineState> pso = (__bridge id<MTLComputePipelineState>)cp->pso;

        /* Async inside a frame: encode on the frame's own command buffer between
         * the render encoders. Metal orders the encoders and tracks the resource
         * hazards, so a later draw in this frame sees the kernel's writes without
         * any CPU sync; completion is observed via compute_wait / read_buffer. */
        int in_frame_async = cmd->async && vio_mtl.current_cmd_buf != nil;
        id<MTLCommandBuffer> cbuf;
        if (in_frame_async) {
            if (vio_mtl.current_encoder) {
                [vio_mtl.current_encoder endEncoding];
                vio_mtl.current_encoder = nil;
            }
            cbuf = vio_mtl.current_cmd_buf;
        } else {
            cbuf = metal_new_command_buffer();
        }
        id<MTLComputeCommandEncoder> enc = [cbuf computeCommandEncoder];
        [enc setComputePipelineState:pso];

        /* Ray query: the bound acceleration structure at its GLSL binding (the
         * compute remap pins binding n to [[buffer(n)]]). */
        if (cp->uses_as && metal_bound_as && metal_bound_as->tlas) {
            if (@available(macOS 11.0, iOS 14.0, *)) {
                [enc setAccelerationStructure:(__bridge id<MTLAccelerationStructure>)metal_bound_as->tlas
                                atBufferIndex:(NSUInteger)metal_bound_as_binding];
                metal_use_as(nil, enc, metal_bound_as);
            }
        }

        if (cp->uses_bindless) metal_bindless_bind_compute(enc);

        /* Bind each storage buffer at its MSL buffer index (== GLSL binding,
         * guaranteed by the explicit resource-binding remap at compile time). */
        for (int i = 0; i < cp->binding_count; i++) {
            vio_metal_compute_binding *b = &cp->bindings[i];
            if (!b->buffer || !b->buffer->buffer) continue;
            id<MTLBuffer> mb = (__bridge id<MTLBuffer>)b->buffer->buffer;
            [enc setBuffer:mb offset:0 atIndex:(NSUInteger)b->slot];
        }

        /* Params UBO at its MSL buffer index (canonical 2). setBytes copies the
         * staged values into the encoder now. Binding the shared params buffer
         * let a later compute_set_uniforms (a second dispatch this frame, or the
         * next frame while this one is in flight) rewrite what an async dispatch
         * reads. setBytes is capped at 4 KB; larger blocks keep the binding. */
        if (cp->params_buffer && cp->params_size > 0) {
            id<MTLBuffer> pb = (__bridge id<MTLBuffer>)cp->params_buffer;
            if (cp->params_size <= 4096) {
                [enc setBytes:[pb contents] length:cp->params_size atIndex:(NSUInteger)cp->params_index];
            } else {
                [enc setBuffer:pb offset:0 atIndex:(NSUInteger)cp->params_index];
            }
        }

        /* Storage images at their MSL texture index (== GLSL binding). */
        for (int i = 0; i < cp->image_count; i++) {
            vio_metal_texture *mt = cp->images[i].tex;
            if (!mt || !mt->tex) continue;
            [enc setTexture:(__bridge id<MTLTexture>)mt->tex atIndex:(NSUInteger)cp->images[i].slot];
        }

        /* threadsPerThreadgroup = the kernel's local_size (reflected from the
         * SPIR-V ExecutionMode at pipeline creation), so 2D/3D kernels dispatch
         * correctly; group_count_* is the threadgroup grid the caller computed
         * (ceil(total / local_size)). Legacy 1D kernels without the reflection
         * fall back to the old (64,1,1) contract. */
        NSUInteger gx = cmd->group_count_x > 0 ? (NSUInteger)cmd->group_count_x : 1;
        NSUInteger gy = cmd->group_count_y > 0 ? (NSUInteger)cmd->group_count_y : 1;
        NSUInteger gz = cmd->group_count_z > 0 ? (NSUInteger)cmd->group_count_z : 1;
        MTLSize groups  = MTLSizeMake(gx, gy, gz);
        MTLSize tpt     = MTLSizeMake(cp->local_size[0] ? cp->local_size[0] : 64,
                                      cp->local_size[1] ? cp->local_size[1] : 1,
                                      cp->local_size[2] ? cp->local_size[2] : 1);
        [enc dispatchThreadgroups:groups threadsPerThreadgroup:tpt];

        [enc endEncoding];
        if (in_frame_async) {
            metal_async_cb = cbuf;
            /* Resume the render pass with Load so earlier draws survive. */
            metal_open_encoder(/*load_clear=*/0);
            return;
        }
        [cbuf commit];
        /* Synchronous, like the other backends: by the time this returns the
         * dispatch is done and the Shared output buffer is coherent for the
         * subsequent read_buffer memcpy. */
        [cbuf waitUntilCompleted];
    }
}

/* GPU->CPU readback of a storage buffer. The dispatch already ran fully
 * synchronously (waitUntilCompleted), so the Shared-mode .contents is current:
 * memcpy out. On Apple silicon Shared memory is coherent (no synchronize); a
 * Managed buffer on an Intel Mac would need [blit synchronizeResource] +
 * didModifyRange — but we use Shared everywhere, so a plain memcpy is correct.
 * Returns bytes written. */
static size_t metal_read_buffer(void *backend_buffer, void *out, size_t size)
{
    vio_metal_compute_buffer *buf = (vio_metal_compute_buffer *)backend_buffer;
    if (!buf || !buf->buffer || !out || size == 0) return 0;
    if (metal_fs_storage_used) {
        /* Draws may have written it (A15): the frame so far, or every committed one. */
        metal_fs_storage_used = 0;
        if (vio_mtl.current_cmd_buf) metal_flush_for_readback();
        else @autoreleasepool {
            id<MTLCommandBuffer> cb = [vio_mtl.command_queue commandBuffer];
            [cb commit];
            [cb waitUntilCompleted];
        }
    }
    metal_compute_wait();   /* async dispatches must have landed before the memcpy */

    size_t n = size < buf->size ? size : buf->size;
    @autoreleasepool {
        id<MTLBuffer> mb = (__bridge id<MTLBuffer>)buf->buffer;
        const void *contents = [mb contents];
        if (!contents) return 0;
        memcpy(out, contents, n);
    }
    return n;
}
static double metal_gpu_frame_time(void)
{
    return vio_mtl.initialized && vio_mtl.last_gpu_ms > 0.0 ? vio_mtl.last_gpu_ms : -1.0;
}

static int metal_gpu_mark(const char *name)
{
    if (!vio_mtl.initialized || !vio_mtl.current_cmd_buf) return 0;
    @autoreleasepool {
        metal_mark_frame *mf = (metal_mark_frame *)vio_mtl.cur_marks;
        if (!mf) {
            mf = calloc(1, sizeof(*mf));
            if (!mf) return 0;
            atomic_init(&mf->remaining, 1);   /* the frame's last buffer (metal_present) */
            vio_mtl.cur_marks = mf;
        }
        int i = vio_gpu_mark_push(&mf->names, name);
        if (i < 0) return 0;
        if (vio_mtl.current_encoder) {
            [vio_mtl.current_encoder endEncoding];
            vio_mtl.current_encoder = nil;
        }
        atomic_fetch_add(&mf->remaining, 1);
        [vio_mtl.current_cmd_buf addCompletedHandler:^(id<MTLCommandBuffer> done) {
            if (i == 0) mf->start = done.GPUStartTime;
            mf->end[i] = done.GPUEndTime;
            metal_marks_done(mf);
        }];
        [vio_mtl.current_cmd_buf commit];
        vio_mtl.current_cmd_buf = metal_new_command_buffer();
        metal_open_encoder(/*load_clear=*/0);
    }
    return 1;
}

static const vio_gpu_mark_result *metal_gpu_marks(void)
{
    int valid;
    os_unfair_lock_lock(&metal_marks_lock);
    valid = metal_marks_valid;
    if (valid) metal_marks_snapshot = metal_marks_result;
    os_unfair_lock_unlock(&metal_marks_lock);
    return vio_mtl.initialized && valid ? &metal_marks_snapshot : NULL;
}

/* vio_swapchain_info(): drawables, frames in flight (frame_latency semaphore),
 * HDR10 layer. */
static void metal_swapchain_info(vio_swapchain_info *out)
{
    if (!vio_mtl.initialized) return;
    out->buffer_count  = vio_mtl.metal_layer ? (int)vio_mtl.metal_layer.maximumDrawableCount : 0;
    out->frame_latency = vio_mtl.frame_semaphore ? vio_mtl.frame_latency : 0;
    out->waitable      = vio_mtl.frame_semaphore ? 1 : 0;
    out->hdr_output    = vio_mtl.hdr_output;
    out->format        = vio_mtl.swap_format == MTLPixelFormatRGB10A2Unorm ? VIO_FORMAT_RGB10A2 : VIO_FORMAT_RGBA8;
}

/* VIO_FEATURE_COOPERATIVE_MATRIX: simdgroup_matrix comes in 8x8 half and
 * float; simdgroup_multiply_accumulate also takes half A / B with a float
 * accumulator. */
static int metal_cooperative_matrix_shapes(vio_coopmat_shape *out, int max)
{
    static const vio_coopmat_type types[][2] = {
        { VIO_COOPMAT_FLOAT16, VIO_COOPMAT_FLOAT16 },
        { VIO_COOPMAT_FLOAT16, VIO_COOPMAT_FLOAT32 },
        { VIO_COOPMAT_FLOAT32, VIO_COOPMAT_FLOAT32 },
    };
    if (!vio_mtl.caps.cooperative_matrix || !out) return 0;
    int n = 0;
    for (int i = 0; i < 3 && n < max; i++, n++) {
        out[n].m = out[n].n = out[n].k = 8;
        out[n].a = out[n].b = types[i][0];
        out[n].c = out[n].result = types[i][1];
    }
    return n;
}

/* vio_adapters (A6): every Metal device (MTLCopyAllDevices on macOS). */
static int metal_enumerate_adapters(vio_adapter_info *out, int max)
{
    int n = 0;
    @autoreleasepool {
#if TARGET_OS_OSX
        NSArray<id<MTLDevice>> *devices = MTLCopyAllDevices();
#else
        id<MTLDevice> one = MTLCreateSystemDefaultDevice();
        NSArray<id<MTLDevice>> *devices = one ? @[ one ] : @[];
#endif
        for (id<MTLDevice> dev in devices) {
            if (n >= max) break;
            vio_adapter_info *a = &out[n++];
            memset(a, 0, sizeof(*a));
            snprintf(a->name, sizeof(a->name), "%s", dev.name.UTF8String ? dev.name.UTF8String : "Metal");
            a->vendor_id = strstr(a->name, "AMD") ? 0x1002 : strstr(a->name, "Intel") ? 0x8086
                         : strstr(a->name, "NVIDIA") ? 0x10DE : 0x106B;
            a->device_type = dev.hasUnifiedMemory ? "integrated" : "discrete";
            a->vram_bytes = (uint64_t)dev.recommendedMaxWorkingSetSize;
            a->features = VIO_FEATURE_BIT(VIO_FEATURE_COMPUTE) | VIO_FEATURE_BIT(VIO_FEATURE_3D_PIPELINE)
                        | VIO_FEATURE_BIT(VIO_FEATURE_TESSELLATION) | VIO_FEATURE_BIT(VIO_FEATURE_INDIRECT_DRAW);
            if ([dev respondsToSelector:@selector(supportsRaytracing)] && dev.supportsRaytracing)
                a->features |= VIO_FEATURE_BIT(VIO_FEATURE_RAY_QUERY);
            if ([dev supportsFamily:MTLGPUFamilyApple2])
                a->features |= VIO_FEATURE_BIT(VIO_FEATURE_TEXTURE_COMPRESSION_ASTC);
            if ([dev supportsFamily:MTLGPUFamilyApple7] || [dev supportsFamily:MTLGPUFamilyMac2])
                a->features |= VIO_FEATURE_BIT(VIO_FEATURE_MESH_SHADER) | VIO_FEATURE_BIT(VIO_FEATURE_SUBGROUP);
        }
    }
    return n;
}

/* vio_backend_info(): the version ladder rung and the capability set. */
static int metal_describe(vio_backend_description *out)
{
    if (!vio_mtl.initialized || !vio_mtl.device || !out) return -1;
    static const char *apple_names[] = { "apple1", "apple2", "apple3", "apple4", "apple5", "apple6",
                                         "apple7", "apple8", "apple9", "apple10", "apple11" };
    const vio_metal_caps *c = &vio_mtl.caps;
    out->api = vio_mtl.api_name;
    out->device = vio_mtl.gpu_name;
    out->shading_language = "MSL";
    out->shading_language_version = c->msl_version;
    out->shading_language_max = c->msl_max;
    out->family_count = 0;
    for (int a = 1; a <= 11; a++) {
        if (metal_has_family(1000 + a)) out->families[out->family_count++] = apple_names[a - 1];
    }
    if (metal_has_family(2001)) out->families[out->family_count++] = "mac1";
    if (c->mac2)                out->families[out->family_count++] = "mac2";
    if (metal_has_family(3001)) out->families[out->family_count++] = "common1";
    if (metal_has_family(3002)) out->families[out->family_count++] = "common2";
    if (metal_has_family(3003)) out->families[out->family_count++] = "common3";
    if (c->metal3)              out->families[out->family_count++] = "metal3";
    if (c->metal4)              out->families[out->family_count++] = "metal4";
#define CAP(n) do { out->cap_names[out->cap_count] = #n; out->cap_values[out->cap_count++] = c->n; } while (0)
    out->cap_count = 0;
    CAP(tessellation); CAP(layered_vertex); CAP(quad_group); CAP(simd_group);
    CAP(barycentrics); CAP(vertex_amplification); CAP(argument_buffers_tier2);
    CAP(raytracing); CAP(function_pointers); CAP(raytracing_from_render); CAP(cooperative_matrix);
    CAP(mesh_shaders); CAP(atomic64); CAP(tensors); CAP(bindless);
    CAP(rasterization_rate_map); CAP(bc_texture_compression); CAP(unified_memory);
#undef CAP
    /* Metal has no PCI vendor id: Intel Macs carry AMD / Intel GPUs. */
    const char *nm = vio_mtl.gpu_name;
    out->vendor_id = strstr(nm, "AMD") ? 0x1002 : strstr(nm, "Intel") ? 0x8086
                   : strstr(nm, "NVIDIA") ? 0x10DE : 0x106B;
    out->driver = "";
    out->device_type = c->unified_memory ? "integrated" : "discrete";
    const char *unused = NULL;
    vio_metal_gpu_info(&unused, &out->vram_bytes);
    return 0;
}

/* vio_feature_info: features metal_supports_feature reports that Metal has no
 * stage or call for. */
static const char *metal_feature_emulation(vio_feature f)
{
    switch (f) {
    case VIO_FEATURE_GEOMETRY:
    case VIO_FEATURE_GEOMETRY_INSTANCING:
        return "compute kernels (vertex + geometry) and a pass-through vertex function";
    case VIO_FEATURE_MULTIVIEW:
        return "instancing, one instance per view ([[render_target_array_index]])";
    case VIO_FEATURE_RAY_QUERY:
        /* Hardware ray tracing from Apple9 on; older GPUs run Metal's own
         * software intersector behind the same API. */
        return vio_mtl.caps.apple_family >= 9 ? NULL : "Metal software intersector (no ray tracing hardware)";
    default:
        return NULL;
    }
}

static int metal_supports_feature(vio_feature f)
{
    switch (f) {
    case VIO_FEATURE_COMPUTE:
    case VIO_FEATURE_3D_PIPELINE:
    case VIO_FEATURE_VERTEX_STORAGE:
    case VIO_FEATURE_FRAGMENT_STORAGE:
    case VIO_FEATURE_SAMPLER_FEEDBACK_GLSL:
    case VIO_FEATURE_STORAGE_IMAGE:
#ifdef HAVE_SPIRV_CROSS
        /* Every shader stage reaches the GPU through GLSL -> SPIR-V -> MSL, so
         * compute, the 3D draw pipeline and the vertex-stage SSBO path (Path B)
         * all hinge on SPIRV-Cross. Without it the engine stays on the CPU /
         * OpenGL fallbacks. */
        return 1;
#else
        return 0;
#endif
    case VIO_FEATURE_SHADER_FLOAT16:
        /* float16_t -> MSL half, native on every Metal GPU. */
#ifdef HAVE_SPIRV_CROSS
        return 1;
#else
        return 0;
#endif
    case VIO_FEATURE_BASE_VERTEX:
        /* [[base_vertex]] / [[base_instance]] (Mac2 / Apple3); the indirect
         * argument records carry both. Not in the emulated GS / tessellation
         * pipelines (their vertex stage runs as a kernel). */
        return vio_mtl.caps.mac2 || vio_mtl.caps.apple_family >= 3;
    case VIO_FEATURE_COMPUTE_DERIVATIVES:
        /* Kernel functions have no dfdx / dfdy (the driver rejects them). */
        return 0;
    case VIO_FEATURE_RAY_QUERY:
        /* Ray queries in compute (MSL 2.3) and fragment functions (2.4, ray
         * tracing from render pipelines); hardware RT on Apple9+, else emulated. */
#ifdef HAVE_SPIRV_CROSS
        return vio_mtl.caps.raytracing && vio_mtl.caps.raytracing_from_render;
#else
        return 0;
#endif
    case VIO_FEATURE_ATOMIC64:
        /* Metal has 64-bit atomic min / max only (atomic64 cap, MSL 3.1) and
         * SPIRV-Cross refuses 64-bit atomics for MSL: not offered. */
        return 0;
    case VIO_FEATURE_SUBGROUP_QUAD:
        /* quad_shuffle / quad_broadcast in fragment functions (Mac2 / Apple4, MSL 2.1). */
        return vio_mtl.caps.quad_group;
    case VIO_FEATURE_BARYCENTRICS:
        /* [[barycentric_coord]] (MSL 2.2, supportsShaderBarycentricCoordinates). */
        return vio_mtl.caps.barycentrics;
    case VIO_FEATURE_SUBGROUP:
        /* GL_KHR_shader_subgroup_* -> SPIRV-Cross simd_* / quad_* functions
         * in compute and fragment stages (MSL 2.2+, SIMD-group reductions on
         * Mac2 / Apple7). */
        return vio_mtl.caps.simd_group;
    case VIO_FEATURE_TESSELLATION:
        /* vertex + control kernels, then drawPatches (metal_draw_tess); the
         * [[patch]] functions need MSL 2.1 (version ladder). */
        return vio_mtl.caps.tessellation;
    case VIO_FEATURE_COOPERATIVE_MATRIX:
        /* coopmat -> simdgroup_matrix (SPIRV-Cross, 8x8 only, MSL 3.1, Apple7+). */
        return vio_mtl.caps.cooperative_matrix;
    case VIO_FEATURE_MESH_SHADER:
        /* MTLMeshRenderPipelineDescriptor + drawMeshThreadgroups: Metal 3 on
         * Apple7 / Mac2, MSL 3.0 rung. */
#ifdef HAVE_SPIRV_CROSS
        return vio_mtl.caps.mesh_shaders;
#else
        return 0;
#endif
    case VIO_FEATURE_INSTANCED_DRAW:
        /* vio_draw_instanced -> per-draw ring slice bound as the per-instance
         * mat4 buffer (locations 3..6). */
        return 1;
    case VIO_FEATURE_READ_PIXELS:
        /* Blit into a Shared staging buffer (vio_metal_read_pixels). */
        return 1;
    case VIO_FEATURE_CUBEMAP:
        /* MTLTextureTypeCube via upload_cubemap / vio_metal_bind_cubemap. */
        return 1;
    case VIO_FEATURE_NATIVE_2D_BATCH:
        /* Metal ships its own 2D-batch renderer (vio_metal_2d_*). */
        return 1;
    case VIO_FEATURE_SCISSOR:
    case VIO_FEATURE_DEPTH_BIAS:
        /* Encoder state: setScissorRect / setDepthBias. */
        return 1;
    case VIO_FEATURE_TEXTURE_SWIZZLE:
        /* MTLTextureSwizzleChannels on the texture descriptor. */
        return 1;
    case VIO_FEATURE_TEXTURE_3D:
        /* MTLTextureType3D, sampled through bind_texture like any 2D texture. */
        return 1;
    case VIO_FEATURE_RENDER_TARGET:
    case VIO_FEATURE_RENDER_TARGET_HDR:
    case VIO_FEATURE_RENDER_TARGET_DEPTH:
    case VIO_FEATURE_RENDER_TARGET_MSAA:
    case VIO_FEATURE_RENDER_TARGET_CUBE:
    case VIO_FEATURE_MIPMAP_GEN:
    case VIO_FEATURE_MRT:
        /* MTLTexture-backed offscreen RTs via create/bind/unbind/destroy;
         * MSAA colour targets render into a 2DMultisample pair and resolve
         * at every pass end (`samples` on vio_render_target). */
        return 1;
    case VIO_FEATURE_GPU_TIMESTAMP: /* MTLCommandBuffer GPUStartTime / GPUEndTime */
        return 1;
    case VIO_FEATURE_FRAME_LATENCY:
        /* frame_latency => n: dispatch semaphore over the frames in flight,
         * signalled by each frame's command buffer on completion. */
        return 1;
    case VIO_FEATURE_HDR_OUTPUT:
#if TARGET_OS_OSX
        /* RGB10A2 CAMetalLayer in the BT.2100 PQ colour space (macOS 10.15+). */
        if (@available(macOS 10.15, *)) return 1;
#endif
        return 0;
    case VIO_FEATURE_INDIRECT_DRAW: /* drawIndexedPrimitives:indirectBuffer: */
        return 1;
    case VIO_FEATURE_TEXTURE_ARRAY: /* MTLTextureType2DArray */
        return 1;
    case VIO_FEATURE_DEPTH_MIPMAPS: /* metal_generate_depth_mips (A26) */
        return 1;
    case VIO_FEATURE_TEXTURE_COMPRESSION_ASTC: /* ASTC LDR on Apple GPUs (Apple2+), not on Intel / AMD Macs */
        return vio_mtl.caps.apple_family >= 2;
    case VIO_FEATURE_TEXTURE_COMPRESSION_BC: /* BC1-BC7 pixel formats (macOS) */
        return metal_supports_bc();
    case VIO_FEATURE_STENCIL:
        /* Swapchain and colour targets use Depth32Float_Stencil8. */
        return 1;
    case VIO_FEATURE_RENDER_TARGET_LAYERED:
        /* 2DArray colour + depth, depth-stencil cubes (depth_only). */
        return 1;
    case VIO_FEATURE_LAYERED_RENDER:
    case VIO_FEATURE_VERTEX_LAYER:
    case VIO_FEATURE_MULTI_VIEWPORT:
        /* renderTargetArrayLength + [[render_target_array_index]] /
         * [[viewport_array_index]] from the vertex function and
         * setViewports:count: - Mac2 / Apple5 GPU families. No geometry stage,
         * so gl_Layer / gl_ViewportIndex come from the vertex stage only. */
        return metal_supports_layered_vertex();
    case VIO_FEATURE_GEOMETRY:
    case VIO_FEATURE_GEOMETRY_INSTANCING:
        /* No geometry stage in Metal: vertex + geometry kernels and a
         * pass-through vertex function (metal_draw_gs). */
#ifdef HAVE_SPIRV_CROSS
        return 1;
#else
        return 0;
#endif
    case VIO_FEATURE_BINDLESS:
        /* Texture handles (gpuResourceID) in a Set 1 argument buffer: Metal3 +
         * argument buffers tier 2, MSL 3.0 (caps.bindless). */
#ifdef HAVE_SPIRV_CROSS
        return vio_mtl.caps.bindless;
#else
        return 0;
#endif
    case VIO_FEATURE_MULTIVIEW:
        /* SPIRV-Cross's instancing emulation writes [[render_target_array_index]]
         * from the vertex stage (Mac2 / Apple5). */
#ifdef HAVE_SPIRV_CROSS
        return vio_mtl.caps.layered_vertex;
#else
        return 0;
#endif
    case VIO_FEATURE_RAYTRACING:
        /* No ray tracing pipeline: Metal has no raygen / miss / hit stages
         * (an intersector in a compute kernel plus intersection function
         * tables), and SPIRV-Cross cannot translate traceRayEXT or the
         * payload / hit attribute storage classes to MSL. Ray queries
         * (VIO_FEATURE_RAY_QUERY) cover inline tracing. */
        return 0;
    default:
        return 0;
    }
}

/* ── Object destructors ─────────────────────────────────────────── */

#include "../../vio_font.h"
#include "../../vio_texture.h"

static void metal_destroy_texture_obj(void *tex_ptr)
{
    vio_texture_object *tex = (vio_texture_object *)tex_ptr;
    if (tex->borrowed) {
        /* Render-target wrapper: MTLTexture + wrapper struct belong to the RT. */
        return;
    }
    if (tex->backend_texture) {
        /* Owns the MTLTexture, its samplers and the 2D registry slot. */
        metal_destroy_texture(tex->backend_texture);
        tex->backend_texture = NULL;
        tex->texture_id = 0;
        return;
    }
    if (tex->texture_id) {
        vio_metal_delete_texture(tex->texture_id);
        tex->texture_id = 0;
    }
}

static void metal_destroy_font_atlas(void *font_ptr)
{
    vio_font_object *font = (vio_font_object *)font_ptr;
    if (font->atlas_texture) {
        vio_metal_delete_texture(font->atlas_texture);
        font->atlas_texture = 0;
    }
}

static int metal_upload_font_atlas(void *font_obj, int width, int height,
                                   const unsigned char *r8_data, int swizzle_red_to_alpha)
{
    (void)swizzle_red_to_alpha;  /* Metal's path handles channel mapping internally */
    vio_font_object *font = (vio_font_object *)font_obj;
    font->atlas_texture = vio_metal_create_font_atlas(width, height, r8_data);
    return font->atlas_texture ? 0 : -1;
}

/* Glyph atlas filled on demand (A33): replaceRegion on the R8 atlas texture. */
static int metal_update_font_atlas(void *font_obj, const unsigned char *r8, int x, int y, int w, int h)
{
    vio_font_object *font = (vio_font_object *)font_obj;
    if (!font || !r8 || font->atlas_texture == 0 || font->atlas_texture >= VIO_METAL_MAX_TEXTURES) return -1;
    @autoreleasepool {
        id<MTLTexture> tex = metal_textures[font->atlas_texture];
        if (!tex) return -1;
        [tex replaceRegion:MTLRegionMake2D(x, y, w, h) mipmapLevel:0 withBytes:r8 bytesPerRow:w];
    }
    return 0;
}

/* ── Backend registration ────────────────────────────────────────── */

static const vio_backend metal_backend = {
    .name              = "metal",
    .api_version       = VIO_BACKEND_API_VERSION,
    .init              = metal_init,
    .shutdown          = metal_shutdown,
    .create_surface    = metal_create_surface,
    .destroy_surface   = metal_destroy_surface,
    .resize            = metal_resize,
    .create_pipeline   = metal_create_pipeline,
    .destroy_pipeline  = metal_destroy_pipeline,
    .bind_pipeline     = metal_bind_pipeline,
    .create_buffer     = metal_create_buffer,
    .update_buffer     = metal_update_buffer,
    .destroy_buffer    = metal_destroy_buffer,
    .create_texture    = metal_create_texture,
    .create_texture_3d = metal_create_texture_3d,
    .destroy_texture   = metal_destroy_texture,
    .compile_shader    = metal_compile_shader,
    .destroy_shader    = metal_destroy_shader,
    .begin_frame       = metal_begin_frame,
    .end_frame         = metal_end_frame,
    .draw              = metal_draw,
    .draw_indexed      = metal_draw_indexed,
    .present           = metal_present,
    .clear             = metal_clear,
    .gpu_flush         = metal_gpu_flush,
    .read_pixels       = metal_read_pixels_slot,
    .bind_texture      = metal_bind_texture,
    .set_viewport      = metal_set_viewport,
    .set_viewports     = metal_set_viewports,
    .dispatch_compute  = metal_dispatch_compute,
    .supports_feature  = metal_supports_feature,
    .apply_mesh_layout = metal_apply_mesh_layout,
    .feature_emulation = metal_feature_emulation,
    .gpu_frame_time    = metal_gpu_frame_time,
    .gpu_mark          = metal_gpu_mark,
    .gpu_marks         = metal_gpu_marks,
    .swapchain_info    = metal_swapchain_info,
    .describe          = metal_describe,
    .enumerate_adapters = metal_enumerate_adapters,
    .create_acceleration_structure  = metal_create_acceleration_structure,
    .update_acceleration_structure  = metal_update_acceleration_structure,
    .destroy_acceleration_structure = metal_destroy_acceleration_structure,
    .bind_acceleration_structure    = metal_bind_acceleration_structure,
    .bindless_set      = metal_bindless_set,
    .destroy_mesh      = metal_destroy_mesh,
    .destroy_shader_obj = metal_destroy_shader_obj,
    .upload_cubemap    = metal_upload_cubemap,
    .destroy_cubemap   = metal_destroy_cubemap,
    .bind_render_target_face = metal_bind_render_target_face,
    .render_target_cubemap   = metal_render_target_cubemap,
    .read_render_target      = metal_read_render_target,
    .update_texture          = metal_update_texture,
    .generate_mipmaps        = metal_generate_mipmaps,
    /* Path B: vertex-stage SSBO bound at its pinned MSL index, drawn with
     * instance_count instances and no per-instance vertex buffer. */
    .bind_storage_buffer         = metal_bind_storage_buffer,
    .bind_fragment_storage       = metal_bind_fragment_storage,
    .draw_instanced_from_storage = metal_draw_instanced_from_storage,
    .draw_indirect     = metal_draw_indirect,
    .draw_mesh_tasks          = metal_draw_mesh_tasks,
    .draw_mesh_tasks_indirect = metal_draw_mesh_tasks_indirect,
    .cooperative_matrix_shapes = metal_cooperative_matrix_shapes,
    .bind_stage_constants = metal_bind_stage_constants,
    .gpu_info           = vio_metal_gpu_info,
    .destroy_font_atlas = metal_destroy_font_atlas,
    .upload_font_atlas  = metal_upload_font_atlas,
    .update_font_atlas  = metal_update_font_atlas,
    .destroy_texture_obj = metal_destroy_texture_obj,
    .destroy_buffer_obj  = metal_destroy_buffer_obj,
    .create_render_target  = metal_create_render_target,
    .bind_render_target    = metal_bind_render_target,
    .unbind_render_target  = metal_unbind_render_target,
    .destroy_render_target = metal_destroy_render_target,

    /* GPU compute primitive (SDF voxelization). Synchronous dispatch on the
     * backend command queue; storage buffers + params staged on the pipeline
     * object, read back from Shared-mode MTLBuffer. */
    .create_compute_pipeline  = metal_create_compute_pipeline,
    .destroy_compute_pipeline = metal_destroy_compute_pipeline,
    .compute_bind_buffer      = metal_compute_bind_buffer,
    .compute_set_uniforms     = metal_compute_set_uniforms,
    .compute_bind_image       = metal_compute_bind_image,
    .compute_wait             = metal_compute_wait,
    .read_buffer              = metal_read_buffer,
};

void vio_backend_metal_register(void)
{
    vio_register_backend(&metal_backend);
}

#endif /* HAVE_METAL */

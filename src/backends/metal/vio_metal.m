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

#ifdef HAVE_GLFW
#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>
#define GLFW_EXPOSE_NATIVE_COCOA
#include <GLFW/glfw3native.h>
#endif

#include "vio_metal.h"
#include "../../shaders/shaders_2d.h"
#include "../../vio_render_target.h"
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

/* ── Metal state ─────────────────────────────────────────────────── */

typedef struct _vio_metal_state {
    id<MTLDevice>              device;
    id<MTLCommandQueue>        command_queue;
    CAMetalLayer              *metal_layer;
    id<CAMetalDrawable>        current_drawable;
    id<MTLCommandBuffer>       current_cmd_buf;
    id<MTLRenderCommandEncoder> current_encoder;
    double                     last_gpu_ms;     /* GPUEndTime - GPUStartTime of the last completed frame */
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
#ifdef HAVE_GLFW
    /* When the backend was bootstrapped via vio_metal_setup_context() the
     * GLFW window is polled each frame to discover resizes. Pure-native
     * setups (iOS, headless) leave this NULL and call
     * vio_metal_handle_resize() externally instead. */
    GLFWwindow                *glfw_window;
#endif
} vio_metal_state;

static vio_metal_state vio_mtl = {0};

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
} vio_metal_target_desc;
static void metal_current_target(vio_metal_target_desc *t);
static MTLPixelFormat metal_pixel_format(int vio_fmt);
static int metal_rt_attachment_count(const vio_render_target_object *rt);
static void metal_destroy_texture(void *texture);
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
        texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm width:w height:h mipmapped:NO];
    cd.textureType = MTLTextureType2DMultisample;
    cd.sampleCount = (NSUInteger)vio_mtl.samples;
    cd.usage = MTLTextureUsageRenderTarget;
    cd.storageMode = MTLStorageModePrivate;
    vio_mtl.msaa_color = [vio_mtl.device newTextureWithDescriptor:cd];

    MTLTextureDescriptor *dd = [MTLTextureDescriptor
        texture2DDescriptorWithPixelFormat:MTLPixelFormatDepth32Float width:w height:h mipmapped:NO];
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
        texture2DDescriptorWithPixelFormat:MTLPixelFormatDepth32Float
        width:w height:h mipmapped:NO];
    desc.usage = MTLTextureUsageRenderTarget;
    desc.storageMode = MTLStorageModePrivate;

    vio_mtl.depth_texture = [vio_mtl.device newTextureWithDescriptor:desc];
}

/* ── Setup / Teardown ────────────────────────────────────────────── */

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
        vio_mtl.metal_layer.pixelFormat = MTLPixelFormatBGRA8Unorm;
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
        vio_mtl.metal_layer.drawableSize = CGSizeMake(width, height);

        vio_mtl.width  = width;
        vio_mtl.height = height;

        /* Create depth texture (+ the MSAA pair when 'samples' > 1) */
        create_depth_texture(width, height);
        metal_create_swapchain_msaa(width, height);

        /* Create offscreen render target for vsync-off mode */
        if (!cfg->vsync) {
            MTLTextureDescriptor *offDesc = [MTLTextureDescriptor
                texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
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
    int count = t->count < 1 ? 1 : t->count;
    for (int i = 0; i < mtl_2d.variant_count; i++) {
        vio_metal_2d_variant *v = &mtl_2d.variants[i];
        if (v->pixel_format != (int)fmt || v->samples != samples || v->has_depth != has_depth ||
            v->color_count != count) continue;
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
        pipeDesc.depthAttachmentPixelFormat = has_depth ? MTLPixelFormatDepth32Float : MTLPixelFormatInvalid;

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
                                                              options:nil
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
        vio_metal_target_desc swap = { { MTLPixelFormatBGRA8Unorm, 0, 0, 0 }, 1, 1, 1 };
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

        /* Upload projection matrix as a small buffer */
        id<MTLBuffer> projBuf = [vio_mtl.device
            newBufferWithBytes:state->projection
            length:sizeof(float) * 16
            options:MTLResourceStorageModeShared];

        /* Disable depth for 2D, and undo any 3D encoder state (a preceding
         * vio_draw may have left back-face culling / depth bias set — the 2D
         * quads are emitted in screen space with no fixed winding). */
        [vio_mtl.current_encoder setDepthStencilState:mtl_2d.depth_disabled];
        [vio_mtl.current_encoder setCullMode:MTLCullModeNone];
        [vio_mtl.current_encoder setDepthBias:0.0f slopeScale:0.0f clamp:0.0f];

        /* Bind vertex buffer and projection */
        [vio_mtl.current_encoder setVertexBuffer:mtl_2d.vertex_buffer offset:0 atIndex:0];
        [vio_mtl.current_encoder setVertexBuffer:projBuf offset:0 atIndex:1];

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

        /* Convert BGRA -> RGBA */
        for (int i = 0; i < use_w * use_h; i++) {
            int off = i * 4;
            out_rgba[off + 0] = bgra[off + 2]; /* R */
            out_rgba[off + 1] = bgra[off + 1]; /* G */
            out_rgba[off + 2] = bgra[off + 0]; /* B */
            out_rgba[off + 3] = bgra[off + 3]; /* A */
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
    unsigned char *rgba = emalloc(width * height * 4);
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
                texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
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
        NSUInteger cube_slice = 0, cube_level = 0;
        if (current_bound_rt) {
            depth_target = (__bridge id<MTLTexture>)current_bound_rt->metal_depth_texture;
            target_w = current_bound_rt->width;
            target_h = current_bound_rt->height;
            if (!current_bound_rt->depth_only) {
                n_color = metal_rt_attachment_count(current_bound_rt);
                for (int i = 0; i < n_color; i++) {
                    id<MTLTexture> c = i == 0 ? metal_current_color_texture()
                                              : (__bridge id<MTLTexture>)current_bound_rt->metal_color_textures[i];
                    if (current_bound_rt->samples > 1) {
                        /* MSAA: render into the multisample pair, resolve into the
                         * single-sample colour texture at every pass end. */
                        resolve_targets[i] = c;
                        color_targets[i] = (__bridge id<MTLTexture>)current_bound_rt->metal_msaa_color_textures[i];
                    } else {
                        color_targets[i] = c;
                    }
                }
            }
            if (current_bound_rt->is_cube) {
                cube_slice = (NSUInteger)(current_bound_face >= 0 ? current_bound_face : 0);
                cube_level = (NSUInteger)current_bound_level;
                target_w >>= cube_level; if (target_w < 1) target_w = 1;
                target_h >>= cube_level; if (target_h < 1) target_h = 1;
                /* The shared depth texture only matches level 0. */
                if (cube_level > 0) depth_target = nil;
            }
            if (current_bound_rt->samples > 1) {
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
                /* Keep the MSAA contents too, so a reopened pass (RT bind /
                 * unbind / eager clear / mid-frame readback) can Load them. */
                ca.storeAction = MTLStoreActionStoreAndMultisampleResolve;
            } else {
                ca.storeAction = MTLStoreActionStore;
            }
            ca.clearColor = MTLClearColorMake(vio_mtl.clear_r, vio_mtl.clear_g, vio_mtl.clear_b, vio_mtl.clear_a);
        }

        if (depth_target) {
            desc.depthAttachment.texture = depth_target;
            desc.depthAttachment.loadAction = load_clear ? MTLLoadActionClear : MTLLoadActionLoad;
            /* Always Store: a mid-frame RT bind/unbind closes this encoder and
             * reopens one with Load — DontCare would hand that Load undefined
             * depth and 3D draws after the switch would fail the depth test. */
            desc.depthAttachment.storeAction = MTLStoreActionStore;
            desc.depthAttachment.clearDepth = 1.0;
        }

        vio_mtl.current_encoder = [vio_mtl.current_cmd_buf
            renderCommandEncoderWithDescriptor:desc];

        MTLViewport viewport = {0, 0, (double)target_w, (double)target_h, 0.0, 1.0};
        [vio_mtl.current_encoder setViewport:viewport];

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
        if (current_bound_rt) {
            vio_mtl.current_drawable = nil;
        } else if (!vio_mtl.vsync && vio_mtl.offscreen_texture) {
            vio_mtl.current_drawable = nil;
        } else {
            vio_mtl.current_drawable = [vio_mtl.metal_layer nextDrawable];
            if (!vio_mtl.current_drawable) return;
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

static void metal_present(void)
{
    @autoreleasepool {
        if (!vio_mtl.current_cmd_buf) return;

        /* GPU timestamps (GAP-PHASE5 Block 3): the command buffer reports its
         * own GPU span once it completes. */
        [vio_mtl.current_cmd_buf addCompletedHandler:^(id<MTLCommandBuffer> done) {
            double ms = (done.GPUEndTime - done.GPUStartTime) * 1000.0;
            if (ms >= 0.0) vio_mtl.last_gpu_ms = ms;
        }];

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
#include "../../vio_cubemap.h"
#include "../../vio_texture.h"

/* Vertex-buffer indices reserved for [[stage_in]] data. Every shader resource
 * (UBO / SSBO / push constant) is renumbered to MSL buffer index 0..N-1 at
 * compile time (metal_gfx_spirv_to_msl), so these two can never collide with
 * them — Metal exposes 31 vertex-stage buffer slots (0..30). */
#define VIO_METAL_VB_MESH      30
#define VIO_METAL_VB_INSTANCE  29

#define VIO_METAL_MAX_RES 16

/* One buffer-like resource of a shader stage after the MSL renumbering. */
typedef struct _vio_metal_res_buffer {
    int kind;       /* 0 = UBO, 1 = SSBO, 2 = push-constant block */
    int set;        /* original GLSL descriptor set */
    int binding;    /* original GLSL binding (what vio_bind_buffer / vio_bind_storage_buffer pass) */
    int msl_index;  /* [[buffer(N)]] the resource was pinned to */
} vio_metal_res_buffer;

/* One combined image-sampler of a shader stage. Listed in SPIRV-Cross's
 * sampled_images order — the SAME order vio_spirv_reflect() reports and the PHP
 * layer's gl_to_hlsl_sampler remap indexes (see vio_bind_texture_internal). */
typedef struct _vio_metal_res_texture {
    int binding;    /* original GLSL binding */
    int msl_index;  /* [[texture(N)]] == [[sampler(N)]] */
    int is_depth;   /* sampler2DShadow -> needs a compare sampler */
    int is_cube;    /* samplerCube */
} vio_metal_res_texture;

typedef struct _vio_metal_vs_input {
    int location;
    int components; /* 1..4 */
    int columns;    /* 1 for vectors, 4 for a mat4 (occupies location..location+3) */
} vio_metal_vs_input;

typedef struct _vio_metal_stage_res {
    vio_metal_res_buffer  buffers[VIO_METAL_MAX_RES];
    int                   buffer_count;
    vio_metal_res_texture textures[VIO_METAL_MAX_RES];
    int                   texture_count;
    /* MSL buffer index of the stage's default uniform block — ubos[0], else the
     * push-constant block; -1 when the stage has neither. This is the block
     * vio_spirv_get_uniform_offsets() reflects into sh->cbuffer_data, so the
     * per-draw cbuffer slice is bound here. */
    int                   cbuffer_index;
} vio_metal_stage_res;

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
    vio_metal_vs_input  inputs[VIO_METAL_MAX_RES];
    int                 input_count;
    int                 vertex_stride;          /* bytes of per-vertex attributes (locations outside 3..6) */
    int                 uses_instance_attribs;  /* any input at location 3..6 (per-instance mat4 columns) */
} vio_metal_shader;

static id<MTLFunction> metal_build_function(const char *msl, char **error_out)
{
    @autoreleasepool {
        NSString *src = [NSString stringWithUTF8String:msl];
        NSError *err = nil;
        MTLCompileOptions *opts = [MTLCompileOptions new];
        opts.languageVersion = MTLLanguageVersion2_0;
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

#ifdef HAVE_SPIRV_CROSS
static void metal_gfx_add_binding(spvc_compiler compiler, SpvExecutionModel stage,
                                  unsigned desc_set, unsigned binding, unsigned msl_index)
{
    spvc_msl_resource_binding_2 rb;
    spvc_msl_resource_binding_init_2(&rb);
    rb.stage       = stage;
    rb.desc_set    = desc_set;
    rb.binding     = binding;
    rb.count       = 1;
    /* One entry serves whichever resource kind lives at this (set, binding):
     * a buffer reads msl_buffer, a texture reads msl_texture + msl_sampler. */
    rb.msl_buffer  = msl_index;
    rb.msl_texture = msl_index;
    rb.msl_sampler = msl_index;
    spvc_compiler_msl_add_resource_binding_2(compiler, &rb);
}

/* Transpile one GRAPHICS stage to MSL with deterministic resource indices.
 *
 * glslang's AUTO_MAP_BINDINGS leaves every resource of an OpenGL-style shader
 * at (set 0, binding 0) — three sampler2Ds all report binding 0 — so the GLSL
 * bindings cannot be used as MSL indices directly (SPIRV-Cross keys explicit
 * bindings by (set, binding) and would collapse them). Instead every UBO, SSBO,
 * push-constant block and sampled image is renumbered to a unique binding k in
 * list order and pinned to [[buffer(k)]] / [[texture(k)]] / [[sampler(k)]]. The
 * resulting tables (vio_metal_stage_res) are what draw / bind_texture /
 * bind_storage_buffer consult at runtime. Mirrors what vio_spirv_to_hlsl does
 * with SpvDecorationBinding for the D3D register spaces.
 *
 * Returns a malloc'd MSL string (caller frees) or NULL. */
static char *metal_gfx_spirv_to_msl(const uint32_t *spirv, size_t spirv_size, int is_vertex,
                                    vio_metal_shader *sh, unsigned frag_output_mask,
                                    char **error_msg)
{
    spvc_context   ctx = NULL;
    spvc_parsed_ir ir = NULL;
    spvc_compiler  compiler = NULL;
    spvc_compiler_options opts = NULL;
    spvc_resources resources = NULL;
    const char    *result = NULL;

    vio_metal_stage_res *res = is_vertex ? &sh->vs : &sh->fs;
    memset(res, 0, sizeof(*res));
    res->cbuffer_index = -1;

    if (spvc_context_create(&ctx) != SPVC_SUCCESS) {
        if (error_msg) *error_msg = strdup("Failed to create SPIRV-Cross context");
        return NULL;
    }
    if (spvc_context_parse_spirv(ctx, spirv, spirv_size / sizeof(uint32_t), &ir) != SPVC_SUCCESS ||
        spvc_context_create_compiler(ctx, SPVC_BACKEND_MSL, ir, SPVC_CAPTURE_MODE_TAKE_OWNERSHIP,
                                     &compiler) != SPVC_SUCCESS) {
        if (error_msg) *error_msg = strdup(spvc_context_get_last_error_string(ctx));
        spvc_context_destroy(ctx);
        return NULL;
    }

    if (spvc_compiler_create_compiler_options(compiler, &opts) == SPVC_SUCCESS) {
        spvc_compiler_options_set_uint(opts, SPVC_COMPILER_OPTION_MSL_VERSION, SPVC_MAKE_MSL_VERSION(2, 0, 0));
        spvc_compiler_options_set_uint(opts, SPVC_COMPILER_OPTION_MSL_PLATFORM, SPVC_MSL_PLATFORM_MACOS);
        if (!is_vertex) {
            spvc_compiler_options_set_uint(opts, SPVC_COMPILER_OPTION_MSL_ENABLE_FRAG_OUTPUT_MASK, frag_output_mask);
        }
        spvc_compiler_install_compiler_options(compiler, opts);
    }

    if (spvc_compiler_create_shader_resources(compiler, &resources) != SPVC_SUCCESS) {
        if (error_msg) *error_msg = strdup(spvc_context_get_last_error_string(ctx));
        spvc_context_destroy(ctx);
        return NULL;
    }

    SpvExecutionModel em = is_vertex ? SpvExecutionModelVertex : SpvExecutionModelFragment;
    unsigned next = 0;

    /* Buffers: UBOs first so ubos[0] becomes the default cbuffer (the block
     * vio_spirv_get_uniform_offsets picks), then SSBOs, then push constants. */
    static const struct { spvc_resource_type type; int kind; } buffer_kinds[] = {
        { SPVC_RESOURCE_TYPE_UNIFORM_BUFFER, 0 },
        { SPVC_RESOURCE_TYPE_STORAGE_BUFFER, 1 },
        { SPVC_RESOURCE_TYPE_PUSH_CONSTANT,  2 },
    };
    for (size_t k = 0; k < sizeof(buffer_kinds) / sizeof(buffer_kinds[0]); k++) {
        const spvc_reflected_resource *list = NULL;
        size_t count = 0;
        spvc_resources_get_resource_list_for_type(resources, buffer_kinds[k].type, &list, &count);
        for (size_t i = 0; i < count && res->buffer_count < VIO_METAL_MAX_RES; i++) {
            vio_metal_res_buffer *b = &res->buffers[res->buffer_count++];
            b->kind      = buffer_kinds[k].kind;
            b->set       = (int)spvc_compiler_get_decoration(compiler, list[i].id, SpvDecorationDescriptorSet);
            b->binding   = (int)spvc_compiler_get_decoration(compiler, list[i].id, SpvDecorationBinding);
            b->msl_index = (int)next;
            if (b->kind == 2) {
                metal_gfx_add_binding(compiler, em, SPVC_MSL_PUSH_CONSTANT_DESC_SET,
                                      SPVC_MSL_PUSH_CONSTANT_BINDING, next);
            } else {
                spvc_compiler_set_decoration(compiler, list[i].id, SpvDecorationDescriptorSet, 0);
                spvc_compiler_set_decoration(compiler, list[i].id, SpvDecorationBinding, next);
                metal_gfx_add_binding(compiler, em, 0, next, next);
            }
            if (res->cbuffer_index < 0 && b->kind != 1) {
                res->cbuffer_index = (int)next;
            }
            next++;
        }
    }

    /* Combined image-samplers, in sampled_images order. */
    {
        const spvc_reflected_resource *list = NULL;
        size_t count = 0;
        spvc_resources_get_resource_list_for_type(resources, SPVC_RESOURCE_TYPE_SAMPLED_IMAGE, &list, &count);
        for (size_t i = 0; i < count && res->texture_count < VIO_METAL_MAX_RES; i++) {
            vio_metal_res_texture *t = &res->textures[res->texture_count++];
            t->binding   = (int)spvc_compiler_get_decoration(compiler, list[i].id, SpvDecorationBinding);
            t->msl_index = (int)next;
            spvc_type type = spvc_compiler_get_type_handle(compiler, list[i].type_id);
            spvc_type image = type ? spvc_compiler_get_type_handle(compiler, spvc_type_get_base_type_id(type)) : NULL;
            if (image) {
                t->is_depth = spvc_type_get_image_is_depth(image) ? 1 : 0;
                t->is_cube  = (spvc_type_get_image_dimension(image) == SpvDimCube) ? 1 : 0;
            }
            spvc_compiler_set_decoration(compiler, list[i].id, SpvDecorationDescriptorSet, 0);
            spvc_compiler_set_decoration(compiler, list[i].id, SpvDecorationBinding, next);
            metal_gfx_add_binding(compiler, em, 0, next, next);
            next++;
        }
    }

    /* Vertex inputs: what the [[stage_in]] struct expects, so the pipeline can
     * build a matching MTLVertexDescriptor without trusting the caller's layout
     * (a mat4 instance attribute reflects as ONE input with 4 columns). */
    if (is_vertex) {
        const spvc_reflected_resource *list = NULL;
        size_t count = 0;
        spvc_resources_get_resource_list_for_type(resources, SPVC_RESOURCE_TYPE_STAGE_INPUT, &list, &count);
        sh->input_count = 0;
        sh->vertex_stride = 0;
        sh->uses_instance_attribs = 0;
        for (size_t i = 0; i < count && sh->input_count < VIO_METAL_MAX_RES; i++) {
            vio_metal_vs_input *in = &sh->inputs[sh->input_count++];
            in->location = (int)spvc_compiler_get_decoration(compiler, list[i].id, SpvDecorationLocation);
            spvc_type type = spvc_compiler_get_type_handle(compiler, list[i].type_id);
            in->components = type ? (int)spvc_type_get_vector_size(type) : 3;
            in->columns    = type ? (int)spvc_type_get_columns(type) : 1;
            if (in->components < 1 || in->components > 4) in->components = 3;
            if (in->columns < 1) in->columns = 1;
            if (in->location >= 3 && in->location <= 6) {
                sh->uses_instance_attribs = 1;
            } else {
                sh->vertex_stride += in->components * in->columns * (int)sizeof(float);
            }
        }
    }

    if (spvc_compiler_compile(compiler, &result) != SPVC_SUCCESS) {
        if (error_msg) *error_msg = strdup(spvc_context_get_last_error_string(ctx));
        spvc_context_destroy(ctx);
        return NULL;
    }

    if (getenv("VIO_DUMP_MSL")) {
        fprintf(stderr, "==== Metal %s MSL ====\n%s\n==== end ====\n",
                is_vertex ? "vertex" : "fragment", result);
        fflush(stderr);
    }

    char *output = strdup(result);
    spvc_context_destroy(ctx);
    return output;
}
#endif /* HAVE_SPIRV_CROSS */

static void metal_destroy_shader(void *s)
{
    if (!s) return;
    vio_metal_shader *sh = (vio_metal_shader *)s;
    if (sh->vert_fn) { CFRelease((CFTypeRef)sh->vert_fn); sh->vert_fn = NULL; }
    if (sh->frag_fn) { CFRelease((CFTypeRef)sh->frag_fn); sh->frag_fn = NULL; }
    if (sh->frag_fn_noout) { CFRelease((CFTypeRef)sh->frag_fn_noout); sh->frag_fn_noout = NULL; }
    free(sh);
}

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
    char *err = NULL;
    uint32_t *vs_spirv = NULL;
    uint32_t *fs_spirv = NULL;
    size_t vs_size = 0, fs_size = 0;
    int vs_owned = 0, fs_owned = 0;

    /* Detect SPIR-V magic 0x07230203 vs raw GLSL source */
    int vs_is_spirv = (desc->vertex_size >= 4 &&
        *(const uint32_t *)desc->vertex_data == 0x07230203);
    int fs_is_spirv = (desc->fragment_size >= 4 &&
        *(const uint32_t *)desc->fragment_data == 0x07230203);

    if (vs_is_spirv) {
        vs_spirv = (uint32_t *)desc->vertex_data;
        vs_size  = desc->vertex_size;
    } else {
        vs_spirv = vio_compile_glsl_to_spirv(
            (const char *)desc->vertex_data, 0, &vs_size, &err);
        if (!vs_spirv) {
            php_error_docref(NULL, E_WARNING, "Metal: VS GLSL→SPIR-V failed: %s",
                err ? err : "unknown");
            free(err);
            return NULL;
        }
        vs_owned = 1;
    }

    if (fs_is_spirv) {
        fs_spirv = (uint32_t *)desc->fragment_data;
        fs_size  = desc->fragment_size;
    } else {
        fs_spirv = vio_compile_glsl_to_spirv(
            (const char *)desc->fragment_data, 1, &fs_size, &err);
        if (!fs_spirv) {
            php_error_docref(NULL, E_WARNING, "Metal: FS GLSL→SPIR-V failed: %s",
                err ? err : "unknown");
            free(err);
            if (vs_owned) free(vs_spirv);
            return NULL;
        }
        fs_owned = 1;
    }

    vio_metal_shader *sh = calloc(1, sizeof(vio_metal_shader));
    if (!sh) {
        if (vs_owned) free(vs_spirv);
        if (fs_owned) free(fs_spirv);
        return NULL;
    }

    char *vs_msl = metal_gfx_spirv_to_msl(vs_spirv, vs_size, 1, sh, 0xFFFFFFFFu, &err);
    if (vs_owned) free(vs_spirv);
    if (!vs_msl) {
        php_error_docref(NULL, E_WARNING, "Metal: VS SPIR-V→MSL failed: %s",
            err ? err : "unknown");
        free(err);
        if (fs_owned) free(fs_spirv);
        free(sh);
        return NULL;
    }

    char *fs_msl = metal_gfx_spirv_to_msl(fs_spirv, fs_size, 0, sh, 0xFFFFFFFFu, &err);

    /* Depth-only variant: same resource renumbering (identical inputs, identical
     * algorithm => identical tables), colour outputs masked. Best effort. */
    char *fs_msl_noout = NULL;
    if (fs_msl) {
        vio_metal_shader scratch;
        memset(&scratch, 0, sizeof(scratch));
        char *err2 = NULL;
        fs_msl_noout = metal_gfx_spirv_to_msl(fs_spirv, fs_size, 0, &scratch, 0u, &err2);
        free(err2);
    }
    if (fs_owned) free(fs_spirv);
    if (!fs_msl) {
        php_error_docref(NULL, E_WARNING, "Metal: FS SPIR-V→MSL failed: %s",
            err ? err : "unknown");
        free(err);
        free(vs_msl);
        free(sh);
        return NULL;
    }

    id<MTLFunction> vfn = metal_build_function(vs_msl, &err);
    free(vs_msl);
    if (!vfn) {
        php_error_docref(NULL, E_WARNING, "Metal: VS MSL→MTLFunction failed: %s",
            err ? err : "unknown");
        free(err);
        free(fs_msl);
        free(sh);
        return NULL;
    }

    id<MTLFunction> ffn = metal_build_function(fs_msl, &err);
    free(fs_msl);
    if (!ffn) {
        php_error_docref(NULL, E_WARNING, "Metal: FS MSL→MTLFunction failed: %s",
            err ? err : "unknown");
        free(err);
        free(fs_msl_noout);
        free(sh);
        return NULL;
    }

    id<MTLFunction> ffn_noout = nil;
    if (fs_msl_noout) {
        char *err2 = NULL;
        ffn_noout = metal_build_function(fs_msl_noout, &err2);
        free(err2);
        free(fs_msl_noout);
    }

    sh->vert_fn = (void *)CFBridgingRetain(vfn);
    sh->frag_fn = (void *)CFBridgingRetain(ffn);
    sh->frag_fn_noout = ffn_noout ? (void *)CFBridgingRetain(ffn_noout) : NULL;
    return sh;
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
    id<MTLTexture> depth = rt->metal_depth_texture ? (__bridge id<MTLTexture>)rt->metal_depth_texture
                         : (rt->metal_msaa_depth_texture ? (__bridge id<MTLTexture>)rt->metal_msaa_depth_texture : nil);
    if (!rt->metal_color_texture && !depth) return;
    id<MTLCommandBuffer> cb = metal_new_command_buffer();
    int slices = rt->is_cube ? 6 : 1;
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
                d.colorAttachments[i].storeAction = MTLStoreActionStoreAndMultisampleResolve;
            } else {
                d.colorAttachments[i].storeAction = MTLStoreActionStore;
            }
        }
        if (depth) {
            d.depthAttachment.texture = depth;
            d.depthAttachment.loadAction = MTLLoadActionClear;
            d.depthAttachment.storeAction = MTLStoreActionStore;
            d.depthAttachment.clearDepth = 1.0;
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
        int samples = (depth_only || rt->is_cube) ? 1 : metal_clamp_sample_count(rt->samples);
        rt->samples = samples;

        int n_color = metal_rt_attachment_count(rt);
        if (rt->attachment_count <= 0) {
            rt->attachment_count = 1;
            rt->formats[0] = hdr ? VIO_FORMAT_RGBA16F : VIO_FORMAT_RGBA8;
        }
        MTLPixelFormat color_fmt = metal_pixel_format(rt->formats[0]);

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
            MTLTextureDescriptor *dd = [MTLTextureDescriptor
                texture2DDescriptorWithPixelFormat:MTLPixelFormatDepth32Float
                width:width height:width mipmapped:NO];
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
            rt->backend_type = VIO_RT_BACKEND_METAL;
            metal_rt_initial_clear(rt);
            return 0;
        }

        /* Depth texture — always created (parallel to OpenGL's "always create
         * depth attachment" pattern so shadow-map RTs work uniformly). */
        MTLTextureDescriptor *depth_desc = [MTLTextureDescriptor
            texture2DDescriptorWithPixelFormat:MTLPixelFormatDepth32Float
            width:width height:height mipmapped:NO];
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
    /* A cube RT bound without an explicit face renders into +X, level 0. */
    metal_bind_render_target_at(rt, rt->is_cube ? 0 : -1, 0);
}

static int metal_bind_render_target_face(void *rt_ptr, int face, int level)
{
    vio_render_target_object *rt = (vio_render_target_object *)rt_ptr;
    if (!rt || !rt->is_cube || face < 0 || face > 5 || level < 0 || level >= rt->mip_levels) return -1;
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

static void metal_update_buffer(void *buffer_ptr, const void *data, size_t size)
{
    vio_metal_buffer *buf = (vio_metal_buffer *)buffer_ptr;
    if (!buf || !data || size == 0) return;

    if (buf->type == VIO_BUFFER_UNIFORM) {
        if (!buf->shadow) return;
        memcpy(buf->shadow, data, size < buf->shadow_size ? size : buf->shadow_size);
        return;
    }
    if (!buf->buffer) return;
    @autoreleasepool {
        id<MTLBuffer> mb = (__bridge id<MTLBuffer>)buf->buffer;
        size_t n = size < buf->size ? size : buf->size;
        memcpy([mb contents], data, n);
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

static void *metal_create_texture(vio_texture_desc *desc)
{
    if (!desc || !desc->data || !vio_mtl.device || desc->width <= 0 || desc->height <= 0) return NULL;

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
    int   samples;     /* raster sample count of the target (1 or the RT's MSAA count) */
    int   has_depth;   /* 0 when the target has no depth attachment (cube-RT mip > 0) */
    void *pso;         /* id<MTLRenderPipelineState>, +1 retained */
} vio_metal_pso_variant;

typedef struct _vio_metal_pipeline {
    vio_metal_shader *shader;       /* borrowed — the VioPipeline holds a strong ref to its VioShader */
    void            *vert_fn;       /* +1 retained copies so a PSO can be built after the shader is gone */
    void            *frag_fn;
    void            *frag_fn_noout; /* depth-only variant, may be NULL */
    void            *depth_state;   /* id<MTLDepthStencilState>, +1 retained */
    MTLPrimitiveType primitive;
    MTLCullMode      cull;
    vio_blend_mode   blend;
    int              color_mask;    /* VIO_COLOR_* bits */
    int              per_attachment; /* attachment_blend[] / attachment_mask[] override blend / color_mask per attachment */
    int              attachment_blend[VIO_MAX_COLOR_ATTACHMENTS];
    int              attachment_mask[VIO_MAX_COLOR_ATTACHMENTS];
    float            depth_bias;
    float            slope_scaled_depth_bias;
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

        MTLDepthStencilDescriptor *ds = [[MTLDepthStencilDescriptor alloc] init];
        if (desc->depth_test) {
            ds.depthCompareFunction = (desc->depth_func == VIO_DEPTH_LEQUAL)
                ? MTLCompareFunctionLessEqual : MTLCompareFunctionLess;
        } else {
            ds.depthCompareFunction = MTLCompareFunctionAlways;
        }
        ds.depthWriteEnabled = (desc->depth_test && desc->depth_write) ? YES : NO;
        id<MTLDepthStencilState> dss = [vio_mtl.device newDepthStencilStateWithDescriptor:ds];
        p->depth_state = dss ? (void *)CFBridgingRetain(dss) : NULL;

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
    if (p->vert_fn)     CFRelease((CFTypeRef)p->vert_fn);
    if (p->frag_fn)     CFRelease((CFTypeRef)p->frag_fn);
    if (p->frag_fn_noout) CFRelease((CFTypeRef)p->frag_fn_noout);
    free(p);
}

static void metal_bind_pipeline(void *pipeline_ptr)
{
    metal_current_pipeline = (vio_metal_pipeline *)pipeline_ptr;
    /* Fixed-function + PSO are (re)applied per draw by metal_prepare_draw —
     * the target format may change between bind and draw (RT bind). */
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
        if (current_bound_rt->is_cube && current_bound_level > 0) t->has_depth = 0;
        if (current_bound_rt->samples > 1) t->samples = current_bound_rt->samples;
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
    t->fmts[0] = MTLPixelFormatBGRA8Unorm;
    t->count = 1;
    if (vio_mtl.samples > 1 && vio_mtl.msaa_color) t->samples = vio_mtl.samples;
}

/* Find or build the PSO variant for (target colour formats, mesh stride, samples, depth). */
static id<MTLRenderPipelineState> metal_pipeline_pso(vio_metal_pipeline *p, const vio_metal_target_desc *t, int stride)
{
    vio_metal_shader *sh = p->shader;
    if (stride <= 0 || stride < sh->vertex_stride) stride = sh->vertex_stride;
    int samples = t->samples < 1 ? 1 : t->samples;
    int has_depth = t->has_depth;
    int has_color = t->count > 0;

    for (int i = 0; i < p->variant_count; i++) {
        vio_metal_pso_variant *v = &p->variants[i];
        if (v->color_count != t->count || v->stride != stride || v->samples != samples || v->has_depth != has_depth) continue;
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
        int n = sh->input_count;
        memcpy(sorted, sh->inputs, sizeof(vio_metal_vs_input) * (size_t)n);
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
                    vd.attributes[loc].offset = mesh_offset;
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
            vd.layouts[VIO_METAL_VB_INSTANCE].stepRate = 1;
        }
        if (has_mesh_attr || has_inst_attr) {
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
        d.depthAttachmentPixelFormat = has_depth ? MTLPixelFormatDepth32Float : MTLPixelFormatInvalid;

        NSError *err = nil;
        id<MTLRenderPipelineState> pso = [vio_mtl.device newRenderPipelineStateWithDescriptor:d error:&err];
        if (!pso) {
            php_error_docref(NULL, E_WARNING, "Metal: render pipeline creation failed: %s",
                err ? [[err localizedDescription] UTF8String] : "unknown");
            return nil;
        }
        vio_metal_pso_variant *v = &p->variants[p->variant_count++];
        v->color_count = t->count;
        for (int k = 0; k < t->count; k++) v->color_fmts[k] = (int)t->fmts[k];
        v->stride    = stride;
        v->samples   = samples;
        v->has_depth = has_depth;
        v->pso       = (void *)CFBridgingRetain(pso);
        return pso;
    }
}

/* Apply everything a draw needs on the open encoder: PSO variant for the
 * current target, depth/cull/bias state, this draw's cbuffer slices and the
 * identity instance buffer. Returns 0 when there is nothing to draw into. */
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
    } else if (p->depth_state) {
        [enc setDepthStencilState:(__bridge id<MTLDepthStencilState>)p->depth_state];
    }
    [enc setCullMode:p->cull];
    [enc setFrontFacingWinding:MTLWindingCounterClockwise];  /* match GL / D3D FrontCounterClockwise */
    [enc setDepthBias:p->depth_bias slopeScale:p->slope_scaled_depth_bias clamp:0.0f];

    /* Per-draw uniform slices. A fragment stage that reflects a default block
     * but got no separate cbuffer object shares the vertex data (D3D11 binds
     * the VS cbuffer to PS b0 in that case). */
    vio_metal_shader *sh = p->shader;
    if (metal_current_vs_cb) {
        metal_bind_uniform_slice(metal_current_vs_cb, sh->vs.cbuffer_index,
                                 metal_current_fs_cb ? -1 : sh->fs.cbuffer_index);
    }
    if (metal_current_fs_cb) {
        metal_bind_uniform_slice(metal_current_fs_cb, -1, sh->fs.cbuffer_index);
    }

    if (sh->uses_instance_attribs && metal_identity_instance) {
        [enc setVertexBuffer:metal_identity_instance offset:0 atIndex:VIO_METAL_VB_INSTANCE];
    }
    return 1;
}

static void metal_bind_mesh_vb(void *vertex_buffer)
{
    vio_metal_buffer *vb = (vio_metal_buffer *)vertex_buffer;
    if (vb && vb->buffer) {
        [vio_mtl.current_encoder setVertexBuffer:(__bridge id<MTLBuffer>)vb->buffer
                                          offset:0 atIndex:VIO_METAL_VB_MESH];
    }
}

static void metal_draw(vio_draw_cmd *cmd)
{
    if (!cmd || cmd->vertex_count <= 0) return;
    @autoreleasepool {
        if (!metal_prepare_draw(cmd->vertex_stride)) return;
        metal_bind_mesh_vb(cmd->vertex_buffer);
        NSUInteger instances = cmd->instance_count > 0 ? (NSUInteger)cmd->instance_count : 1;
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
    @autoreleasepool {
        if (!metal_prepare_draw(cmd->vertex_stride)) return;
        metal_bind_mesh_vb(cmd->vertex_buffer);
        NSUInteger instances = cmd->instance_count > 0 ? (NSUInteger)cmd->instance_count : 1;
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
                                         instanceCount:(NSUInteger)instance_count
                                            baseVertex:0
                                          baseInstance:0];
    } else if (mesh->vertex_count > 0) {
        [vio_mtl.current_encoder drawPrimitives:metal_current_pipeline->primitive
                                    vertexStart:0
                                    vertexCount:(NSUInteger)mesh->vertex_count
                                  instanceCount:(NSUInteger)instance_count];
    }
}

void vio_metal_draw_instanced(void *mesh_obj, const float *matrices_4x4, int instance_count)
{
    vio_mesh_object *mesh = (vio_mesh_object *)mesh_obj;
    if (!mesh || !matrices_4x4 || instance_count <= 0) return;
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
static void metal_draw_indirect(void *mesh_obj, void *args_buffer, int max_draws, size_t offset)
{
    vio_mesh_object *mesh = (vio_mesh_object *)mesh_obj;
    vio_metal_buffer *args = (vio_metal_buffer *)args_buffer;
    if (!mesh || !args || !args->buffer || max_draws <= 0) return;
    @autoreleasepool {
        if (!metal_prepare_draw(mesh->stride)) return;
        vio_metal_buffer *sb = metal_pending_storage;
        if (sb && sb->buffer) {
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

static void metal_draw_instanced_from_storage(void *mesh_obj, int instance_count)
{
    vio_mesh_object *mesh = (vio_mesh_object *)mesh_obj;
    if (!mesh || instance_count <= 0) return;
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

static void metal_bind_texture(void *texture, int slot)
{
    vio_metal_texture *t = (vio_metal_texture *)texture;
    if (!t || !t->tex || !vio_mtl.current_encoder || slot < 0) return;
    @autoreleasepool {
        int is_depth = 0;
        int idx = metal_resolve_fs_texture(slot, &is_depth);
        if (idx < 0 || idx > 30) return;
        id<MTLSamplerState> s = is_depth ? metal_texture_cmp_sampler(t)
                                         : (__bridge id<MTLSamplerState>)t->sampler;
        [vio_mtl.current_encoder setFragmentTexture:(__bridge id<MTLTexture>)t->tex atIndex:(NSUInteger)idx];
        if (s) [vio_mtl.current_encoder setFragmentSamplerState:s atIndex:(NSUInteger)idx];
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
    if (!rt || !cm || !rt->is_cube || !rt->metal_color_texture || !vio_mtl.device) return -1;
    @autoreleasepool {
        id<MTLSamplerState> s = metal_make_sampler(VIO_FILTER_LINEAR, VIO_WRAP_CLAMP, rt->mip_levels > 1 ? 1 : 0, 0, 1);
        cm->metal_texture = (void *)CFRetain((CFTypeRef)rt->metal_color_texture);
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
        if (rt->is_cube) slice = (NSUInteger)(face >= 0 ? face : (rt->bound_face >= 0 ? rt->bound_face : 0));

        metal_flush_for_readback();

        int w = rt->width, h = rt->height;
        MTLPixelFormat fmt = src.pixelFormat;
        int bgra = 0;
        int vfmt = (fmt == MTLPixelFormatDepth32Float) ? -1 : metal_vio_format(fmt, &bgra);
        if (vfmt < -1) return -1;
        NSUInteger bpp = (fmt == MTLPixelFormatDepth32Float) ? 4 : (NSUInteger)vio_rt_format_bpp(vfmt);
        NSUInteger bpr = (NSUInteger)w * bpp;
        id<MTLBuffer> staging = [vio_mtl.device newBufferWithLength:bpr * h options:MTLResourceStorageModeShared];
        if (!staging) return -1;

        id<MTLCommandBuffer> cb = metal_new_command_buffer();
        id<MTLBlitCommandEncoder> blit = [cb blitCommandEncoder];
        [blit copyFromTexture:src sourceSlice:slice sourceLevel:0
                 sourceOrigin:MTLOriginMake(0, 0, 0) sourceSize:MTLSizeMake(w, h, 1)
                     toBuffer:staging destinationOffset:0
       destinationBytesPerRow:bpr destinationBytesPerImage:bpr * h];
        [blit endEncoding];
        [cb commit];
        [cb waitUntilCompleted];

        const unsigned char *s = (const unsigned char *)[staging contents];
        unsigned char *out = (unsigned char *)out_rgba;
        size_t n = (size_t)w * h;
        if (fmt == MTLPixelFormatDepth32Float) {
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
static int metal_generate_mipmaps(void *obj, int kind)
{
    if (!obj || !vio_mtl.device) return -1;
    @autoreleasepool {
        id<MTLTexture> tex = nil;
        switch (kind) {
            case 0: {
                vio_render_target_object *rt = (vio_render_target_object *)obj;
                if (rt->backend_type != VIO_RT_BACKEND_METAL || !rt->metal_color_texture) return -1;
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
        [vio_mtl.current_encoder setFragmentTexture:(__bridge id<MTLTexture>)cm->metal_texture
                                            atIndex:(NSUInteger)idx];
        if (cm->metal_sampler) {
            [vio_mtl.current_encoder setFragmentSamplerState:(__bridge id<MTLSamplerState>)cm->metal_sampler
                                                     atIndex:(NSUInteger)idx];
        }
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
    }
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
} vio_metal_compute_pipeline;

#ifdef HAVE_SPIRV_CROSS
/* Transpile a COMPUTE SPIR-V module to MSL, pinning the SSBO/UBO bindings 0/1/2
 * (set 0) to MSL buffer indices 0/1/2 so the PHP-side GLSL binding == the Metal
 * buffer index. Returns a malloc'd MSL string (caller frees) or NULL on failure.
 * On success *params_index_out receives the MSL buffer index of the Params UBO
 * (the explicit binding we installed for it, canonical 2). */
static char *metal_cs_spirv_to_msl(const uint32_t *spirv, size_t spirv_size,
                                   int *params_index_out, unsigned local_size_out[3],
                                   char **error_msg)
{
    spvc_context  ctx = NULL;
    spvc_parsed_ir ir = NULL;
    spvc_compiler compiler = NULL;
    const char   *result = NULL;
    char         *output = NULL;

    if (params_index_out) *params_index_out = 2; /* canonical default */

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
    char *msl = metal_cs_spirv_to_msl(spirv, spirv_size, &params_index, local_size, &err);
    if (free_spirv) free(spirv);
    if (!msl) {
        php_error_docref(NULL, E_WARNING, "Metal: CS SPIR-V->MSL failed: %s",
                         err ? err : "unknown");
        free(err);
        return NULL;
    }

    vio_metal_compute_pipeline *cp = NULL;

    @autoreleasepool {
        NSString *msl_src = [NSString stringWithUTF8String:msl];
        free(msl);
        msl = NULL;

        NSError *nerr = nil;
        MTLCompileOptions *opts = [MTLCompileOptions new];
        opts.languageVersion = MTLLanguageVersion2_0;
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

/* Command buffer that carries async dispatches not yet known to be complete:
 * the open frame buffer while recording, the committed one after present. */
static id<MTLCommandBuffer> metal_async_cb = nil;

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

        /* Bind each storage buffer at its MSL buffer index (== GLSL binding,
         * guaranteed by the explicit resource-binding remap at compile time). */
        for (int i = 0; i < cp->binding_count; i++) {
            vio_metal_compute_binding *b = &cp->bindings[i];
            if (!b->buffer || !b->buffer->buffer) continue;
            id<MTLBuffer> mb = (__bridge id<MTLBuffer>)b->buffer->buffer;
            [enc setBuffer:mb offset:0 atIndex:(NSUInteger)b->slot];
        }

        /* Params UBO at its MSL buffer index (canonical 2). */
        if (cp->params_buffer && cp->params_size > 0) {
            id<MTLBuffer> pb = (__bridge id<MTLBuffer>)cp->params_buffer;
            [enc setBuffer:pb offset:0 atIndex:(NSUInteger)cp->params_index];
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

static int metal_supports_feature(vio_feature f)
{
    switch (f) {
    case VIO_FEATURE_COMPUTE:
    case VIO_FEATURE_3D_PIPELINE:
    case VIO_FEATURE_VERTEX_STORAGE:
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
    case VIO_FEATURE_INDIRECT_DRAW: /* drawIndexedPrimitives:indirectBuffer: */
        return 1;
    case VIO_FEATURE_STENCIL:       /* depth attachments are Depth32Float (no stencil plane) — macOS follow-up */
    case VIO_FEATURE_GEOMETRY:      /* Metal has no geometry stage */
    case VIO_FEATURE_TESSELLATION:  /* not wired (Metal tessellation is compute-driven) */
    case VIO_FEATURE_RAYTRACING:
    case VIO_FEATURE_MULTIVIEW:
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
    .dispatch_compute  = metal_dispatch_compute,
    .supports_feature  = metal_supports_feature,
    .gpu_frame_time    = metal_gpu_frame_time,
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
    .draw_instanced_from_storage = metal_draw_instanced_from_storage,
    .draw_indirect     = metal_draw_indirect,
    .destroy_font_atlas = metal_destroy_font_atlas,
    .upload_font_atlas  = metal_upload_font_atlas,
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

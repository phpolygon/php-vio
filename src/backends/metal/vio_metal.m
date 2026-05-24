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
    MTLRenderPassDescriptor   *render_pass_desc;
    id<MTLTexture>             depth_texture;
    int                        width;
    int                        height;
    float                      clear_r, clear_g, clear_b, clear_a;
    int                        initialized;
    int                        vsync;
    id<MTLTexture>             offscreen_texture; /* for vsync-off rendering */
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

/* ── Metal 2D pipeline state ─────────────────────────────────────── */

typedef struct _vio_metal_2d {
    id<MTLRenderPipelineState> pipeline_shapes;
    id<MTLRenderPipelineState> pipeline_sprites;
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

        /* Create Metal device */
        vio_mtl.device = MTLCreateSystemDefaultDevice();
        if (!vio_mtl.device) {
            php_error_docref(NULL, E_WARNING, "Metal: no GPU device found");
            return -1;
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

        /* Create depth texture */
        create_depth_texture(width, height);

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
        glfwGetFramebufferSize(win, &fb_w, &fb_h);

        if (vio_metal_setup_context_native((__bridge void *)layer, fb_w, fb_h, cfg) != 0) {
            return -1;
        }

        /* Remember the GLFW window for pull-based resize polling in
         * metal_begin_frame. iOS / headless setups skip this and push
         * resizes through vio_metal_handle_resize() instead. */
        vio_mtl.glfw_window = win;
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
            mtl_2d.sampler          = nil;
            mtl_2d.vertex_buffer    = nil;
            mtl_2d.initialized      = 0;
        }

        /* Release all registered textures */
        for (unsigned int i = 1; i < VIO_METAL_MAX_TEXTURES; i++) {
            metal_textures[i] = nil;
        }
        metal_next_texture_id = 1;

        /* Wait for GPU to finish */
        if (vio_mtl.command_queue) {
            id<MTLCommandBuffer> cmd = [vio_mtl.command_queue commandBuffer];
            [cmd commit];
            [cmd waitUntilCompleted];
        }

        vio_mtl.depth_texture    = nil;
        vio_mtl.render_pass_desc = nil;
        vio_mtl.command_queue    = nil;
        vio_mtl.metal_layer      = nil;
        vio_mtl.device           = nil;

        vio_mtl.initialized = 0;
    }
}

/* ── Metal 2D pipeline initialization ────────────────────────────── */

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

        /* Pipeline: shapes (no texture, alpha blending) */
        MTLRenderPipelineDescriptor *pipeDesc = [[MTLRenderPipelineDescriptor alloc] init];
        pipeDesc.vertexFunction = vertexFunc;
        pipeDesc.fragmentFunction = fragShapes;
        pipeDesc.vertexDescriptor = vertDesc;
        pipeDesc.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;
        pipeDesc.colorAttachments[0].blendingEnabled = YES;
        pipeDesc.colorAttachments[0].sourceRGBBlendFactor = MTLBlendFactorSourceAlpha;
        pipeDesc.colorAttachments[0].destinationRGBBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
        pipeDesc.colorAttachments[0].sourceAlphaBlendFactor = MTLBlendFactorSourceAlpha;
        pipeDesc.colorAttachments[0].destinationAlphaBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
        pipeDesc.depthAttachmentPixelFormat = MTLPixelFormatDepth32Float;

        mtl_2d.pipeline_shapes = [vio_mtl.device newRenderPipelineStateWithDescriptor:pipeDesc error:&error];
        if (!mtl_2d.pipeline_shapes) {
            php_error_docref(NULL, E_WARNING, "Metal 2D: shapes pipeline failed: %s",
                [[error localizedDescription] UTF8String]);
            return -1;
        }

        /* Pipeline: sprites (textured, alpha blending) */
        pipeDesc.fragmentFunction = fragSprites;
        mtl_2d.pipeline_sprites = [vio_mtl.device newRenderPipelineStateWithDescriptor:pipeDesc error:&error];
        if (!mtl_2d.pipeline_sprites) {
            php_error_docref(NULL, E_WARNING, "Metal 2D: sprites pipeline failed: %s",
                [[error localizedDescription] UTF8String]);
            return -1;
        }

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

        /* Disable depth for 2D */
        [vio_mtl.current_encoder setDepthStencilState:mtl_2d.depth_disabled];

        /* Bind vertex buffer and projection */
        [vio_mtl.current_encoder setVertexBuffer:mtl_2d.vertex_buffer offset:0 atIndex:0];
        [vio_mtl.current_encoder setVertexBuffer:projBuf offset:0 atIndex:1];

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
                (item->texture_id > 0) ? mtl_2d.pipeline_sprites : mtl_2d.pipeline_shapes;
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
        desc.storageMode = MTLStorageModeShared;

        id<MTLTexture> tex = [vio_mtl.device newTextureWithDescriptor:desc];
        if (!tex) return 0;

        MTLRegion region = MTLRegionMake2D(0, 0, width, height);
        [tex replaceRegion:region mipmapLevel:0
               withBytes:pixels bytesPerRow:width * 4];

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
        desc.storageMode = MTLStorageModeShared;
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

        /* Wait for GPU to finish rendering */
        if (last_presented_cmd_buf) {
            [last_presented_cmd_buf waitUntilCompleted];
        }

        id<MTLTexture> drawableTexture = last_presented_texture;
        if (!drawableTexture) return -1;

        int tw = (int)drawableTexture.width;
        int th = (int)drawableTexture.height;
        int use_w = (width < tw) ? width : tw;
        int use_h = (height < th) ? height : th;

        /* Read BGRA from drawable texture */
        unsigned char *bgra = emalloc(use_w * use_h * 4);
        MTLRegion region = MTLRegionMake2D(0, 0, use_w, use_h);
        [drawableTexture getBytes:bgra bytesPerRow:use_w * 4
                      fromRegion:region mipmapLevel:0];

        /* Convert BGRA -> RGBA */
        for (int i = 0; i < use_w * use_h; i++) {
            int off = i * 4;
            unsigned char b = bgra[off + 0];
            unsigned char r = bgra[off + 2];
            out_rgba[off + 0] = r;
            out_rgba[off + 1] = bgra[off + 1]; /* G */
            out_rgba[off + 2] = b;
            out_rgba[off + 3] = bgra[off + 3]; /* A */
        }

        efree(bgra);
        return 0;
    }
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

/* Open a render-pass encoder against either the currently bound offscreen RT
 * (current_bound_rt) or the swapchain drawable. Assumes cmd buffer exists.
 * load_clear=1 uses Clear (and clear_r/g/b/a); 0 uses Load (preserves existing
 * contents). Sets viewport + scissor to the target's dimensions. */
static void metal_open_encoder(int load_clear)
{
    @autoreleasepool {
        if (!vio_mtl.current_cmd_buf) return;

        id<MTLTexture> color_target = nil;
        id<MTLTexture> depth_target = vio_mtl.depth_texture; /* default swapchain depth */
        int target_w = vio_mtl.width;
        int target_h = vio_mtl.height;
        int depth_only = 0;

        if (current_bound_rt) {
            color_target = (__bridge id<MTLTexture>)current_bound_rt->metal_color_texture;
            depth_target = (__bridge id<MTLTexture>)current_bound_rt->metal_depth_texture;
            target_w = current_bound_rt->width;
            target_h = current_bound_rt->height;
            depth_only = current_bound_rt->depth_only;
        } else if (!vio_mtl.vsync && vio_mtl.offscreen_texture) {
            color_target = vio_mtl.offscreen_texture;
        } else if (vio_mtl.current_drawable) {
            color_target = vio_mtl.current_drawable.texture;
        }

        MTLRenderPassDescriptor *desc = [MTLRenderPassDescriptor renderPassDescriptor];

        if (!depth_only && color_target) {
            desc.colorAttachments[0].texture = color_target;
            desc.colorAttachments[0].loadAction = load_clear ? MTLLoadActionClear : MTLLoadActionLoad;
            desc.colorAttachments[0].storeAction = MTLStoreActionStore;
            desc.colorAttachments[0].clearColor =
                MTLClearColorMake(vio_mtl.clear_r, vio_mtl.clear_g, vio_mtl.clear_b, vio_mtl.clear_a);
        }

        if (depth_target) {
            desc.depthAttachment.texture = depth_target;
            desc.depthAttachment.loadAction = load_clear ? MTLLoadActionClear : MTLLoadActionLoad;
            desc.depthAttachment.storeAction = depth_only ? MTLStoreActionStore : MTLStoreActionDontCare;
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

        vio_mtl.current_cmd_buf = [vio_mtl.command_queue commandBuffer];
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
}

/* ── Shader compilation: SPIR-V → MSL → MTLLibrary ────────────────── */

extern char *vio_spirv_to_msl(const uint32_t *spirv, size_t spirv_size,
                              char **error_msg);
extern uint32_t *vio_compile_glsl_to_spirv(const char *source, int stage,
                                            size_t *out_size, char **error_msg);

/* ARC does not track strong references stored in C structs, so we keep
 * Metal objects as opaque `void *` and bridge across this boundary
 * manually with __bridge_retained / CFRelease. */
typedef struct _vio_metal_shader {
    void *vert_fn;  /* id<MTLFunction>, +1 retained */
    void *frag_fn;  /* id<MTLFunction>, +1 retained */
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

    char *vs_msl = vio_spirv_to_msl(vs_spirv, vs_size, &err);
    if (vs_owned) free(vs_spirv);
    if (!vs_msl) {
        php_error_docref(NULL, E_WARNING, "Metal: VS SPIR-V→MSL failed: %s",
            err ? err : "unknown");
        free(err);
        if (fs_owned) free(fs_spirv);
        return NULL;
    }

    char *fs_msl = vio_spirv_to_msl(fs_spirv, fs_size, &err);
    if (fs_owned) free(fs_spirv);
    if (!fs_msl) {
        php_error_docref(NULL, E_WARNING, "Metal: FS SPIR-V→MSL failed: %s",
            err ? err : "unknown");
        free(err);
        free(vs_msl);
        return NULL;
    }

    id<MTLFunction> vfn = metal_build_function(vs_msl, &err);
    free(vs_msl);
    if (!vfn) {
        php_error_docref(NULL, E_WARNING, "Metal: VS MSL→MTLFunction failed: %s",
            err ? err : "unknown");
        free(err);
        free(fs_msl);
        return NULL;
    }

    id<MTLFunction> ffn = metal_build_function(fs_msl, &err);
    free(fs_msl);
    if (!ffn) {
        php_error_docref(NULL, E_WARNING, "Metal: FS MSL→MTLFunction failed: %s",
            err ? err : "unknown");
        free(err);
        return NULL;
    }

    vio_metal_shader *sh = calloc(1, sizeof(vio_metal_shader));
    if (!sh) return NULL;
    sh->vert_fn = (void *)CFBridgingRetain(vfn);
    sh->frag_fn = (void *)CFBridgingRetain(ffn);
    return sh;
}

static void metal_destroy_shader(void *s)
{
    if (!s) return;
    vio_metal_shader *sh = (vio_metal_shader *)s;
    if (sh->vert_fn) { CFRelease((CFTypeRef)sh->vert_fn); sh->vert_fn = NULL; }
    if (sh->frag_fn) { CFRelease((CFTypeRef)sh->frag_fn); sh->frag_fn = NULL; }
    free(sh);
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

static int metal_create_render_target(void *rt_ptr, int width, int height, int hdr, int depth_only)
{
    @autoreleasepool {
        vio_render_target_object *rt = (vio_render_target_object *)rt_ptr;
        if (!vio_mtl.initialized || !vio_mtl.device) return -1;

        /* Depth texture — always created (parallel to OpenGL's "always create
         * depth attachment" pattern so shadow-map RTs work uniformly). */
        MTLTextureDescriptor *depth_desc = [MTLTextureDescriptor
            texture2DDescriptorWithPixelFormat:MTLPixelFormatDepth32Float
            width:width height:height mipmapped:NO];
        depth_desc.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
        depth_desc.storageMode = MTLStorageModePrivate;

        id<MTLTexture> depth_tex = [vio_mtl.device newTextureWithDescriptor:depth_desc];
        if (!depth_tex) {
            php_error_docref(NULL, E_WARNING,
                "Metal: failed to create render-target depth texture (%dx%d)", width, height);
            return -1;
        }

        id<MTLTexture> color_tex = nil;
        if (!depth_only) {
            MTLTextureDescriptor *color_desc = [MTLTextureDescriptor
                texture2DDescriptorWithPixelFormat:(hdr ? MTLPixelFormatRGBA16Float
                                                        : MTLPixelFormatBGRA8Unorm)
                width:width height:height mipmapped:NO];
            color_desc.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
            color_desc.storageMode = MTLStorageModePrivate;

            color_tex = [vio_mtl.device newTextureWithDescriptor:color_desc];
            if (!color_tex) {
                php_error_docref(NULL, E_WARNING,
                    "Metal: failed to create render-target color texture (%dx%d, hdr=%d)",
                    width, height, hdr);
                return -1;
            }
        }

        /* Bridge strong references into the RT's opaque void * slots. */
        rt->metal_depth_texture = (void *)CFBridgingRetain(depth_tex);
        rt->metal_color_texture = color_tex ? (void *)CFBridgingRetain(color_tex) : NULL;
        rt->backend_type = VIO_RT_BACKEND_METAL;

        return 0;
    }
}

static void metal_bind_render_target(void *rt_ptr)
{
    @autoreleasepool {
        vio_render_target_object *rt = (vio_render_target_object *)rt_ptr;
        if (rt->backend_type != VIO_RT_BACKEND_METAL || !vio_mtl.initialized) return;

        current_bound_rt = rt;

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

static void metal_unbind_render_target(unsigned int default_fbo, int width, int height)
{
    (void)default_fbo; (void)width; (void)height;
    @autoreleasepool {
        if (!vio_mtl.initialized) return;

        /* State mutation must run regardless of frame state, otherwise the
         * stash keeps a dangling pointer to a release()'d RT object and the
         * next metal_begin_frame opens an encoder on freed Metal textures.
         * Symptom: every-few-frames flicker after a warmRender pass. */
        current_bound_rt = NULL;

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

    if (rt->metal_color_texture) {
        CFBridgingRelease(rt->metal_color_texture);
        rt->metal_color_texture = NULL;
    }
    if (rt->metal_depth_texture) {
        CFBridgingRelease(rt->metal_depth_texture);
        rt->metal_depth_texture = NULL;
    }
}

/* ── 3D pipeline / buffer / texture / draw: not yet wired ─────────
 *
 * Returning NULL/no-op causes vio_pipeline()/vio_mesh()/vio_texture()/
 * vio_draw() to fail gracefully (the PHP-level wrappers check for false).
 * Implementing these requires:
 *   - MTLRenderPipelineState cache keyed by (shader, vertex layout, blend,
 *     depth, cull, topology) — pipeline state in Metal is monolithic
 *   - Vertex descriptor inferred from vio_vertex_attrib[] OR from SPIR-V
 *     reflection, mapping locations 0..N to attribute indices
 *   - MTLBuffer for vertex/index/uniform with per-frame ring allocator
 *   - MTLTexture creation with format mapping (RGBA8/Depth32F/etc.) plus
 *     MTLSamplerState cached by filter/wrap combinations
 *   - draw/draw_indexed encoding cbuffer→setVertexBytes/setFragmentBytes,
 *     binding textures+samplers, then drawPrimitives:
 * Until those land, projects on macOS must run with vioBackend='opengl'.
 */

static void *metal_create_pipeline(vio_pipeline_desc *desc) { (void)desc; return NULL; }
static void  metal_destroy_pipeline(void *p) { (void)p; }
static void  metal_bind_pipeline(void *p) { (void)p; }
static void *metal_create_buffer(vio_buffer_desc *desc) { (void)desc; return NULL; }
static void  metal_update_buffer(void *buf, const void *data, size_t size) { (void)buf; (void)data; (void)size; }
static void  metal_destroy_buffer(void *buf) { (void)buf; }
static void *metal_create_texture(vio_texture_desc *desc) { (void)desc; return NULL; }
static void  metal_destroy_texture(void *tex) { (void)tex; }
static void  metal_draw(vio_draw_cmd *cmd) { (void)cmd; }
static void  metal_draw_indexed(vio_draw_indexed_cmd *cmd) { (void)cmd; }
static void  metal_dispatch_compute(vio_compute_cmd *cmd) { (void)cmd; }
static int metal_supports_feature(vio_feature f)
{
    switch (f) {
    case VIO_FEATURE_3D_PIPELINE:
        /* TODO(metal-3d): 3D pipeline / buffer / texture / draw are stubbed
         * out (see comment above metal_create_pipeline). Flip to 1 once the
         * Metal 3D path is wired so vio_get_auto_backend can prefer Metal. */
        return 0;
    case VIO_FEATURE_NATIVE_2D_BATCH:
        /* Metal ships its own 2D-batch renderer (vio_metal_2d_*). */
        return 1;
    case VIO_FEATURE_SCISSOR:
    case VIO_FEATURE_DEPTH_BIAS:
        /* Pipeline state exposes both. */
        return 1;
    case VIO_FEATURE_TEXTURE_SWIZZLE:
        /* MTLTextureSwizzleChannels on the texture descriptor. */
        return 1;
    case VIO_FEATURE_RENDER_TARGET:
    case VIO_FEATURE_RENDER_TARGET_HDR:
    case VIO_FEATURE_RENDER_TARGET_DEPTH:
        /* MTLTexture-backed offscreen RTs via create/bind/unbind/destroy. */
        return 1;
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
    if (tex->texture_id && !tex->borrowed) {
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
    .dispatch_compute  = metal_dispatch_compute,
    .supports_feature  = metal_supports_feature,
    .destroy_font_atlas = metal_destroy_font_atlas,
    .upload_font_atlas  = metal_upload_font_atlas,
    .destroy_texture_obj = metal_destroy_texture_obj,
    .create_render_target  = metal_create_render_target,
    .bind_render_target    = metal_bind_render_target,
    .unbind_render_target  = metal_unbind_render_target,
    .destroy_render_target = metal_destroy_render_target,
};

void vio_backend_metal_register(void)
{
    vio_register_backend(&metal_backend);
}

#endif /* HAVE_METAL */

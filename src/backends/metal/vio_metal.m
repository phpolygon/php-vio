/*
 * php-vio - Metal Backend implementation (macOS)
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#ifdef HAVE_METAL

#include "php.h"

#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>
#define GLFW_EXPOSE_NATIVE_COCOA
#include <GLFW/glfw3native.h>

#include "vio_metal.h"

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
    GLFWwindow                *glfw_window;
} vio_metal_state;

static vio_metal_state vio_mtl = {0};

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

int vio_metal_setup_context(void *glfw_window, vio_config *cfg)
{
    @autoreleasepool {
        vio_mtl.glfw_window = (GLFWwindow *)glfw_window;

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

        /* Get NSWindow from GLFW and attach CAMetalLayer */
        NSWindow *ns_window = (NSWindow *)glfwGetCocoaWindow(vio_mtl.glfw_window);
        if (!ns_window) {
            php_error_docref(NULL, E_WARNING, "Metal: failed to get Cocoa window");
            return -1;
        }

        vio_mtl.metal_layer = [CAMetalLayer layer];
        vio_mtl.metal_layer.device = vio_mtl.device;
        vio_mtl.metal_layer.pixelFormat = MTLPixelFormatBGRA8Unorm;
        vio_mtl.metal_layer.framebufferOnly = YES;

        /* Get framebuffer size */
        int fb_w, fb_h;
        glfwGetFramebufferSize(vio_mtl.glfw_window, &fb_w, &fb_h);
        vio_mtl.metal_layer.drawableSize = CGSizeMake(fb_w, fb_h);

        /* Replace content view's layer */
        NSView *content_view = [ns_window contentView];
        [content_view setWantsLayer:YES];
        [content_view setLayer:vio_mtl.metal_layer];

        vio_mtl.width  = fb_w;
        vio_mtl.height = fb_h;

        /* Create depth texture */
        create_depth_texture(fb_w, fb_h);

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

void vio_metal_shutdown_context(void)
{
    @autoreleasepool {
        if (!vio_mtl.initialized) return;

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
    }
}

static void metal_begin_frame(void)
{
    @autoreleasepool {
        if (!vio_mtl.initialized) return;

        /* Check for resize */
        int fb_w, fb_h;
        glfwGetFramebufferSize(vio_mtl.glfw_window, &fb_w, &fb_h);
        if (fb_w != vio_mtl.width || fb_h != vio_mtl.height) {
            metal_resize(fb_w, fb_h);
        }

        vio_mtl.current_drawable = [vio_mtl.metal_layer nextDrawable];
        if (!vio_mtl.current_drawable) return;

        /* Configure color attachment */
        vio_mtl.render_pass_desc.colorAttachments[0].texture = vio_mtl.current_drawable.texture;
        vio_mtl.render_pass_desc.colorAttachments[0].loadAction = MTLLoadActionClear;
        vio_mtl.render_pass_desc.colorAttachments[0].storeAction = MTLStoreActionStore;
        vio_mtl.render_pass_desc.colorAttachments[0].clearColor =
            MTLClearColorMake(vio_mtl.clear_r, vio_mtl.clear_g, vio_mtl.clear_b, vio_mtl.clear_a);

        vio_mtl.current_cmd_buf = [vio_mtl.command_queue commandBuffer];
        vio_mtl.current_encoder = [vio_mtl.current_cmd_buf
            renderCommandEncoderWithDescriptor:vio_mtl.render_pass_desc];

        /* Set viewport */
        MTLViewport viewport = {0, 0, (double)vio_mtl.width, (double)vio_mtl.height, 0.0, 1.0};
        [vio_mtl.current_encoder setViewport:viewport];

        MTLScissorRect scissor = {0, 0, (NSUInteger)vio_mtl.width, (NSUInteger)vio_mtl.height};
        [vio_mtl.current_encoder setScissorRect:scissor];
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
        if (!vio_mtl.current_cmd_buf || !vio_mtl.current_drawable) return;

        [vio_mtl.current_cmd_buf presentDrawable:vio_mtl.current_drawable];
        [vio_mtl.current_cmd_buf commit];

        vio_mtl.current_cmd_buf  = nil;
        vio_mtl.current_drawable = nil;
    }
}

static void metal_clear(float r, float g, float b, float a)
{
    vio_mtl.clear_r = r;
    vio_mtl.clear_g = g;
    vio_mtl.clear_b = b;
    vio_mtl.clear_a = a;
}

/* ── Stub implementations for not-yet-implemented operations ─────── */

static void *metal_create_pipeline(vio_pipeline_desc *desc) { (void)desc; return NULL; }
static void  metal_destroy_pipeline(void *p) { (void)p; }
static void  metal_bind_pipeline(void *p) { (void)p; }
static void *metal_create_buffer(vio_buffer_desc *desc) { (void)desc; return NULL; }
static void  metal_update_buffer(void *buf, const void *data, size_t size) { (void)buf; (void)data; (void)size; }
static void  metal_destroy_buffer(void *buf) { (void)buf; }
static void *metal_create_texture(vio_texture_desc *desc) { (void)desc; return NULL; }
static void  metal_destroy_texture(void *tex) { (void)tex; }
static void *metal_compile_shader(vio_shader_desc *desc) { (void)desc; return NULL; }
static void  metal_destroy_shader(void *s) { (void)s; }
static void  metal_draw(vio_draw_cmd *cmd) { (void)cmd; }
static void  metal_draw_indexed(vio_draw_indexed_cmd *cmd) { (void)cmd; }
static void  metal_dispatch_compute(vio_compute_cmd *cmd) { (void)cmd; }
static int   metal_supports_feature(vio_feature f) { (void)f; return 0; }

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
    .dispatch_compute  = metal_dispatch_compute,
    .supports_feature  = metal_supports_feature,
};

void vio_backend_metal_register(void)
{
    vio_register_backend(&metal_backend);
}

#endif /* HAVE_METAL */

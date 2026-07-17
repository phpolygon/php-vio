/*
 * php-vio - PHP Video Input Output
 * Extension entry point
 */

#include "php_vio.h"

#if defined(ZTS) && defined(COMPILE_DL_VIO)
ZEND_TSRMLS_CACHE_DEFINE()
#endif

#include "php_vio_arginfo.h"
#include "src/vio_context.h"
#include "src/vio_backend_registry.h"
#include "src/vio_resource.h"
#include "src/vio_window.h"
#include "src/vio_input.h"
#include "src/vio_backend_null.h"
#include "src/vio_mesh.h"
#include "src/vio_shader.h"
#include "src/vio_pipeline.h"
#include "src/vio_texture.h"
#include "src/vio_buffer.h"
#include "src/vio_compute_pipeline.h"
#include "src/vio_2d.h"
#include "src/vio_font.h"
#include "src/vio_text_shape.h"
#include "src/vio_shader_compiler.h"
#include "src/vio_shader_reflect.h"
#include "src/vio_audio.h"
#include "src/vio_render_target.h"
#include "src/vio_cubemap.h"
#include "src/vio_recorder.h"
#include "src/vio_stream.h"
#include "src/vio_thermal.h"
#include "include/vio_constants.h"
#include "include/vio_plugin.h"
#include "vendor/stb/stb_image.h"
#include "vendor/stb/stb_image_write.h"
#ifndef PHP_WIN32
#include <pthread.h>
#else
#include <windows.h>
#include <process.h>
#endif

/* Platform headers for vio_query_total_ram_bytes() (vio_gpu_info). */
#if defined(__APPLE__)
#include <sys/sysctl.h>
#elif defined(__linux__)
#include <unistd.h>
#endif

#include <string.h>
#include <stdlib.h>

#ifdef HAVE_GLFW
#include <glad/glad.h>
#include "src/backends/opengl/vio_opengl.h"
int vio_opengl_setup_context(void);
#endif

#ifdef HAVE_VULKAN
#include "src/backends/vulkan/vio_vulkan.h"
#endif
#ifdef HAVE_METAL
#include "src/backends/metal/vio_metal.h"
#include "src/backends/ios/vio_ios.h"
#endif
#if defined(HAVE_D3D11) || defined(HAVE_D3D12)
#ifndef COBJMACROS
#define COBJMACROS
#endif
#endif
#ifdef HAVE_D3D11
#include "src/backends/d3d11/vio_d3d11.h"
#endif
#ifdef HAVE_D3D12
#include "src/backends/d3d12/vio_d3d12.h"
#endif

ZEND_DECLARE_MODULE_GLOBALS(vio)

/* ── INI entries ──────────────────────────────────────────────────── */

PHP_INI_BEGIN()
    STD_PHP_INI_ENTRY("vio.default_backend", "auto", PHP_INI_ALL, OnUpdateString, default_backend, zend_vio_globals, vio_globals)
    STD_PHP_INI_BOOLEAN("vio.debug", "0", PHP_INI_ALL, OnUpdateBool, debug, zend_vio_globals, vio_globals)
    STD_PHP_INI_BOOLEAN("vio.vsync", "1", PHP_INI_ALL, OnUpdateBool, vsync, zend_vio_globals, vio_globals)
PHP_INI_END()

/* ── PHP function implementations ─────────────────────────────────── */

ZEND_FUNCTION(vio_create)
{
    char *backend_name = NULL;
    size_t backend_name_len = 0;
    zval *options = NULL;
    HashTable *options_ht = NULL;

    ZEND_PARSE_PARAMETERS_START(0, 2)
        Z_PARAM_OPTIONAL
        Z_PARAM_STRING(backend_name, backend_name_len)
        Z_PARAM_ARRAY_HT(options_ht)
    ZEND_PARSE_PARAMETERS_END();

    /* Find backend */
    const vio_backend *backend;
    if (!backend_name || strcmp(backend_name, "auto") == 0) {
        backend = vio_get_auto_backend();
    } else {
        backend = vio_find_backend(backend_name);
    }

    if (!backend) {
        if (backend_name && strcmp(backend_name, "auto") != 0) {
            php_error_docref(NULL, E_WARNING, "Backend \"%s\" is not available", backend_name);
        } else {
            php_error_docref(NULL, E_WARNING, "No graphics backend available. Load a backend extension (e.g., vio_opengl)");
        }
        RETURN_FALSE;
    }

    /* Create context object */
    zval obj;
    object_init_ex(&obj, vio_context_ce);
    vio_context_object *ctx = Z_VIO_CONTEXT_P(&obj);

    ctx->backend = backend;

    /* Parse options */
    ctx->config.width  = 800;
    ctx->config.height = 600;
    ctx->config.title  = "php-vio";
    ctx->config.vsync  = VIO_G(vsync);
    ctx->config.samples = 0;
    ctx->config.debug  = VIO_G(debug);

    if (options_ht) {
        zval *val;
        if ((val = zend_hash_str_find(options_ht, "width", sizeof("width") - 1)) != NULL) {
            ctx->config.width = (int)zval_get_long(val);
        }
        if ((val = zend_hash_str_find(options_ht, "height", sizeof("height") - 1)) != NULL) {
            ctx->config.height = (int)zval_get_long(val);
        }
        if ((val = zend_hash_str_find(options_ht, "title", sizeof("title") - 1)) != NULL) {
            ctx->config.title = Z_STRVAL_P(val);
        }
        if ((val = zend_hash_str_find(options_ht, "vsync", sizeof("vsync") - 1)) != NULL) {
            ctx->config.vsync = (int)zval_get_long(val);
        }
        if ((val = zend_hash_str_find(options_ht, "samples", sizeof("samples") - 1)) != NULL) {
            ctx->config.samples = (int)zval_get_long(val);
        }
        /* Backbuffers / in-flight frames. Honoured by D3D12 (2 or 3, clamped there);
         * ignored by the other backends. 0 or absent = backend default. */
        if ((val = zend_hash_str_find(options_ht, "frame_count", sizeof("frame_count") - 1)) != NULL) {
            ctx->config.frame_count = (int)zval_get_long(val);
        }
        if ((val = zend_hash_str_find(options_ht, "debug", sizeof("debug") - 1)) != NULL) {
            ctx->config.debug = (int)zval_get_long(val);
        }
        if ((val = zend_hash_str_find(options_ht, "headless", sizeof("headless") - 1)) != NULL) {
            ctx->config.headless = zend_is_true(val);
        }
    }

    /* Initialize backend */
    if (ctx->backend->init && ctx->backend->init(&ctx->config) != 0) {
        php_error_docref(NULL, E_WARNING, "Failed to initialize backend \"%s\"", ctx->backend->name);
        zval_ptr_dtor(&obj);
        RETURN_FALSE;
    }

#ifdef HAVE_GLFW
    /* Create GLFW window (unless null/headless backend) */
    if (strcmp(ctx->backend->name, "null") != 0) {
        ctx->window = vio_window_create(&ctx->config, ctx->backend->name);
        if (!ctx->window) {
            if (ctx->backend->shutdown) {
                ctx->backend->shutdown();
            }
            zval_ptr_dtor(&obj);
            RETURN_FALSE;
        }

        /* Install input callbacks */
        vio_input_install_callbacks(ctx->window, &ctx->input);

        /* OpenGL: load GL functions and compile default shaders */
        if (strcmp(ctx->backend->name, "opengl") == 0) {
            if (vio_opengl_setup_context() != 0) {
                vio_window_destroy(ctx->window);
                ctx->window = NULL;
                if (ctx->backend->shutdown) {
                    ctx->backend->shutdown();
                }
                zval_ptr_dtor(&obj);
                RETURN_FALSE;
            }

            /* Headless: ask the backend to set up its offscreen target. For
             * OpenGL that means an FBO with RGBA8 + depth/stencil renderbuffers;
             * other backends (D3D/Vulkan/Metal) keep the swapchain backbuffer
             * offscreen by themselves and leave setup_headless NULL. */
            if (ctx->config.headless && ctx->backend->setup_headless) {
                ctx->headless_fbo = ctx->backend->setup_headless(
                    ctx->config.width, ctx->config.height);
                if (!ctx->headless_fbo) {
                    php_error_docref(NULL, E_WARNING, "Headless FBO is not complete");
                    vio_window_destroy(ctx->window);
                    ctx->window = NULL;
                    zval_ptr_dtor(&obj);
                    RETURN_FALSE;
                }
            }
        }

#ifdef HAVE_VULKAN
        /* Vulkan: create instance, device, swapchain, etc. */
        if (strcmp(ctx->backend->name, "vulkan") == 0) {
            if (vio_vulkan_setup_context(ctx->window, &ctx->config) != 0) {
                vio_window_destroy(ctx->window);
                ctx->window = NULL;
                if (ctx->backend->shutdown) {
                    ctx->backend->shutdown();
                }
                zval_ptr_dtor(&obj);
                RETURN_FALSE;
            }
        }
#endif

#ifdef HAVE_METAL
        /* Metal: create device, command queue, CAMetalLayer */
        if (strcmp(ctx->backend->name, "metal") == 0) {
            if (vio_metal_setup_context(ctx->window, &ctx->config) != 0) {
                vio_window_destroy(ctx->window);
                ctx->window = NULL;
                if (ctx->backend->shutdown) {
                    ctx->backend->shutdown();
                }
                zval_ptr_dtor(&obj);
                RETURN_FALSE;
            }
        }
#endif

#ifdef HAVE_D3D11
        /* D3D11: set GLFW window handle and create swapchain */
        if (strcmp(ctx->backend->name, "d3d11") == 0) {
            if (vio_d3d11_setup_context(ctx->window, &ctx->config) != 0) {
                vio_window_destroy(ctx->window);
                ctx->window = NULL;
                if (ctx->backend->shutdown) {
                    ctx->backend->shutdown();
                }
                zval_ptr_dtor(&obj);
                RETURN_FALSE;
            }
        }
#endif

#ifdef HAVE_D3D12
        /* D3D12: set GLFW window handle and create swapchain */
        if (strcmp(ctx->backend->name, "d3d12") == 0) {
            if (vio_d3d12_setup_context(ctx->window, &ctx->config) != 0) {
                vio_window_destroy(ctx->window);
                ctx->window = NULL;
                if (ctx->backend->shutdown) {
                    ctx->backend->shutdown();
                }
                zval_ptr_dtor(&obj);
                RETURN_FALSE;
            }
        }
#endif
    }
#endif

#ifdef HAVE_IOS
    /* iOS path: there is no GLFW window. The iOS backend creates a
     * VioRenderView (UIView with CAMetalLayer) on the hosting Xcode
     * wrapper's UIWindow, then delegates Metal init to the GLFW-agnostic
     * vio_metal_setup_context_native(). UITouch events are routed into
     * ctx->input by the same view. The null/headless backends do not
     * need a render surface. */
    if (strcmp(ctx->backend->name, "null") != 0) {
        if (vio_ios_setup_context(ctx->config.width, ctx->config.height,
                                  &ctx->config, &ctx->input) != 0) {
            if (ctx->backend->shutdown) {
                ctx->backend->shutdown();
            }
            zval_ptr_dtor(&obj);
            RETURN_FALSE;
        }
    }
#endif

    /* Initialize 2D rendering system */
    vio_2d_init(&ctx->state_2d, ctx->config.width, ctx->config.height);

#ifdef HAVE_METAL
    /* Initialize Metal 2D pipeline (shaders, pipeline states) */
    if (strcmp(ctx->backend->name, "metal") == 0) {
        vio_metal_2d_init(ctx->config.width, ctx->config.height);
    }
#endif

    ctx->initialized = 1;
    RETURN_COPY_VALUE(&obj);
}

ZEND_FUNCTION(vio_destroy)
{
    zval *ctx_zval;

    ZEND_PARSE_PARAMETERS_START(1, 1)
        Z_PARAM_OBJECT_OF_CLASS(ctx_zval, vio_context_ce)
    ZEND_PARSE_PARAMETERS_END();

    vio_context_object *ctx = Z_VIO_CONTEXT_P(ctx_zval);

    if (ctx->initialized && ctx->backend) {
        if (ctx->surface && ctx->backend->destroy_surface) {
            ctx->backend->destroy_surface(ctx->surface);
            ctx->surface = NULL;
        }
        /* Cleanup headless FBO before tearing down the backend context. */
        if (ctx->headless_fbo && ctx->backend->teardown_headless) {
            ctx->backend->teardown_headless(ctx->headless_fbo);
            ctx->headless_fbo = 0;
        }
        /* Tear down the 2D renderer's backend resources BEFORE the backend
         * device is destroyed. The Vulkan 2D state (pipelines, buffer, layouts,
         * shader modules) is created against vio_vk.device; vulkan_shutdown()
         * destroys the device and zeroes vio_vk, so destroying these afterward
         * would dereference a NULL device (crash) and leave child objects alive
         * at vkDestroyDevice (validation error). vio_2d_shutdown is idempotent,
         * so the free handler's later call is a no-op. */
        vio_2d_shutdown(&ctx->state_2d);
        if (ctx->backend->shutdown) {
            ctx->backend->shutdown();
        }
#ifdef HAVE_GLFW
        if (ctx->window) {
            vio_window_destroy(ctx->window);
            ctx->window = NULL;
        }
#endif
#ifdef HAVE_IOS
        /* Tear down the iOS render view; the Metal context is shut down
         * via ctx->backend->shutdown above. */
        vio_ios_shutdown_context();
#endif
        ctx->initialized = 0;
    }
}

ZEND_FUNCTION(vio_should_close)
{
    zval *ctx_zval;

    ZEND_PARSE_PARAMETERS_START(1, 1)
        Z_PARAM_OBJECT_OF_CLASS(ctx_zval, vio_context_ce)
    ZEND_PARSE_PARAMETERS_END();

    vio_context_object *ctx = Z_VIO_CONTEXT_P(ctx_zval);

    if (ctx->should_close) {
        RETURN_TRUE;
    }

#ifdef HAVE_GLFW
    if (ctx->window && vio_window_should_close(ctx->window)) {
        ctx->should_close = 1;
        RETURN_TRUE;
    }
#endif

    RETURN_FALSE;
}

ZEND_FUNCTION(vio_close)
{
    zval *ctx_zval;

    ZEND_PARSE_PARAMETERS_START(1, 1)
        Z_PARAM_OBJECT_OF_CLASS(ctx_zval, vio_context_ce)
    ZEND_PARSE_PARAMETERS_END();

    vio_context_object *ctx = Z_VIO_CONTEXT_P(ctx_zval);
    ctx->should_close = 1;

#ifdef HAVE_GLFW
    if (ctx->window) {
        vio_window_set_should_close(ctx->window, 1);
    }
#endif
}

ZEND_FUNCTION(vio_poll_events)
{
    zval *ctx_zval;

    ZEND_PARSE_PARAMETERS_START(1, 1)
        Z_PARAM_OBJECT_OF_CLASS(ctx_zval, vio_context_ce)
    ZEND_PARSE_PARAMETERS_END();

#ifdef HAVE_GLFW
    vio_window_poll_events();
#endif
#ifdef HAVE_IOS
    /* No OS event pump on iOS (UIKit drives that). Drain the soft-keyboard
     * codepoints queued by the UIKeyInput view on the main thread, emitting
     * them on this (render) thread through the normal char path - mirrors the
     * GLFW char callback firing during glfwPollEvents. */
    {
        vio_context_object *ctx = Z_VIO_CONTEXT_P(ctx_zval);
        vio_input_drain_ime(&ctx->input);
    }
#endif
}

#ifdef HAVE_D3D12
/* Defined further down (next to vio_bind_render_target); forward-declared here
 * so vio_begin() can flush a render-target bind that was requested before the
 * frame's command list was open. */
static void d3d12_record_bind_render_target(vio_render_target_object *rt);
#endif

#ifdef HAVE_D3D11
/* Defined further down (next to vio_bind_render_target); forward-declared here
 * so vio_begin() can re-apply an offscreen render-target bind that was
 * requested before the frame began (and would otherwise be clobbered by
 * d3d11_begin_frame()'s current_rtv = rtv reset). */
static void d3d11_apply_render_target_bind(vio_render_target_object *rt);
#endif

ZEND_FUNCTION(vio_begin)
{
    zval *ctx_zval;

    ZEND_PARSE_PARAMETERS_START(1, 1)
        Z_PARAM_OBJECT_OF_CLASS(ctx_zval, vio_context_ce)
    ZEND_PARSE_PARAMETERS_END();

    vio_context_object *ctx = Z_VIO_CONTEXT_P(ctx_zval);

    if (!ctx->initialized) {
        php_error_docref(NULL, E_WARNING, "Context is not initialized");
        return;
    }

    if (ctx->in_frame) {
        php_error_docref(NULL, E_WARNING, "Already in a frame (vio_begin called twice without vio_end)");
        return;
    }

    vio_input_update(&ctx->input);

#ifdef HAVE_GLFW
    /* Sync 2D projection and viewport to current window size.
     * Projection (state_2d.width/height) is in LOGICAL coords: framebuffer/scale.
     * Viewport (state_2d.fb_width/height) is in PHYSICAL pixels.
     * This decouples layout from monitor DPI: a 1280x720 game renders sharp
     * at native 4K on a 300%-scale monitor without changing layout constants. */
    if (ctx->window && vio_gl.initialized) {
        int fb_w, fb_h;
        float sx = 1.0f, sy = 1.0f;
        glfwGetFramebufferSize(ctx->window, &fb_w, &fb_h);
        glfwGetWindowContentScale(ctx->window, &sx, &sy);
        if (sx <= 0.0f) sx = 1.0f;
        if (sy <= 0.0f) sy = 1.0f;
        int logical_w = (int)((float)fb_w / sx + 0.5f);
        int logical_h = (int)((float)fb_h / sy + 0.5f);
        if (logical_w > 0 && logical_h > 0 &&
            (logical_w != ctx->state_2d.width || logical_h != ctx->state_2d.height)) {
            vio_2d_set_size(&ctx->state_2d, logical_w, logical_h);
        }
        if (ctx->backend->set_viewport) {
            ctx->backend->set_viewport(0, 0, fb_w, fb_h);
        }
        ctx->state_2d.fb_width  = fb_w;
        ctx->state_2d.fb_height = fb_h;
    }
#endif

#ifdef HAVE_METAL
    if (strcmp(ctx->backend->name, "metal") == 0) {
        int fb_w = 0, fb_h = 0;
        float sx = 1.0f, sy = 1.0f;
        int have_size = 0;
#ifdef HAVE_GLFW
        if (ctx->window) {
            if (ctx->config.headless) {
                /* Headless renders into a 1:1 offscreen texture (sized from the
                 * logical window size, see vio_metal_setup_context). Use that
                 * same logical size here so state_2d.width == fb_width and the
                 * viewport/scissor scale stays 1 — otherwise the Retina
                 * framebuffer (2x) would scale scissors to 2x screen positions
                 * against a 1x target, clipping content at double coordinates. */
                glfwGetWindowSize((GLFWwindow *)ctx->window, &fb_w, &fb_h);
                sx = sy = 1.0f;
            } else {
                glfwGetFramebufferSize((GLFWwindow *)ctx->window, &fb_w, &fb_h);
                glfwGetWindowContentScale((GLFWwindow *)ctx->window, &sx, &sy);
            }
            have_size = 1;
        }
#endif
#ifdef HAVE_IOS
        /* iOS: derive the LOGICAL window size (physical framebuffer / content
         * scale), exactly like the GLFW retina path. The game lays out in
         * logical points and queries vio_window_size for letterboxing; the 2D
         * design space (state_2d.width/height) must match that logical size so
         * the game's screen-space scissor/transform line up, while the
         * viewport renders at full physical resolution for sharpness. */
        vio_ios_get_framebuffer_size(&fb_w, &fb_h);
        if (fb_w > 0 && fb_h > 0) {
            sx = sy = vio_ios_get_content_scale();
            have_size = 1;
        }
#endif
        if (have_size) {
            if (sx <= 0.0f) sx = 1.0f;
            if (sy <= 0.0f) sy = 1.0f;
            int logical_w = (int)((float)fb_w / sx + 0.5f);
            int logical_h = (int)((float)fb_h / sy + 0.5f);
            if (logical_w > 0 && logical_h > 0 &&
                (logical_w != ctx->state_2d.width || logical_h != ctx->state_2d.height)) {
                vio_2d_set_size(&ctx->state_2d, logical_w, logical_h);
            }
            ctx->state_2d.fb_width  = fb_w;
            ctx->state_2d.fb_height = fb_h;
        }
    }
#endif

#if (defined(HAVE_D3D11) || defined(HAVE_D3D12)) && defined(HAVE_GLFW)
    if (ctx->window && (strcmp(ctx->backend->name, "d3d11") == 0
                     || strcmp(ctx->backend->name, "d3d12") == 0)) {
        int fb_w, fb_h;
        float sx = 1.0f, sy = 1.0f;
        glfwGetFramebufferSize(ctx->window, &fb_w, &fb_h);
        glfwGetWindowContentScale(ctx->window, &sx, &sy);
        if (sx <= 0.0f) sx = 1.0f;
        if (sy <= 0.0f) sy = 1.0f;
        int logical_w = (int)((float)fb_w / sx + 0.5f);
        int logical_h = (int)((float)fb_h / sy + 0.5f);
        /* Resize swapchain buffers when framebuffer size changed.
         * The backend resize functions are idempotent (early-return on same size). */
        if (fb_w > 0 && fb_h > 0 && ctx->backend->resize) {
            ctx->backend->resize(fb_w, fb_h);
        }
        if (logical_w > 0 && logical_h > 0 &&
            (logical_w != ctx->state_2d.width || logical_h != ctx->state_2d.height)) {
            vio_2d_set_size(&ctx->state_2d, logical_w, logical_h);
        }
        ctx->state_2d.fb_width  = fb_w;
        ctx->state_2d.fb_height = fb_h;
    }
#endif

    vio_2d_begin(&ctx->state_2d);

    /* Bind headless FBO before begin_frame so clear/draw go to offscreen
     * target. The unbind_render_target slot does exactly "bind default FBO
     * and set viewport"; reusing it keeps the dispatch consistent. */
    if (ctx->headless_fbo && ctx->backend->unbind_render_target) {
        ctx->backend->unbind_render_target(ctx->headless_fbo,
            ctx->config.width, ctx->config.height);
    }

    if (ctx->backend->begin_frame) {
        ctx->backend->begin_frame();
    }

#ifdef HAVE_D3D12
    /* Apply a render-target bind requested before vio_begin(): the command
     * list is now open (begin_frame() reset it), so the deferred bind can
     * finally be recorded. Without this, a pre-begin vio_bind_render_target on
     * D3D12 is dropped and draws hit the swapchain instead of the offscreen
     * target (the warm-render "bind then begin" order). */
    if (vio_d3d12.initialized && vio_d3d12.pending_bound_rt
            && strcmp(ctx->backend->name, "d3d12") == 0) {
        vio_render_target_object *prt =
            (vio_render_target_object *)vio_d3d12.pending_bound_rt;
        vio_d3d12.pending_bound_rt = NULL;
        if (prt->valid) {
            d3d12_record_bind_render_target(prt);
        }
    }
#endif

#ifdef HAVE_D3D11
    /* Re-apply a render-target bind requested before vio_begin() (the
     * warm-render "bind then begin" order). D3D11 is immediate-mode, so the
     * bind was NOT recorded into any command list — instead it was deferred
     * because d3d11_begin_frame() (called just above) unconditionally resets
     * current_rtv = rtv and re-binds the backbuffer, which would clobber a
     * pre-begin offscreen bind. Applying it HERE, after begin_frame(), makes
     * the offscreen redirect survive so the frame's draws hit the offscreen
     * target and d3d11_present() skips Present (current_rtv != rtv) — no
     * visible warm-render flash.
     *
     * STRICT NO-OP on a normal frame: pending_bound_rt is NULL unless an
     * out-of-frame vio_bind_render_target set it, so the guard short-circuits
     * and the immediate-context state is identical to pre-fix. The
     * vio_d3d11.initialized + backend-name guard ensures this never touches a
     * non-D3D11 context. */
    if (strcmp(ctx->backend->name, "d3d11") == 0 && vio_d3d11.initialized
            && vio_d3d11.pending_bound_rt) {
        vio_render_target_object *prt =
            (vio_render_target_object *)vio_d3d11.pending_bound_rt;
        vio_d3d11.pending_bound_rt = NULL;
        if (prt->valid) {
            d3d11_apply_render_target_bind(prt);
        }
    }
#endif

#ifdef HAVE_VULKAN
    /* Apply a render-target bind requested before vio_begin() (the warm-render
     * "bind then begin" order). begin_frame() above opened the swapchain pass on
     * this frame's command buffer, so the deferred switch can now be recorded.
     *
     * STRICT NO-OP on a normal frame: this block does nothing unless
     * pending_bound_rt is non-NULL (set only by an out-of-frame
     * vio_bind_render_target). On a plain shapes/text frame pending_bound_rt is
     * NULL, the guard short-circuits, and the command stream is byte-identical to
     * pre-Phase-3. The vio_vk.initialized + backend-name guard further ensures
     * this never touches a non-Vulkan context. */
    if (vio_vk.initialized && strcmp(ctx->backend->name, "vulkan") == 0
            && vio_vk.pending_bound_rt) {
        vio_render_target_object *prt =
            (vio_render_target_object *)vio_vk.pending_bound_rt;
        vio_vk.pending_bound_rt = NULL;
        /* m1 — if the deferred RT is invalid or not a Vulkan target, the
         * offscreen pass is never begun, yet vulkan_begin_frame already latched
         * frame_is_offscreen=1 (it keys off pending_bound_rt being non-NULL at
         * begin). The result is a silently-dropped frame: it acquires nothing,
         * draws nothing, presents nothing (end_frame's "if (current_bound_rt)"
         * no-ops the pass-end and the zero-semaphore submit is harmless; present
         * skips). That is SAFE but invisible, so warn — a bound-then-invalidated
         * RT eating a frame is almost always a caller bug. */
        if (!(prt->valid && prt->backend_type == VIO_RT_BACKEND_VULKAN)) {
            php_error_docref(NULL, E_WARNING,
                "vio_begin: pending Vulkan render target is invalid or not a Vulkan target; "
                "this frame renders nothing and is not presented");
        }
        if (prt->valid && prt->backend_type == VIO_RT_BACKEND_VULKAN && vio_vk.in_frame) {
            /* Phase 4: vulkan_begin_frame set vio_vk.frame_is_offscreen=1 for this
             * frame (pending_bound_rt was non-NULL at begin), and crucially did
             * NOT begin the swapchain render pass and did NOT acquire a swapchain
             * image. So begin the OFFSCREEN pass DIRECTLY (no preceding
             * vkCmdEndRenderPass) — using the mid-frame switch
             * vulkan_record_bind_render_target() here would call vkCmdEndRenderPass
             * on a frame with no open pass (a validation error).
             *
             * The frame_is_offscreen guard keeps Phase 3's mid-frame switch path
             * (vulkan_record_bind_render_target) for the hypothetical case where a
             * pending bind coexists with an already-open swapchain pass; with the
             * current flow that path is not reached from here (an in-frame bind
             * records immediately in vio_bind_render_target, never via pending). */
            if (vio_vk.frame_is_offscreen) {
                vulkan_begin_offscreen_render_pass(prt);
            } else {
                vulkan_record_bind_render_target(prt);
            }
        }
    }
#endif

    ctx->in_frame = 1;
}

ZEND_FUNCTION(vio_end)
{
    zval *ctx_zval;

    ZEND_PARSE_PARAMETERS_START(1, 1)
        Z_PARAM_OBJECT_OF_CLASS(ctx_zval, vio_context_ce)
    ZEND_PARSE_PARAMETERS_END();

    vio_context_object *ctx = Z_VIO_CONTEXT_P(ctx_zval);

    if (!ctx->initialized) {
        php_error_docref(NULL, E_WARNING, "Context is not initialized");
        return;
    }

    if (!ctx->in_frame) {
        php_error_docref(NULL, E_WARNING, "Not in a frame (vio_end called without vio_begin)");
        return;
    }

    if (ctx->backend->end_frame) {
        ctx->backend->end_frame();
    }

    if (ctx->backend->present) {
        ctx->backend->present();
    }

#ifdef HAVE_GLFW
    /* Only OpenGL uses GLFW swap buffers; Vulkan presents via vkQueuePresentKHR */
    if (ctx->window && strcmp(ctx->backend->name, "opengl") == 0) {
        vio_window_swap_buffers(ctx->window);
    }
#endif

    ctx->in_frame = 0;
}

ZEND_FUNCTION(vio_gpu_flush)
{
    zval *ctx_zval;

    ZEND_PARSE_PARAMETERS_START(1, 1)
        Z_PARAM_OBJECT_OF_CLASS(ctx_zval, vio_context_ce)
    ZEND_PARSE_PARAMETERS_END();

    vio_context_object *ctx = Z_VIO_CONTEXT_P(ctx_zval);

    if (!ctx->initialized) {
        php_error_docref(NULL, E_WARNING, "Context is not initialized");
        return;
    }

    if (ctx->backend->gpu_flush) {
        ctx->backend->gpu_flush();
    }
}

ZEND_FUNCTION(vio_clear)
{
    zval *ctx_zval;
    double r = 0.1, g = 0.1, b = 0.1, a = 1.0;

    ZEND_PARSE_PARAMETERS_START(1, 5)
        Z_PARAM_OBJECT_OF_CLASS(ctx_zval, vio_context_ce)
        Z_PARAM_OPTIONAL
        Z_PARAM_DOUBLE(r)
        Z_PARAM_DOUBLE(g)
        Z_PARAM_DOUBLE(b)
        Z_PARAM_DOUBLE(a)
    ZEND_PARSE_PARAMETERS_END();

    vio_context_object *ctx = Z_VIO_CONTEXT_P(ctx_zval);

    if (ctx->backend && ctx->backend->clear) {
        ctx->backend->clear((float)r, (float)g, (float)b, (float)a);
    }
}

ZEND_FUNCTION(vio_key_pressed)
{
    zval *ctx_zval;
    zend_long key;

    ZEND_PARSE_PARAMETERS_START(2, 2)
        Z_PARAM_OBJECT_OF_CLASS(ctx_zval, vio_context_ce)
        Z_PARAM_LONG(key)
    ZEND_PARSE_PARAMETERS_END();

    vio_context_object *ctx = Z_VIO_CONTEXT_P(ctx_zval);
    if (key >= 0 && key <= VIO_KEY_LAST) {
        RETURN_BOOL(ctx->input.keys[key]);
    }
    RETURN_FALSE;
}

ZEND_FUNCTION(vio_key_just_pressed)
{
    zval *ctx_zval;
    zend_long key;

    ZEND_PARSE_PARAMETERS_START(2, 2)
        Z_PARAM_OBJECT_OF_CLASS(ctx_zval, vio_context_ce)
        Z_PARAM_LONG(key)
    ZEND_PARSE_PARAMETERS_END();

    vio_context_object *ctx = Z_VIO_CONTEXT_P(ctx_zval);
    if (key >= 0 && key <= VIO_KEY_LAST) {
        RETURN_BOOL(ctx->input.keys[key] && !ctx->input.keys_prev[key]);
    }
    RETURN_FALSE;
}

ZEND_FUNCTION(vio_key_released)
{
    zval *ctx_zval;
    zend_long key;

    ZEND_PARSE_PARAMETERS_START(2, 2)
        Z_PARAM_OBJECT_OF_CLASS(ctx_zval, vio_context_ce)
        Z_PARAM_LONG(key)
    ZEND_PARSE_PARAMETERS_END();

    vio_context_object *ctx = Z_VIO_CONTEXT_P(ctx_zval);
    if (key >= 0 && key <= VIO_KEY_LAST) {
        RETURN_BOOL(!ctx->input.keys[key] && ctx->input.keys_prev[key]);
    }
    RETURN_FALSE;
}

/* GLFW's cursor-position contract is platform-dependent:
 *   Windows (DPI-aware): glfwGetCursorPos returns physical pixels, so we
 *     divide by content scale to recover logical coords matching the layout.
 *   macOS: glfwGetCursorPos returns points (logical coords) regardless of
 *     Retina scale; dividing again would halve mouse positions on 2x displays.
 *   Linux (X11/Wayland): GLFW reports the same units as the window size, so
 *     no scaling is needed for the logical layout.
 * Only Windows needs the division. */
static double vio_input_logical_scale_x(vio_context_object *ctx)
{
#if defined(HAVE_GLFW) && defined(_WIN32)
    if (ctx && ctx->window) {
        float sx = 1.0f, sy = 1.0f;
        glfwGetWindowContentScale(ctx->window, &sx, &sy);
        if (sx > 0.0f) return (double)sx;
    }
#endif
    (void)ctx;
    return 1.0;
}

static double vio_input_logical_scale_y(vio_context_object *ctx)
{
#if defined(HAVE_GLFW) && defined(_WIN32)
    if (ctx && ctx->window) {
        float sx = 1.0f, sy = 1.0f;
        glfwGetWindowContentScale(ctx->window, &sx, &sy);
        if (sy > 0.0f) return (double)sy;
    }
#endif
    (void)ctx;
    return 1.0;
}

ZEND_FUNCTION(vio_mouse_position)
{
    zval *ctx_zval;

    ZEND_PARSE_PARAMETERS_START(1, 1)
        Z_PARAM_OBJECT_OF_CLASS(ctx_zval, vio_context_ce)
    ZEND_PARSE_PARAMETERS_END();

    vio_context_object *ctx = Z_VIO_CONTEXT_P(ctx_zval);
    double sx = vio_input_logical_scale_x(ctx);
    double sy = vio_input_logical_scale_y(ctx);
    array_init(return_value);
    add_next_index_double(return_value, ctx->input.mouse_x / sx);
    add_next_index_double(return_value, ctx->input.mouse_y / sy);
}

ZEND_FUNCTION(vio_mouse_delta)
{
    zval *ctx_zval;

    ZEND_PARSE_PARAMETERS_START(1, 1)
        Z_PARAM_OBJECT_OF_CLASS(ctx_zval, vio_context_ce)
    ZEND_PARSE_PARAMETERS_END();

    vio_context_object *ctx = Z_VIO_CONTEXT_P(ctx_zval);
    double sx = vio_input_logical_scale_x(ctx);
    double sy = vio_input_logical_scale_y(ctx);
    array_init(return_value);
    add_next_index_double(return_value, (ctx->input.mouse_x - ctx->input.mouse_prev_x) / sx);
    add_next_index_double(return_value, (ctx->input.mouse_y - ctx->input.mouse_prev_y) / sy);
}

ZEND_FUNCTION(vio_mouse_button)
{
    zval *ctx_zval;
    zend_long button;

    ZEND_PARSE_PARAMETERS_START(2, 2)
        Z_PARAM_OBJECT_OF_CLASS(ctx_zval, vio_context_ce)
        Z_PARAM_LONG(button)
    ZEND_PARSE_PARAMETERS_END();

    vio_context_object *ctx = Z_VIO_CONTEXT_P(ctx_zval);
    if (button >= 0 && button <= VIO_MOUSE_LAST) {
        RETURN_BOOL(ctx->input.mouse_buttons[button]);
    }
    RETURN_FALSE;
}

ZEND_FUNCTION(vio_mouse_scroll)
{
    zval *ctx_zval;

    ZEND_PARSE_PARAMETERS_START(1, 1)
        Z_PARAM_OBJECT_OF_CLASS(ctx_zval, vio_context_ce)
    ZEND_PARSE_PARAMETERS_END();

    vio_context_object *ctx = Z_VIO_CONTEXT_P(ctx_zval);
    array_init(return_value);
    add_next_index_double(return_value, ctx->input.scroll_x);
    add_next_index_double(return_value, ctx->input.scroll_y);
}

ZEND_FUNCTION(vio_touch_count)
{
    zval *ctx_zval;

    ZEND_PARSE_PARAMETERS_START(1, 1)
        Z_PARAM_OBJECT_OF_CLASS(ctx_zval, vio_context_ce)
    ZEND_PARSE_PARAMETERS_END();

    vio_context_object *ctx = Z_VIO_CONTEXT_P(ctx_zval);
    RETURN_LONG((zend_long)ctx->input.touch_count);
}

/*
 * vio_touch_get(VioContext $ctx, int $idx): ?array
 *
 * Returns the touch at the *active* index $idx, where active touches are
 * compacted to indices 0..touch_count-1 (inactive slots are skipped).
 * Returns null when $idx is out of range.
 *
 * Touch positions are reported in logical (post-DPI) coordinates, matching
 * the existing vio_mouse_position contract.
 *
 * Shape: ['id' => int, 'x' => float, 'y' => float, 'phase' => int,
 *         'delta_x' => float, 'delta_y' => float]
 */
ZEND_FUNCTION(vio_touch_get)
{
    zval *ctx_zval;
    zend_long idx;

    ZEND_PARSE_PARAMETERS_START(2, 2)
        Z_PARAM_OBJECT_OF_CLASS(ctx_zval, vio_context_ce)
        Z_PARAM_LONG(idx)
    ZEND_PARSE_PARAMETERS_END();

    if (idx < 0) RETURN_NULL();

    vio_context_object *ctx = Z_VIO_CONTEXT_P(ctx_zval);

    /* Touch coordinates are stored in logical window points (the iOS view
     * delivers points; GLFW would deliver logical too), the same space as
     * vio_mouse_position. Divide by the logical scale for parity with the
     * mouse API (1.0 on iOS, content-scale on Win V2-DPI). */
    double sx = vio_input_logical_scale_x(ctx);
    double sy = vio_input_logical_scale_y(ctx);

    /* Walk active slots, count up to idx */
    int seen = -1;
    for (int i = 0; i < VIO_MAX_TOUCHES; i++) {
        vio_touch *t = &ctx->input.touches[i];
        if (t->id == 0) continue;
        seen++;
        if (seen == idx) {
            array_init(return_value);
            add_assoc_long(return_value,   "id",      (zend_long)t->id);
            add_assoc_double(return_value, "x",       t->x / sx);
            add_assoc_double(return_value, "y",       t->y / sy);
            add_assoc_long(return_value,   "phase",   (zend_long)t->phase);
            add_assoc_double(return_value, "delta_x", (t->x - t->prev_x) / sx);
            add_assoc_double(return_value, "delta_y", (t->y - t->prev_y) / sy);
            return;
        }
    }

    RETURN_NULL();
}

/*
 * vio_touch_inject(VioContext $ctx, int $id, int $phase, float $x, float $y): bool
 *
 * Synthetic touch injection for testing and headless replays. Backends
 * push touches through C calls; this PHP entry-point lets test runners
 * simulate the same path without a window.
 *
 * Coordinates are in framebuffer pixels (the same space that the GLFW
 * cursor_pos callback uses). Tests in logical points should multiply by
 * the content scale themselves.
 *
 * Returns true on success, false when the phase is invalid or the touch
 * array is full (BEGAN with no free slot).
 */
ZEND_FUNCTION(vio_touch_inject)
{
    zval *ctx_zval;
    zend_long id, phase;
    double x = 0.0, y = 0.0;

    ZEND_PARSE_PARAMETERS_START(3, 5)
        Z_PARAM_OBJECT_OF_CLASS(ctx_zval, vio_context_ce)
        Z_PARAM_LONG(id)
        Z_PARAM_LONG(phase)
        Z_PARAM_OPTIONAL
        Z_PARAM_DOUBLE(x)
        Z_PARAM_DOUBLE(y)
    ZEND_PARSE_PARAMETERS_END();

    vio_context_object *ctx = Z_VIO_CONTEXT_P(ctx_zval);
    unsigned long long uid = (unsigned long long)id;

    switch (phase) {
        case VIO_TOUCH_BEGAN:
            RETURN_BOOL(vio_input_touch_began(&ctx->input, uid, x, y) >= 0);
        case VIO_TOUCH_MOVED:
            vio_input_touch_moved(&ctx->input, uid, x, y);
            RETURN_TRUE;
        case VIO_TOUCH_ENDED:
            vio_input_touch_ended(&ctx->input, uid);
            RETURN_TRUE;
        case VIO_TOUCH_CANCELLED:
            vio_input_touch_cancelled(&ctx->input, uid);
            RETURN_TRUE;
        default:
            RETURN_FALSE;
    }
}

ZEND_FUNCTION(vio_set_cursor_mode)
{
    zval *ctx_zval;
    zend_long mode;

    ZEND_PARSE_PARAMETERS_START(2, 2)
        Z_PARAM_OBJECT_OF_CLASS(ctx_zval, vio_context_ce)
        Z_PARAM_LONG(mode)
    ZEND_PARSE_PARAMETERS_END();

    vio_context_object *ctx = Z_VIO_CONTEXT_P(ctx_zval);

#ifdef HAVE_GLFW
    if (ctx->window) {
        int glfw_mode;
        switch (mode) {
            case 1:  glfw_mode = GLFW_CURSOR_DISABLED; break;  /* VIO_CURSOR_DISABLED */
            case 2:  glfw_mode = GLFW_CURSOR_HIDDEN; break;    /* VIO_CURSOR_HIDDEN */
            default: glfw_mode = GLFW_CURSOR_NORMAL; break;    /* VIO_CURSOR_NORMAL */
        }
        glfwSetInputMode(ctx->window, GLFW_CURSOR, glfw_mode);

        /* When switching to disabled mode, enable raw mouse motion if available */
        if (mode == 1 && glfwRawMouseMotionSupported()) {
            glfwSetInputMode(ctx->window, GLFW_RAW_MOUSE_MOTION, GLFW_TRUE);
        }
    }
#endif
}

ZEND_FUNCTION(vio_on_key)
{
    zval *ctx_zval;
    zval *callback;

    ZEND_PARSE_PARAMETERS_START(2, 2)
        Z_PARAM_OBJECT_OF_CLASS(ctx_zval, vio_context_ce)
        Z_PARAM_ZVAL(callback)
    ZEND_PARSE_PARAMETERS_END();

    if (!zend_is_callable(callback, 0, NULL)) {
        php_error_docref(NULL, E_WARNING, "Expected callable for key callback");
        return;
    }

    vio_context_object *ctx = Z_VIO_CONTEXT_P(ctx_zval);

    if (ctx->input.has_key_callback) {
        zval_ptr_dtor(&ctx->input.on_key_callback);
    }
    ZVAL_COPY(&ctx->input.on_key_callback, callback);
    ctx->input.has_key_callback = 1;
}

ZEND_FUNCTION(vio_on_resize)
{
    zval *ctx_zval;
    zval *callback;

    ZEND_PARSE_PARAMETERS_START(2, 2)
        Z_PARAM_OBJECT_OF_CLASS(ctx_zval, vio_context_ce)
        Z_PARAM_ZVAL(callback)
    ZEND_PARSE_PARAMETERS_END();

    if (!zend_is_callable(callback, 0, NULL)) {
        php_error_docref(NULL, E_WARNING, "Expected callable for resize callback");
        return;
    }

    vio_context_object *ctx = Z_VIO_CONTEXT_P(ctx_zval);

    if (ctx->input.has_resize_callback) {
        zval_ptr_dtor(&ctx->input.on_resize_callback);
    }
    ZVAL_COPY(&ctx->input.on_resize_callback, callback);
    ctx->input.has_resize_callback = 1;
}

ZEND_FUNCTION(vio_on_char)
{
    zval *ctx_zval;
    zval *callback;

    ZEND_PARSE_PARAMETERS_START(2, 2)
        Z_PARAM_OBJECT_OF_CLASS(ctx_zval, vio_context_ce)
        Z_PARAM_ZVAL(callback)
    ZEND_PARSE_PARAMETERS_END();

    if (!zend_is_callable(callback, 0, NULL)) {
        php_error_docref(NULL, E_WARNING, "Expected callable for char callback");
        return;
    }

    vio_context_object *ctx = Z_VIO_CONTEXT_P(ctx_zval);

    if (ctx->input.has_char_callback) {
        zval_ptr_dtor(&ctx->input.on_char_callback);
    }
    ZVAL_COPY(&ctx->input.on_char_callback, callback);
    ctx->input.has_char_callback = 1;
}

ZEND_FUNCTION(vio_chars_typed)
{
    zval *ctx_zval;

    ZEND_PARSE_PARAMETERS_START(1, 1)
        Z_PARAM_OBJECT_OF_CLASS(ctx_zval, vio_context_ce)
    ZEND_PARSE_PARAMETERS_END();

    vio_context_object *ctx = Z_VIO_CONTEXT_P(ctx_zval);

    RETURN_STRINGL(ctx->input.char_buffer, ctx->input.char_buffer_len);
}

/*
 * vio_ime_backspaces(VioContext $ctx): int
 *
 * Number of soft-keyboard backspaces since the last call (read-and-clear).
 * Non-zero only on iOS, where the on-screen keyboard's delete key feeds them
 * in; 0 on desktop (physical Backspace flows through the key API instead).
 */
ZEND_FUNCTION(vio_ime_backspaces)
{
    zval *ctx_zval;

    ZEND_PARSE_PARAMETERS_START(1, 1)
        Z_PARAM_OBJECT_OF_CLASS(ctx_zval, vio_context_ce)
    ZEND_PARSE_PARAMETERS_END();

    vio_context_object *ctx = Z_VIO_CONTEXT_P(ctx_zval);
    RETURN_LONG((zend_long) vio_input_take_ime_backspaces(&ctx->input));
}

/*
 * vio_keyboard_show(VioContext $ctx): void
 * vio_keyboard_hide(VioContext $ctx): void
 *
 * Show / hide the on-screen keyboard. No-op on desktop (physical keyboard);
 * on iOS this toggles the render view's first-responder status. Call when a
 * text field gains / loses focus.
 */
ZEND_FUNCTION(vio_keyboard_show)
{
    zval *ctx_zval;

    ZEND_PARSE_PARAMETERS_START(1, 1)
        Z_PARAM_OBJECT_OF_CLASS(ctx_zval, vio_context_ce)
    ZEND_PARSE_PARAMETERS_END();

    (void) ctx_zval;
#ifdef HAVE_IOS
    vio_ios_keyboard_show();
#endif
}

ZEND_FUNCTION(vio_keyboard_hide)
{
    zval *ctx_zval;

    ZEND_PARSE_PARAMETERS_START(1, 1)
        Z_PARAM_OBJECT_OF_CLASS(ctx_zval, vio_context_ce)
    ZEND_PARSE_PARAMETERS_END();

    (void) ctx_zval;
#ifdef HAVE_IOS
    vio_ios_keyboard_hide();
#endif
}

ZEND_FUNCTION(vio_toggle_fullscreen)
{
    zval *ctx_zval;

    ZEND_PARSE_PARAMETERS_START(1, 1)
        Z_PARAM_OBJECT_OF_CLASS(ctx_zval, vio_context_ce)
    ZEND_PARSE_PARAMETERS_END();

#ifdef HAVE_GLFW
    vio_context_object *ctx = Z_VIO_CONTEXT_P(ctx_zval);
    if (!ctx->window) return;

    GLFWmonitor *monitor = glfwGetWindowMonitor(ctx->window);
    if (monitor) {
        /* Currently fullscreen -> go windowed */
        glfwSetWindowMonitor(ctx->window, NULL,
            100, 100, ctx->config.width, ctx->config.height, GLFW_DONT_CARE);
    } else {
        /* Currently windowed -> go fullscreen */
        monitor = glfwGetPrimaryMonitor();
        const GLFWvidmode *mode = glfwGetVideoMode(monitor);
        glfwSetWindowMonitor(ctx->window, monitor,
            0, 0, mode->width, mode->height, mode->refreshRate);
    }
#endif
}

ZEND_FUNCTION(vio_set_title)
{
    zval *ctx_zval;
    zend_string *title;

    ZEND_PARSE_PARAMETERS_START(2, 2)
        Z_PARAM_OBJECT_OF_CLASS(ctx_zval, vio_context_ce)
        Z_PARAM_STR(title)
    ZEND_PARSE_PARAMETERS_END();

#ifdef HAVE_GLFW
    vio_context_object *ctx = Z_VIO_CONTEXT_P(ctx_zval);
    if (!ctx->window) return;

    glfwSetWindowTitle(ctx->window, ZSTR_VAL(title));
#endif
}

ZEND_FUNCTION(vio_set_borderless)
{
    zval *ctx_zval;

    ZEND_PARSE_PARAMETERS_START(1, 1)
        Z_PARAM_OBJECT_OF_CLASS(ctx_zval, vio_context_ce)
    ZEND_PARSE_PARAMETERS_END();

#ifdef HAVE_GLFW
    vio_context_object *ctx = Z_VIO_CONTEXT_P(ctx_zval);
    if (!ctx->window) return;

    /* Capture the current windowed rect (only when we're actually a normal
     * window, not already fullscreen or maximized) so vio_set_windowed can
     * return to it. */
    if (glfwGetWindowMonitor(ctx->window) == NULL
        && !glfwGetWindowAttrib(ctx->window, GLFW_MAXIMIZED)) {
        glfwGetWindowPos(ctx->window, &ctx->saved_win_x, &ctx->saved_win_y);
        glfwGetWindowSize(ctx->window, &ctx->saved_win_w, &ctx->saved_win_h);
        ctx->has_saved_win_geometry = 1;
    }

    glfwSetWindowAttrib(ctx->window, GLFW_DECORATED, GLFW_FALSE);
    glfwMaximizeWindow(ctx->window);
#endif
}

ZEND_FUNCTION(vio_set_windowed)
{
    zval *ctx_zval;

    ZEND_PARSE_PARAMETERS_START(1, 1)
        Z_PARAM_OBJECT_OF_CLASS(ctx_zval, vio_context_ce)
    ZEND_PARSE_PARAMETERS_END();

#ifdef HAVE_GLFW
    vio_context_object *ctx = Z_VIO_CONTEXT_P(ctx_zval);
    if (!ctx->window) return;

    int rx = ctx->has_saved_win_geometry ? ctx->saved_win_x : 100;
    int ry = ctx->has_saved_win_geometry ? ctx->saved_win_y : 100;
    int rw = ctx->has_saved_win_geometry ? ctx->saved_win_w
           : (ctx->config.width  > 0 ? ctx->config.width  : 1280);
    int rh = ctx->has_saved_win_geometry ? ctx->saved_win_h
           : (ctx->config.height > 0 ? ctx->config.height : 720);

    if (glfwGetWindowMonitor(ctx->window) != NULL) {
        /* Real (monitor) fullscreen — glfwRestoreWindow does NOT exit this.
         * Detach the monitor to return to a windowed rect. Without this the
         * window stays fullscreen and a follow-up glfwSetWindowSize merely
         * switches the fullscreen video mode (the "back to windowed doesn't
         * work" bug). */
        glfwSetWindowMonitor(ctx->window, NULL, rx, ry, rw, rh, GLFW_DONT_CARE);
    } else {
        /* Borderless / maximized — un-maximize, then restore the saved rect. */
        glfwRestoreWindow(ctx->window);
        if (ctx->has_saved_win_geometry) {
            glfwSetWindowSize(ctx->window, rw, rh);
            glfwSetWindowPos(ctx->window, rx, ry);
        }
    }
    glfwSetWindowAttrib(ctx->window, GLFW_DECORATED, GLFW_TRUE);
#endif
}

ZEND_FUNCTION(vio_set_fullscreen)
{
    zval *ctx_zval;
    zend_long monitor_index = -1; /* -1 = primary monitor (default) */
    zend_long req_w = 0;          /* 0 = use the monitor's native mode */
    zend_long req_h = 0;
    zend_long req_refresh = 0;    /* 0 = let the video mode decide */

    ZEND_PARSE_PARAMETERS_START(1, 5)
        Z_PARAM_OBJECT_OF_CLASS(ctx_zval, vio_context_ce)
        Z_PARAM_OPTIONAL
        Z_PARAM_LONG(monitor_index)
        Z_PARAM_LONG(req_w)
        Z_PARAM_LONG(req_h)
        Z_PARAM_LONG(req_refresh)
    ZEND_PARSE_PARAMETERS_END();

#ifdef HAVE_GLFW
    vio_context_object *ctx = Z_VIO_CONTEXT_P(ctx_zval);
    if (!ctx->window) return;

    /* Capture the windowed rect before leaving it so the round-trip back to
     * windowed lands at the same pos/size. */
    if (glfwGetWindowMonitor(ctx->window) == NULL) {
        glfwGetWindowPos(ctx->window, &ctx->saved_win_x, &ctx->saved_win_y);
        glfwGetWindowSize(ctx->window, &ctx->saved_win_w, &ctx->saved_win_h);
        ctx->has_saved_win_geometry = 1;
    }

    /* Pick the requested monitor by index; fall back to primary for -1 or an
     * out-of-range index (e.g. a monitor that was unplugged since selection). */
    GLFWmonitor *monitor = NULL;
    if (monitor_index >= 0) {
        int count = 0;
        GLFWmonitor **mons = glfwGetMonitors(&count);
        if (mons && monitor_index < count) {
            monitor = mons[monitor_index];
        }
    }
    if (!monitor) {
        monitor = glfwGetPrimaryMonitor();
    }
    const GLFWvidmode *mode = glfwGetVideoMode(monitor);

    /* A caller-supplied resolution (req_w/req_h > 0) switches the display to
     * that exclusive-fullscreen video mode instead of the native one. Callers
     * are expected to pass a mode enumerated by vio_video_modes(); GLFW picks
     * the closest supported mode if it does not match exactly. Otherwise we
     * keep the native mode. Refresh falls back to the chosen mode's rate, then
     * to GLFW_DONT_CARE. */
    int out_w = (mode ? mode->width : 0);
    int out_h = (mode ? mode->height : 0);
    int out_refresh = (mode ? mode->refreshRate : GLFW_DONT_CARE);
    if (req_w > 0 && req_h > 0) {
        out_w = (int)req_w;
        out_h = (int)req_h;
        out_refresh = (req_refresh > 0) ? (int)req_refresh : GLFW_DONT_CARE;
    } else if (req_refresh > 0) {
        out_refresh = (int)req_refresh;
    }

    if (out_w > 0 && out_h > 0) {
        glfwSetWindowMonitor(ctx->window, monitor,
            0, 0, out_w, out_h, out_refresh);
    }
#endif
}

/* Report whether the window auto-minimizes when a fullscreen window loses focus.
 * vio forces this OFF at window creation so Print Screen / the snipping tool /
 * alt-tab no longer drops fullscreen players to the desktop; exposed so that
 * behaviour is testable without a visible window and a real focus change. */
ZEND_FUNCTION(vio_get_auto_iconify)
{
    zval *ctx_zval;

    ZEND_PARSE_PARAMETERS_START(1, 1)
        Z_PARAM_OBJECT_OF_CLASS(ctx_zval, vio_context_ce)
    ZEND_PARSE_PARAMETERS_END();

#ifdef HAVE_GLFW
    vio_context_object *ctx = Z_VIO_CONTEXT_P(ctx_zval);
    if (ctx->window) {
        RETURN_BOOL(glfwGetWindowAttrib(ctx->window, GLFW_AUTO_ICONIFY) != 0);
    }
#endif
    /* No GLFW window (null backend): report GLFW's default of "on". */
    RETURN_TRUE;
}

ZEND_FUNCTION(vio_window_size)
{
    zval *ctx_zval;

    ZEND_PARSE_PARAMETERS_START(1, 1)
        Z_PARAM_OBJECT_OF_CLASS(ctx_zval, vio_context_ce)
    ZEND_PARSE_PARAMETERS_END();

    vio_context_object *ctx = Z_VIO_CONTEXT_P(ctx_zval);

    array_init(return_value);
#ifdef HAVE_GLFW
    if (ctx->window) {
        /* Return the LOGICAL window size (framebuffer divided by content
         * scale). This gives a DPI-independent layout space across platforms:
         *   - macOS retina: glfwGetWindowSize is already logical, but
         *     fb/scale yields the same value (e.g. 1600/2 = 800).
         *   - Windows V2-DPI-aware: glfwGetWindowSize returns physical
         *     pixels; fb/scale recovers the logical size (e.g. 3840/3 = 1280).
         *   - DPI-unaware/100% scale: fb/scale equals window size.
         * Games can keep using fixed pixel constants for layout regardless
         * of monitor DPI, while rendering happens at native physical resolution. */
        int fb_w = 0, fb_h = 0;
        float sx = 1.0f, sy = 1.0f;
        glfwGetFramebufferSize(ctx->window, &fb_w, &fb_h);
        glfwGetWindowContentScale(ctx->window, &sx, &sy);
        if (sx <= 0.0f) sx = 1.0f;
        if (sy <= 0.0f) sy = 1.0f;
        int logical_w = (int)((float)fb_w / sx + 0.5f);
        int logical_h = (int)((float)fb_h / sy + 0.5f);
        add_next_index_long(return_value, logical_w);
        add_next_index_long(return_value, logical_h);
        return;
    }
#endif
#ifdef HAVE_IOS
    {
        int fb_w = 0, fb_h = 0;
        vio_ios_get_framebuffer_size(&fb_w, &fb_h);
        float scale = vio_ios_get_content_scale();
        if (scale <= 0.0f) scale = 1.0f;
        if (fb_w > 0 && fb_h > 0) {
            add_next_index_long(return_value, (int)((float)fb_w / scale + 0.5f));
            add_next_index_long(return_value, (int)((float)fb_h / scale + 0.5f));
            return;
        }
    }
#endif
    add_next_index_long(return_value, ctx->config.width > 0 ? ctx->config.width : 800);
    add_next_index_long(return_value, ctx->config.height > 0 ? ctx->config.height : 600);
}

#ifdef HAVE_GLFW
/* Pull in native handle accessors from GLFW. We define platform macros
 * conditionally so this compiles on every host: only the matching
 * accessor (Cocoa on macOS, Win32 on Windows, X11 on Linux) is exposed. */
#if defined(__APPLE__)
#  define GLFW_EXPOSE_NATIVE_COCOA
#elif defined(_WIN32)
#  define GLFW_EXPOSE_NATIVE_WIN32
#elif defined(__linux__)
#  define GLFW_EXPOSE_NATIVE_X11
#endif
#include <GLFW/glfw3native.h>
#endif

/*
 * vio_native_window_handle($ctx): int
 *
 * Returns the platform-native window handle as an integer:
 *   - macOS:   NSWindow* cast to uintptr_t (suitable for php-metal's
 *              Metal\CAMetalLayer::createFromWindow($handle, $device)).
 *   - Windows: HWND cast to uintptr_t.
 *   - Linux:   Window XID cast to uintptr_t.
 *
 * Returns 0 if no window exists, glfw isn't available, or the platform
 * isn't supported. Lets PHP code reuse vio's GLFW window with native
 * graphics SDKs (php-metal, php-d3d11, ...) that bypass the vio
 * rendering pipeline.
 */
ZEND_FUNCTION(vio_native_window_handle)
{
    zval *ctx_zval;

    ZEND_PARSE_PARAMETERS_START(1, 1)
        Z_PARAM_OBJECT_OF_CLASS(ctx_zval, vio_context_ce)
    ZEND_PARSE_PARAMETERS_END();

    vio_context_object *ctx = Z_VIO_CONTEXT_P(ctx_zval);
    if (!ctx->window) {
        RETURN_LONG(0);
    }

#ifdef HAVE_GLFW
#  if defined(__APPLE__)
    RETURN_LONG((zend_long)(uintptr_t)glfwGetCocoaWindow(ctx->window));
#  elif defined(_WIN32)
    RETURN_LONG((zend_long)(uintptr_t)glfwGetWin32Window(ctx->window));
#  elif defined(__linux__)
    RETURN_LONG((zend_long)(uintptr_t)glfwGetX11Window(ctx->window));
#  else
    RETURN_LONG(0);
#  endif
#else
    RETURN_LONG(0);
#endif
}

ZEND_FUNCTION(vio_framebuffer_size)
{
    zval *ctx_zval;

    ZEND_PARSE_PARAMETERS_START(1, 1)
        Z_PARAM_OBJECT_OF_CLASS(ctx_zval, vio_context_ce)
    ZEND_PARSE_PARAMETERS_END();

    vio_context_object *ctx = Z_VIO_CONTEXT_P(ctx_zval);

    array_init(return_value);
#ifdef HAVE_GLFW
    if (ctx->window) {
        int w = 0, h = 0;
        glfwGetFramebufferSize(ctx->window, &w, &h);
        add_next_index_long(return_value, w);
        add_next_index_long(return_value, h);
        return;
    }
#endif
#ifdef HAVE_IOS
    {
        int w = 0, h = 0;
        vio_ios_get_framebuffer_size(&w, &h);
        if (w > 0 && h > 0) {
            add_next_index_long(return_value, w);
            add_next_index_long(return_value, h);
            return;
        }
    }
#endif
    add_next_index_long(return_value, ctx->config.width > 0 ? ctx->config.width : 800);
    add_next_index_long(return_value, ctx->config.height > 0 ? ctx->config.height : 600);
}

ZEND_FUNCTION(vio_content_scale)
{
    zval *ctx_zval;

    ZEND_PARSE_PARAMETERS_START(1, 1)
        Z_PARAM_OBJECT_OF_CLASS(ctx_zval, vio_context_ce)
    ZEND_PARSE_PARAMETERS_END();

    vio_context_object *ctx = Z_VIO_CONTEXT_P(ctx_zval);

    array_init(return_value);
#ifdef HAVE_GLFW
    if (ctx->window) {
        float sx = 1.0f, sy = 1.0f;
        glfwGetWindowContentScale(ctx->window, &sx, &sy);
        add_next_index_double(return_value, (double)sx);
        add_next_index_double(return_value, (double)sy);
        return;
    }
#endif
    add_next_index_double(return_value, 1.0);
    add_next_index_double(return_value, 1.0);
}

ZEND_FUNCTION(vio_monitor_info)
{
    zval *ctx_zval;

    ZEND_PARSE_PARAMETERS_START(1, 1)
        Z_PARAM_OBJECT_OF_CLASS(ctx_zval, vio_context_ce)
    ZEND_PARSE_PARAMETERS_END();

    (void)ctx_zval; /* monitor info is window-independent; ctx just guards init */

    array_init(return_value);
#ifdef HAVE_GLFW
    GLFWmonitor *monitor = glfwGetPrimaryMonitor();
    if (monitor) {
        const GLFWvidmode *mode = glfwGetVideoMode(monitor);
        int wx = 0, wy = 0, ww = 0, wh = 0;
        float sx = 1.0f, sy = 1.0f;
        const char *name = glfwGetMonitorName(monitor);
        glfwGetMonitorWorkarea(monitor, &wx, &wy, &ww, &wh);
        glfwGetMonitorContentScale(monitor, &sx, &sy);
        add_assoc_long(return_value, "width", mode ? mode->width : 0);
        add_assoc_long(return_value, "height", mode ? mode->height : 0);
        add_assoc_long(return_value, "refresh_rate", mode ? mode->refreshRate : 0);
        add_assoc_long(return_value, "work_x", wx);
        add_assoc_long(return_value, "work_y", wy);
        add_assoc_long(return_value, "work_width", ww);
        add_assoc_long(return_value, "work_height", wh);
        add_assoc_double(return_value, "scale_x", (double)sx);
        add_assoc_double(return_value, "scale_y", (double)sy);
        add_assoc_string(return_value, "name", name ? name : "");
        return;
    }
#endif
    add_assoc_long(return_value, "width", 0);
    add_assoc_long(return_value, "height", 0);
    add_assoc_long(return_value, "refresh_rate", 0);
    add_assoc_long(return_value, "work_x", 0);
    add_assoc_long(return_value, "work_y", 0);
    add_assoc_long(return_value, "work_width", 0);
    add_assoc_long(return_value, "work_height", 0);
    add_assoc_double(return_value, "scale_x", 1.0);
    add_assoc_double(return_value, "scale_y", 1.0);
    add_assoc_string(return_value, "name", "");
}

ZEND_FUNCTION(vio_monitors)
{
    zval *ctx_zval;

    ZEND_PARSE_PARAMETERS_START(1, 1)
        Z_PARAM_OBJECT_OF_CLASS(ctx_zval, vio_context_ce)
    ZEND_PARSE_PARAMETERS_END();

    (void)ctx_zval;

    array_init(return_value);
#ifdef HAVE_GLFW
    int count = 0;
    GLFWmonitor **mons = glfwGetMonitors(&count);
    GLFWmonitor *primary = glfwGetPrimaryMonitor();
    for (int i = 0; i < count; i++) {
        GLFWmonitor *m = mons[i];
        const GLFWvidmode *mode = glfwGetVideoMode(m);
        int mx = 0, my = 0, wx = 0, wy = 0, ww = 0, wh = 0;
        float sx = 1.0f, sy = 1.0f;
        const char *name = glfwGetMonitorName(m);
        glfwGetMonitorPos(m, &mx, &my);
        glfwGetMonitorWorkarea(m, &wx, &wy, &ww, &wh);
        glfwGetMonitorContentScale(m, &sx, &sy);

        zval entry;
        array_init(&entry);
        add_assoc_long(&entry, "index", i);
        add_assoc_string(&entry, "name", name ? name : "");
        add_assoc_bool(&entry, "primary", m == primary);
        add_assoc_long(&entry, "x", mx);
        add_assoc_long(&entry, "y", my);
        add_assoc_long(&entry, "width", mode ? mode->width : 0);
        add_assoc_long(&entry, "height", mode ? mode->height : 0);
        add_assoc_long(&entry, "refresh_rate", mode ? mode->refreshRate : 0);
        add_assoc_long(&entry, "work_x", wx);
        add_assoc_long(&entry, "work_y", wy);
        add_assoc_long(&entry, "work_width", ww);
        add_assoc_long(&entry, "work_height", wh);
        add_assoc_double(&entry, "scale_x", (double)sx);
        add_assoc_double(&entry, "scale_y", (double)sy);
        add_next_index_zval(return_value, &entry);
    }
#endif
}

ZEND_FUNCTION(vio_video_modes)
{
    zval *ctx_zval;
    zend_long monitor_index = -1; /* -1 = primary monitor (default) */

    ZEND_PARSE_PARAMETERS_START(1, 2)
        Z_PARAM_OBJECT_OF_CLASS(ctx_zval, vio_context_ce)
        Z_PARAM_OPTIONAL
        Z_PARAM_LONG(monitor_index)
    ZEND_PARSE_PARAMETERS_END();

    (void)ctx_zval;

    array_init(return_value);
#ifdef HAVE_GLFW
    /* Resolve the monitor the same way vio_set_fullscreen does. */
    GLFWmonitor *monitor = NULL;
    if (monitor_index >= 0) {
        int mcount = 0;
        GLFWmonitor **mons = glfwGetMonitors(&mcount);
        if (mons && monitor_index < mcount) {
            monitor = mons[monitor_index];
        }
    }
    if (!monitor) {
        monitor = glfwGetPrimaryMonitor();
    }
    if (!monitor) return;

    int count = 0;
    const GLFWvidmode *modes = glfwGetVideoModes(monitor, &count);
    if (!modes) return;

    /* GLFW returns modes sorted ascending and may list the same (width,height,
     * refresh) several times for different bit depths. Collapse duplicates so
     * the picker shows each resolution/refresh combination once. */
    for (int i = 0; i < count; i++) {
        const GLFWvidmode *m = &modes[i];
        if (i > 0) {
            const GLFWvidmode *p = &modes[i - 1];
            if (p->width == m->width && p->height == m->height
                && p->refreshRate == m->refreshRate) {
                continue;
            }
        }
        zval entry;
        array_init(&entry);
        add_assoc_long(&entry, "width", m->width);
        add_assoc_long(&entry, "height", m->height);
        add_assoc_long(&entry, "refresh_rate", m->refreshRate);
        add_next_index_zval(return_value, &entry);
    }
#endif
}

ZEND_FUNCTION(vio_pixel_ratio)
{
    zval *ctx_zval;

    ZEND_PARSE_PARAMETERS_START(1, 1)
        Z_PARAM_OBJECT_OF_CLASS(ctx_zval, vio_context_ce)
    ZEND_PARSE_PARAMETERS_END();

    vio_context_object *ctx = Z_VIO_CONTEXT_P(ctx_zval);

#ifdef HAVE_GLFW
    if (ctx->window) {
        int fb_w = 0, win_w = 0, fb_h = 0, win_h = 0;
        glfwGetFramebufferSize(ctx->window, &fb_w, &fb_h);
        glfwGetWindowSize(ctx->window, &win_w, &win_h);
        if (win_w > 0) {
            RETURN_DOUBLE((double)fb_w / (double)win_w);
        }
    }
#endif
    RETURN_DOUBLE(1.0);
}

ZEND_FUNCTION(vio_mesh)
{
    zval *ctx_zval;
    HashTable *config_ht;

    ZEND_PARSE_PARAMETERS_START(2, 2)
        Z_PARAM_OBJECT_OF_CLASS(ctx_zval, vio_context_ce)
        Z_PARAM_ARRAY_HT(config_ht)
    ZEND_PARSE_PARAMETERS_END();

    vio_context_object *ctx = Z_VIO_CONTEXT_P(ctx_zval);

    if (!ctx->initialized) {
        php_error_docref(NULL, E_WARNING, "Context is not initialized");
        RETURN_FALSE;
    }

    /* Get vertices array */
    zval *vertices_zval = zend_hash_str_find(config_ht, "vertices", sizeof("vertices") - 1);
    if (!vertices_zval || Z_TYPE_P(vertices_zval) != IS_ARRAY) {
        php_error_docref(NULL, E_WARNING, "vio_mesh requires 'vertices' array");
        RETURN_FALSE;
    }

    HashTable *vertices_ht = Z_ARRVAL_P(vertices_zval);
    int vertex_data_count = zend_hash_num_elements(vertices_ht);

    if (vertex_data_count == 0) {
        php_error_docref(NULL, E_WARNING, "Vertices array is empty");
        RETURN_FALSE;
    }

    /* Get optional layout */
    zval *layout_zval = zend_hash_str_find(config_ht, "layout", sizeof("layout") - 1);
    int has_colors = 0;
    int floats_per_vertex = 3; /* default: position only (vec3) */

    /* Parsed layout: up to 16 vertex attributes (locations 0-15) */
    #define VIO_MAX_VERTEX_ATTRIBS 16
    typedef struct {
        int location;
        int components;
    } vio_vertex_attrib;
    vio_vertex_attrib parsed_layout[VIO_MAX_VERTEX_ATTRIBS];
    int parsed_layout_count = 0;
    int has_explicit_layout = 0; /* 1 if new dict-style layout was provided */

    if (layout_zval && Z_TYPE_P(layout_zval) == IS_ARRAY) {
        HashTable *layout_ht = Z_ARRVAL_P(layout_zval);
        int layout_count = zend_hash_num_elements(layout_ht);

        /* Detect format: check if first element is an array (new dict-style)
         * or a scalar (old flat-style: [VIO_FLOAT3, VIO_FLOAT4]) */
        zval *first_elem = zend_hash_index_find(layout_ht, 0);
        if (first_elem && Z_TYPE_P(first_elem) == IS_ARRAY) {
            /* New dict-style layout: [['location' => 0, 'components' => 3], ...] */
            has_explicit_layout = 1;
            floats_per_vertex = 0;
            zval *elem;
            ZEND_HASH_FOREACH_VAL(layout_ht, elem) {
                if (Z_TYPE_P(elem) != IS_ARRAY || parsed_layout_count >= VIO_MAX_VERTEX_ATTRIBS) {
                    continue;
                }
                HashTable *attr_ht = Z_ARRVAL_P(elem);
                zval *loc_zval = zend_hash_str_find(attr_ht, "location", sizeof("location") - 1);
                zval *comp_zval = zend_hash_str_find(attr_ht, "components", sizeof("components") - 1);

                int location = loc_zval ? (int)zval_get_long(loc_zval) : parsed_layout_count;
                int components = comp_zval ? (int)zval_get_long(comp_zval) : 3;

                /* Fallback: derive components from 'type' if 'components' not given */
                if (!comp_zval) {
                    zval *type_zval = zend_hash_str_find(attr_ht, "type", sizeof("type") - 1);
                    if (type_zval) {
                        int fmt = (int)zval_get_long(type_zval);
                        switch (fmt) {
                            case VIO_FLOAT1: components = 1; break;
                            case VIO_FLOAT2: components = 2; break;
                            case VIO_FLOAT3: components = 3; break;
                            case VIO_FLOAT4: components = 4; break;
                        }
                    }
                }

                if (location < 0 || location >= VIO_MAX_VERTEX_ATTRIBS) {
                    php_error_docref(NULL, E_WARNING, "Vertex attribute location %d out of range (0-%d)", location, VIO_MAX_VERTEX_ATTRIBS - 1);
                    continue;
                }
                if (components < 1 || components > 4) {
                    php_error_docref(NULL, E_WARNING, "Vertex attribute components must be 1-4, got %d", components);
                    continue;
                }

                parsed_layout[parsed_layout_count].location = location;
                parsed_layout[parsed_layout_count].components = components;
                parsed_layout_count++;
                floats_per_vertex += components;
            } ZEND_HASH_FOREACH_END();
        } else {
            /* Old flat-style layout: [VIO_FLOAT3, VIO_FLOAT4, ...] */
            floats_per_vertex = 0;
            zval *elem;
            ZEND_HASH_FOREACH_VAL(layout_ht, elem) {
                int fmt = (int)zval_get_long(elem);
                switch (fmt) {
                    case VIO_FLOAT1: floats_per_vertex += 1; break;
                    case VIO_FLOAT2: floats_per_vertex += 2; break;
                    case VIO_FLOAT3: floats_per_vertex += 3; break;
                    case VIO_FLOAT4: floats_per_vertex += 4; break;
                    default: floats_per_vertex += 3; break;
                }
            } ZEND_HASH_FOREACH_END();

            /* If layout has 2 entries and second is FLOAT4, assume colors */
            if (layout_count >= 2) {
                has_colors = 1;
            }
        }
    }

    int vertex_count = vertex_data_count / floats_per_vertex;

    /* Convert PHP array to float array */
    float *data = emalloc(sizeof(float) * vertex_data_count);
    int i = 0;
    zval *val;
    ZEND_HASH_FOREACH_VAL(vertices_ht, val) {
        data[i++] = (float)zval_get_double(val);
    } ZEND_HASH_FOREACH_END();

    /* Get optional indices */
    zval *indices_zval = zend_hash_str_find(config_ht, "indices", sizeof("indices") - 1);
    unsigned int *indices = NULL;
    int index_count = 0;

    if (indices_zval && Z_TYPE_P(indices_zval) == IS_ARRAY) {
        HashTable *indices_ht = Z_ARRVAL_P(indices_zval);
        index_count = zend_hash_num_elements(indices_ht);
        indices = emalloc(sizeof(unsigned int) * index_count);
        int j = 0;
        ZEND_HASH_FOREACH_VAL(indices_ht, val) {
            indices[j++] = (unsigned int)zval_get_long(val);
        } ZEND_HASH_FOREACH_END();
    }

    /* Create VioMesh object */
    zval mesh_zval;
    object_init_ex(&mesh_zval, vio_mesh_ce);
    vio_mesh_object *mesh = Z_VIO_MESH_P(&mesh_zval);

    mesh->vertex_count = vertex_count;
    mesh->index_count  = index_count;
    mesh->has_colors   = has_colors;
    mesh->stride       = floats_per_vertex * sizeof(float);
    mesh->backend      = ctx->backend;

    /* Normalize the layout into a backend-agnostic array so create_mesh can
     * do one straight glVertexAttribPointer-loop without re-deriving the
     * legacy "pos-only" / "pos+color" shapes inside the backend. */
    vio_mesh_attrib normalized_layout[VIO_MAX_VERTEX_ATTRIBS];
    int normalized_layout_count = 0;
    if (has_explicit_layout && parsed_layout_count > 0) {
        int offset = 0;
        for (int a = 0; a < parsed_layout_count; a++) {
            normalized_layout[a].location   = parsed_layout[a].location;
            normalized_layout[a].components = parsed_layout[a].components;
            normalized_layout[a].offset     = offset * (int)sizeof(float);
            offset += parsed_layout[a].components;
        }
        normalized_layout_count = parsed_layout_count;
    } else if (has_colors && floats_per_vertex >= 7) {
        normalized_layout[0] = (vio_mesh_attrib){0, 3, 0};
        normalized_layout[1] = (vio_mesh_attrib){1, 4, 3 * (int)sizeof(float)};
        normalized_layout_count = 2;
    } else {
        normalized_layout[0] = (vio_mesh_attrib){0, 3, 0};
        normalized_layout_count = 1;
    }

    if (ctx->backend->create_mesh) {
        ctx->backend->create_mesh(mesh,
            data, (int)(sizeof(float) * vertex_data_count), mesh->stride,
            normalized_layout, normalized_layout_count,
            indices, index_count);
    }

    /* Backend buffer creation (D3D11/D3D12/Vulkan) */
    if (strcmp(ctx->backend->name, "opengl") != 0 && ctx->backend->create_buffer) {
        /* Vertex buffer */
        vio_buffer_desc vb_desc = {0};
        vb_desc.type = VIO_BUFFER_VERTEX;
        vb_desc.data = data;
        vb_desc.size = sizeof(float) * vertex_data_count;
        mesh->backend_vb = ctx->backend->create_buffer(&vb_desc);

        /* Index buffer */
        if (indices && index_count > 0) {
            vio_buffer_desc ib_desc = {0};
            ib_desc.type = VIO_BUFFER_INDEX;
            ib_desc.data = indices;
            ib_desc.size = sizeof(unsigned int) * index_count;
            mesh->backend_ib = ctx->backend->create_buffer(&ib_desc);
        }
    }

    efree(data);
    if (indices) {
        efree(indices);
    }

    RETURN_COPY_VALUE(&mesh_zval);
}

/* Shared draw core: record ONE mesh draw onto the open frame command list.
 * Extracted from vio_draw so the single-draw entry point AND vio_submit_batch
 * issue byte-identical GPU commands (the same per-draw cbuffer slice + root-CBV
 * bind + draw_indexed path, on every backend). The bound pipeline's shader is
 * read from ctx->bound_shader_object, so the caller MUST have bound a pipeline.
 * Caller guarantees ctx is initialized + in_frame and mesh is non-NULL. */
static void vio_submit_one(vio_context_object *ctx, vio_mesh_object *mesh)
{
    if (ctx->backend->draw_mesh) {
        ctx->backend->draw_mesh(mesh);
    }

    /* Backend draw (D3D11/D3D12/Vulkan) */
    if (strcmp(ctx->backend->name, "opengl") != 0) {
        /* Flush uniform cbuffers before drawing */
        if (ctx->bound_shader_object) {
            vio_shader_object *sh = (vio_shader_object *)ctx->bound_shader_object;

            /* Upload vertex cbuffer */
            if (sh->cbuffer_dirty && sh->cbuffer_backend && ctx->backend->update_buffer) {
                ctx->backend->update_buffer(sh->cbuffer_backend,
                    sh->cbuffer_data, sh->cbuffer_total_size);
                sh->cbuffer_dirty = 0;
            }
            /* Upload fragment cbuffer */
            if (sh->frag_cbuffer_dirty && sh->frag_cbuffer_backend && ctx->backend->update_buffer) {
                ctx->backend->update_buffer(sh->frag_cbuffer_backend,
                    sh->frag_cbuffer_data, sh->frag_cbuffer_total_size);
                sh->frag_cbuffer_dirty = 0;
            }

#ifdef HAVE_D3D11
            if (strcmp(ctx->backend->name, "d3d11") == 0 && vio_d3d11.initialized) {
                /* Bind vertex cbuffer to VS slot b0 */
                if (sh->cbuffer_backend) {
                    vio_d3d11_buffer *cb = (vio_d3d11_buffer *)sh->cbuffer_backend;
                    ID3D11DeviceContext_VSSetConstantBuffers(vio_d3d11.context, 0, 1, &cb->buffer);
                    /* Also bind to PS b0 if no separate fragment cbuffer */
                    if (!sh->frag_cbuffer_backend) {
                        ID3D11DeviceContext_PSSetConstantBuffers(vio_d3d11.context, 0, 1, &cb->buffer);
                    }
                }
                /* Bind fragment cbuffer to PS slot b0 */
                if (sh->frag_cbuffer_backend) {
                    vio_d3d11_buffer *fcb = (vio_d3d11_buffer *)sh->frag_cbuffer_backend;
                    ID3D11DeviceContext_PSSetConstantBuffers(vio_d3d11.context, 0, 1, &fcb->buffer);
                }
            }
#endif
#ifdef HAVE_D3D12
            if (strcmp(ctx->backend->name, "d3d12") == 0 && vio_d3d12.initialized) {
                /* Allocate per-draw cbuffer slices from the linear allocator.
                 * Each draw gets its own 256-byte-aligned slice so previous
                 * draw data isn't overwritten (D3D12 has no buffer renaming). */
                if (sh->cbuffer_total_size > 0 && vio_d3d12.cbuffer_heap_mapped) {
                    UINT aligned = (sh->cbuffer_total_size + 255) & ~255;
                    UINT offset = vio_d3d12.cbuffer_heap_offset;
                    if (offset + aligned <= vio_d3d12.cbuffer_heap_capacity) {
                        memcpy(vio_d3d12.cbuffer_heap_mapped + offset,
                               sh->cbuffer_data, sh->cbuffer_total_size);
                        ID3D12GraphicsCommandList_SetGraphicsRootConstantBufferView(
                            vio_d3d12.cmd_list, 0,
                            vio_d3d12.cbuffer_heap_gpu + offset);
                        vio_d3d12.cbuffer_heap_offset = offset + aligned;
                    }
                }
                if (sh->frag_cbuffer_total_size > 0 && vio_d3d12.cbuffer_heap_mapped) {
                    UINT aligned = (sh->frag_cbuffer_total_size + 255) & ~255;
                    UINT offset = vio_d3d12.cbuffer_heap_offset;
                    if (offset + aligned <= vio_d3d12.cbuffer_heap_capacity) {
                        memcpy(vio_d3d12.cbuffer_heap_mapped + offset,
                               sh->frag_cbuffer_data, sh->frag_cbuffer_total_size);
                        ID3D12GraphicsCommandList_SetGraphicsRootConstantBufferView(
                            vio_d3d12.cmd_list, 1,
                            vio_d3d12.cbuffer_heap_gpu + offset);
                        vio_d3d12.cbuffer_heap_offset = offset + aligned;
                    }
                }
            }
#endif
        }
        if (mesh->index_count > 0 && mesh->backend_ib && ctx->backend->draw_indexed) {
            vio_draw_indexed_cmd cmd = {0};
            cmd.vertex_buffer = mesh->backend_vb;
            cmd.index_buffer = mesh->backend_ib;
            cmd.index_count = mesh->index_count;
            cmd.first_index = 0;
            cmd.vertex_offset = 0;
            cmd.instance_count = 1;
            cmd.vertex_stride = mesh->stride;
            ctx->backend->draw_indexed(&cmd);
        } else if (mesh->backend_vb && ctx->backend->draw) {
            vio_draw_cmd cmd = {0};
            cmd.vertex_buffer = mesh->backend_vb;
            cmd.vertex_count = mesh->vertex_count;
            cmd.first_vertex = 0;
            cmd.instance_count = 1;
            cmd.vertex_stride = mesh->stride;
            ctx->backend->draw(&cmd);
        }
    }
}

ZEND_FUNCTION(vio_draw)
{
    zval *ctx_zval;
    zval *mesh_zval;

    ZEND_PARSE_PARAMETERS_START(2, 2)
        Z_PARAM_OBJECT_OF_CLASS(ctx_zval, vio_context_ce)
        Z_PARAM_OBJECT_OF_CLASS(mesh_zval, vio_mesh_ce)
    ZEND_PARSE_PARAMETERS_END();

    vio_context_object *ctx = Z_VIO_CONTEXT_P(ctx_zval);
    vio_mesh_object *mesh = Z_VIO_MESH_P(mesh_zval);

    if (!ctx->initialized || !ctx->in_frame) {
        php_error_docref(NULL, E_WARNING, "Must call vio_draw between vio_begin and vio_end");
        return;
    }

    vio_submit_one(ctx, mesh);
}

/* ── Phase 4: Shader / Pipeline / Texture / Buffer functions ──────── */

/* Helper: detect SPIR-V magic number in a PHP string */
static int vio_is_spirv(const char *data, size_t len)
{
    if (len < 4) return 0;
    const uint32_t magic = *(const uint32_t *)data;
    return magic == 0x07230203;
}

ZEND_FUNCTION(vio_shader)
{
    zval *ctx_zval;
    HashTable *config_ht;

    ZEND_PARSE_PARAMETERS_START(2, 2)
        Z_PARAM_OBJECT_OF_CLASS(ctx_zval, vio_context_ce)
        Z_PARAM_ARRAY_HT(config_ht)
    ZEND_PARSE_PARAMETERS_END();

    vio_context_object *ctx = Z_VIO_CONTEXT_P(ctx_zval);

    if (!ctx->initialized) {
        php_error_docref(NULL, E_WARNING, "Context is not initialized");
        RETURN_FALSE;
    }

    /* Get vertex shader source */
    zval *vert_zval = zend_hash_str_find(config_ht, "vertex", sizeof("vertex") - 1);
    if (!vert_zval || Z_TYPE_P(vert_zval) != IS_STRING) {
        php_error_docref(NULL, E_WARNING, "vio_shader requires 'vertex' string");
        RETURN_FALSE;
    }

    /* Get fragment shader source */
    zval *frag_zval = zend_hash_str_find(config_ht, "fragment", sizeof("fragment") - 1);
    if (!frag_zval || Z_TYPE_P(frag_zval) != IS_STRING) {
        php_error_docref(NULL, E_WARNING, "vio_shader requires 'fragment' string");
        RETURN_FALSE;
    }

    /* Get optional format (auto-detect if VIO_SHADER_AUTO) */
    vio_shader_format format = VIO_SHADER_AUTO;
    zval *fmt_zval = zend_hash_str_find(config_ht, "format", sizeof("format") - 1);
    if (fmt_zval) {
        format = (vio_shader_format)zval_get_long(fmt_zval);
    }

    /* Auto-detect: check vertex shader for SPIR-V magic */
    if (format == VIO_SHADER_AUTO) {
        if (vio_is_spirv(Z_STRVAL_P(vert_zval), Z_STRLEN_P(vert_zval))) {
            format = VIO_SHADER_SPIRV;
        } else {
            format = VIO_SHADER_GLSL;
        }
    }

    /* GLSL #version sanity check (Issue #3 part 3): if the user's shader asks
     * for a GLSL version higher than the runtime context provides, the driver
     * would emit a cryptic shader-compile error inside vio_opengl_compile_*.
     * Catch it up front. Only relevant for OpenGL backends with text GLSL. */
#ifdef HAVE_GLFW
    if (strcmp(ctx->backend->name, "opengl") == 0 && vio_gl.initialized &&
        (format == VIO_SHADER_GLSL || format == VIO_SHADER_GLSL_RAW)) {
        int runtime_glsl = vio_opengl_get_glsl_version();
        const char *sources[2] = { Z_STRVAL_P(vert_zval), Z_STRVAL_P(frag_zval) };
        const char *stages[2]  = { "vertex", "fragment" };
        for (int s = 0; s < 2; s++) {
            /* Skip whitespace, look for #version */
            const char *p = sources[s];
            while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
            if (strncmp(p, "#version", 8) == 0) {
                p += 8;
                while (*p == ' ' || *p == '\t') p++;
                int req = 0;
                while (*p >= '0' && *p <= '9') {
                    req = req * 10 + (*p - '0');
                    p++;
                }
                if (req > runtime_glsl) {
                    php_error_docref(NULL, E_WARNING,
                        "vio_shader: %s shader requires GLSL >= %d, but runtime context "
                        "provides %d. Lower the #version directive or run on a newer GPU.",
                        stages[s], req, runtime_glsl);
                    RETURN_FALSE;
                }
            }
        }
    }
#endif

    /* Create VioShader object */
    zval shader_zval;
    object_init_ex(&shader_zval, vio_shader_ce);
    vio_shader_object *shader = Z_VIO_SHADER_P(&shader_zval);
    shader->format  = format;
    shader->backend = ctx->backend;

    /* --- SPIR-V input: store directly --- */
    if (format == VIO_SHADER_SPIRV) {
        shader->vert_spirv_size = Z_STRLEN_P(vert_zval);
        shader->vert_spirv = malloc(shader->vert_spirv_size);
        memcpy(shader->vert_spirv, Z_STRVAL_P(vert_zval), shader->vert_spirv_size);

        shader->frag_spirv_size = Z_STRLEN_P(frag_zval);
        shader->frag_spirv = malloc(shader->frag_spirv_size);
        memcpy(shader->frag_spirv, Z_STRVAL_P(frag_zval), shader->frag_spirv_size);
    }
    /* --- GLSL input: compile to SPIR-V via glslang (skip for raw) --- */
    else if (format == VIO_SHADER_GLSL) {
        char *error_msg = NULL;

        shader->vert_spirv = vio_compile_glsl_to_spirv(
            Z_STRVAL_P(vert_zval), 0, &shader->vert_spirv_size, &error_msg);
        if (!shader->vert_spirv) {
            php_error_docref(NULL, E_WARNING, "Vertex shader compilation failed: %s",
                error_msg ? error_msg : "unknown error");
            free(error_msg);
            zval_ptr_dtor(&shader_zval);
            RETURN_FALSE;
        }

        shader->frag_spirv = vio_compile_glsl_to_spirv(
            Z_STRVAL_P(frag_zval), 1, &shader->frag_spirv_size, &error_msg);
        if (!shader->frag_spirv) {
            php_error_docref(NULL, E_WARNING, "Fragment shader compilation failed: %s",
                error_msg ? error_msg : "unknown error");
            free(error_msg);
            zval_ptr_dtor(&shader_zval);
            RETURN_FALSE;
        }
    }

    /* --- For OpenGL backend --- */
#ifdef HAVE_GLFW
    if (strcmp(ctx->backend->name, "opengl") == 0 && vio_gl.initialized) {
        if (format == VIO_SHADER_GLSL_RAW) {
            /* Raw GLSL: compile directly, no SPIR-V round-trip */
            shader->program = vio_opengl_compile_shader_source(
                Z_STRVAL_P(vert_zval), Z_STRVAL_P(frag_zval));
            if (!shader->program) {
                php_error_docref(NULL, E_WARNING, "OpenGL shader compilation failed (raw GLSL)");
                zval_ptr_dtor(&shader_zval);
                RETURN_FALSE;
            }
        } else {
            /* Transpile SPIR-V to GLSL matching the runtime context version
             * (e.g. 330 on HD 3000, 410 on macOS, 460 on modern Linux). */
            int glsl_version = vio_opengl_get_glsl_version();
            char *error_msg = NULL;

            char *vert_glsl = vio_spirv_to_glsl(shader->vert_spirv, shader->vert_spirv_size, glsl_version, &error_msg);
            if (!vert_glsl) {
                php_error_docref(NULL, E_WARNING, "Vertex SPIR-V to GLSL transpilation failed: %s",
                    error_msg ? error_msg : "unknown error");
                free(error_msg);
                zval_ptr_dtor(&shader_zval);
                RETURN_FALSE;
            }

            char *frag_glsl = vio_spirv_to_glsl(shader->frag_spirv, shader->frag_spirv_size, glsl_version, &error_msg);
            if (!frag_glsl) {
                php_error_docref(NULL, E_WARNING, "Fragment SPIR-V to GLSL transpilation failed: %s",
                    error_msg ? error_msg : "unknown error");
                free(error_msg);
                free(vert_glsl);
                zval_ptr_dtor(&shader_zval);
                RETURN_FALSE;
            }

            shader->program = vio_opengl_compile_shader_source(vert_glsl, frag_glsl);
            free(vert_glsl);
            free(frag_glsl);

            if (!shader->program) {
                zval_ptr_dtor(&shader_zval);
                RETURN_FALSE;
            }
        }
    }
#endif

    /* --- For D3D/Vulkan backends: use backend compile_shader --- */
    if (strcmp(ctx->backend->name, "opengl") != 0 && ctx->backend->compile_shader) {
        /* Ensure SPIR-V is available (compile from GLSL if needed) */
        if (!shader->vert_spirv && (format == VIO_SHADER_GLSL_RAW || format == VIO_SHADER_GLSL)) {
            char *error_msg = NULL;
            shader->vert_spirv = vio_compile_glsl_to_spirv(
                Z_STRVAL_P(vert_zval), 0, &shader->vert_spirv_size, &error_msg);
            if (!shader->vert_spirv) {
                php_error_docref(NULL, E_WARNING, "VS GLSL->SPIR-V failed: %s", error_msg ? error_msg : "unknown");
                free(error_msg);
                zval_ptr_dtor(&shader_zval);
                RETURN_FALSE;
            }

            shader->frag_spirv = vio_compile_glsl_to_spirv(
                Z_STRVAL_P(frag_zval), 1, &shader->frag_spirv_size, &error_msg);
            if (!shader->frag_spirv) {
                php_error_docref(NULL, E_WARNING, "FS GLSL->SPIR-V failed: %s", error_msg ? error_msg : "unknown");
                free(error_msg);
                zval_ptr_dtor(&shader_zval);
                RETURN_FALSE;
            }
        }

        /* Pass SPIR-V data to backend (it will transpile to HLSL/MSL as needed) */
        vio_shader_desc desc = {0};
        desc.format = VIO_SHADER_GLSL; /* Backend expects SPIRV data when format=GLSL */
        desc.vertex_data = shader->vert_spirv;
        desc.vertex_size = shader->vert_spirv_size;
        desc.fragment_data = shader->frag_spirv;
        desc.fragment_size = shader->frag_spirv_size;

        shader->backend_shader = ctx->backend->compile_shader(&desc);
        if (!shader->backend_shader) {
            php_error_docref(NULL, E_WARNING, "Backend shader compilation failed");
            zval_ptr_dtor(&shader_zval);
            RETURN_FALSE;
        }

        /* Extract uniform offsets from SPIRV for constant buffer mapping */
        if (shader->vert_spirv) {
            shader->uniform_count = vio_spirv_get_uniform_offsets(
                shader->vert_spirv, shader->vert_spirv_size,
                shader->uniforms, VIO_MAX_UNIFORMS,
                &shader->cbuffer_total_size);
            memset(shader->cbuffer_data, 0, sizeof(shader->cbuffer_data));


            /* Create backend constant buffer for vertex stage */
            if (shader->cbuffer_total_size > 0 && ctx->backend->create_buffer) {
                vio_buffer_desc cb_desc = {0};
                cb_desc.type = VIO_BUFFER_UNIFORM;
                cb_desc.data = NULL;
                cb_desc.size = shader->cbuffer_total_size;
                cb_desc.binding = 0;
                shader->cbuffer_backend = ctx->backend->create_buffer(&cb_desc);
            }
        }

        /* Extract fragment shader uniforms */
        if (shader->frag_spirv) {
            shader->frag_uniform_count = vio_spirv_get_uniform_offsets(
                shader->frag_spirv, shader->frag_spirv_size,
                shader->frag_uniforms, VIO_MAX_UNIFORMS,
                &shader->frag_cbuffer_total_size);
            memset(shader->frag_cbuffer_data, 0, sizeof(shader->frag_cbuffer_data));
            /* Create backend constant buffer for fragment stage */
            if (shader->frag_cbuffer_total_size > 0 && ctx->backend->create_buffer) {
                vio_buffer_desc cb_desc = {0};
                cb_desc.type = VIO_BUFFER_UNIFORM;
                cb_desc.data = NULL;
                cb_desc.size = shader->frag_cbuffer_total_size;
                cb_desc.binding = 0;
                shader->frag_cbuffer_backend = ctx->backend->create_buffer(&cb_desc);
            }

            /* Extract sampler names from fragment SPIRV for GL->HLSL slot remapping.
             * HLSL binding = index in sampled_images list (matches vio_spirv_to_hlsl). */
            {
                vio_reflect_result frag_reflect = {0};
                char *err = NULL;
                if (vio_spirv_reflect(shader->frag_spirv, shader->frag_spirv_size, &frag_reflect, &err) == 0) {
                    shader->sampler_count = frag_reflect.texture_count < VIO_MAX_SAMPLERS
                                          ? frag_reflect.texture_count : VIO_MAX_SAMPLERS;
                    /* Recompute the HLSL t-register the cross-compiler assigned to each
                     * sampler. The reflection list here is the SAME sampled_images list,
                     * in the SAME order, that vio_spirv_to_hlsl walks when it assigns
                     * SpvDecorationBinding (vio_shader_reflect.c): regular samplers get
                     * 0,1,2..; depth/shadow samplers get 4,5,6.. So we can reproduce the
                     * exact register by replaying that counter scheme. This is correct by
                     * construction and replaces the fragile "reflection index + 4" guess. */
                    /* MUST match vio_shader_reflect.c: regular 0+, shadow 8+. */
                    int regular_reg = 0;
                    int shadow_reg = 8;
                    for (int s = 0; s < shader->sampler_count; s++) {
                        strncpy(shader->sampler_names[s], frag_reflect.textures[s].name,
                                sizeof(shader->sampler_names[s]) - 1);
                        int is_depth = frag_reflect.textures[s].is_depth ? 1 : 0;
                        shader->sampler_is_depth[s] = is_depth;
                        shader->sampler_hlsl_reg[s] = is_depth ? shadow_reg++ : regular_reg++;
                    }
                    vio_reflect_free(&frag_reflect);
                }
                if (err) free(err);
                /* Init remap table to identity (no remapping) */
                for (int s = 0; s < 16; s++) shader->gl_to_hlsl_sampler[s] = -1;
            }
        }
    }

    shader->valid = 1;
    RETURN_COPY_VALUE(&shader_zval);
}

ZEND_FUNCTION(vio_shader_reflect)
{
    zval *shader_zval;

    ZEND_PARSE_PARAMETERS_START(1, 1)
        Z_PARAM_OBJECT_OF_CLASS(shader_zval, vio_shader_ce)
    ZEND_PARSE_PARAMETERS_END();

    vio_shader_object *shader = Z_VIO_SHADER_P(shader_zval);

    if (!shader->valid) {
        php_error_docref(NULL, E_WARNING, "Shader is not valid");
        RETURN_FALSE;
    }

    if (!shader->vert_spirv && !shader->frag_spirv) {
        php_error_docref(NULL, E_WARNING, "Shader has no SPIR-V data for reflection");
        RETURN_FALSE;
    }

    array_init(return_value);

    /* Reflect vertex shader */
    if (shader->vert_spirv) {
        vio_reflect_result result;
        char *error_msg = NULL;
        if (vio_spirv_reflect(shader->vert_spirv, shader->vert_spirv_size, &result, &error_msg) == 0) {
            zval vert_arr;
            array_init(&vert_arr);

            /* Inputs */
            zval inputs_arr;
            array_init(&inputs_arr);
            for (int i = 0; i < result.input_count; i++) {
                zval item;
                array_init(&item);
                add_assoc_string(&item, "name", (char *)result.inputs[i].name);
                add_assoc_long(&item, "location", result.inputs[i].location);
                add_assoc_long(&item, "binding", result.inputs[i].binding);
                add_next_index_zval(&inputs_arr, &item);
            }
            add_assoc_zval(&vert_arr, "inputs", &inputs_arr);

            /* UBOs */
            zval ubos_arr;
            array_init(&ubos_arr);
            for (int i = 0; i < result.ubo_count; i++) {
                zval item;
                array_init(&item);
                add_assoc_string(&item, "name", (char *)result.ubos[i].name);
                add_assoc_long(&item, "set", result.ubos[i].set);
                add_assoc_long(&item, "binding", result.ubos[i].binding);
                add_next_index_zval(&ubos_arr, &item);
            }
            add_assoc_zval(&vert_arr, "ubos", &ubos_arr);

            /* Textures */
            zval tex_arr;
            array_init(&tex_arr);
            for (int i = 0; i < result.texture_count; i++) {
                zval item;
                array_init(&item);
                add_assoc_string(&item, "name", (char *)result.textures[i].name);
                add_assoc_long(&item, "set", result.textures[i].set);
                add_assoc_long(&item, "binding", result.textures[i].binding);
                add_next_index_zval(&tex_arr, &item);
            }
            add_assoc_zval(&vert_arr, "textures", &tex_arr);

            /* Uniforms (push constants) */
            zval uni_arr;
            array_init(&uni_arr);
            for (int i = 0; i < result.uniform_count; i++) {
                zval item;
                array_init(&item);
                add_assoc_string(&item, "name", (char *)result.uniforms[i].name);
                add_assoc_long(&item, "binding", result.uniforms[i].binding);
                add_next_index_zval(&uni_arr, &item);
            }
            add_assoc_zval(&vert_arr, "uniforms", &uni_arr);

            add_assoc_zval(return_value, "vertex", &vert_arr);
            vio_reflect_free(&result);
        } else {
            php_error_docref(NULL, E_NOTICE, "Vertex reflection failed: %s",
                error_msg ? error_msg : "unknown error");
            free(error_msg);
        }
    }

    /* Reflect fragment shader */
    if (shader->frag_spirv) {
        vio_reflect_result result;
        char *error_msg = NULL;
        if (vio_spirv_reflect(shader->frag_spirv, shader->frag_spirv_size, &result, &error_msg) == 0) {
            zval frag_arr;
            array_init(&frag_arr);

            /* Inputs */
            zval inputs_arr;
            array_init(&inputs_arr);
            for (int i = 0; i < result.input_count; i++) {
                zval item;
                array_init(&item);
                add_assoc_string(&item, "name", (char *)result.inputs[i].name);
                add_assoc_long(&item, "location", result.inputs[i].location);
                add_assoc_long(&item, "binding", result.inputs[i].binding);
                add_next_index_zval(&inputs_arr, &item);
            }
            add_assoc_zval(&frag_arr, "inputs", &inputs_arr);

            /* UBOs */
            zval ubos_arr;
            array_init(&ubos_arr);
            for (int i = 0; i < result.ubo_count; i++) {
                zval item;
                array_init(&item);
                add_assoc_string(&item, "name", (char *)result.ubos[i].name);
                add_assoc_long(&item, "set", result.ubos[i].set);
                add_assoc_long(&item, "binding", result.ubos[i].binding);
                add_next_index_zval(&ubos_arr, &item);
            }
            add_assoc_zval(&frag_arr, "ubos", &ubos_arr);

            /* Textures */
            zval tex_arr;
            array_init(&tex_arr);
            for (int i = 0; i < result.texture_count; i++) {
                zval item;
                array_init(&item);
                add_assoc_string(&item, "name", (char *)result.textures[i].name);
                add_assoc_long(&item, "set", result.textures[i].set);
                add_assoc_long(&item, "binding", result.textures[i].binding);
                add_next_index_zval(&tex_arr, &item);
            }
            add_assoc_zval(&frag_arr, "textures", &tex_arr);

            /* Uniforms (push constants) */
            zval uni_arr;
            array_init(&uni_arr);
            for (int i = 0; i < result.uniform_count; i++) {
                zval item;
                array_init(&item);
                add_assoc_string(&item, "name", (char *)result.uniforms[i].name);
                add_assoc_long(&item, "binding", result.uniforms[i].binding);
                add_next_index_zval(&uni_arr, &item);
            }
            add_assoc_zval(&frag_arr, "uniforms", &uni_arr);

            add_assoc_zval(return_value, "fragment", &frag_arr);
            vio_reflect_free(&result);
        } else {
            php_error_docref(NULL, E_NOTICE, "Fragment reflection failed: %s",
                error_msg ? error_msg : "unknown error");
            free(error_msg);
        }
    }
}

ZEND_FUNCTION(vio_pipeline)
{
    zval *ctx_zval;
    HashTable *config_ht;

    ZEND_PARSE_PARAMETERS_START(2, 2)
        Z_PARAM_OBJECT_OF_CLASS(ctx_zval, vio_context_ce)
        Z_PARAM_ARRAY_HT(config_ht)
    ZEND_PARSE_PARAMETERS_END();

    vio_context_object *ctx = Z_VIO_CONTEXT_P(ctx_zval);

    if (!ctx->initialized) {
        php_error_docref(NULL, E_WARNING, "Context is not initialized");
        RETURN_FALSE;
    }

    /* Get shader */
    zval *shader_zval = zend_hash_str_find(config_ht, "shader", sizeof("shader") - 1);
    if (!shader_zval || Z_TYPE_P(shader_zval) != IS_OBJECT ||
        !instanceof_function(Z_OBJCE_P(shader_zval), vio_shader_ce)) {
        php_error_docref(NULL, E_WARNING, "vio_pipeline requires 'shader' (VioShader object)");
        RETURN_FALSE;
    }

    vio_shader_object *shader = Z_VIO_SHADER_P(shader_zval);
    if (!shader->valid) {
        php_error_docref(NULL, E_WARNING, "Shader is not valid");
        RETURN_FALSE;
    }

    /* Create VioPipeline object */
    zval pipe_zval;
    object_init_ex(&pipe_zval, vio_pipeline_ce);
    vio_pipeline_object *pipe = Z_VIO_PIPELINE_P(&pipe_zval);

    pipe->shader_program = shader->program;

    /* Parse optional settings */
    zval *val;
    if ((val = zend_hash_str_find(config_ht, "topology", sizeof("topology") - 1)) != NULL) {
        pipe->topology = (vio_topology)zval_get_long(val);
    }
    if ((val = zend_hash_str_find(config_ht, "cull_mode", sizeof("cull_mode") - 1)) != NULL) {
        pipe->cull_mode = (vio_cull_mode)zval_get_long(val);
    }
    if ((val = zend_hash_str_find(config_ht, "depth_test", sizeof("depth_test") - 1)) != NULL) {
        pipe->depth_test = zend_is_true(val);
    }
    if ((val = zend_hash_str_find(config_ht, "depth_func", sizeof("depth_func") - 1)) != NULL) {
        pipe->depth_func = (vio_depth_func)zval_get_long(val);
    }
    if ((val = zend_hash_str_find(config_ht, "blend", sizeof("blend") - 1)) != NULL) {
        pipe->blend = (vio_blend_mode)zval_get_long(val);
    }
    if ((val = zend_hash_str_find(config_ht, "depth_bias", sizeof("depth_bias") - 1)) != NULL) {
        pipe->depth_bias = (float)zval_get_double(val);
    }
    if ((val = zend_hash_str_find(config_ht, "slope_scaled_depth_bias", sizeof("slope_scaled_depth_bias") - 1)) != NULL) {
        pipe->slope_scaled_depth_bias = (float)zval_get_double(val);
    }
    /* Optional 'hdr' flag: when true the PSO's render-target (RTV) format becomes
     * FP16 (R16G16B16A16_FLOAT) instead of the default R8G8B8A8_UNORM. Needed when
     * the pipeline draws into a vio_render_target created with 'hdr' => true, since
     * D3D12 requires the PSO RTV format to match the bound target's format. Default
     * 0 keeps every existing pipeline byte-for-byte at R8G8B8A8_UNORM. */
    if ((val = zend_hash_str_find(config_ht, "hdr", sizeof("hdr") - 1)) != NULL) {
        pipe->hdr_output = zend_is_true(val) ? 1 : 0;
    }

    /* Store backend shader reference for lazy pipeline creation */
    pipe->backend_shader = shader->backend_shader;
    pipe->shader_ref = shader;

    /* Hold a strong reference to the VioShader zend_object so the pipeline keeps
     * its shader alive. vio_bind_pipeline assigns shader_ref to
     * ctx->bound_shader_object, which vio_set_uniform / vio_draw later
     * dereference; if the only PHP owner of the VioShader was a local that fell
     * out of scope after vio_pipeline() returned, the object would be freed and
     * those dereferences would walk freed memory (observed as a 0xC0000005 in
     * the shadow-debug pass, whose shader was an unstored local). */
    pipe->shader_obj = Z_OBJ_P(shader_zval);
    GC_ADDREF(pipe->shader_obj);

    /* Create backend pipeline (D3D11/D3D12/Vulkan) */
    if (strcmp(ctx->backend->name, "opengl") != 0 &&
        shader->backend_shader && ctx->backend->create_pipeline) {

        /* Build vertex layout from shader SPIR-V reflection */
        vio_vertex_attrib layout[16];
        int attrib_count = 0;

        if (shader->vert_spirv && shader->vert_spirv_size > 0) {
            vio_reflect_result reflect = {0};
            char *err = NULL;
            if (vio_spirv_reflect(shader->vert_spirv, shader->vert_spirv_size, &reflect, &err) == 0) {
                for (int i = 0; i < reflect.input_count && i < 16; i++) {
                    layout[attrib_count].location = reflect.inputs[i].location;
                    /* Map SPIRV vecsize to VIO format (enum values match: 1=FLOAT1..4=FLOAT4) */
                    unsigned int vs = reflect.inputs[i].vecsize;
                    if (vs < 1 || vs > 4) vs = 3;
                    layout[attrib_count].format = (vio_format)vs;
                    /* SPIRV-Cross maps all GLSL inputs to TEXCOORD{location} in HLSL */
                    layout[attrib_count].usage = VIO_TEXCOORD;
                    attrib_count++;
                }
                vio_reflect_free(&reflect);
            }
            if (err) free(err);
        }

        /* Fallback: at least position attribute */
        if (attrib_count == 0) {
            layout[0].location = 0;
            layout[0].format = VIO_FLOAT3;
            layout[0].usage = VIO_TEXCOORD;
            attrib_count = 1;
        }

        /* Sort attributes by location ascending. The backend assigns each
         * attribute's AlignedByteOffset incrementally in ARRAY order, which only
         * matches the vertex buffer (attributes packed in increasing-location
         * order) if the array is location-sorted. SPIRV-Cross does NOT guarantee
         * reflection order == location order: e.g. postprocess.vert reflects
         * a_uv(loc1) before a_position(loc0), producing offsets [uv@0, pos@8]
         * instead of [pos@0, uv@12] — which fed a_position garbage and collapsed
         * the fullscreen blit quad to a single point (black offscreen present,
         * no FXAA/SSAO). Sorting here makes the offsets match the buffer. */
        for (int a = 0; a < attrib_count - 1; a++) {
            for (int b = 0; b < attrib_count - 1 - a; b++) {
                if (layout[b].location > layout[b + 1].location) {
                    vio_vertex_attrib tmp = layout[b];
                    layout[b] = layout[b + 1];
                    layout[b + 1] = tmp;
                }
            }
        }

        vio_pipeline_desc desc = {0};
        desc.shader = shader->backend_shader;
        desc.vertex_layout = layout;
        desc.vertex_attrib_count = attrib_count;
        desc.topology = pipe->topology;
        desc.cull_mode = pipe->cull_mode;
        desc.depth_test = pipe->depth_test;
        desc.depth_func = pipe->depth_func;
        desc.blend = pipe->blend;
        desc.depth_bias = pipe->depth_bias;
        desc.slope_scaled_depth_bias = pipe->slope_scaled_depth_bias;
        desc.hdr_output = pipe->hdr_output;

        pipe->backend_pipeline = ctx->backend->create_pipeline(&desc);
    }

    pipe->valid = 1;
    RETURN_COPY_VALUE(&pipe_zval);
}

ZEND_FUNCTION(vio_bind_pipeline)
{
    zval *ctx_zval;
    zval *pipe_zval;

    ZEND_PARSE_PARAMETERS_START(2, 2)
        Z_PARAM_OBJECT_OF_CLASS(ctx_zval, vio_context_ce)
        Z_PARAM_OBJECT_OF_CLASS(pipe_zval, vio_pipeline_ce)
    ZEND_PARSE_PARAMETERS_END();

    vio_context_object *ctx = Z_VIO_CONTEXT_P(ctx_zval);

    if (!ctx->initialized || !ctx->in_frame) {
        php_error_docref(NULL, E_WARNING, "Must call vio_bind_pipeline between vio_begin and vio_end");
        return;
    }

    vio_pipeline_object *pipe = Z_VIO_PIPELINE_P(pipe_zval);
    if (!pipe->valid) {
        php_error_docref(NULL, E_WARNING, "Pipeline is not valid");
        return;
    }

    /* Track bound shader in context for vio_draw() and uniform cbuffer */
    ctx->bound_shader_program = pipe->shader_program;
    ctx->bound_shader_object = pipe->shader_ref;

    /* OpenGL: apply the pipeline's GL state (program, cull/depth/blend etc.) */
    if (ctx->backend->bind_pipeline_state) {
        ctx->backend->bind_pipeline_state(pipe);
    }

    /* D3D11/D3D12/Vulkan: dispatch the backend's own pipeline-bind */
    if (!ctx->backend->bind_pipeline_state &&
        pipe->backend_pipeline && ctx->backend->bind_pipeline) {
        ctx->backend->bind_pipeline(pipe->backend_pipeline);
    }
}

ZEND_FUNCTION(vio_texture)
{
    zval *ctx_zval;
    HashTable *config_ht;

    ZEND_PARSE_PARAMETERS_START(2, 2)
        Z_PARAM_OBJECT_OF_CLASS(ctx_zval, vio_context_ce)
        Z_PARAM_ARRAY_HT(config_ht)
    ZEND_PARSE_PARAMETERS_END();

    vio_context_object *ctx = Z_VIO_CONTEXT_P(ctx_zval);

    if (!ctx->initialized) {
        php_error_docref(NULL, E_WARNING, "Context is not initialized");
        RETURN_FALSE;
    }

    /* Create VioTexture object */
    zval tex_zval;
    object_init_ex(&tex_zval, vio_texture_ce);
    vio_texture_object *tex = Z_VIO_TEXTURE_P(&tex_zval);
    tex->backend = ctx->backend;

    /* Parse filter/wrap options */
    zval *val;
    if ((val = zend_hash_str_find(config_ht, "filter", sizeof("filter") - 1)) != NULL) {
        tex->filter = (vio_filter)zval_get_long(val);
    }
    if ((val = zend_hash_str_find(config_ht, "wrap", sizeof("wrap") - 1)) != NULL) {
        tex->wrap = (vio_wrap)zval_get_long(val);
    }

    /* Load from file or raw data */
    zval *file_zval = zend_hash_str_find(config_ht, "file", sizeof("file") - 1);
    zval *data_zval = zend_hash_str_find(config_ht, "data", sizeof("data") - 1);
    zval *width_zval = zend_hash_str_find(config_ht, "width", sizeof("width") - 1);
    zval *height_zval = zend_hash_str_find(config_ht, "height", sizeof("height") - 1);

    unsigned char *pixels = NULL;
    int w = 0, h = 0, channels = 0;
    int from_stbi = 0;

    if (file_zval && Z_TYPE_P(file_zval) == IS_STRING) {
        /* Load from file using stb_image */
        pixels = stbi_load(Z_STRVAL_P(file_zval), &w, &h, &channels, 4);
        if (!pixels) {
            php_error_docref(NULL, E_WARNING, "Failed to load texture: %s", stbi_failure_reason());
            zval_ptr_dtor(&tex_zval);
            RETURN_FALSE;
        }
        channels = 4; /* forced RGBA */
        from_stbi = 1;
    } else if (data_zval && Z_TYPE_P(data_zval) == IS_STRING && width_zval && height_zval) {
        /* Raw pixel data */
        w = (int)zval_get_long(width_zval);
        h = (int)zval_get_long(height_zval);
        channels = 4;

        /* Reject non-positive dimensions outright. Previously these were
         * silently accepted and the resulting "texture" had garbage on the
         * GPU (or read past the data buffer for w*h*4 bytes). */
        if (w <= 0 || h <= 0) {
            php_error_docref(NULL, E_WARNING,
                "vio_texture: width/height must be positive (got %dx%d)", w, h);
            zval_ptr_dtor(&tex_zval);
            RETURN_FALSE;
        }

        /* Verify the data buffer is large enough for w*h RGBA bytes.
         * Without this the backend uploaded uninitialised memory past
         * the end of the PHP string (visible as garbage texture content
         * in production, an out-of-bounds read under valgrind/ASAN). */
        size_t need = (size_t)w * (size_t)h * 4u;
        if (Z_STRLEN_P(data_zval) < need) {
            php_error_docref(NULL, E_WARNING,
                "vio_texture: data is %zu bytes but %dx%d RGBA needs %zu",
                Z_STRLEN_P(data_zval), w, h, need);
            zval_ptr_dtor(&tex_zval);
            RETURN_FALSE;
        }

        pixels = (unsigned char *)Z_STRVAL_P(data_zval);
    } else {
        php_error_docref(NULL, E_WARNING, "vio_texture requires 'file' or 'data'+'width'+'height'");
        zval_ptr_dtor(&tex_zval);
        RETURN_FALSE;
    }

    tex->width    = w;
    tex->height   = h;
    tex->channels = channels;

    zval *mipmap_zval = zend_hash_str_find(config_ht, "mipmaps", sizeof("mipmaps") - 1);
    int mipmaps = (mipmap_zval && zend_is_true(mipmap_zval)) ? 1 : 0;

    if (ctx->backend->upload_texture_2d) {
        /* OpenGL writes texture_id directly into tex_obj */
        ctx->backend->upload_texture_2d(tex, pixels, w, h, channels,
                                        (int)tex->filter, (int)tex->wrap, mipmaps);
    } else if (ctx->backend->create_texture) {
        /* D3D11/D3D12/Vulkan return an opaque backend handle */
        vio_texture_desc desc = {0};
        desc.data = pixels;
        desc.data_size = (size_t)(w * h * channels);
        desc.width = w;
        desc.height = h;
        desc.filter = tex->filter;
        desc.wrap = tex->wrap;
        desc.mipmaps = mipmaps;
        tex->backend_texture = ctx->backend->create_texture(&desc);
    }

#ifdef HAVE_METAL
    if (strcmp(ctx->backend->name, "metal") == 0) {
        tex->texture_id = vio_metal_create_texture_rgba(w, h, pixels,
            tex->filter != VIO_FILTER_NEAREST, tex->wrap == VIO_WRAP_CLAMP);
    }
#endif

    if (from_stbi) {
        stbi_image_free(pixels);
    }

    tex->valid = 1;
    RETURN_COPY_VALUE(&tex_zval);
}

ZEND_FUNCTION(vio_texture_3d)
{
    zval *ctx_zval;
    HashTable *config_ht;

    ZEND_PARSE_PARAMETERS_START(2, 2)
        Z_PARAM_OBJECT_OF_CLASS(ctx_zval, vio_context_ce)
        Z_PARAM_ARRAY_HT(config_ht)
    ZEND_PARSE_PARAMETERS_END();

    vio_context_object *ctx = Z_VIO_CONTEXT_P(ctx_zval);

    if (!ctx->initialized) {
        php_error_docref(NULL, E_WARNING, "Context is not initialized");
        RETURN_FALSE;
    }

    /* 3D textures are created via one of three backend paths: OpenGL writes a
     * GL_TEXTURE_3D into texture_id (upload_texture_3d); D3D11/D3D12/Vulkan
     * return an opaque handle (create_texture_3d); Metal uses its own id-based
     * helper. A backend with none of these reports VIO_FEATURE_TEXTURE_3D = 0
     * and fails here gracefully — callers fall back to the analytic / 2D-atlas
     * trace path. */
    int has_3d_path = (ctx->backend->upload_texture_3d != NULL)
                   || (ctx->backend->create_texture_3d != NULL);
#ifdef HAVE_METAL
    if (ctx->backend->name && strcmp(ctx->backend->name, "metal") == 0) {
        has_3d_path = 1;
    }
#endif
    if (!has_3d_path) {
        php_error_docref(NULL, E_WARNING,
            "vio_texture_3d: backend '%s' has no 3D-texture path",
            ctx->backend->name ? ctx->backend->name : "?");
        RETURN_FALSE;
    }

    zval *data_zval   = zend_hash_str_find(config_ht, "data", sizeof("data") - 1);
    zval *width_zval  = zend_hash_str_find(config_ht, "width", sizeof("width") - 1);
    zval *height_zval = zend_hash_str_find(config_ht, "height", sizeof("height") - 1);
    zval *depth_zval  = zend_hash_str_find(config_ht, "depth", sizeof("depth") - 1);

    if (!data_zval || Z_TYPE_P(data_zval) != IS_STRING || !width_zval || !height_zval || !depth_zval) {
        php_error_docref(NULL, E_WARNING,
            "vio_texture_3d requires 'data'+'width'+'height'+'depth'");
        RETURN_FALSE;
    }

    int w = (int)zval_get_long(width_zval);
    int h = (int)zval_get_long(height_zval);
    int d = (int)zval_get_long(depth_zval);
    if (w <= 0 || h <= 0 || d <= 0) {
        php_error_docref(NULL, E_WARNING,
            "vio_texture_3d: width/height/depth must be positive (got %dx%dx%d)", w, h, d);
        RETURN_FALSE;
    }

    size_t need = (size_t)w * (size_t)h * (size_t)d * 4u;
    if (Z_STRLEN_P(data_zval) < need) {
        php_error_docref(NULL, E_WARNING,
            "vio_texture_3d: data is %zu bytes but %dx%dx%d RGBA needs %zu",
            Z_STRLEN_P(data_zval), w, h, d, need);
        RETURN_FALSE;
    }

    zval tex_zval;
    object_init_ex(&tex_zval, vio_texture_ce);
    vio_texture_object *tex = Z_VIO_TEXTURE_P(&tex_zval);
    tex->backend = ctx->backend;

    zval *val;
    tex->filter = VIO_FILTER_LINEAR;  /* trilinear by default — SDF wants smooth */
    tex->wrap   = VIO_WRAP_CLAMP;     /* sensible default for a bounded SDF volume */
    if ((val = zend_hash_str_find(config_ht, "filter", sizeof("filter") - 1)) != NULL) {
        tex->filter = (vio_filter)zval_get_long(val);
    }
    if ((val = zend_hash_str_find(config_ht, "wrap", sizeof("wrap") - 1)) != NULL) {
        tex->wrap = (vio_wrap)zval_get_long(val);
    }

    tex->width    = w;
    tex->height   = h;
    tex->depth    = d;
    tex->is_3d    = 1;
    tex->channels = 4;

    const unsigned char *pixels = (const unsigned char *)Z_STRVAL_P(data_zval);

    if (ctx->backend->upload_texture_3d) {
        /* OpenGL: writes a GL_TEXTURE_3D into tex->texture_id. */
        int rc = ctx->backend->upload_texture_3d(
            tex, pixels, w, h, d, 4, (int)tex->filter, (int)tex->wrap);
        if (rc != 0) {
            php_error_docref(NULL, E_WARNING, "vio_texture_3d: backend upload failed");
            zval_ptr_dtor(&tex_zval);
            RETURN_FALSE;
        }
    } else if (ctx->backend->create_texture_3d) {
        /* D3D11 / D3D12 / Vulkan: opaque backend handle, bound via bind_texture. */
        vio_texture_desc desc = {0};
        desc.data      = pixels;
        desc.data_size = need;
        desc.width     = w;
        desc.height    = h;
        desc.depth     = d;
        desc.filter    = tex->filter;
        desc.wrap      = tex->wrap;
        tex->backend_texture = ctx->backend->create_texture_3d(&desc);
        if (!tex->backend_texture) {
            php_error_docref(NULL, E_WARNING, "vio_texture_3d: backend create failed");
            zval_ptr_dtor(&tex_zval);
            RETURN_FALSE;
        }
    }
#ifdef HAVE_METAL
    else if (ctx->backend->name && strcmp(ctx->backend->name, "metal") == 0) {
        tex->texture_id = vio_metal_create_texture_3d_rgba(
            w, h, d, pixels, tex->filter != VIO_FILTER_NEAREST, tex->wrap == VIO_WRAP_CLAMP);
        if (tex->texture_id == 0) {
            php_error_docref(NULL, E_WARNING, "vio_texture_3d: metal create failed");
            zval_ptr_dtor(&tex_zval);
            RETURN_FALSE;
        }
    }
#endif
    else {
        zval_ptr_dtor(&tex_zval);
        RETURN_FALSE;
    }

    tex->valid = 1;
    RETURN_COPY_VALUE(&tex_zval);
}

/* Shared core: bind ONE texture to a GL slot (remapped to the cross-compiler's
 * HLSL t-register for the typed-register backends). Extracted from
 * vio_bind_texture so vio_submit_batch binds textures through the EXACT same
 * sampler remap + depth-sampler path. Caller guarantees ctx is initialized +
 * in_frame and tex is valid. */
static void vio_bind_texture_internal(vio_context_object *ctx, vio_texture_object *tex, zend_long slot)
{
    if (tex->is_3d && tex->texture_id && ctx->backend->bind_texture_3d_id) {
        /* OpenGL volume texture (Fieldtracing SDF): bind via the GL_TEXTURE_3D
         * target. D3D11/D3D12/Vulkan volumes have texture_id == 0 and fall
         * through to the opaque backend_texture path below (their SRV / image
         * view already encodes the 3D dimension, so bind_texture works as-is). */
        ctx->backend->bind_texture_3d_id(tex->texture_id, (int)slot);
        return;
    }

    if (ctx->backend->bind_texture_id) {
        ctx->backend->bind_texture_id(tex->texture_id, (int)slot);
    }

    /* Backend texture binding (D3D11/D3D12/Vulkan) */
    if (strcmp(ctx->backend->name, "opengl") != 0 &&
        tex->backend_texture && ctx->backend->bind_texture) {
        int hlsl_slot = (int)slot;
        int shader_wants_depth = 0;
        int sampler_idx = -1;
        /* Remap GL texture unit to HLSL register using sampler map */
        if (ctx->bound_shader_object) {
            vio_shader_object *sh = (vio_shader_object *)ctx->bound_shader_object;
            if (slot >= 0 && slot < 16 && sh->gl_to_hlsl_sampler[slot] >= 0) {
                sampler_idx = sh->gl_to_hlsl_sampler[slot];
                hlsl_slot = sampler_idx;
                if (sampler_idx >= 0 && sampler_idx < sh->sampler_count) {
                    shader_wants_depth = sh->sampler_is_depth[sampler_idx];
                }
            }
        }
        /* D3D12: use the EXACT t-register the cross-compiler assigned to this
         * sampler (sampler_hlsl_reg). Regular samplers land at 0,1,2..; depth/
         * shadow samplers at 4,5,6.. The previous code used the reflection list
         * index + a flat "+4" for depth samplers, which over-shot to slots >= 8
         * (past the table) whenever regular textures preceded the shadow samplers
         * in the reflection list — silently dropping the CSM cascades on D3D12.
         * Using the recomputed register removes both the guesswork and the +4. */
#ifdef HAVE_D3D12
        if (strcmp(ctx->backend->name, "d3d12") == 0 && sampler_idx >= 0 &&
            ctx->bound_shader_object) {
            vio_shader_object *sh = (vio_shader_object *)ctx->bound_shader_object;
            if (sampler_idx < sh->sampler_count) {
                hlsl_slot = sh->sampler_hlsl_reg[sampler_idx];
            }
        }
#endif

        ctx->backend->bind_texture(tex->backend_texture, hlsl_slot);

#ifdef HAVE_D3D11
        /* If the shader uses sampler2DShadow, bind the comparison sampler instead */
        if (strcmp(ctx->backend->name, "d3d11") == 0 && vio_d3d11.initialized && shader_wants_depth) {
            vio_d3d11_texture *d3d_tex = (vio_d3d11_texture *)tex->backend_texture;
            if (d3d_tex->sampler_cmp) {
                ID3D11DeviceContext_PSSetSamplers(vio_d3d11.context, (UINT)hlsl_slot, 1, &d3d_tex->sampler_cmp);
            }
        }
#endif
    }
}

ZEND_FUNCTION(vio_bind_texture)
{
    zval *ctx_zval;
    zval *tex_zval;
    zend_long slot = 0;

    ZEND_PARSE_PARAMETERS_START(2, 3)
        Z_PARAM_OBJECT_OF_CLASS(ctx_zval, vio_context_ce)
        Z_PARAM_OBJECT_OF_CLASS(tex_zval, vio_texture_ce)
        Z_PARAM_OPTIONAL
        Z_PARAM_LONG(slot)
    ZEND_PARSE_PARAMETERS_END();

    vio_context_object *ctx = Z_VIO_CONTEXT_P(ctx_zval);

    if (!ctx->initialized || !ctx->in_frame) {
        php_error_docref(NULL, E_WARNING, "Must call vio_bind_texture between vio_begin and vio_end");
        return;
    }

    vio_texture_object *tex = Z_VIO_TEXTURE_P(tex_zval);
    if (!tex->valid) {
        php_error_docref(NULL, E_WARNING, "Texture is not valid");
        return;
    }

    vio_bind_texture_internal(ctx, tex, slot);
}

ZEND_FUNCTION(vio_uniform_buffer)
{
    zval *ctx_zval;
    HashTable *config_ht;

    ZEND_PARSE_PARAMETERS_START(2, 2)
        Z_PARAM_OBJECT_OF_CLASS(ctx_zval, vio_context_ce)
        Z_PARAM_ARRAY_HT(config_ht)
    ZEND_PARSE_PARAMETERS_END();

    vio_context_object *ctx = Z_VIO_CONTEXT_P(ctx_zval);

    if (!ctx->initialized) {
        php_error_docref(NULL, E_WARNING, "Context is not initialized");
        RETURN_FALSE;
    }

    /* Get size */
    zval *size_zval = zend_hash_str_find(config_ht, "size", sizeof("size") - 1);
    if (!size_zval) {
        php_error_docref(NULL, E_WARNING, "vio_uniform_buffer requires 'size'");
        RETURN_FALSE;
    }
    size_t size = (size_t)zval_get_long(size_zval);

    /* Get optional binding point */
    int binding = 0;
    zval *bind_zval = zend_hash_str_find(config_ht, "binding", sizeof("binding") - 1);
    if (bind_zval) {
        binding = (int)zval_get_long(bind_zval);
    }

    /* Create VioBuffer object */
    zval buf_zval;
    object_init_ex(&buf_zval, vio_buffer_ce);
    vio_buffer_object *buf = Z_VIO_BUFFER_P(&buf_zval);

    buf->type    = VIO_BUFFER_UNIFORM;
    buf->size    = size;
    buf->binding = binding;

    /* Get optional initial data */
    zval *data_zval = zend_hash_str_find(config_ht, "data", sizeof("data") - 1);
    const void *init_data = NULL;
    if (data_zval && Z_TYPE_P(data_zval) == IS_STRING) {
        init_data = Z_STRVAL_P(data_zval);
    }

    buf->backend = ctx->backend;
    if (ctx->backend->create_uniform_buffer) {
        ctx->backend->create_uniform_buffer(buf, (int)size, init_data, binding);
    } else if (ctx->backend->create_buffer) {
        vio_buffer_desc desc = {0};
        desc.type = VIO_BUFFER_UNIFORM;
        desc.data = init_data;
        desc.size = size;
        desc.binding = binding;
        buf->backend_buffer = ctx->backend->create_buffer(&desc);
    }

    buf->valid = 1;
    RETURN_COPY_VALUE(&buf_zval);
}

ZEND_FUNCTION(vio_update_buffer)
{
    zval *buf_zval;
    char *data;
    size_t data_len;
    zend_long offset = 0;

    ZEND_PARSE_PARAMETERS_START(2, 3)
        Z_PARAM_OBJECT_OF_CLASS(buf_zval, vio_buffer_ce)
        Z_PARAM_STRING(data, data_len)
        Z_PARAM_OPTIONAL
        Z_PARAM_LONG(offset)
    ZEND_PARSE_PARAMETERS_END();

    vio_buffer_object *buf = Z_VIO_BUFFER_P(buf_zval);
    if (!buf->valid) {
        php_error_docref(NULL, E_WARNING, "Buffer is not valid");
        return;
    }

    if ((size_t)(offset + data_len) > buf->size) {
        php_error_docref(NULL, E_WARNING, "Data exceeds buffer size");
        return;
    }

    /* OpenGL stores its handle in buf->buffer_id and goes through
     * update_uniform_buffer; D3D / Vulkan use the backend handle path. */
    if (buf->backend) {
        const vio_backend *be = (const vio_backend *)buf->backend;
        if (be->update_uniform_buffer && buf->buffer_id) {
            be->update_uniform_buffer(buf, data, (int)data_len, (int)offset);
        }
    }
    if (buf->backend_buffer && buf->backend) {
        const vio_backend *be = (const vio_backend *)buf->backend;
        if (be->update_buffer) {
            be->update_buffer(buf->backend_buffer, data, data_len);
        }
    }
}

ZEND_FUNCTION(vio_bind_buffer)
{
    zval *ctx_zval;
    zval *buf_zval;
    zend_long binding = -1;

    ZEND_PARSE_PARAMETERS_START(2, 3)
        Z_PARAM_OBJECT_OF_CLASS(ctx_zval, vio_context_ce)
        Z_PARAM_OBJECT_OF_CLASS(buf_zval, vio_buffer_ce)
        Z_PARAM_OPTIONAL
        Z_PARAM_LONG(binding)
    ZEND_PARSE_PARAMETERS_END();

    vio_context_object *ctx = Z_VIO_CONTEXT_P(ctx_zval);

    if (!ctx->initialized || !ctx->in_frame) {
        php_error_docref(NULL, E_WARNING, "Must call vio_bind_buffer between vio_begin and vio_end");
        return;
    }

    vio_buffer_object *buf = Z_VIO_BUFFER_P(buf_zval);
    if (!buf->valid) {
        php_error_docref(NULL, E_WARNING, "Buffer is not valid");
        return;
    }

    int bind_point = (binding >= 0) ? (int)binding : buf->binding;

    if (ctx->backend->bind_uniform_buffer && buf->buffer_id) {
        ctx->backend->bind_uniform_buffer(buf, bind_point);
    }

#ifdef HAVE_D3D11
    if (strcmp(ctx->backend->name, "d3d11") == 0 && vio_d3d11.initialized && buf->backend_buffer) {
        vio_d3d11_buffer *d3d_buf = (vio_d3d11_buffer *)buf->backend_buffer;
        if (d3d_buf->buffer) {
            UINT slot = (UINT)bind_point;
            ID3D11DeviceContext_VSSetConstantBuffers(vio_d3d11.context, slot, 1, &d3d_buf->buffer);
            ID3D11DeviceContext_PSSetConstantBuffers(vio_d3d11.context, slot, 1, &d3d_buf->buffer);
        }
    }
#endif

#ifdef HAVE_D3D12
    if (strcmp(ctx->backend->name, "d3d12") == 0 && vio_d3d12.initialized && buf->backend_buffer) {
        vio_d3d12_buffer *d3d_buf = (vio_d3d12_buffer *)buf->backend_buffer;
        if (d3d_buf->resource && d3d_buf->gpu_address && vio_d3d12.cmd_list) {
            /* Root signature declares b0 per stage at params 0 (VS) and 1 (PS).
             * Other binding points aren't in the root sig — ignore them silently. */
            if (bind_point == 0) {
                ID3D12GraphicsCommandList_SetGraphicsRootConstantBufferView(
                    vio_d3d12.cmd_list, 0, d3d_buf->gpu_address);
                ID3D12GraphicsCommandList_SetGraphicsRootConstantBufferView(
                    vio_d3d12.cmd_list, 1, d3d_buf->gpu_address);
            }
        }
    }
#endif
}

/* ── GPU compute primitive (backend-agnostic; M1 = D3D12) ─────────────
 *
 * Every vio_compute_* function feature-gates on
 * ctx->backend->supports_feature(VIO_FEATURE_COMPUTE) plus the specific vtable
 * hook it needs; when unsupported it emits an E_NOTICE and returns false/void
 * so the PHP layer (GpuSdfBaker) silently falls back to the CPU path. */

static int vio_compute_supported(vio_context_object *ctx)
{
    if (!ctx || !ctx->initialized || !ctx->backend) return 0;
    if (!ctx->backend->supports_feature) return 0;
    return ctx->backend->supports_feature(VIO_FEATURE_COMPUTE) ? 1 : 0;
}

ZEND_FUNCTION(vio_compute_pipeline)
{
    zval *ctx_zval;
    HashTable *config_ht;

    ZEND_PARSE_PARAMETERS_START(2, 2)
        Z_PARAM_OBJECT_OF_CLASS(ctx_zval, vio_context_ce)
        Z_PARAM_ARRAY_HT(config_ht)
    ZEND_PARSE_PARAMETERS_END();

    vio_context_object *ctx = Z_VIO_CONTEXT_P(ctx_zval);

    if (!vio_compute_supported(ctx) || !ctx->backend->create_compute_pipeline) {
        php_error_docref(NULL, E_NOTICE, "vio_compute_pipeline: compute not supported on this backend");
        RETURN_FALSE;
    }

    zval *src_zval = zend_hash_str_find(config_ht, "source", sizeof("source") - 1);
    if (!src_zval || Z_TYPE_P(src_zval) != IS_STRING) {
        php_error_docref(NULL, E_WARNING, "vio_compute_pipeline requires 'source' (GLSL compute string)");
        RETURN_FALSE;
    }

    /* Hand the GLSL compute source to the backend via the shader desc's
     * fragment_data slot (the backend compiles GLSL->SPIR-V->HLSL->cs blob). */
    vio_shader_desc desc = {0};
    desc.fragment_data = Z_STRVAL_P(src_zval);
    desc.fragment_size = Z_STRLEN_P(src_zval);
    desc.format = VIO_SHADER_GLSL;

    void *backend_pipeline = ctx->backend->create_compute_pipeline(&desc);
    if (!backend_pipeline) {
        php_error_docref(NULL, E_WARNING, "vio_compute_pipeline: backend compute pipeline creation failed");
        RETURN_FALSE;
    }

    zval p_zval;
    object_init_ex(&p_zval, vio_compute_pipeline_ce);
    vio_compute_pipeline_object *p = Z_VIO_COMPUTE_PIPELINE_P(&p_zval);
    p->backend = ctx->backend;
    p->backend_pipeline = backend_pipeline;
    p->valid = 1;

    RETURN_COPY_VALUE(&p_zval);
}

ZEND_FUNCTION(vio_storage_buffer)
{
    zval *ctx_zval;
    HashTable *config_ht;

    ZEND_PARSE_PARAMETERS_START(2, 2)
        Z_PARAM_OBJECT_OF_CLASS(ctx_zval, vio_context_ce)
        Z_PARAM_ARRAY_HT(config_ht)
    ZEND_PARSE_PARAMETERS_END();

    vio_context_object *ctx = Z_VIO_CONTEXT_P(ctx_zval);

    if (!vio_compute_supported(ctx) || !ctx->backend->create_buffer) {
        php_error_docref(NULL, E_NOTICE, "vio_storage_buffer: compute not supported on this backend");
        RETURN_FALSE;
    }

    /* 'data' (binary string -> SRV input) OR 'size' (zeroed UAV output). */
    zval *data_zval = zend_hash_str_find(config_ht, "data", sizeof("data") - 1);
    zval *size_zval = zend_hash_str_find(config_ht, "size", sizeof("size") - 1);

    const void *init_data = NULL;
    size_t size = 0;
    if (data_zval && Z_TYPE_P(data_zval) == IS_STRING) {
        init_data = Z_STRVAL_P(data_zval);
        size = Z_STRLEN_P(data_zval);
    } else if (size_zval) {
        size = (size_t)zval_get_long(size_zval);
    } else {
        php_error_docref(NULL, E_WARNING, "vio_storage_buffer requires 'data' or 'size'");
        RETURN_FALSE;
    }
    if (size == 0) {
        php_error_docref(NULL, E_WARNING, "vio_storage_buffer: zero-length buffer");
        RETURN_FALSE;
    }

    int stride = 0;
    zval *stride_zval = zend_hash_str_find(config_ht, "stride", sizeof("stride") - 1);
    if (stride_zval) stride = (int)zval_get_long(stride_zval);

    vio_buffer_desc desc = {0};
    desc.type = VIO_BUFFER_STORAGE;
    desc.data = init_data;
    desc.size = size;
    desc.binding = 0;
    desc.stride = stride;

    void *backend_buffer = ctx->backend->create_buffer(&desc);
    if (!backend_buffer) {
        php_error_docref(NULL, E_WARNING, "vio_storage_buffer: backend buffer creation failed");
        RETURN_FALSE;
    }

    zval buf_zval;
    object_init_ex(&buf_zval, vio_buffer_ce);
    vio_buffer_object *buf = Z_VIO_BUFFER_P(&buf_zval);
    buf->type = VIO_BUFFER_STORAGE;
    buf->size = size;
    buf->binding = 0;
    buf->stride = stride;
    buf->backend = ctx->backend;
    buf->backend_buffer = backend_buffer;
    buf->valid = 1;

    RETURN_COPY_VALUE(&buf_zval);
}

ZEND_FUNCTION(vio_compute_bind_buffer)
{
    zval *ctx_zval;
    zval *pipe_zval;
    zval *buf_zval;
    zend_long slot;
    zend_long access;

    ZEND_PARSE_PARAMETERS_START(5, 5)
        Z_PARAM_OBJECT_OF_CLASS(ctx_zval, vio_context_ce)
        Z_PARAM_OBJECT_OF_CLASS(pipe_zval, vio_compute_pipeline_ce)
        Z_PARAM_OBJECT_OF_CLASS(buf_zval, vio_buffer_ce)
        Z_PARAM_LONG(slot)
        Z_PARAM_LONG(access)
    ZEND_PARSE_PARAMETERS_END();

    vio_context_object *ctx = Z_VIO_CONTEXT_P(ctx_zval);
    if (!vio_compute_supported(ctx) || !ctx->backend->compute_bind_buffer) {
        php_error_docref(NULL, E_NOTICE, "vio_compute_bind_buffer: compute not supported");
        return;
    }

    vio_compute_pipeline_object *p = Z_VIO_COMPUTE_PIPELINE_P(pipe_zval);
    vio_buffer_object *buf = Z_VIO_BUFFER_P(buf_zval);
    if (!p->valid || !p->backend_pipeline || !buf->valid || !buf->backend_buffer) {
        php_error_docref(NULL, E_WARNING, "vio_compute_bind_buffer: invalid pipeline or buffer");
        return;
    }

    /* Structured-view element count = size / stride. A stride of 0 means the
     * buffer holds raw 4-byte (float/uint) elements. The backend receives the
     * same stride so its SRV/UAV StructureByteStride matches NumElements. */
    int elem_stride = buf->stride > 0 ? buf->stride : 4;
    int element_count = (int)(buf->size / (size_t)elem_stride);

    ctx->backend->compute_bind_buffer(p->backend_pipeline, buf->backend_buffer,
                                      (int)slot, (int)access, element_count, elem_stride);
}

ZEND_FUNCTION(vio_compute_set_uniforms)
{
    zval *ctx_zval;
    zval *pipe_zval;
    char *data;
    size_t data_len;

    ZEND_PARSE_PARAMETERS_START(3, 3)
        Z_PARAM_OBJECT_OF_CLASS(ctx_zval, vio_context_ce)
        Z_PARAM_OBJECT_OF_CLASS(pipe_zval, vio_compute_pipeline_ce)
        Z_PARAM_STRING(data, data_len)
    ZEND_PARSE_PARAMETERS_END();

    vio_context_object *ctx = Z_VIO_CONTEXT_P(ctx_zval);
    if (!vio_compute_supported(ctx) || !ctx->backend->compute_set_uniforms) {
        php_error_docref(NULL, E_NOTICE, "vio_compute_set_uniforms: compute not supported");
        return;
    }

    vio_compute_pipeline_object *p = Z_VIO_COMPUTE_PIPELINE_P(pipe_zval);
    if (!p->valid || !p->backend_pipeline) {
        php_error_docref(NULL, E_WARNING, "vio_compute_set_uniforms: invalid pipeline");
        return;
    }

    ctx->backend->compute_set_uniforms(p->backend_pipeline, data, (int)data_len);
}

ZEND_FUNCTION(vio_compute_dispatch)
{
    zval *ctx_zval;
    zval *pipe_zval;
    zend_long gx, gy, gz;

    ZEND_PARSE_PARAMETERS_START(5, 5)
        Z_PARAM_OBJECT_OF_CLASS(ctx_zval, vio_context_ce)
        Z_PARAM_OBJECT_OF_CLASS(pipe_zval, vio_compute_pipeline_ce)
        Z_PARAM_LONG(gx)
        Z_PARAM_LONG(gy)
        Z_PARAM_LONG(gz)
    ZEND_PARSE_PARAMETERS_END();

    vio_context_object *ctx = Z_VIO_CONTEXT_P(ctx_zval);
    if (!vio_compute_supported(ctx) || !ctx->backend->dispatch_compute) {
        php_error_docref(NULL, E_NOTICE, "vio_compute_dispatch: compute not supported");
        return;
    }

    vio_compute_pipeline_object *p = Z_VIO_COMPUTE_PIPELINE_P(pipe_zval);
    if (!p->valid || !p->backend_pipeline) {
        php_error_docref(NULL, E_WARNING, "vio_compute_dispatch: invalid pipeline");
        return;
    }

    vio_compute_cmd cmd = {0};
    cmd.pipeline = p->backend_pipeline;
    cmd.group_count_x = (int)gx;
    cmd.group_count_y = (int)gy;
    cmd.group_count_z = (int)gz;
    ctx->backend->dispatch_compute(&cmd);
}

ZEND_FUNCTION(vio_storage_buffer_read)
{
    zval *ctx_zval;
    zval *buf_zval;

    ZEND_PARSE_PARAMETERS_START(2, 2)
        Z_PARAM_OBJECT_OF_CLASS(ctx_zval, vio_context_ce)
        Z_PARAM_OBJECT_OF_CLASS(buf_zval, vio_buffer_ce)
    ZEND_PARSE_PARAMETERS_END();

    vio_context_object *ctx = Z_VIO_CONTEXT_P(ctx_zval);
    if (!vio_compute_supported(ctx) || !ctx->backend->read_buffer) {
        php_error_docref(NULL, E_NOTICE, "vio_storage_buffer_read: compute not supported");
        RETURN_FALSE;
    }

    vio_buffer_object *buf = Z_VIO_BUFFER_P(buf_zval);
    if (!buf->valid || !buf->backend_buffer || buf->size == 0) {
        php_error_docref(NULL, E_WARNING, "vio_storage_buffer_read: invalid buffer");
        RETURN_FALSE;
    }

    zend_string *out = zend_string_alloc(buf->size, 0);
    size_t n = ctx->backend->read_buffer(buf->backend_buffer, ZSTR_VAL(out), buf->size);
    if (n == 0) {
        zend_string_release(out);
        RETURN_FALSE;
    }
    /* read_buffer may return fewer bytes than requested; trim. */
    ZSTR_LEN(out) = n;
    ZSTR_VAL(out)[n] = '\0';
    RETURN_STR(out);
}

/* Lazily build a per-shader name -> packed (stage|offset|size) map, then do an
 * O(1) lookup. Replaces the linear strcmp scan over every uniform that previously
 * ran for EACH uniform set on the hot draw path. Encoding (non-zero on a hit):
 *   bit48 present, bit40 is_frag, bits16..31 offset, bits0..15 size. The uniform
 * tables are populated once at shader reflection and never change, so the map is
 * safe to build once and reuse for the shader's lifetime. */
static zend_long vio_uniform_lookup(vio_shader_object *sh, const char *name)
{
    if (!sh->uniform_lookup) {
        ALLOC_HASHTABLE(sh->uniform_lookup);
        zend_hash_init(sh->uniform_lookup,
                       sh->uniform_count + sh->frag_uniform_count + 1, NULL, NULL, 0);
        /* Vertex first so it wins on a name collision (add-if-absent), matching
         * the original vertex-before-fragment scan order. */
        for (int u = 0; u < sh->uniform_count; u++) {
            zend_long enc = (1LL << 48)
                | ((zend_long)(sh->uniforms[u].offset & 0xFFFF) << 16)
                | (zend_long)(sh->uniforms[u].size & 0xFFFF);
            zend_hash_str_add_ptr(sh->uniform_lookup, sh->uniforms[u].name,
                                  strlen(sh->uniforms[u].name), (void *)(intptr_t)enc);
        }
        for (int u = 0; u < sh->frag_uniform_count; u++) {
            zend_long enc = (1LL << 48) | (1LL << 40)
                | ((zend_long)(sh->frag_uniforms[u].offset & 0xFFFF) << 16)
                | (zend_long)(sh->frag_uniforms[u].size & 0xFFFF);
            zend_hash_str_add_ptr(sh->uniform_lookup, sh->frag_uniforms[u].name,
                                  strlen(sh->frag_uniforms[u].name), (void *)(intptr_t)enc);
        }
    }
    void *p = zend_hash_str_find_ptr(sh->uniform_lookup, name, strlen(name));
    return p ? (zend_long)(intptr_t)p : 0;
}

/* Shared core for vio_set_uniform / vio_set_uniforms: marshal one (name, value)
 * into the OpenGL set_uniform path and/or the bound shader's cbuffer at the
 * reflected offset. Extracted so the single + batch entry points write
 * byte-identical state. Caller guarantees ctx is initialized + in_frame. */
static void vio_apply_uniform(vio_context_object *ctx, const char *name, zval *value_zval)
{
    /* Marshal zval -> typed (data, count, type) once, then dispatch via the
     * backend's set_uniform slot. OpenGL uses it to do the glGetUniformLocation
     * + glUniform* dance; D3D/Vulkan/Metal also implement set_uniform but with
     * different semantics (raw cbuffer slot 0), so the per-uniform cbuffer-data
     * write below is the actual D3D path. The strcmp keeps the two from
     * stepping on each other until set_uniform is fully unified. */
    if (ctx->backend->set_uniform && strcmp(ctx->backend->name, "opengl") == 0) {
        if (Z_TYPE_P(value_zval) == IS_LONG) {
            int v = (int)Z_LVAL_P(value_zval);
            ctx->backend->set_uniform(name, &v, 1, VIO_UNIFORM_INT);
        } else if (Z_TYPE_P(value_zval) == IS_DOUBLE) {
            float v = (float)Z_DVAL_P(value_zval);
            ctx->backend->set_uniform(name, &v, 1, VIO_UNIFORM_FLOAT);
        } else if (Z_TYPE_P(value_zval) == IS_ARRAY) {
            HashTable *ht = Z_ARRVAL_P(value_zval);
            zval *first = zend_hash_index_find(ht, 0);
            if (first && Z_TYPE_P(first) == IS_ARRAY) {
                /* Nested array → matrix (3x3 or 4x4) */
                float mat[16];
                int i = 0;
                zval *row;
                ZEND_HASH_FOREACH_VAL(ht, row) {
                    if (Z_TYPE_P(row) == IS_ARRAY) {
                        zval *elem;
                        ZEND_HASH_FOREACH_VAL(Z_ARRVAL_P(row), elem) {
                            if (i < 16) mat[i++] = (float)zval_get_double(elem);
                        } ZEND_HASH_FOREACH_END();
                    }
                } ZEND_HASH_FOREACH_END();
                if (i == 16) ctx->backend->set_uniform(name, mat, 1, VIO_UNIFORM_MAT4);
                else if (i == 9) ctx->backend->set_uniform(name, mat, 1, VIO_UNIFORM_MAT3);
            } else {
                /* Flat array: vec2/3/4 or flat mat3/4 */
                float vals[16];
                int i = 0;
                zval *elem;
                ZEND_HASH_FOREACH_VAL(ht, elem) {
                    if (i < 16) vals[i++] = (float)zval_get_double(elem);
                } ZEND_HASH_FOREACH_END();
                switch (i) {
                    case 2:  ctx->backend->set_uniform(name, vals, 1, VIO_UNIFORM_VEC2); break;
                    case 3:  ctx->backend->set_uniform(name, vals, 1, VIO_UNIFORM_VEC3); break;
                    case 4:  ctx->backend->set_uniform(name, vals, 1, VIO_UNIFORM_VEC4); break;
                    case 9:  ctx->backend->set_uniform(name, vals, 1, VIO_UNIFORM_MAT3); break;
                    case 16: ctx->backend->set_uniform(name, vals, 1, VIO_UNIFORM_MAT4); break;
                }
            }
        }
    }

    /* Backend uniform setting: write into shader's cbuffer_data at correct offset */
    if (strcmp(ctx->backend->name, "opengl") != 0 && ctx->bound_shader_object) {
        vio_shader_object *sh = (vio_shader_object *)ctx->bound_shader_object;

        /* Check if this is a sampler uniform (int value = GL texture unit).
         * Build GL-slot -> HLSL-binding remap for vio_bind_texture. */
        if (Z_TYPE_P(value_zval) == IS_LONG) {
            int gl_slot = (int)Z_LVAL_P(value_zval);
            for (int s = 0; s < sh->sampler_count; s++) {
                if (strcmp(sh->sampler_names[s], name) == 0) {
                    if (gl_slot >= 0 && gl_slot < 16) {
                        sh->gl_to_hlsl_sampler[gl_slot] = s;  /* HLSL binding = index */
                    }
                    break;
                }
            }
        }

        /* O(1) name -> (stage, offset, size) lookup (built once per shader),
         * replacing the former linear strcmp scan over both uniform arrays. */
        unsigned char *dst = NULL;
        int max_size = 0;
        int is_frag = 0;

        zend_long enc = vio_uniform_lookup(sh, name);
        if (enc) {
            is_frag = (int)((enc >> 40) & 0x1);
            int offset = (int)((enc >> 16) & 0xFFFF);
            max_size = (int)(enc & 0xFFFF);
            if (offset >= 0 && offset + max_size <= VIO_CBUFFER_SIZE) {
                dst = (is_frag ? sh->frag_cbuffer_data : sh->cbuffer_data) + offset;
            }
        }

        if (dst) {
            if (Z_TYPE_P(value_zval) == IS_LONG) {
                int v = (int)Z_LVAL_P(value_zval);
                if (max_size >= (int)sizeof(int)) memcpy(dst, &v, sizeof(int));
            } else if (Z_TYPE_P(value_zval) == IS_DOUBLE) {
                float v = (float)Z_DVAL_P(value_zval);
                if (max_size >= (int)sizeof(float)) memcpy(dst, &v, sizeof(float));
            } else if (Z_TYPE_P(value_zval) == IS_ARRAY) {
                float vals[16];
                int i = 0;
                HashTable *ht = Z_ARRVAL_P(value_zval);
                zval *first = zend_hash_index_find(ht, 0);
                if (first && Z_TYPE_P(first) == IS_ARRAY) {
                    zval *row;
                    ZEND_HASH_FOREACH_VAL(ht, row) {
                        if (Z_TYPE_P(row) == IS_ARRAY) {
                            zval *elem;
                            ZEND_HASH_FOREACH_VAL(Z_ARRVAL_P(row), elem) {
                                if (i < 16) vals[i++] = (float)zval_get_double(elem);
                            } ZEND_HASH_FOREACH_END();
                        }
                    } ZEND_HASH_FOREACH_END();
                } else {
                    zval *elem;
                    ZEND_HASH_FOREACH_VAL(ht, elem) {
                        if (i < 16) vals[i++] = (float)zval_get_double(elem);
                    } ZEND_HASH_FOREACH_END();
                }
                /* mat3 special case: HLSL cbuffer pads each row to 16 bytes.
                 * 9 floats (36 bytes) must become 12 floats (48 bytes) with
                 * 4-byte padding after every 3 floats. */
                if (i == 9 && max_size >= 48) {
                    float padded[12];
                    padded[0]  = vals[0]; padded[1]  = vals[1]; padded[2]  = vals[2]; padded[3]  = 0.0f;
                    padded[4]  = vals[3]; padded[5]  = vals[4]; padded[6]  = vals[5]; padded[7]  = 0.0f;
                    padded[8]  = vals[6]; padded[9]  = vals[7]; padded[10] = vals[8]; padded[11] = 0.0f;
                    memcpy(dst, padded, 48);
                } else {
                    int copy_size = (int)(i * sizeof(float));
                    if (copy_size > max_size) copy_size = max_size;
                    memcpy(dst, vals, copy_size);
                }
            }

            if (is_frag) {
                sh->frag_cbuffer_dirty = 1;
            } else {
                sh->cbuffer_dirty = 1;
            }
        }
    }
}

ZEND_FUNCTION(vio_set_uniform)
{
    zval *ctx_zval;
    char *name;
    size_t name_len;
    zval *value_zval;

    ZEND_PARSE_PARAMETERS_START(3, 3)
        Z_PARAM_OBJECT_OF_CLASS(ctx_zval, vio_context_ce)
        Z_PARAM_STRING(name, name_len)
        Z_PARAM_ZVAL(value_zval)
    ZEND_PARSE_PARAMETERS_END();

    vio_context_object *ctx = Z_VIO_CONTEXT_P(ctx_zval);

    if (!ctx->initialized || !ctx->in_frame) {
        php_error_docref(NULL, E_WARNING, "Must call vio_set_uniform between vio_begin and vio_end");
        return;
    }

    vio_apply_uniform(ctx, name, value_zval);
}

/* Batch form: one PHP->C crossing for an array of ['u_name' => value, ...].
 * Each pair is applied through the exact same core as vio_set_uniform, so the
 * cbuffer ends up byte-identical to issuing the calls individually — it just
 * avoids the per-uniform Zend call + zend_parse_parameters overhead, which is
 * the dominant cost on the hot draw path (material + per-draw uniforms). */
ZEND_FUNCTION(vio_set_uniforms)
{
    zval *ctx_zval;
    HashTable *pairs;

    ZEND_PARSE_PARAMETERS_START(2, 2)
        Z_PARAM_OBJECT_OF_CLASS(ctx_zval, vio_context_ce)
        Z_PARAM_ARRAY_HT(pairs)
    ZEND_PARSE_PARAMETERS_END();

    vio_context_object *ctx = Z_VIO_CONTEXT_P(ctx_zval);

    if (!ctx->initialized || !ctx->in_frame) {
        php_error_docref(NULL, E_WARNING, "Must call vio_set_uniforms between vio_begin and vio_end");
        return;
    }

    zend_string *key;
    zval *val;
    ZEND_HASH_FOREACH_STR_KEY_VAL(pairs, key, val) {
        if (key != NULL) {
            vio_apply_uniform(ctx, ZSTR_VAL(key), val);
        }
    } ZEND_HASH_FOREACH_END();
}

/* Batched draw submission: ONE PHP->C crossing for an ordered list of draws.
 * Each element of $draws is an associative array describing one draw:
 *
 *   [
 *     'mesh'     => VioMesh,                 // required
 *     'pipeline' => VioPipeline,             // optional; bound only when it
 *                                            //   differs from the previously
 *                                            //   bound pipeline (state-change min)
 *     'textures' => [ slot => VioTexture ],  // optional; GL slot => texture
 *     'uniforms' => [ 'u_name' => value ],   // optional; per-draw + on-change
 *                                            //   material uniform deltas
 *   ]
 *
 * Records are processed STRICTLY in array order, so the bound shader's sticky
 * cbuffer (frame globals + material + per-draw uniforms accumulate into
 * sh->cbuffer_data, and each draw snapshots the current contents into its own
 * 256-byte slice) ends up byte-identical to issuing vio_bind_pipeline /
 * vio_bind_texture / vio_set_uniforms / vio_draw individually per draw. This
 * collapses the 2N..3N FFI crossings of the per-draw path down to ONE and drops
 * the per-call zend_parse_parameters plus the per-draw uniform-array rebuild,
 * which is the dominant CPU cost on a heavy submit (hundreds of opaque meshes).
 *
 * LEVER #2 (future multithreaded recording): the per-record body below is a
 * flat, index-addressable sequence whose only cross-record state is the in-order
 * pipeline/texture/cbuffer accumulation. A threaded version would resolve each
 * record's effective pipeline + texture + uniform state, partition the record
 * index range across worker threads each recording into its OWN command list
 * (own command allocator), then ExecuteCommandLists them in order. The only
 * shared mutable D3D12 state is the per-frame linear cbuffer allocator
 * (vio_d3d12.cbuffer_*) and the pending-SRV table — both would be sub-divided
 * into per-thread sub-ranges (split THIS frame's cbuffer slice into T equal
 * sub-slices). The thread-split point is the per-record loop body marked below. */
ZEND_FUNCTION(vio_submit_batch)
{
    zval *ctx_zval;
    HashTable *draws;

    ZEND_PARSE_PARAMETERS_START(2, 2)
        Z_PARAM_OBJECT_OF_CLASS(ctx_zval, vio_context_ce)
        Z_PARAM_ARRAY_HT(draws)
    ZEND_PARSE_PARAMETERS_END();

    vio_context_object *ctx = Z_VIO_CONTEXT_P(ctx_zval);

    if (!ctx->initialized || !ctx->in_frame) {
        php_error_docref(NULL, E_WARNING, "Must call vio_submit_batch between vio_begin and vio_end");
        return;
    }

    /* Last backend pipeline bound BY this batch, so a sorted batch re-binds the
     * PSO only on a real change. NULL = none bound here yet (the caller may have
     * bound one before the batch — the opaque pass binds once, then its records
     * omit 'pipeline' entirely). */
    void *last_pipeline = NULL;

    zval *rec;
    ZEND_HASH_FOREACH_VAL(draws, rec) {
        if (Z_TYPE_P(rec) != IS_ARRAY) {
            continue;
        }
        HashTable *r = Z_ARRVAL_P(rec);

        /* ---- per-record loop body (Lever #2 thread-split point) ------------ */

        /* (1) Pipeline — bind only on change. Mirrors vio_bind_pipeline: update
         *     the context's bound-shader tracking AND dispatch the backend bind. */
        zval *pz = zend_hash_str_find(r, "pipeline", sizeof("pipeline") - 1);
        if (pz && Z_TYPE_P(pz) == IS_OBJECT &&
            instanceof_function(Z_OBJCE_P(pz), vio_pipeline_ce)) {
            vio_pipeline_object *pipe = Z_VIO_PIPELINE_P(pz);
            if (pipe->valid && pipe->backend_pipeline != last_pipeline) {
                ctx->bound_shader_program = pipe->shader_program;
                ctx->bound_shader_object = pipe->shader_ref;
                if (ctx->backend->bind_pipeline_state) {
                    ctx->backend->bind_pipeline_state(pipe);
                } else if (pipe->backend_pipeline && ctx->backend->bind_pipeline) {
                    ctx->backend->bind_pipeline(pipe->backend_pipeline);
                }
                last_pipeline = pipe->backend_pipeline;
            }
        }

        /* (2) Textures — [ GL-slot => VioTexture ]. Sticky on the backend side
         *     (pending SRVs persist until overwritten), so the caller includes
         *     only the textures this draw changes. Same remap as vio_bind_texture
         *     via the shared core. */
        zval *tz = zend_hash_str_find(r, "textures", sizeof("textures") - 1);
        if (tz && Z_TYPE_P(tz) == IS_ARRAY) {
            zend_ulong slot;
            zval *texv;
            ZEND_HASH_FOREACH_NUM_KEY_VAL(Z_ARRVAL_P(tz), slot, texv) {
                if (Z_TYPE_P(texv) == IS_OBJECT &&
                    instanceof_function(Z_OBJCE_P(texv), vio_texture_ce)) {
                    vio_texture_object *tex = Z_VIO_TEXTURE_P(texv);
                    if (tex->valid) {
                        vio_bind_texture_internal(ctx, tex, (zend_long)slot);
                    }
                }
            } ZEND_HASH_FOREACH_END();
        }

        /* (3) Uniforms — per-draw + on-change material deltas. Applied through
         *     the SAME core as vio_set_uniforms, so the cbuffer bytes are
         *     identical; only the changed offsets are touched (sticky cbuffer). */
        zval *uz = zend_hash_str_find(r, "uniforms", sizeof("uniforms") - 1);
        if (uz && Z_TYPE_P(uz) == IS_ARRAY) {
            zend_string *ukey;
            zval *uval;
            ZEND_HASH_FOREACH_STR_KEY_VAL(Z_ARRVAL_P(uz), ukey, uval) {
                if (ukey != NULL) {
                    vio_apply_uniform(ctx, ZSTR_VAL(ukey), uval);
                }
            } ZEND_HASH_FOREACH_END();
        }

        /* (4) Draw — required 'mesh'. Records the per-draw cbuffer slice +
         *     root-CBV bind + draw_indexed via the shared draw core. */
        zval *mz = zend_hash_str_find(r, "mesh", sizeof("mesh") - 1);
        if (mz && Z_TYPE_P(mz) == IS_OBJECT &&
            instanceof_function(Z_OBJCE_P(mz), vio_mesh_ce)) {
            vio_mesh_object *mesh = Z_VIO_MESH_P(mz);
            vio_submit_one(ctx, mesh);
        }
        /* -------------------------------------------------------------------- */
    } ZEND_HASH_FOREACH_END();
}

/* ── Phase 5: 2D API functions ───────────────────────────────────── */

ZEND_FUNCTION(vio_rect)
{
    zval *ctx_zval;
    double x, y, w, h;
    HashTable *opts_ht = NULL;

    ZEND_PARSE_PARAMETERS_START(5, 6)
        Z_PARAM_OBJECT_OF_CLASS(ctx_zval, vio_context_ce)
        Z_PARAM_DOUBLE(x)
        Z_PARAM_DOUBLE(y)
        Z_PARAM_DOUBLE(w)
        Z_PARAM_DOUBLE(h)
        Z_PARAM_OPTIONAL
        Z_PARAM_ARRAY_HT(opts_ht)
    ZEND_PARSE_PARAMETERS_END();

    vio_context_object *ctx = Z_VIO_CONTEXT_P(ctx_zval);
    if (!ctx->initialized || !ctx->in_frame) {
        php_error_docref(NULL, E_WARNING, "Must call vio_rect between vio_begin and vio_end");
        return;
    }

    /* Parse options */
    float cr = 1.0f, cg = 1.0f, cb = 1.0f, ca = 1.0f;
    float z = 0.0f;
    int outline = 0;
    float line_width = 1.0f;

    if (opts_ht) {
        zval *val;
        if ((val = zend_hash_str_find(opts_ht, "fill", sizeof("fill") - 1)) != NULL) {
            vio_argb_unpack((uint32_t)zval_get_long(val), &cr, &cg, &cb, &ca);
        }
        if ((val = zend_hash_str_find(opts_ht, "color", sizeof("color") - 1)) != NULL) {
            vio_argb_unpack((uint32_t)zval_get_long(val), &cr, &cg, &cb, &ca);
        }
        if ((val = zend_hash_str_find(opts_ht, "z", sizeof("z") - 1)) != NULL) {
            z = (float)zval_get_double(val);
        }
        if ((val = zend_hash_str_find(opts_ht, "outline", sizeof("outline") - 1)) != NULL) {
            outline = zend_is_true(val);
        }
        if ((val = zend_hash_str_find(opts_ht, "line_width", sizeof("line_width") - 1)) != NULL) {
            line_width = (float)zval_get_double(val);
        }
    }

    float fx = (float)x, fy = (float)y, fw = (float)w, fh = (float)h;

    /* Compute corner positions and apply transform */
    float p0x = fx, p0y = fy;
    float p1x = fx + fw, p1y = fy;
    float p2x = fx + fw, p2y = fy + fh;
    float p3x = fx, p3y = fy + fh;
    vio_2d_apply_transform(&ctx->state_2d, &p0x, &p0y);
    vio_2d_apply_transform(&ctx->state_2d, &p1x, &p1y);
    vio_2d_apply_transform(&ctx->state_2d, &p2x, &p2y);
    vio_2d_apply_transform(&ctx->state_2d, &p3x, &p3y);

    if (outline) {
        /* Generate 4 thin filled rectangles as outline (GL_LINE_LOOP is 1px max on macOS Core Profile) */
        float lw = line_width;
        /* Top: (x, y) -> (x+w, y+lw) */
        float t0x = fx, t0y = fy, t1x = fx+fw, t1y = fy, t2x = fx+fw, t2y = fy+lw, t3x = fx, t3y = fy+lw;
        /* Bottom: (x, y+h-lw) -> (x+w, y+h) */
        float b0x = fx, b0y = fy+fh-lw, b1x = fx+fw, b1y = fy+fh-lw, b2x = fx+fw, b2y = fy+fh, b3x = fx, b3y = fy+fh;
        /* Left: (x, y+lw) -> (x+lw, y+h-lw) */
        float l0x = fx, l0y = fy+lw, l1x = fx+lw, l1y = fy+lw, l2x = fx+lw, l2y = fy+fh-lw, l3x = fx, l3y = fy+fh-lw;
        /* Right: (x+w-lw, y+lw) -> (x+w, y+h-lw) */
        float r0x = fx+fw-lw, r0y = fy+lw, r1x = fx+fw, r1y = fy+lw, r2x = fx+fw, r2y = fy+fh-lw, r3x = fx+fw-lw, r3y = fy+fh-lw;

        /* Apply transforms */
        vio_2d_apply_transform(&ctx->state_2d, &t0x, &t0y); vio_2d_apply_transform(&ctx->state_2d, &t1x, &t1y);
        vio_2d_apply_transform(&ctx->state_2d, &t2x, &t2y); vio_2d_apply_transform(&ctx->state_2d, &t3x, &t3y);
        vio_2d_apply_transform(&ctx->state_2d, &b0x, &b0y); vio_2d_apply_transform(&ctx->state_2d, &b1x, &b1y);
        vio_2d_apply_transform(&ctx->state_2d, &b2x, &b2y); vio_2d_apply_transform(&ctx->state_2d, &b3x, &b3y);
        vio_2d_apply_transform(&ctx->state_2d, &l0x, &l0y); vio_2d_apply_transform(&ctx->state_2d, &l1x, &l1y);
        vio_2d_apply_transform(&ctx->state_2d, &l2x, &l2y); vio_2d_apply_transform(&ctx->state_2d, &l3x, &l3y);
        vio_2d_apply_transform(&ctx->state_2d, &r0x, &r0y); vio_2d_apply_transform(&ctx->state_2d, &r1x, &r1y);
        vio_2d_apply_transform(&ctx->state_2d, &r2x, &r2y); vio_2d_apply_transform(&ctx->state_2d, &r3x, &r3y);

        vio_2d_vertex verts[24] = {
            /* Top */
            {t0x,t0y, 0,0, cr,cg,cb,ca}, {t1x,t1y, 0,0, cr,cg,cb,ca}, {t2x,t2y, 0,0, cr,cg,cb,ca},
            {t0x,t0y, 0,0, cr,cg,cb,ca}, {t2x,t2y, 0,0, cr,cg,cb,ca}, {t3x,t3y, 0,0, cr,cg,cb,ca},
            /* Bottom */
            {b0x,b0y, 0,0, cr,cg,cb,ca}, {b1x,b1y, 0,0, cr,cg,cb,ca}, {b2x,b2y, 0,0, cr,cg,cb,ca},
            {b0x,b0y, 0,0, cr,cg,cb,ca}, {b2x,b2y, 0,0, cr,cg,cb,ca}, {b3x,b3y, 0,0, cr,cg,cb,ca},
            /* Left */
            {l0x,l0y, 0,0, cr,cg,cb,ca}, {l1x,l1y, 0,0, cr,cg,cb,ca}, {l2x,l2y, 0,0, cr,cg,cb,ca},
            {l0x,l0y, 0,0, cr,cg,cb,ca}, {l2x,l2y, 0,0, cr,cg,cb,ca}, {l3x,l3y, 0,0, cr,cg,cb,ca},
            /* Right */
            {r0x,r0y, 0,0, cr,cg,cb,ca}, {r1x,r1y, 0,0, cr,cg,cb,ca}, {r2x,r2y, 0,0, cr,cg,cb,ca},
            {r0x,r0y, 0,0, cr,cg,cb,ca}, {r2x,r2y, 0,0, cr,cg,cb,ca}, {r3x,r3y, 0,0, cr,cg,cb,ca},
        };
        int start = vio_2d_push_vertices(&ctx->state_2d, verts, 24);
        if (start >= 0) vio_2d_push_item(&ctx->state_2d, VIO_2D_RECT_OUTLINE, z, 0, NULL, start, 24);
    } else {
        /* 6 vertices for 2 triangles */
        vio_2d_vertex verts[6] = {
            {p0x, p0y, 0, 0, cr, cg, cb, ca},
            {p1x, p1y, 0, 0, cr, cg, cb, ca},
            {p2x, p2y, 0, 0, cr, cg, cb, ca},
            {p0x, p0y, 0, 0, cr, cg, cb, ca},
            {p2x, p2y, 0, 0, cr, cg, cb, ca},
            {p3x, p3y, 0, 0, cr, cg, cb, ca},
        };
        int start = vio_2d_push_vertices(&ctx->state_2d, verts, 6);
        if (start >= 0) vio_2d_push_item(&ctx->state_2d, VIO_2D_RECT, z, 0, NULL, start, 6);
    }
}

ZEND_FUNCTION(vio_circle)
{
    zval *ctx_zval;
    double cx_d, cy_d, radius;
    HashTable *opts_ht = NULL;

    ZEND_PARSE_PARAMETERS_START(4, 5)
        Z_PARAM_OBJECT_OF_CLASS(ctx_zval, vio_context_ce)
        Z_PARAM_DOUBLE(cx_d)
        Z_PARAM_DOUBLE(cy_d)
        Z_PARAM_DOUBLE(radius)
        Z_PARAM_OPTIONAL
        Z_PARAM_ARRAY_HT(opts_ht)
    ZEND_PARSE_PARAMETERS_END();

    vio_context_object *ctx = Z_VIO_CONTEXT_P(ctx_zval);
    if (!ctx->initialized || !ctx->in_frame) {
        php_error_docref(NULL, E_WARNING, "Must call vio_circle between vio_begin and vio_end");
        return;
    }

    float cr = 1.0f, cg = 1.0f, cb = 1.0f, ca = 1.0f;
    float z = 0.0f;
    int outline = 0;
    float line_width = 2.0f;
    int segments = VIO_2D_CIRCLE_SEGS;

    if (opts_ht) {
        zval *val;
        if ((val = zend_hash_str_find(opts_ht, "fill", sizeof("fill") - 1)) != NULL) {
            vio_argb_unpack((uint32_t)zval_get_long(val), &cr, &cg, &cb, &ca);
        }
        if ((val = zend_hash_str_find(opts_ht, "color", sizeof("color") - 1)) != NULL) {
            vio_argb_unpack((uint32_t)zval_get_long(val), &cr, &cg, &cb, &ca);
        }
        if ((val = zend_hash_str_find(opts_ht, "z", sizeof("z") - 1)) != NULL) {
            z = (float)zval_get_double(val);
        }
        if ((val = zend_hash_str_find(opts_ht, "outline", sizeof("outline") - 1)) != NULL) {
            outline = zend_is_true(val);
        }
        if ((val = zend_hash_str_find(opts_ht, "line_width", sizeof("line_width") - 1)) != NULL) {
            line_width = (float)zval_get_double(val);
        }
        if ((val = zend_hash_str_find(opts_ht, "segments", sizeof("segments") - 1)) != NULL) {
            segments = (int)zval_get_long(val);
            if (segments < 3) segments = 3;
            if (segments > 128) segments = 128;
        }
    }

    float fcx = (float)cx_d, fcy = (float)cy_d, fr = (float)radius;

    if (outline) {
        /* Generate ring of quads (GL_LINE_LOOP is 1px max on macOS Core Profile) */
        float lw = line_width;
        float r_inner = fr - lw * 0.5f;
        float r_outer = fr + lw * 0.5f;
        if (r_inner < 0) r_inner = 0;
        int vert_count = segments * 6;
        vio_2d_vertex *verts = emalloc(sizeof(vio_2d_vertex) * vert_count);
        int vi = 0;
        for (int i = 0; i < segments; i++) {
            float a1 = (float)i / (float)segments * 2.0f * (float)M_PI;
            float a2 = (float)(i + 1) / (float)segments * 2.0f * (float)M_PI;
            float ix1 = fcx + cosf(a1) * r_inner, iy1 = fcy + sinf(a1) * r_inner;
            float ox1 = fcx + cosf(a1) * r_outer, oy1 = fcy + sinf(a1) * r_outer;
            float ix2 = fcx + cosf(a2) * r_inner, iy2 = fcy + sinf(a2) * r_inner;
            float ox2 = fcx + cosf(a2) * r_outer, oy2 = fcy + sinf(a2) * r_outer;
            vio_2d_apply_transform(&ctx->state_2d, &ix1, &iy1);
            vio_2d_apply_transform(&ctx->state_2d, &ox1, &oy1);
            vio_2d_apply_transform(&ctx->state_2d, &ix2, &iy2);
            vio_2d_apply_transform(&ctx->state_2d, &ox2, &oy2);
            verts[vi++] = (vio_2d_vertex){ox1, oy1, 0, 0, cr, cg, cb, ca};
            verts[vi++] = (vio_2d_vertex){ix1, iy1, 0, 0, cr, cg, cb, ca};
            verts[vi++] = (vio_2d_vertex){ix2, iy2, 0, 0, cr, cg, cb, ca};
            verts[vi++] = (vio_2d_vertex){ox1, oy1, 0, 0, cr, cg, cb, ca};
            verts[vi++] = (vio_2d_vertex){ix2, iy2, 0, 0, cr, cg, cb, ca};
            verts[vi++] = (vio_2d_vertex){ox2, oy2, 0, 0, cr, cg, cb, ca};
        }
        int start = vio_2d_push_vertices(&ctx->state_2d, verts, vi);
        if (start >= 0) vio_2d_push_item(&ctx->state_2d, VIO_2D_CIRCLE_OUTLINE, z, 0, NULL, start, vi);
        efree(verts);
    } else {
        /* Triangle fan as individual triangles */
        int tri_count = segments;
        vio_2d_vertex *verts = emalloc(sizeof(vio_2d_vertex) * tri_count * 3);
        float tcx = fcx, tcy = fcy;
        vio_2d_apply_transform(&ctx->state_2d, &tcx, &tcy);
        for (int i = 0; i < tri_count; i++) {
            float a1 = (float)i / (float)segments * 2.0f * 3.14159265f;
            float a2 = (float)(i + 1) / (float)segments * 2.0f * 3.14159265f;
            float p1x = fcx + cosf(a1) * fr, p1y = fcy + sinf(a1) * fr;
            float p2x = fcx + cosf(a2) * fr, p2y = fcy + sinf(a2) * fr;
            vio_2d_apply_transform(&ctx->state_2d, &p1x, &p1y);
            vio_2d_apply_transform(&ctx->state_2d, &p2x, &p2y);
            verts[i * 3 + 0] = (vio_2d_vertex){tcx, tcy, 0, 0, cr, cg, cb, ca};
            verts[i * 3 + 1] = (vio_2d_vertex){p1x, p1y, 0, 0, cr, cg, cb, ca};
            verts[i * 3 + 2] = (vio_2d_vertex){p2x, p2y, 0, 0, cr, cg, cb, ca};
        }
        int start = vio_2d_push_vertices(&ctx->state_2d, verts, tri_count * 3);
        if (start >= 0) vio_2d_push_item(&ctx->state_2d, VIO_2D_CIRCLE, z, 0, NULL, start, tri_count * 3);
        efree(verts);
    }
}

ZEND_FUNCTION(vio_line)
{
    zval *ctx_zval;
    double x1, y1, x2, y2;
    HashTable *opts_ht = NULL;

    ZEND_PARSE_PARAMETERS_START(5, 6)
        Z_PARAM_OBJECT_OF_CLASS(ctx_zval, vio_context_ce)
        Z_PARAM_DOUBLE(x1)
        Z_PARAM_DOUBLE(y1)
        Z_PARAM_DOUBLE(x2)
        Z_PARAM_DOUBLE(y2)
        Z_PARAM_OPTIONAL
        Z_PARAM_ARRAY_HT(opts_ht)
    ZEND_PARSE_PARAMETERS_END();

    vio_context_object *ctx = Z_VIO_CONTEXT_P(ctx_zval);
    if (!ctx->initialized || !ctx->in_frame) {
        php_error_docref(NULL, E_WARNING, "Must call vio_line between vio_begin and vio_end");
        return;
    }

    float cr = 1.0f, cg = 1.0f, cb = 1.0f, ca = 1.0f;
    float z = 0.0f;
    float line_width = 1.0f;

    if (opts_ht) {
        zval *val;
        if ((val = zend_hash_str_find(opts_ht, "color", sizeof("color") - 1)) != NULL) {
            vio_argb_unpack((uint32_t)zval_get_long(val), &cr, &cg, &cb, &ca);
        }
        if ((val = zend_hash_str_find(opts_ht, "z", sizeof("z") - 1)) != NULL) {
            z = (float)zval_get_double(val);
        }
        if ((val = zend_hash_str_find(opts_ht, "width", sizeof("width") - 1)) != NULL) {
            line_width = (float)zval_get_double(val);
        }
    }

    float lx1 = (float)x1, ly1 = (float)y1, lx2 = (float)x2, ly2 = (float)y2;

    /* Generate a quad with perpendicular width (GL_LINES is 1px max on macOS Core Profile) */
    float dx = lx2 - lx1, dy = ly2 - ly1;
    float len = sqrtf(dx * dx + dy * dy);
    if (len < 0.001f) return;
    float nx = (-dy / len) * line_width * 0.5f;
    float ny = ( dx / len) * line_width * 0.5f;

    float p0x = lx1 + nx, p0y = ly1 + ny;
    float p1x = lx1 - nx, p1y = ly1 - ny;
    float p2x = lx2 - nx, p2y = ly2 - ny;
    float p3x = lx2 + nx, p3y = ly2 + ny;
    vio_2d_apply_transform(&ctx->state_2d, &p0x, &p0y);
    vio_2d_apply_transform(&ctx->state_2d, &p1x, &p1y);
    vio_2d_apply_transform(&ctx->state_2d, &p2x, &p2y);
    vio_2d_apply_transform(&ctx->state_2d, &p3x, &p3y);

    vio_2d_vertex verts[6] = {
        {p0x, p0y, 0, 0, cr, cg, cb, ca},
        {p1x, p1y, 0, 0, cr, cg, cb, ca},
        {p2x, p2y, 0, 0, cr, cg, cb, ca},
        {p0x, p0y, 0, 0, cr, cg, cb, ca},
        {p2x, p2y, 0, 0, cr, cg, cb, ca},
        {p3x, p3y, 0, 0, cr, cg, cb, ca},
    };
    int start = vio_2d_push_vertices(&ctx->state_2d, verts, 6);
    if (start >= 0) vio_2d_push_item(&ctx->state_2d, VIO_2D_LINE, z, 0, NULL, start, 6);
}

ZEND_FUNCTION(vio_sprite)
{
    zval *ctx_zval;
    zval *tex_zval;
    HashTable *opts_ht = NULL;

    ZEND_PARSE_PARAMETERS_START(2, 3)
        Z_PARAM_OBJECT_OF_CLASS(ctx_zval, vio_context_ce)
        Z_PARAM_OBJECT_OF_CLASS(tex_zval, vio_texture_ce)
        Z_PARAM_OPTIONAL
        Z_PARAM_ARRAY_HT(opts_ht)
    ZEND_PARSE_PARAMETERS_END();

    vio_context_object *ctx = Z_VIO_CONTEXT_P(ctx_zval);
    if (!ctx->initialized || !ctx->in_frame) {
        php_error_docref(NULL, E_WARNING, "Must call vio_sprite between vio_begin and vio_end");
        return;
    }

    vio_texture_object *tex = Z_VIO_TEXTURE_P(tex_zval);
    if (!tex->valid) return;

    float x = 0, y = 0, w = (float)tex->width, h = (float)tex->height;
    float sx = 1.0f, sy = 1.0f;
    float cr = 1.0f, cg = 1.0f, cb = 1.0f, ca = 1.0f;
    float z = 0.0f;

    /* Source region (default: full texture) */
    float src_x = 0, src_y = 0;
    float src_w = (float)tex->width, src_h = (float)tex->height;
    int has_src = 0;

    if (opts_ht) {
        zval *val;
        if ((val = zend_hash_str_find(opts_ht, "x", sizeof("x") - 1)) != NULL) x = (float)zval_get_double(val);
        if ((val = zend_hash_str_find(opts_ht, "y", sizeof("y") - 1)) != NULL) y = (float)zval_get_double(val);
        if ((val = zend_hash_str_find(opts_ht, "width", sizeof("width") - 1)) != NULL) w = (float)zval_get_double(val);
        if ((val = zend_hash_str_find(opts_ht, "height", sizeof("height") - 1)) != NULL) h = (float)zval_get_double(val);
        if ((val = zend_hash_str_find(opts_ht, "scale_x", sizeof("scale_x") - 1)) != NULL) sx = (float)zval_get_double(val);
        if ((val = zend_hash_str_find(opts_ht, "scale_y", sizeof("scale_y") - 1)) != NULL) sy = (float)zval_get_double(val);
        if ((val = zend_hash_str_find(opts_ht, "color", sizeof("color") - 1)) != NULL) {
            vio_argb_unpack((uint32_t)zval_get_long(val), &cr, &cg, &cb, &ca);
        }
        if ((val = zend_hash_str_find(opts_ht, "z", sizeof("z") - 1)) != NULL) z = (float)zval_get_double(val);

        /* Source region options */
        if ((val = zend_hash_str_find(opts_ht, "src_x", sizeof("src_x") - 1)) != NULL) {
            src_x = (float)zval_get_double(val); has_src = 1;
        }
        if ((val = zend_hash_str_find(opts_ht, "src_y", sizeof("src_y") - 1)) != NULL) {
            src_y = (float)zval_get_double(val); has_src = 1;
        }
        if ((val = zend_hash_str_find(opts_ht, "src_w", sizeof("src_w") - 1)) != NULL) {
            src_w = (float)zval_get_double(val); has_src = 1;
        }
        if ((val = zend_hash_str_find(opts_ht, "src_h", sizeof("src_h") - 1)) != NULL) {
            src_h = (float)zval_get_double(val); has_src = 1;
        }
    }

    /* Compute UV coordinates from source region */
    float u0 = 0.0f, v0 = 0.0f, u1 = 1.0f, v1 = 1.0f;
    if (has_src && tex->width > 0 && tex->height > 0) {
        float inv_tw = 1.0f / (float)tex->width;
        float inv_th = 1.0f / (float)tex->height;
        u0 = src_x * inv_tw;
        v0 = src_y * inv_th;
        u1 = (src_x + src_w) * inv_tw;
        v1 = (src_y + src_h) * inv_th;
    }

    float dw = w * sx, dh = h * sy;

    /* Apply transform to sprite corners */
    float p0x = x, p0y = y;
    float p1x = x + dw, p1y = y;
    float p2x = x + dw, p2y = y + dh;
    float p3x = x, p3y = y + dh;
    vio_2d_apply_transform(&ctx->state_2d, &p0x, &p0y);
    vio_2d_apply_transform(&ctx->state_2d, &p1x, &p1y);
    vio_2d_apply_transform(&ctx->state_2d, &p2x, &p2y);
    vio_2d_apply_transform(&ctx->state_2d, &p3x, &p3y);

    vio_2d_vertex verts[6] = {
        {p0x, p0y, u0, v0, cr, cg, cb, ca},
        {p1x, p1y, u1, v0, cr, cg, cb, ca},
        {p2x, p2y, u1, v1, cr, cg, cb, ca},
        {p0x, p0y, u0, v0, cr, cg, cb, ca},
        {p2x, p2y, u1, v1, cr, cg, cb, ca},
        {p3x, p3y, u0, v1, cr, cg, cb, ca},
    };
    int start = vio_2d_push_vertices(&ctx->state_2d, verts, 6);
    if (start >= 0) vio_2d_push_item(&ctx->state_2d, VIO_2D_SPRITE, z, tex->texture_id, tex->backend_texture, start, 6);
}

/* Upload a freshly-packed R8 atlas bitmap into the font's GPU resource.
 *
 * MUST run on the render thread — every branch here issues GPU work (texture
 * creation / atlas upload) against the currently-current GL / Metal / D3D /
 * Vulkan context. Shared by the synchronous vio_font() path and the async
 * completion path (vio_font_load_poll), so the backend-specific atlas handling
 * stays in exactly one place.
 *
 * `backend` is passed explicitly (rather than read from a context) so the async
 * path can use the backend captured at submit time. atlas_bitmap is the R8
 * coverage atlas; it is not freed here (caller owns it). */
void vio_font_upload_atlas_to_gpu(vio_font_object *font,
                                  const vio_backend *backend,
                                  unsigned char *atlas_bitmap)
{
    if (backend && backend->upload_font_atlas) {
        backend->upload_font_atlas(font, font->atlas_w, font->atlas_h, atlas_bitmap, 1);
    }

#ifdef HAVE_D3D11
    if (backend && strcmp(backend->name, "d3d11") == 0 && vio_d3d11.initialized) {
        /* Upload the R8 coverage atlas directly as a single-channel texture —
         * no white-RGB/coverage-alpha RGBA8 expansion. ps_text samples .r as
         * the glyph alpha, so the upload is 1/4 the size and skips the per-texel
         * conversion loop. */
        vio_texture_desc desc = {0};
        desc.width  = font->atlas_w;
        desc.height = font->atlas_h;
        desc.data   = atlas_bitmap;
        desc.filter = VIO_FILTER_LINEAR;
        desc.wrap   = VIO_WRAP_CLAMP;
        desc.mipmaps = 0;
        desc.single_channel = 1;
        font->atlas_backend_texture = backend->create_texture(&desc);
    }
#endif

#ifdef HAVE_D3D12
    if (backend && strcmp(backend->name, "d3d12") == 0 && vio_d3d12.initialized) {
        /* R8 coverage atlas uploaded directly; the SRV swizzles it to
         * (1,1,1,R) so the shared sprite pipeline renders glyphs unchanged. */
        vio_texture_desc desc = {0};
        desc.width  = font->atlas_w;
        desc.height = font->atlas_h;
        desc.data   = atlas_bitmap;
        desc.filter = VIO_FILTER_LINEAR;
        desc.wrap   = VIO_WRAP_CLAMP;
        desc.mipmaps = 0;
        desc.single_channel = 1;
        font->atlas_backend_texture = backend->create_texture(&desc);
    }
#endif

#ifdef HAVE_VULKAN
    if (backend && strcmp(backend->name, "vulkan") == 0 && vio_vk.initialized) {
        /* Upload the R8 coverage atlas directly; the image view swizzles it to
         * (1,1,1,R) (white RGB, coverage in alpha) so the single sprites
         * pipeline serves both PNG sprites and glyphs — no RGBA8 expansion. */
        vio_texture_desc desc = {0};
        desc.width  = font->atlas_w;
        desc.height = font->atlas_h;
        desc.data   = atlas_bitmap;
        desc.filter = VIO_FILTER_LINEAR;
        desc.wrap   = VIO_WRAP_CLAMP;
        desc.mipmaps = 0;
        desc.single_channel = 1;
        font->atlas_backend_texture = backend->create_texture(&desc);
    }
#endif
}

ZEND_FUNCTION(vio_font)
{
    zval *ctx_zval;
    char *path;
    size_t path_len;
    double size = 24.0;
    double scale = 1.0;

    ZEND_PARSE_PARAMETERS_START(2, 4)
        Z_PARAM_OBJECT_OF_CLASS(ctx_zval, vio_context_ce)
        Z_PARAM_STRING(path, path_len)
        Z_PARAM_OPTIONAL
        Z_PARAM_DOUBLE(size)
        Z_PARAM_DOUBLE(scale)
    ZEND_PARSE_PARAMETERS_END();

    vio_context_object *ctx = Z_VIO_CONTEXT_P(ctx_zval);
    if (!ctx->initialized) {
        php_error_docref(NULL, E_WARNING, "Context is not initialized");
        RETURN_FALSE;
    }

    /* devicePixelRatio: the atlas is rasterized at size*scale physical pixels
     * but every glyph metric is reported back divided by render_scale, so the
     * caller still positions text in logical (size) units while the texture
     * carries enough detail to stay crisp when a transform magnifies it. */
    if (scale < 1.0) scale = 1.0;

    /* Read TTF file */
    php_stream *stream = php_stream_open_wrapper(path, "rb", REPORT_ERRORS, NULL);
    if (!stream) {
        php_error_docref(NULL, E_WARNING, "Failed to open font file: %s", path);
        RETURN_FALSE;
    }

    zend_string *contents = php_stream_copy_to_mem(stream, PHP_STREAM_COPY_ALL, 0);
    php_stream_close(stream);

    if (!contents) {
        php_error_docref(NULL, E_WARNING, "Failed to read font file: %s", path);
        RETURN_FALSE;
    }

    /* Create VioFont object */
    zval font_zval;
    object_init_ex(&font_zval, vio_font_ce);
    vio_font_object *font = Z_VIO_FONT_P(&font_zval);

    font->render_scale = (float)scale;
    font->font_size = (float)(size * scale);
    font->backend   = ctx->backend;
    font->ttf_len = ZSTR_LEN(contents);
    font->ttf_data = emalloc(font->ttf_len);
    memcpy(font->ttf_data, ZSTR_VAL(contents), font->ttf_len);
    zend_string_release(contents);

    int shaping_active = 0;
#ifdef HAVE_HARFBUZZ
    /* Preferred path: build the glyph-index atlas + HarfBuzz font. This packs
     * every glyph (by index) and uploads once, so the atlas handle is stable
     * for the font's life. On success the legacy codepoint pack is skipped;
     * all text then flows through the shaping pipeline (vio_text_shape.c). */
    shaping_active = vio_text_shape_init_font(font);
#endif

    if (!shaping_active) {
        /* Legacy path: multi-range atlas packing (Latin, Cyrillic, Greek, CJK,
         * Hangul, ...), dynamically sized to this font's glyph set. */
        unsigned char *atlas_bitmap = NULL;
        int atlas_side = 0;
        vio_font_packed_glyph *glyphs = NULL;
        int glyph_count = 0;
        vio_font_pack_atlas_dynamic(font->ttf_data, font->font_size,
                                    &atlas_bitmap, &atlas_side, &glyphs, &glyph_count);
        if (atlas_bitmap) {
            font->atlas_w = font->atlas_h = atlas_side;
            if (glyphs) {
                vio_font_finalize_glyphs(font, glyphs, glyph_count);
                free(glyphs);
            }
            vio_font_upload_atlas_to_gpu(font, ctx->backend, atlas_bitmap);
            free(atlas_bitmap);
        }
    }

    font->valid = 1;
    RETURN_COPY_VALUE(&font_zval);
}

/**
 * Decode one UTF-8 codepoint from `text` starting at position `*i`.
 * Advances `*i` past the decoded sequence. Returns the codepoint,
 * or 0 on invalid/incomplete sequences.
 */
static inline uint32_t vio_utf8_decode(const char *text, size_t text_len, size_t *i)
{
    unsigned char c = (unsigned char)text[*i];
    uint32_t cp = 0;
    int extra = 0;

    if (c < 0x80) {
        cp = c;
    } else if ((c & 0xE0) == 0xC0) {
        cp = c & 0x1F;
        extra = 1;
    } else if ((c & 0xF0) == 0xE0) {
        cp = c & 0x0F;
        extra = 2;
    } else if ((c & 0xF8) == 0xF0) {
        cp = c & 0x07;
        extra = 3;
    } else {
        /* Invalid lead byte — skip */
        return 0;
    }

    for (int j = 0; j < extra; j++) {
        if (*i + 1 + j >= text_len) return 0;
        unsigned char cont = (unsigned char)text[*i + 1 + j];
        if ((cont & 0xC0) != 0x80) return 0;
        cp = (cp << 6) | (cont & 0x3F);
    }
    *i += extra; /* caller already increments by 1 */
    return cp;
}

ZEND_FUNCTION(vio_text)
{
    zval *ctx_zval;
    zval *font_zval;
    char *text;
    size_t text_len;
    double x, y;
    HashTable *opts_ht = NULL;

    ZEND_PARSE_PARAMETERS_START(5, 6)
        Z_PARAM_OBJECT_OF_CLASS(ctx_zval, vio_context_ce)
        Z_PARAM_OBJECT_OF_CLASS(font_zval, vio_font_ce)
        Z_PARAM_STRING(text, text_len)
        Z_PARAM_DOUBLE(x)
        Z_PARAM_DOUBLE(y)
        Z_PARAM_OPTIONAL
        Z_PARAM_ARRAY_HT(opts_ht)
    ZEND_PARSE_PARAMETERS_END();

    vio_context_object *ctx = Z_VIO_CONTEXT_P(ctx_zval);
    if (!ctx->initialized || !ctx->in_frame) {
        php_error_docref(NULL, E_WARNING, "Must call vio_text between vio_begin and vio_end");
        return;
    }

    vio_font_object *font = Z_VIO_FONT_P(font_zval);
    if (!font->valid) return;

    float cr = 1.0f, cg = 1.0f, cb = 1.0f, ca = 1.0f;
    float z = 0.0f;
    float max_width = 0.0f, line_height = 0.0f;

    if (opts_ht) {
        zval *val;
        if ((val = zend_hash_str_find(opts_ht, "color", sizeof("color") - 1)) != NULL) {
            vio_argb_unpack((uint32_t)zval_get_long(val), &cr, &cg, &cb, &ca);
        }
        if ((val = zend_hash_str_find(opts_ht, "z", sizeof("z") - 1)) != NULL) {
            z = (float)zval_get_double(val);
        }
        /* Wrapping: max_width (logical px, > 0 enables soft word wrap) and an
         * optional line_height override (logical px). '\n' always hard-breaks. */
        if ((val = zend_hash_str_find(opts_ht, "max_width", sizeof("max_width") - 1)) != NULL) {
            max_width = (float)zval_get_double(val);
        }
        if ((val = zend_hash_str_find(opts_ht, "line_height", sizeof("line_height") - 1)) != NULL) {
            line_height = (float)zval_get_double(val);
        }
    }

#ifdef HAVE_HARFBUZZ
    if (vio_text_shape_available(font)) {
        vio_text_shape_draw(ctx, font, text, text_len,
                            (float)x, (float)y, z, cr, cg, cb, ca,
                            max_width, line_height);
        return;
    }
#endif

    float fx = (float)x, fy = (float)y;
    float inv_w = 1.0f / (float)font->atlas_w;
    float inv_h = 1.0f / (float)font->atlas_h;
    /* devicePixelRatio: glyph metrics live in atlas (physical) pixels; divide
     * them by render_scale so the quad occupies its logical size while sampling
     * the higher-resolution atlas. inv_rs == 1.0 for the default scale=1 path. */
    float inv_rs = (font->render_scale > 0.0f) ? (1.0f / font->render_scale) : 1.0f;

    /* Generate quads for each character (UTF-8 aware, hashmap lookup) */
    for (size_t i = 0; i < text_len; i++) {
        uint32_t cp = vio_utf8_decode(text, text_len, &i);
        zval *entry = zend_hash_index_find(&font->glyph_map, (zend_long)cp);
        if (!entry) continue;
        vio_stbtt_packedchar *b = (vio_stbtt_packedchar *)Z_STRVAL_P(entry);

        float px = fx + b->xoff * inv_rs;
        float py = fy + b->yoff * inv_rs;
        float pw = (float)(b->x1 - b->x0) * inv_rs;
        float ph = (float)(b->y1 - b->y0) * inv_rs;

        float u0 = b->x0 * inv_w;
        float v0 = b->y0 * inv_h;
        float u1 = b->x1 * inv_w;
        float v1 = b->y1 * inv_h;

        /* Apply transform to glyph corners */
        float g0x = px, g0y = py;
        float g1x = px + pw, g1y = py;
        float g2x = px + pw, g2y = py + ph;
        float g3x = px, g3y = py + ph;
        vio_2d_apply_transform(&ctx->state_2d, &g0x, &g0y);
        vio_2d_apply_transform(&ctx->state_2d, &g1x, &g1y);
        vio_2d_apply_transform(&ctx->state_2d, &g2x, &g2y);
        vio_2d_apply_transform(&ctx->state_2d, &g3x, &g3y);

        vio_2d_vertex verts[6] = {
            {g0x, g0y, u0, v0, cr, cg, cb, ca},
            {g1x, g1y, u1, v0, cr, cg, cb, ca},
            {g2x, g2y, u1, v1, cr, cg, cb, ca},
            {g0x, g0y, u0, v0, cr, cg, cb, ca},
            {g2x, g2y, u1, v1, cr, cg, cb, ca},
            {g3x, g3y, u0, v1, cr, cg, cb, ca},
        };
        int start = vio_2d_push_vertices(&ctx->state_2d, verts, 6);
        if (start >= 0) vio_2d_push_item(&ctx->state_2d, VIO_2D_TEXT, z, font->atlas_texture, font->atlas_backend_texture, start, 6);

        fx += b->xadvance * inv_rs;
    }
}

ZEND_FUNCTION(vio_draw_2d)
{
    zval *ctx_zval;

    ZEND_PARSE_PARAMETERS_START(1, 1)
        Z_PARAM_OBJECT_OF_CLASS(ctx_zval, vio_context_ce)
    ZEND_PARSE_PARAMETERS_END();

    vio_context_object *ctx = Z_VIO_CONTEXT_P(ctx_zval);

    if (!ctx->initialized || !ctx->in_frame) {
        php_error_docref(NULL, E_WARNING, "Must call vio_draw_2d between vio_begin and vio_end");
        return;
    }

    vio_2d_flush(&ctx->state_2d);
}

/* ── Rounded rect ────────────────────────────────────────────────── */

ZEND_FUNCTION(vio_rounded_rect)
{
    zval *ctx_zval;
    double x, y, w, h, radius;
    HashTable *opts_ht = NULL;

    ZEND_PARSE_PARAMETERS_START(6, 7)
        Z_PARAM_OBJECT_OF_CLASS(ctx_zval, vio_context_ce)
        Z_PARAM_DOUBLE(x)
        Z_PARAM_DOUBLE(y)
        Z_PARAM_DOUBLE(w)
        Z_PARAM_DOUBLE(h)
        Z_PARAM_DOUBLE(radius)
        Z_PARAM_OPTIONAL
        Z_PARAM_ARRAY_HT(opts_ht)
    ZEND_PARSE_PARAMETERS_END();

    vio_context_object *ctx = Z_VIO_CONTEXT_P(ctx_zval);
    if (!ctx->initialized || !ctx->in_frame) {
        php_error_docref(NULL, E_WARNING, "Must call vio_rounded_rect between vio_begin and vio_end");
        return;
    }

    float cr = 1.0f, cg = 1.0f, cb = 1.0f, ca = 1.0f;
    float z = 0.0f;
    int outline = 0;
    float line_width = 2.0f;

    if (opts_ht) {
        zval *val;
        if ((val = zend_hash_str_find(opts_ht, "fill", sizeof("fill") - 1)) != NULL)
            vio_argb_unpack((uint32_t)zval_get_long(val), &cr, &cg, &cb, &ca);
        if ((val = zend_hash_str_find(opts_ht, "color", sizeof("color") - 1)) != NULL)
            vio_argb_unpack((uint32_t)zval_get_long(val), &cr, &cg, &cb, &ca);
        if ((val = zend_hash_str_find(opts_ht, "z", sizeof("z") - 1)) != NULL)
            z = (float)zval_get_double(val);
        if ((val = zend_hash_str_find(opts_ht, "outline", sizeof("outline") - 1)) != NULL)
            outline = zend_is_true(val);
        if ((val = zend_hash_str_find(opts_ht, "line_width", sizeof("line_width") - 1)) != NULL)
            line_width = (float)zval_get_double(val);
    }

    float fx = (float)x, fy = (float)y, fw = (float)w, fh = (float)h;
    float fr = (float)radius;

    /* Clamp radius to half the smaller dimension */
    float max_r = fminf(fw, fh) * 0.5f;
    if (fr > max_r) fr = max_r;
    if (fr < 0) fr = 0;

    int segs = VIO_2D_CORNER_SEGS;

    if (outline) {
        /* Outline: quad ring along the rounded rect path (GL_LINE_LOOP is 1px max on macOS) */
        float lw = line_width;
        float r_inner = fr - lw * 0.5f;
        float r_outer = fr + lw * 0.5f;
        if (r_inner < 0) r_inner = 0;

        /* Build inner and outer perimeter points */
        int pts_per_corner = segs + 1;
        int total_pts = pts_per_corner * 4;
        float *inner = emalloc(sizeof(float) * total_pts * 2);
        float *outer = emalloc(sizeof(float) * total_pts * 2);
        int pi = 0;

        /* For inner perimeter, inset the straight edges by lw/2 as well */
        float inset = lw * 0.5f;

        float corner_centers[4][3] = {
            {fx + fr,      fy + fr,      (float)M_PI},
            {fx + fw - fr, fy + fr,      (float)M_PI * 1.5f},
            {fx + fw - fr, fy + fh - fr, 0.0f},
            {fx + fr,      fy + fh - fr, (float)M_PI * 0.5f},
        };

        for (int c = 0; c < 4; c++) {
            float ccx = corner_centers[c][0], ccy = corner_centers[c][1], start_a = corner_centers[c][2];
            for (int s = 0; s <= segs; s++) {
                float angle = start_a + ((float)s / (float)segs) * ((float)M_PI * 0.5f);
                float cs = cosf(angle), sn = sinf(angle);
                float ipx = ccx + cs * r_inner, ipy = ccy + sn * r_inner;
                float opx = ccx + cs * r_outer, opy = ccy + sn * r_outer;
                vio_2d_apply_transform(&ctx->state_2d, &ipx, &ipy);
                vio_2d_apply_transform(&ctx->state_2d, &opx, &opy);
                inner[pi * 2] = ipx; inner[pi * 2 + 1] = ipy;
                outer[pi * 2] = opx; outer[pi * 2 + 1] = opy;
                pi++;
            }
        }

        /* Generate quad ring: each segment = 2 triangles = 6 verts */
        int vert_count = pi * 6;
        vio_2d_vertex *verts = emalloc(sizeof(vio_2d_vertex) * vert_count);
        int vi = 0;
        for (int i = 0; i < pi; i++) {
            int next = (i + 1) % pi;
            float ox1 = outer[i*2], oy1 = outer[i*2+1];
            float ix1 = inner[i*2], iy1 = inner[i*2+1];
            float ox2 = outer[next*2], oy2 = outer[next*2+1];
            float ix2 = inner[next*2], iy2 = inner[next*2+1];
            verts[vi++] = (vio_2d_vertex){ox1, oy1, 0, 0, cr, cg, cb, ca};
            verts[vi++] = (vio_2d_vertex){ix1, iy1, 0, 0, cr, cg, cb, ca};
            verts[vi++] = (vio_2d_vertex){ix2, iy2, 0, 0, cr, cg, cb, ca};
            verts[vi++] = (vio_2d_vertex){ox1, oy1, 0, 0, cr, cg, cb, ca};
            verts[vi++] = (vio_2d_vertex){ix2, iy2, 0, 0, cr, cg, cb, ca};
            verts[vi++] = (vio_2d_vertex){ox2, oy2, 0, 0, cr, cg, cb, ca};
        }

        int start = vio_2d_push_vertices(&ctx->state_2d, verts, vi);
        if (start >= 0) vio_2d_push_item(&ctx->state_2d, VIO_2D_ROUNDED_RECT_OUTLINE, z, 0, NULL, start, vi);
        efree(inner); efree(outer); efree(verts);
    } else {
        /* Filled: triangle fan from center */
        int perimeter_verts = (segs + 1) * 4;
        int tri_count = perimeter_verts;
        vio_2d_vertex *verts = emalloc(sizeof(vio_2d_vertex) * tri_count * 3);
        int vi = 0;

        /* Center of the rect */
        float cx_f = fx + fw * 0.5f, cy_f = fy + fh * 0.5f;
        float tcx = cx_f, tcy = cy_f;
        vio_2d_apply_transform(&ctx->state_2d, &tcx, &tcy);

        /* Build perimeter points */
        float *perim = emalloc(sizeof(float) * perimeter_verts * 2);
        int pi = 0;

        float corners[4][3] = {
            {fx + fr,      fy + fr,      (float)M_PI},
            {fx + fw - fr, fy + fr,      (float)M_PI * 1.5f},
            {fx + fw - fr, fy + fh - fr, 0.0f},
            {fx + fr,      fy + fh - fr, (float)M_PI * 0.5f},
        };

        for (int c = 0; c < 4; c++) {
            float ccx = corners[c][0], ccy = corners[c][1], start_a = corners[c][2];
            for (int s = 0; s <= segs; s++) {
                float angle = start_a + ((float)s / (float)segs) * ((float)M_PI * 0.5f);
                float px = ccx + cosf(angle) * fr;
                float py = ccy + sinf(angle) * fr;
                vio_2d_apply_transform(&ctx->state_2d, &px, &py);
                perim[pi * 2] = px;
                perim[pi * 2 + 1] = py;
                pi++;
            }
        }

        /* Generate triangles from center to each edge of the perimeter */
        for (int i = 0; i < pi; i++) {
            int next = (i + 1) % pi;
            verts[vi++] = (vio_2d_vertex){tcx, tcy, 0, 0, cr, cg, cb, ca};
            verts[vi++] = (vio_2d_vertex){perim[i * 2], perim[i * 2 + 1], 0, 0, cr, cg, cb, ca};
            verts[vi++] = (vio_2d_vertex){perim[next * 2], perim[next * 2 + 1], 0, 0, cr, cg, cb, ca};
        }

        int start = vio_2d_push_vertices(&ctx->state_2d, verts, vi);
        if (start >= 0) vio_2d_push_item(&ctx->state_2d, VIO_2D_ROUNDED_RECT, z, 0, NULL, start, vi);

        efree(perim);
        efree(verts);
    }
}

/* ── Text measurement ────────────────────────────────────────────── */

ZEND_FUNCTION(vio_text_measure)
{
    zval *font_zval;
    char *text;
    size_t text_len;
    HashTable *opts_ht = NULL;

    ZEND_PARSE_PARAMETERS_START(2, 3)
        Z_PARAM_OBJECT_OF_CLASS(font_zval, vio_font_ce)
        Z_PARAM_STRING(text, text_len)
        Z_PARAM_OPTIONAL
        Z_PARAM_ARRAY_HT(opts_ht)
    ZEND_PARSE_PARAMETERS_END();

    vio_font_object *font = Z_VIO_FONT_P(font_zval);
    if (!font->valid) {
        php_error_docref(NULL, E_WARNING, "Font is not valid");
        RETURN_FALSE;
    }

    float max_width = 0.0f, line_height = 0.0f;
    if (opts_ht) {
        zval *val;
        if ((val = zend_hash_str_find(opts_ht, "max_width", sizeof("max_width") - 1)) != NULL) {
            max_width = (float)zval_get_double(val);
        }
        if ((val = zend_hash_str_find(opts_ht, "line_height", sizeof("line_height") - 1)) != NULL) {
            line_height = (float)zval_get_double(val);
        }
    }

#ifdef HAVE_HARFBUZZ
    if (vio_text_shape_available(font)) {
        float w = 0.0f, h = 0.0f;
        int lines = 0;
        vio_text_shape_measure(font, text, text_len, max_width, line_height,
                               &w, &h, &lines);
        array_init(return_value);
        add_assoc_double(return_value, "width", (double)w);
        add_assoc_double(return_value, "height", (double)h);
        add_assoc_long(return_value, "lines", (zend_long)lines);
        return;
    }
#endif

    float width = 0.0f;
    float min_y = 0.0f, max_y = 0.0f;

    for (size_t i = 0; i < text_len; i++) {
        uint32_t cp = vio_utf8_decode(text, text_len, &i);
        zval *entry = zend_hash_index_find(&font->glyph_map, (zend_long)cp);
        if (!entry) continue;
        vio_stbtt_packedchar *b = (vio_stbtt_packedchar *)Z_STRVAL_P(entry);
        width += b->xadvance;

        float char_top = b->yoff;
        float char_bot = b->yoff + (float)(b->y1 - b->y0);
        if (char_top < min_y) min_y = char_top;
        if (char_bot > max_y) max_y = char_bot;
    }

    float height = max_y - min_y;
    if (height < 0) height = 0;

    /* devicePixelRatio: glyph advances/extents are in atlas (physical) pixels;
     * report logical units so layout code is unaffected by the atlas scale. */
    float inv_rs = (font->render_scale > 0.0f) ? (1.0f / font->render_scale) : 1.0f;
    width  *= inv_rs;
    height *= inv_rs;

    array_init(return_value);
    add_assoc_double(return_value, "width", (double)width);
    add_assoc_double(return_value, "height", (double)height);
}

/* True iff `font` carries a real glyph for the given Unicode codepoint.
 * Reliable coverage detection for fallback-chain routing: unlike advance width
 * (a font's .notdef box can measure non-zero), this reports actual glyph
 * presence, so callers never let a primary font claim an uncovered codepoint. */
ZEND_FUNCTION(vio_font_has_glyph)
{
    zval *font_zval;
    zend_long codepoint;

    ZEND_PARSE_PARAMETERS_START(2, 2)
        Z_PARAM_OBJECT_OF_CLASS(font_zval, vio_font_ce)
        Z_PARAM_LONG(codepoint)
    ZEND_PARSE_PARAMETERS_END();

    vio_font_object *font = Z_VIO_FONT_P(font_zval);
    if (!font->valid) {
        RETURN_FALSE;
    }

#ifdef HAVE_HARFBUZZ
    if (vio_text_shape_available(font)) {
        RETURN_BOOL(vio_text_shape_has_glyph(font, (uint32_t)codepoint));
    }
#endif

    /* Legacy stb path: glyph_map holds exactly the codepoints packed into the
     * atlas, so presence there is coverage. */
    RETURN_BOOL(zend_hash_index_find(&font->glyph_map, (zend_long)codepoint) != NULL);
}

/* ── Transform stack ─────────────────────────────────────────────── */

ZEND_FUNCTION(vio_push_transform)
{
    zval *ctx_zval;
    double a, b, c, d, e, f;

    ZEND_PARSE_PARAMETERS_START(7, 7)
        Z_PARAM_OBJECT_OF_CLASS(ctx_zval, vio_context_ce)
        Z_PARAM_DOUBLE(a)
        Z_PARAM_DOUBLE(b)
        Z_PARAM_DOUBLE(c)
        Z_PARAM_DOUBLE(d)
        Z_PARAM_DOUBLE(e)
        Z_PARAM_DOUBLE(f)
    ZEND_PARSE_PARAMETERS_END();

    vio_context_object *ctx = Z_VIO_CONTEXT_P(ctx_zval);
    if (!ctx->initialized) {
        php_error_docref(NULL, E_WARNING, "Context is not initialized");
        return;
    }

    vio_2d_push_transform(&ctx->state_2d, (float)a, (float)b, (float)c, (float)d, (float)e, (float)f);
}

ZEND_FUNCTION(vio_pop_transform)
{
    zval *ctx_zval;

    ZEND_PARSE_PARAMETERS_START(1, 1)
        Z_PARAM_OBJECT_OF_CLASS(ctx_zval, vio_context_ce)
    ZEND_PARSE_PARAMETERS_END();

    vio_context_object *ctx = Z_VIO_CONTEXT_P(ctx_zval);
    if (!ctx->initialized) {
        php_error_docref(NULL, E_WARNING, "Context is not initialized");
        return;
    }

    vio_2d_pop_transform(&ctx->state_2d);
}

/* ── Scissor stack ───────────────────────────────────────────────── */

ZEND_FUNCTION(vio_push_scissor)
{
    zval *ctx_zval;
    double x, y, w, h;

    ZEND_PARSE_PARAMETERS_START(5, 5)
        Z_PARAM_OBJECT_OF_CLASS(ctx_zval, vio_context_ce)
        Z_PARAM_DOUBLE(x)
        Z_PARAM_DOUBLE(y)
        Z_PARAM_DOUBLE(w)
        Z_PARAM_DOUBLE(h)
    ZEND_PARSE_PARAMETERS_END();

    vio_context_object *ctx = Z_VIO_CONTEXT_P(ctx_zval);
    if (!ctx->initialized) {
        php_error_docref(NULL, E_WARNING, "Context is not initialized");
        return;
    }

    vio_2d_push_scissor(&ctx->state_2d, (float)x, (float)y, (float)w, (float)h);
}

ZEND_FUNCTION(vio_pop_scissor)
{
    zval *ctx_zval;

    ZEND_PARSE_PARAMETERS_START(1, 1)
        Z_PARAM_OBJECT_OF_CLASS(ctx_zval, vio_context_ce)
    ZEND_PARSE_PARAMETERS_END();

    vio_context_object *ctx = Z_VIO_CONTEXT_P(ctx_zval);
    if (!ctx->initialized) {
        php_error_docref(NULL, E_WARNING, "Context is not initialized");
        return;
    }

    vio_2d_pop_scissor(&ctx->state_2d);
}

ZEND_FUNCTION(vio_backend_name)
{
    zval *ctx_zval;

    ZEND_PARSE_PARAMETERS_START(1, 1)
        Z_PARAM_OBJECT_OF_CLASS(ctx_zval, vio_context_ce)
    ZEND_PARSE_PARAMETERS_END();

    vio_context_object *ctx = Z_VIO_CONTEXT_P(ctx_zval);

    if (ctx->backend && ctx->backend->name) {
        RETURN_STRING(ctx->backend->name);
    }

    RETURN_EMPTY_STRING();
}

ZEND_FUNCTION(vio_backend_count)
{
    ZEND_PARSE_PARAMETERS_NONE();
    RETURN_LONG(vio_backend_count());
}

ZEND_FUNCTION(vio_backends)
{
    ZEND_PARSE_PARAMETERS_NONE();

    array_init(return_value);
    int count = vio_backend_count();
    for (int i = 0; i < count; i++) {
        const char *name = vio_get_backend_name(i);
        if (name) {
            add_next_index_string(return_value, name);
        }
    }
}

ZEND_FUNCTION(vio_thermal_state)
{
    ZEND_PARSE_PARAMETERS_NONE();
    RETURN_STRING(vio_get_thermal_state_str());
}

/* ── Audio functions ──────────────────────────────────────────────── */

ZEND_FUNCTION(vio_audio_load)
{
    char *path;
    size_t path_len;

    ZEND_PARSE_PARAMETERS_START(1, 1)
        Z_PARAM_STRING(path, path_len)
    ZEND_PARSE_PARAMETERS_END();

    /* Lazy-init audio engine on first use */
    if (!vio_audio.initialized) {
        if (vio_audio_engine_init() != 0) {
            php_error_docref(NULL, E_WARNING, "Failed to initialize audio engine");
            RETURN_FALSE;
        }
    }

    /* Create VioSound object */
    zval snd_zval;
    object_init_ex(&snd_zval, vio_sound_ce);
    vio_sound_object *snd = Z_VIO_SOUND_P(&snd_zval);

    ma_result result = ma_sound_init_from_file(&vio_audio.engine, path, 0, NULL, NULL, &snd->sound);
    if (result != MA_SUCCESS) {
        php_error_docref(NULL, E_WARNING, "Failed to load audio file \"%s\" (error %d)", path, result);
        zval_ptr_dtor(&snd_zval);
        RETURN_FALSE;
    }

    snd->loaded = 1;
    RETURN_COPY_VALUE(&snd_zval);
}

ZEND_FUNCTION(vio_audio_play)
{
    zval *snd_zval;
    HashTable *options_ht = NULL;

    ZEND_PARSE_PARAMETERS_START(1, 2)
        Z_PARAM_OBJECT_OF_CLASS(snd_zval, vio_sound_ce)
        Z_PARAM_OPTIONAL
        Z_PARAM_ARRAY_HT(options_ht)
    ZEND_PARSE_PARAMETERS_END();

    vio_sound_object *snd = Z_VIO_SOUND_P(snd_zval);

    if (!snd->loaded) {
        php_error_docref(NULL, E_WARNING, "Sound is not loaded");
        return;
    }

    /* Parse options */
    if (options_ht) {
        zval *val;
        if ((val = zend_hash_str_find(options_ht, "volume", sizeof("volume") - 1)) != NULL) {
            ma_sound_set_volume(&snd->sound, (float)zval_get_double(val));
        }
        if ((val = zend_hash_str_find(options_ht, "loop", sizeof("loop") - 1)) != NULL) {
            ma_sound_set_looping(&snd->sound, zend_is_true(val) ? MA_TRUE : MA_FALSE);
        }
        if ((val = zend_hash_str_find(options_ht, "pan", sizeof("pan") - 1)) != NULL) {
            ma_sound_set_pan(&snd->sound, (float)zval_get_double(val));
        }
        if ((val = zend_hash_str_find(options_ht, "pitch", sizeof("pitch") - 1)) != NULL) {
            ma_sound_set_pitch(&snd->sound, (float)zval_get_double(val));
        }
    }

    /* Rewind if already played */
    ma_sound_seek_to_pcm_frame(&snd->sound, 0);
    ma_sound_start(&snd->sound);
    snd->playing = 1;
}

ZEND_FUNCTION(vio_audio_stop)
{
    zval *snd_zval;

    ZEND_PARSE_PARAMETERS_START(1, 1)
        Z_PARAM_OBJECT_OF_CLASS(snd_zval, vio_sound_ce)
    ZEND_PARSE_PARAMETERS_END();

    vio_sound_object *snd = Z_VIO_SOUND_P(snd_zval);
    if (snd->loaded) {
        ma_sound_stop(&snd->sound);
        ma_sound_seek_to_pcm_frame(&snd->sound, 0);
        snd->playing = 0;
    }
}

ZEND_FUNCTION(vio_audio_pause)
{
    zval *snd_zval;

    ZEND_PARSE_PARAMETERS_START(1, 1)
        Z_PARAM_OBJECT_OF_CLASS(snd_zval, vio_sound_ce)
    ZEND_PARSE_PARAMETERS_END();

    vio_sound_object *snd = Z_VIO_SOUND_P(snd_zval);
    if (snd->loaded && snd->playing) {
        ma_sound_stop(&snd->sound);
        snd->playing = 0;
    }
}

ZEND_FUNCTION(vio_audio_resume)
{
    zval *snd_zval;

    ZEND_PARSE_PARAMETERS_START(1, 1)
        Z_PARAM_OBJECT_OF_CLASS(snd_zval, vio_sound_ce)
    ZEND_PARSE_PARAMETERS_END();

    vio_sound_object *snd = Z_VIO_SOUND_P(snd_zval);
    if (snd->loaded) {
        ma_sound_start(&snd->sound);
        snd->playing = 1;
    }
}

ZEND_FUNCTION(vio_audio_volume)
{
    zval *snd_zval;
    double volume;

    ZEND_PARSE_PARAMETERS_START(2, 2)
        Z_PARAM_OBJECT_OF_CLASS(snd_zval, vio_sound_ce)
        Z_PARAM_DOUBLE(volume)
    ZEND_PARSE_PARAMETERS_END();

    vio_sound_object *snd = Z_VIO_SOUND_P(snd_zval);
    if (snd->loaded) {
        ma_sound_set_volume(&snd->sound, (float)volume);
    }
}

ZEND_FUNCTION(vio_audio_playing)
{
    zval *snd_zval;

    ZEND_PARSE_PARAMETERS_START(1, 1)
        Z_PARAM_OBJECT_OF_CLASS(snd_zval, vio_sound_ce)
    ZEND_PARSE_PARAMETERS_END();

    vio_sound_object *snd = Z_VIO_SOUND_P(snd_zval);
    if (snd->loaded) {
        RETURN_BOOL(ma_sound_is_playing(&snd->sound));
    }
    RETURN_FALSE;
}

ZEND_FUNCTION(vio_audio_position)
{
    zval *snd_zval;
    double x, y, z;

    ZEND_PARSE_PARAMETERS_START(4, 4)
        Z_PARAM_OBJECT_OF_CLASS(snd_zval, vio_sound_ce)
        Z_PARAM_DOUBLE(x)
        Z_PARAM_DOUBLE(y)
        Z_PARAM_DOUBLE(z)
    ZEND_PARSE_PARAMETERS_END();

    vio_sound_object *snd = Z_VIO_SOUND_P(snd_zval);
    if (snd->loaded) {
        ma_sound_set_position(&snd->sound, (float)x, (float)y, (float)z);
    }
}

ZEND_FUNCTION(vio_audio_listener)
{
    double x, y, z, fx, fy, fz;

    ZEND_PARSE_PARAMETERS_START(6, 6)
        Z_PARAM_DOUBLE(x)
        Z_PARAM_DOUBLE(y)
        Z_PARAM_DOUBLE(z)
        Z_PARAM_DOUBLE(fx)
        Z_PARAM_DOUBLE(fy)
        Z_PARAM_DOUBLE(fz)
    ZEND_PARSE_PARAMETERS_END();

    if (!vio_audio.initialized) {
        php_error_docref(NULL, E_WARNING, "Audio engine not initialized");
        return;
    }

    ma_engine_listener_set_position(&vio_audio.engine, 0, (float)x, (float)y, (float)z);
    ma_engine_listener_set_direction(&vio_audio.engine, 0, (float)fx, (float)fy, (float)fz);
}

/* ── Input injection functions ────────────────────────────────────── */

ZEND_FUNCTION(vio_inject_key)
{
    zval *ctx_zval;
    zend_long key, action;

    ZEND_PARSE_PARAMETERS_START(3, 3)
        Z_PARAM_OBJECT_OF_CLASS(ctx_zval, vio_context_ce)
        Z_PARAM_LONG(key)
        Z_PARAM_LONG(action)
    ZEND_PARSE_PARAMETERS_END();

    vio_context_object *ctx = Z_VIO_CONTEXT_P(ctx_zval);

    if (key >= 0 && key <= VIO_KEY_LAST) {
        ctx->input.keys[key] = (action != VIO_RELEASE) ? 1 : 0;
    }
}

ZEND_FUNCTION(vio_inject_mouse_move)
{
    zval *ctx_zval;
    double x, y;

    ZEND_PARSE_PARAMETERS_START(3, 3)
        Z_PARAM_OBJECT_OF_CLASS(ctx_zval, vio_context_ce)
        Z_PARAM_DOUBLE(x)
        Z_PARAM_DOUBLE(y)
    ZEND_PARSE_PARAMETERS_END();

    vio_context_object *ctx = Z_VIO_CONTEXT_P(ctx_zval);
    ctx->input.mouse_x = x;
    ctx->input.mouse_y = y;
}

ZEND_FUNCTION(vio_inject_mouse_button)
{
    zval *ctx_zval;
    zend_long button, action;

    ZEND_PARSE_PARAMETERS_START(3, 3)
        Z_PARAM_OBJECT_OF_CLASS(ctx_zval, vio_context_ce)
        Z_PARAM_LONG(button)
        Z_PARAM_LONG(action)
    ZEND_PARSE_PARAMETERS_END();

    vio_context_object *ctx = Z_VIO_CONTEXT_P(ctx_zval);

    if (button >= 0 && button <= VIO_MOUSE_LAST) {
        ctx->input.mouse_buttons[button] = (action != VIO_RELEASE) ? 1 : 0;
    }
}

/* ── Headless / Screenshot functions ──────────────────────────────── */

ZEND_FUNCTION(vio_read_pixels)
{
    zval *ctx_zval;

    ZEND_PARSE_PARAMETERS_START(1, 1)
        Z_PARAM_OBJECT_OF_CLASS(ctx_zval, vio_context_ce)
    ZEND_PARSE_PARAMETERS_END();

    vio_context_object *ctx = Z_VIO_CONTEXT_P(ctx_zval);

    if (!ctx->initialized) {
        php_error_docref(NULL, E_WARNING, "Context not initialized");
        RETURN_FALSE;
    }

    int w = ctx->config.width;
    int h = ctx->config.height;

    if (ctx->backend->read_pixels && strcmp(ctx->backend->name, "opengl") == 0) {
        size_t size = (size_t)w * h * 4;
        zend_string *buf = zend_string_alloc(size, 0);
        ctx->backend->read_pixels(ctx->headless_fbo, w, h, ZSTR_VAL(buf));
        ZSTR_VAL(buf)[size] = '\0';
        RETURN_NEW_STR(buf);
    }

#ifdef HAVE_D3D11
    if (strcmp(ctx->backend->name, "d3d11") == 0 && vio_d3d11.initialized) {
        /* d3d11_end_frame mirrors the rendered backbuffer into the GPU-local
         * vio_d3d11.readback_mirror. Reading from that mirror avoids the
         * FLIP_DISCARD race where the live RTV has undefined content after
         * Present().
         *
         * vio_d3d11_resolve_readback() does the (PCIe-crossing) mirror->staging
         * copy HERE, on demand — the per-frame path deliberately does not, so a
         * frame nobody reads costs no CPU transfer.
         *
         * If no end_frame has run yet (pre-first-end call from headless tests
         * that read before any vio_end), there is no mirror; fall back to a
         * one-shot copy from the live RTV — still valid at that point, because
         * nothing has been presented. */
        ID3D11Texture2D *staging = NULL;
        ID3D11Texture2D *one_shot = NULL;

        if (vio_d3d11_resolve_readback()) {
            staging = vio_d3d11.readback_staging;
        }

        if (!staging) {
            ID3D11DeviceContext_Flush(vio_d3d11.context);

            ID3D11Texture2D *back_buf = NULL;
            if (vio_d3d11.current_rtv) {
                ID3D11Resource *rtv_res = NULL;
                ID3D11RenderTargetView_GetResource(vio_d3d11.current_rtv, &rtv_res);
                if (rtv_res) {
                    ID3D11Resource_QueryInterface(rtv_res, &IID_ID3D11Texture2D, (void **)&back_buf);
                    ID3D11Resource_Release(rtv_res);
                }
            }
            if (!back_buf) {
                php_error_docref(NULL, E_WARNING, "vio_read_pixels: Failed to get RTV texture");
                RETURN_FALSE;
            }

            D3D11_TEXTURE2D_DESC bb_desc;
            ID3D11Texture2D_GetDesc(back_buf, &bb_desc);

            D3D11_TEXTURE2D_DESC staging_desc = bb_desc;
            staging_desc.Usage = D3D11_USAGE_STAGING;
            staging_desc.BindFlags = 0;
            staging_desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            staging_desc.MiscFlags = 0;

            HRESULT hr = ID3D11Device_CreateTexture2D(vio_d3d11.device, &staging_desc, NULL, &one_shot);
            if (FAILED(hr)) {
                ID3D11Texture2D_Release(back_buf);
                php_error_docref(NULL, E_WARNING, "vio_read_pixels: Failed to create staging texture");
                RETURN_FALSE;
            }

            ID3D11DeviceContext_CopyResource(vio_d3d11.context,
                                              (ID3D11Resource *)one_shot,
                                              (ID3D11Resource *)back_buf);
            ID3D11Texture2D_Release(back_buf);
            staging = one_shot;
        }

        D3D11_TEXTURE2D_DESC st_desc;
        ID3D11Texture2D_GetDesc(staging, &st_desc);

        D3D11_MAPPED_SUBRESOURCE mapped = {0};
        HRESULT hr = ID3D11DeviceContext_Map(vio_d3d11.context, (ID3D11Resource *)staging,
                                              0, D3D11_MAP_READ, 0, &mapped);
        if (FAILED(hr)) {
            if (one_shot) ID3D11Texture2D_Release(one_shot);
            php_error_docref(NULL, E_WARNING, "vio_read_pixels: Failed to map staging texture");
            RETURN_FALSE;
        }

        w = (int)st_desc.Width;
        h = (int)st_desc.Height;
        size_t out_size = (size_t)w * h * 4;
        zend_string *buf = zend_string_alloc(out_size, 0);
        unsigned char *dst = (unsigned char *)ZSTR_VAL(buf);
        unsigned char *src = (unsigned char *)mapped.pData;

        for (int y = 0; y < h; y++) {
            memcpy(dst + y * w * 4, src + y * mapped.RowPitch, w * 4);
        }
        ZSTR_VAL(buf)[out_size] = '\0';

        ID3D11DeviceContext_Unmap(vio_d3d11.context, (ID3D11Resource *)staging, 0);
        if (one_shot) ID3D11Texture2D_Release(one_shot);

        RETURN_NEW_STR(buf);
    }
#endif

#ifdef HAVE_D3D12
    if (strcmp(ctx->backend->name, "d3d12") == 0 && vio_d3d12.initialized) {
        /* vio_d3d12_capture_frame handles BOTH the mid-frame case (screenshot
         * taken after vio_draw_3d but before vio_end — it flushes/re-opens the
         * live frame command list and reads the buffer being drawn this frame)
         * and the post-present case (reads the last presented buffer). Size is
         * taken from the live swapchain resource, so it tracks window resize. */
        int cap_w = 0, cap_h = 0;
        size_t cap_size = 0;
        unsigned char *pixels = vio_d3d12_capture_frame(&cap_w, &cap_h, &cap_size);
        if (!pixels) {
            php_error_docref(NULL, E_WARNING, "vio_read_pixels: D3D12 capture failed");
            RETURN_FALSE;
        }

        zend_string *buf = zend_string_alloc(cap_size, 0);
        memcpy(ZSTR_VAL(buf), pixels, cap_size);
        ZSTR_VAL(buf)[cap_size] = '\0';
        free(pixels);

        RETURN_NEW_STR(buf);
    }
#endif

#ifdef HAVE_VULKAN
    if (strcmp(ctx->backend->name, "vulkan") == 0 && vio_vk.initialized) {
        /* vulkan_read_pixels RE-ACQUIRES a swapchain image (NOT
         * swapchain_images[current_image_index] directly — that just-presented
         * image is owned by the presentation engine and reading it would be a
         * WRITE_AFTER_PRESENT sync hazard) and reads that image back as TOP-DOWN
         * RGBA8, swizzling the B8G8R8A8_UNORM swapchain to R,G,B,A so the bytes
         * match the D3D12 R8G8B8A8_UNORM readback for an apples-to-apples golden
         * compare. It honors the buffer row stride and the caller's w/h (writes
         * only the overlapping region, never overruns).
         *
         * m3 — INTENDED USE is stable/screenshot capture (golden tests, the
         * splash screenshot). The re-acquired buffer holds the most-recent
         * render of that swapchain image; with FIFO/vsync and a static scene
         * that is the just-presented content, but for an ANIMATING scene with
         * >=3 swapchain images the acquired buffer may be 1-2 frames stale.
         * Call this after rendering a steady frame, not as a per-frame capture. */
        size_t size = (size_t)w * h * 4;
        zend_string *buf = zend_string_alloc(size, 0);
        ZSTR_VAL(buf)[size] = '\0';

        if (vulkan_read_pixels(w, h, (unsigned char *)ZSTR_VAL(buf)) == 0) {
            RETURN_NEW_STR(buf);
        }
        zend_string_release(buf);
        php_error_docref(NULL, E_WARNING, "vio_read_pixels: Vulkan readback failed");
        RETURN_FALSE;
    }
#endif

#ifdef HAVE_METAL
    if (strcmp(ctx->backend->name, "metal") == 0) {
        size_t size = (size_t)w * h * 4;
        zend_string *buf = zend_string_alloc(size, 0);
        ZSTR_VAL(buf)[size] = '\0';

        if (vio_metal_read_pixels(w, h, (unsigned char *)ZSTR_VAL(buf)) == 0) {
            RETURN_NEW_STR(buf);
        }
        zend_string_release(buf);
        php_error_docref(NULL, E_WARNING, "vio_read_pixels: Metal readback failed");
        RETURN_FALSE;
    }
#endif

    php_error_docref(NULL, E_WARNING, "vio_read_pixels: unsupported backend");
    RETURN_FALSE;
}

ZEND_FUNCTION(vio_save_screenshot)
{
    zval *ctx_zval;
    char *path;
    size_t path_len;

    ZEND_PARSE_PARAMETERS_START(2, 2)
        Z_PARAM_OBJECT_OF_CLASS(ctx_zval, vio_context_ce)
        Z_PARAM_STRING(path, path_len)
    ZEND_PARSE_PARAMETERS_END();

    vio_context_object *ctx = Z_VIO_CONTEXT_P(ctx_zval);

    if (!ctx->initialized) {
        php_error_docref(NULL, E_WARNING, "Context not initialized");
        RETURN_FALSE;
    }

    int w = ctx->config.width;
    int h = ctx->config.height;

    if (ctx->backend->read_pixels && strcmp(ctx->backend->name, "opengl") == 0) {
        size_t size = (size_t)w * h * 4;
        unsigned char *pixels = emalloc(size);
        ctx->backend->read_pixels(ctx->headless_fbo, w, h, pixels);
        int ok = stbi_write_png(path, w, h, 4, pixels, w * 4);
        efree(pixels);
        RETURN_BOOL(ok);
    }

#ifdef HAVE_METAL
    if (strcmp(ctx->backend->name, "metal") == 0) {
        RETURN_BOOL(vio_metal_save_screenshot(path, w, h) == 0);
    }
#endif

#ifdef HAVE_D3D12
    if (strcmp(ctx->backend->name, "d3d12") == 0 && vio_d3d12.initialized) {
        /* Capture directly so we use the TRUE captured dimensions for the PNG.
         * The generic delegation path below writes with ctx->config.width/height
         * (the creation size, e.g. 1280x720) which mismatches the real, possibly
         * window-resized backbuffer (e.g. 3840x1080) — that wrote a clipped,
         * row-stride-shifted image. vio_d3d12_capture_frame returns top-down
         * RGBA8 at the live resolution. */
        int cap_w = 0, cap_h = 0;
        size_t cap_size = 0;
        unsigned char *pixels = vio_d3d12_capture_frame(&cap_w, &cap_h, &cap_size);
        if (!pixels) {
            php_error_docref(NULL, E_WARNING, "vio_save_screenshot: D3D12 capture failed");
            RETURN_FALSE;
        }
        int ok = stbi_write_png(path, cap_w, cap_h, 4, pixels, cap_w * 4);
        free(pixels);
        RETURN_BOOL(ok);
    }
#endif

#if defined(HAVE_D3D11) || defined(HAVE_VULKAN)
    {
        /* Fallback for backends whose readback lives in vio_read_pixels:
         * delegate to it and write PNG, avoiding duplicate readback logic.
         * vio_read_pixels returns TOP-DOWN RGBA8 for all of these (D3D11/D3D12
         * R8G8B8A8_UNORM, Vulkan B8G8R8A8_UNORM swizzled to RGBA), which is
         * exactly what stbi_write_png(..., 4, pixels, w*4) expects. */
        int is_delegated = 0;
#ifdef HAVE_D3D11
        if (strcmp(ctx->backend->name, "d3d11") == 0) is_delegated = 1;
#endif
        /* D3D12 handled above with the live capture dimensions. */
#ifdef HAVE_VULKAN
        if (strcmp(ctx->backend->name, "vulkan") == 0 && vio_vk.initialized) is_delegated = 1;
#endif
        if (is_delegated) {
            zval retval, func_name, args[1];
            ZVAL_UNDEF(&retval);
            ZVAL_STRING(&func_name, "vio_read_pixels");
            ZVAL_COPY(&args[0], ctx_zval);

            int rc = call_user_function(NULL, NULL, &func_name, &retval, 1, args);
            int ok = 0;
            if (rc == SUCCESS && Z_TYPE(retval) == IS_STRING) {
                const unsigned char *pixels = (const unsigned char *)Z_STRVAL(retval);
                ok = stbi_write_png(path, w, h, 4, pixels, w * 4);
            }

            zval_ptr_dtor(&func_name);
            zval_ptr_dtor(&args[0]);
            zval_ptr_dtor(&retval);
            RETURN_BOOL(ok);
        }
    }
#endif

    php_error_docref(NULL, E_WARNING, "vio_save_screenshot: unsupported backend");
    RETURN_FALSE;
}

/* ── GPU / system memory info ─────────────────────────────────────── */

/* Total physical system RAM in bytes, backend-independent. Returns 0 if the
 * platform query fails. Mirrors the per-OS ifdef structure used in
 * src/vio_thermal.c (__APPLE__ / __linux__ / _WIN32). */
static uint64_t vio_query_total_ram_bytes(void)
{
#if defined(_WIN32)
    MEMORYSTATUSEX statex;
    statex.dwLength = sizeof(statex);
    if (GlobalMemoryStatusEx(&statex)) {
        return (uint64_t)statex.ullTotalPhys;
    }
    return 0;
#elif defined(__APPLE__)
    uint64_t mem = 0;
    size_t len = sizeof(mem);
    if (sysctlbyname("hw.memsize", &mem, &len, NULL, 0) == 0) {
        return mem;
    }
    return 0;
#elif defined(__linux__)
    long pages = sysconf(_SC_PHYS_PAGES);
    long page_size = sysconf(_SC_PAGE_SIZE);
    if (pages > 0 && page_size > 0) {
        return (uint64_t)pages * (uint64_t)page_size;
    }
    return 0;
#else
    return 0;
#endif
}

ZEND_FUNCTION(vio_gpu_info)
{
    ZEND_PARSE_PARAMETERS_NONE();

    const char *gpu_name = "";
    uint64_t    vram_bytes = 0;

#ifdef HAVE_D3D12
    /* GPU name + dedicated VRAM are only known on D3D12, and only once the
     * device has been created at init (so the caller must invoke this after the
     * window/renderer exists). Values were captured from the SELECTED adapter's
     * DXGI_ADAPTER_DESC1 in d3d12_init. On other backends / before init they
     * stay at the defaults above. */
    if (vio_d3d12.initialized) {
        gpu_name   = vio_d3d12.gpu_name;
        vram_bytes = vio_d3d12.vram_bytes;
    }
#endif

    uint64_t ram_bytes = vio_query_total_ram_bytes();

    array_init(return_value);
    add_assoc_string(return_value, "name", (char *)gpu_name);
    add_assoc_long(return_value, "vram_bytes", (zend_long)vram_bytes);
    add_assoc_long(return_value, "ram_bytes", (zend_long)ram_bytes);
}

/* ── Image comparison (VRT) ───────────────────────────────────────── */

ZEND_FUNCTION(vio_compare_images)
{
    char *ref_path, *cur_path;
    size_t ref_len, cur_len;
    HashTable *options_ht = NULL;

    ZEND_PARSE_PARAMETERS_START(2, 3)
        Z_PARAM_STRING(ref_path, ref_len)
        Z_PARAM_STRING(cur_path, cur_len)
        Z_PARAM_OPTIONAL
        Z_PARAM_ARRAY_HT_OR_NULL(options_ht)
    ZEND_PARSE_PARAMETERS_END();

    double threshold = 0.01;
    if (options_ht) {
        zval *val;
        if ((val = zend_hash_str_find(options_ht, "threshold", sizeof("threshold") - 1)) != NULL) {
            threshold = zval_get_double(val);
        }
    }

    int rw, rh, rn, cw, ch, cn;
    unsigned char *ref_data = stbi_load(ref_path, &rw, &rh, &rn, 4);
    if (!ref_data) {
        php_error_docref(NULL, E_WARNING, "Failed to load reference image: %s", ref_path);
        RETURN_FALSE;
    }

    unsigned char *cur_data = stbi_load(cur_path, &cw, &ch, &cn, 4);
    if (!cur_data) {
        stbi_image_free(ref_data);
        php_error_docref(NULL, E_WARNING, "Failed to load current image: %s", cur_path);
        RETURN_FALSE;
    }

    if (rw != cw || rh != ch) {
        stbi_image_free(ref_data);
        stbi_image_free(cur_data);
        array_init(return_value);
        add_assoc_bool(return_value, "passed", 0);
        add_assoc_double(return_value, "diff_ratio", 1.0);
        add_assoc_long(return_value, "diff_pixels", (zend_long)rw * rh);
        add_assoc_string(return_value, "error", "Image dimensions differ");
        return;
    }

    size_t total_pixels = (size_t)rw * rh;
    size_t diff_count = 0;
    unsigned char *diff_data = emalloc(total_pixels * 4);

    for (size_t i = 0; i < total_pixels; i++) {
        size_t off = i * 4;
        int dr = abs((int)ref_data[off] - (int)cur_data[off]);
        int dg = abs((int)ref_data[off + 1] - (int)cur_data[off + 1]);
        int db = abs((int)ref_data[off + 2] - (int)cur_data[off + 2]);
        int da = abs((int)ref_data[off + 3] - (int)cur_data[off + 3]);

        if (dr > 0 || dg > 0 || db > 0 || da > 0) {
            diff_count++;
            /* Highlight diff in red, intensity = max channel diff */
            int maxd = dr;
            if (dg > maxd) maxd = dg;
            if (db > maxd) maxd = db;
            if (da > maxd) maxd = da;
            diff_data[off]     = (unsigned char)(maxd < 255 ? 128 + maxd / 2 : 255);
            diff_data[off + 1] = 0;
            diff_data[off + 2] = 0;
            diff_data[off + 3] = 255;
        } else {
            /* Same pixel: dim grayscale */
            unsigned char gray = (unsigned char)((ref_data[off] + ref_data[off + 1] + ref_data[off + 2]) / 6);
            diff_data[off]     = gray;
            diff_data[off + 1] = gray;
            diff_data[off + 2] = gray;
            diff_data[off + 3] = 255;
        }
    }

    double diff_ratio = (double)diff_count / (double)total_pixels;
    int passed = diff_ratio <= threshold;

    /* Store diff image data as a string for vio_save_diff_image */
    zend_string *diff_str = zend_string_init((char *)diff_data, total_pixels * 4, 0);
    efree(diff_data);

    stbi_image_free(ref_data);
    stbi_image_free(cur_data);

    array_init(return_value);
    add_assoc_bool(return_value, "passed", passed);
    add_assoc_double(return_value, "diff_ratio", diff_ratio);
    add_assoc_long(return_value, "diff_pixels", (zend_long)diff_count);
    add_assoc_long(return_value, "width", rw);
    add_assoc_long(return_value, "height", rh);
    add_assoc_str(return_value, "diff_data", diff_str);
}

ZEND_FUNCTION(vio_save_diff_image)
{
    HashTable *diff_ht;
    char *path;
    size_t path_len;

    ZEND_PARSE_PARAMETERS_START(2, 2)
        Z_PARAM_ARRAY_HT(diff_ht)
        Z_PARAM_STRING(path, path_len)
    ZEND_PARSE_PARAMETERS_END();

    zval *w_val = zend_hash_str_find(diff_ht, "width", sizeof("width") - 1);
    zval *h_val = zend_hash_str_find(diff_ht, "height", sizeof("height") - 1);
    zval *data_val = zend_hash_str_find(diff_ht, "diff_data", sizeof("diff_data") - 1);

    if (!w_val || !h_val || !data_val || Z_TYPE_P(data_val) != IS_STRING) {
        php_error_docref(NULL, E_WARNING, "Invalid diff result array (missing width/height/diff_data)");
        RETURN_FALSE;
    }

    int w = (int)zval_get_long(w_val);
    int h = (int)zval_get_long(h_val);
    size_t expected = (size_t)w * h * 4;

    if (Z_STRLEN_P(data_val) != expected) {
        php_error_docref(NULL, E_WARNING, "Diff data size mismatch");
        RETURN_FALSE;
    }

    int ok = stbi_write_png(path, w, h, 4, Z_STRVAL_P(data_val), w * 4);
    RETURN_BOOL(ok);
}

/* ── Video recording functions ────────────────────────────────────── */

#ifdef HAVE_FFMPEG
ZEND_FUNCTION(vio_recorder)
{
    zval *ctx_zval;
    HashTable *config_ht;

    ZEND_PARSE_PARAMETERS_START(2, 2)
        Z_PARAM_OBJECT_OF_CLASS(ctx_zval, vio_context_ce)
        Z_PARAM_ARRAY_HT(config_ht)
    ZEND_PARSE_PARAMETERS_END();

    vio_context_object *ctx = Z_VIO_CONTEXT_P(ctx_zval);

    if (!ctx->initialized) {
        php_error_docref(NULL, E_WARNING, "Context not initialized");
        RETURN_FALSE;
    }

    /* Parse config */
    zval *val;
    char *path = NULL;
    int fps = 30;
    char *codec = NULL;

    if ((val = zend_hash_str_find(config_ht, "path", sizeof("path") - 1)) != NULL) {
        path = Z_STRVAL_P(val);
    }
    if (!path || !path[0]) {
        php_error_docref(NULL, E_WARNING, "Recording 'path' is required");
        RETURN_FALSE;
    }
    if ((val = zend_hash_str_find(config_ht, "fps", sizeof("fps") - 1)) != NULL) {
        fps = (int)zval_get_long(val);
        if (fps <= 0) fps = 30;
    }
    if ((val = zend_hash_str_find(config_ht, "codec", sizeof("codec") - 1)) != NULL) {
        codec = Z_STRVAL_P(val);
    }

    /* Create recorder object */
    zval obj;
    object_init_ex(&obj, vio_recorder_ce);
    vio_recorder_object *rec = Z_VIO_RECORDER_P(&obj);

    int ret = vio_recorder_init(rec, path, ctx->config.width, ctx->config.height, fps, codec);
    if (ret != 0) {
        php_error_docref(NULL, E_WARNING, "Failed to initialize recorder (error %d)", ret);
        zval_ptr_dtor(&obj);
        RETURN_FALSE;
    }

    RETURN_COPY_VALUE(&obj);
}

ZEND_FUNCTION(vio_recorder_capture)
{
    zval *rec_zval, *ctx_zval;

    ZEND_PARSE_PARAMETERS_START(2, 2)
        Z_PARAM_OBJECT_OF_CLASS(rec_zval, vio_recorder_ce)
        Z_PARAM_OBJECT_OF_CLASS(ctx_zval, vio_context_ce)
    ZEND_PARSE_PARAMETERS_END();

    vio_recorder_object *rec = Z_VIO_RECORDER_P(rec_zval);
    vio_context_object *ctx = Z_VIO_CONTEXT_P(ctx_zval);

    if (!rec->recording) {
        php_error_docref(NULL, E_WARNING, "Recorder is not active");
        RETURN_FALSE;
    }

    int w = ctx->config.width;
    int h = ctx->config.height;

    if (ctx->backend->read_pixels && strcmp(ctx->backend->name, "opengl") == 0) {
        size_t size = (size_t)w * h * 4;
        unsigned char *pixels = emalloc(size);
        ctx->backend->read_pixels(ctx->headless_fbo, w, h, pixels);
        int ret = vio_recorder_write_rgba(rec, pixels);
        efree(pixels);

        if (ret < 0) {
            php_error_docref(NULL, E_WARNING, "Failed to encode frame (error %d)", ret);
            RETURN_FALSE;
        }
        RETURN_TRUE;
    }

    php_error_docref(NULL, E_WARNING, "Capture not supported for this backend");
    RETURN_FALSE;
}

ZEND_FUNCTION(vio_recorder_stop)
{
    zval *rec_zval;

    ZEND_PARSE_PARAMETERS_START(1, 1)
        Z_PARAM_OBJECT_OF_CLASS(rec_zval, vio_recorder_ce)
    ZEND_PARSE_PARAMETERS_END();

    vio_recorder_object *rec = Z_VIO_RECORDER_P(rec_zval);

    if (rec->initialized) {
        vio_recorder_finalize(rec);
    }
}
/* ── Network streaming functions ──────────────────────────────────── */

ZEND_FUNCTION(vio_stream)
{
    zval *ctx_zval;
    HashTable *config_ht;

    ZEND_PARSE_PARAMETERS_START(2, 2)
        Z_PARAM_OBJECT_OF_CLASS(ctx_zval, vio_context_ce)
        Z_PARAM_ARRAY_HT(config_ht)
    ZEND_PARSE_PARAMETERS_END();

    vio_context_object *ctx = Z_VIO_CONTEXT_P(ctx_zval);

    if (!ctx->initialized) {
        php_error_docref(NULL, E_WARNING, "Context not initialized");
        RETURN_FALSE;
    }

    zval *val;
    char *url = NULL;
    int fps = 30;
    int bitrate = 2000000;
    char *codec = NULL;
    char *format = NULL;

    if ((val = zend_hash_str_find(config_ht, "url", sizeof("url") - 1)) != NULL) {
        url = Z_STRVAL_P(val);
    }
    if (!url || !url[0]) {
        php_error_docref(NULL, E_WARNING, "Stream 'url' is required");
        RETURN_FALSE;
    }
    if ((val = zend_hash_str_find(config_ht, "fps", sizeof("fps") - 1)) != NULL) {
        fps = (int)zval_get_long(val);
        if (fps <= 0) fps = 30;
    }
    if ((val = zend_hash_str_find(config_ht, "bitrate", sizeof("bitrate") - 1)) != NULL) {
        bitrate = (int)zval_get_long(val);
        if (bitrate <= 0) bitrate = 2000000;
    }
    if ((val = zend_hash_str_find(config_ht, "codec", sizeof("codec") - 1)) != NULL) {
        codec = Z_STRVAL_P(val);
    }
    if ((val = zend_hash_str_find(config_ht, "format", sizeof("format") - 1)) != NULL) {
        format = Z_STRVAL_P(val);
    }

    zval obj;
    object_init_ex(&obj, vio_stream_ce);
    vio_stream_object *st = Z_VIO_STREAM_P(&obj);

    int ret = vio_stream_init(st, url, ctx->config.width, ctx->config.height, fps, bitrate, codec, format);
    if (ret != 0) {
        php_error_docref(NULL, E_WARNING, "Failed to initialize stream (error %d)", ret);
        zval_ptr_dtor(&obj);
        RETURN_FALSE;
    }

    RETURN_COPY_VALUE(&obj);
}

ZEND_FUNCTION(vio_stream_push)
{
    zval *st_zval, *ctx_zval;

    ZEND_PARSE_PARAMETERS_START(2, 2)
        Z_PARAM_OBJECT_OF_CLASS(st_zval, vio_stream_ce)
        Z_PARAM_OBJECT_OF_CLASS(ctx_zval, vio_context_ce)
    ZEND_PARSE_PARAMETERS_END();

    vio_stream_object *st = Z_VIO_STREAM_P(st_zval);
    vio_context_object *ctx = Z_VIO_CONTEXT_P(ctx_zval);

    if (!st->streaming) {
        php_error_docref(NULL, E_WARNING, "Stream is not active");
        RETURN_FALSE;
    }

    int w = ctx->config.width;
    int h = ctx->config.height;

    if (ctx->backend->read_pixels && strcmp(ctx->backend->name, "opengl") == 0) {
        size_t size = (size_t)w * h * 4;
        unsigned char *pixels = emalloc(size);
        ctx->backend->read_pixels(ctx->headless_fbo, w, h, pixels);
        int ret = vio_stream_write_rgba(st, pixels);
        efree(pixels);

        if (ret < 0) {
            php_error_docref(NULL, E_WARNING, "Failed to encode/send frame (error %d)", ret);
            RETURN_FALSE;
        }
        RETURN_TRUE;
    }

    php_error_docref(NULL, E_WARNING, "Stream push not supported for this backend");
    RETURN_FALSE;
}

ZEND_FUNCTION(vio_stream_stop)
{
    zval *st_zval;

    ZEND_PARSE_PARAMETERS_START(1, 1)
        Z_PARAM_OBJECT_OF_CLASS(st_zval, vio_stream_ce)
    ZEND_PARSE_PARAMETERS_END();

    vio_stream_object *st = Z_VIO_STREAM_P(st_zval);

    if (st->initialized) {
        vio_stream_finalize(st);
    }
}

#else /* !HAVE_FFMPEG */
ZEND_FUNCTION(vio_recorder)
{
    php_error_docref(NULL, E_WARNING, "Video recording requires FFmpeg (compile with --with-ffmpeg)");
    RETURN_FALSE;
}
ZEND_FUNCTION(vio_recorder_capture)
{
    php_error_docref(NULL, E_WARNING, "Video recording requires FFmpeg (compile with --with-ffmpeg)");
    RETURN_FALSE;
}
ZEND_FUNCTION(vio_recorder_stop)
{
    php_error_docref(NULL, E_WARNING, "Video recording requires FFmpeg (compile with --with-ffmpeg)");
}
ZEND_FUNCTION(vio_stream)
{
    php_error_docref(NULL, E_WARNING, "Streaming requires FFmpeg (compile with --with-ffmpeg)");
    RETURN_FALSE;
}
ZEND_FUNCTION(vio_stream_push)
{
    php_error_docref(NULL, E_WARNING, "Streaming requires FFmpeg (compile with --with-ffmpeg)");
    RETURN_FALSE;
}
ZEND_FUNCTION(vio_stream_stop)
{
    php_error_docref(NULL, E_WARNING, "Streaming requires FFmpeg (compile with --with-ffmpeg)");
}
#endif /* HAVE_FFMPEG */

/* ── Gamepad functions ────────────────────────────────────────────── */

ZEND_FUNCTION(vio_gamepads)
{
    ZEND_PARSE_PARAMETERS_NONE();

    array_init(return_value);

#ifdef HAVE_GLFW
    for (int jid = GLFW_JOYSTICK_1; jid <= GLFW_JOYSTICK_LAST; jid++) {
        if (glfwJoystickPresent(jid)) {
            add_next_index_long(return_value, jid);
        }
    }
#endif
}

ZEND_FUNCTION(vio_gamepad_connected)
{
    zend_long id;

    ZEND_PARSE_PARAMETERS_START(1, 1)
        Z_PARAM_LONG(id)
    ZEND_PARSE_PARAMETERS_END();

#ifdef HAVE_GLFW
    if (id >= GLFW_JOYSTICK_1 && id <= GLFW_JOYSTICK_LAST) {
        RETURN_BOOL(glfwJoystickPresent((int)id));
    }
#endif
    RETURN_FALSE;
}

ZEND_FUNCTION(vio_gamepad_name)
{
    zend_long id;

    ZEND_PARSE_PARAMETERS_START(1, 1)
        Z_PARAM_LONG(id)
    ZEND_PARSE_PARAMETERS_END();

#ifdef HAVE_GLFW
    if (id >= GLFW_JOYSTICK_1 && id <= GLFW_JOYSTICK_LAST && glfwJoystickPresent((int)id)) {
        const char *name = glfwJoystickIsGamepad((int)id)
            ? glfwGetGamepadName((int)id)
            : glfwGetJoystickName((int)id);
        if (name) {
            RETURN_STRING(name);
        }
    }
#endif
    RETURN_NULL();
}

ZEND_FUNCTION(vio_gamepad_buttons)
{
    zend_long id;

    ZEND_PARSE_PARAMETERS_START(1, 1)
        Z_PARAM_LONG(id)
    ZEND_PARSE_PARAMETERS_END();

    array_init(return_value);

#ifdef HAVE_GLFW
    if (id >= GLFW_JOYSTICK_1 && id <= GLFW_JOYSTICK_LAST) {
        GLFWgamepadstate state;
        if (glfwGetGamepadState((int)id, &state)) {
            for (int i = 0; i <= GLFW_GAMEPAD_BUTTON_LAST; i++) {
                add_index_bool(return_value, i, state.buttons[i] == GLFW_PRESS);
            }
            return;
        }
        /* Fallback: raw joystick buttons */
        int count = 0;
        const unsigned char *buttons = glfwGetJoystickButtons((int)id, &count);
        if (buttons) {
            for (int i = 0; i < count; i++) {
                add_index_bool(return_value, i, buttons[i] == GLFW_PRESS);
            }
        }
    }
#endif
}

ZEND_FUNCTION(vio_gamepad_axes)
{
    zend_long id;

    ZEND_PARSE_PARAMETERS_START(1, 1)
        Z_PARAM_LONG(id)
    ZEND_PARSE_PARAMETERS_END();

    array_init(return_value);

#ifdef HAVE_GLFW
    if (id >= GLFW_JOYSTICK_1 && id <= GLFW_JOYSTICK_LAST) {
        GLFWgamepadstate state;
        if (glfwGetGamepadState((int)id, &state)) {
            for (int i = 0; i <= GLFW_GAMEPAD_AXIS_LAST; i++) {
                add_index_double(return_value, i, (double)state.axes[i]);
            }
            return;
        }
        /* Fallback: raw joystick axes */
        int count = 0;
        const float *axes = glfwGetJoystickAxes((int)id, &count);
        if (axes) {
            for (int i = 0; i < count; i++) {
                add_index_double(return_value, i, (double)axes[i]);
            }
        }
    }
#endif
}

ZEND_FUNCTION(vio_gamepad_triggers)
{
    zend_long id;

    ZEND_PARSE_PARAMETERS_START(1, 1)
        Z_PARAM_LONG(id)
    ZEND_PARSE_PARAMETERS_END();

    array_init(return_value);

#ifdef HAVE_GLFW
    if (id >= GLFW_JOYSTICK_1 && id <= GLFW_JOYSTICK_LAST) {
        GLFWgamepadstate state;
        if (glfwGetGamepadState((int)id, &state)) {
            add_assoc_double(return_value, "left", (double)state.axes[GLFW_GAMEPAD_AXIS_LEFT_TRIGGER]);
            add_assoc_double(return_value, "right", (double)state.axes[GLFW_GAMEPAD_AXIS_RIGHT_TRIGGER]);
            return;
        }
    }
#endif
    add_assoc_double(return_value, "left", 0.0);
    add_assoc_double(return_value, "right", 0.0);
}

/* ── Constant registration helper ─────────────────────────────────── */

static void vio_register_constants(int module_number)
{
    /* Format */
    REGISTER_LONG_CONSTANT("VIO_FLOAT1", VIO_FLOAT1, CONST_CS | CONST_PERSISTENT);
    REGISTER_LONG_CONSTANT("VIO_FLOAT2", VIO_FLOAT2, CONST_CS | CONST_PERSISTENT);
    REGISTER_LONG_CONSTANT("VIO_FLOAT3", VIO_FLOAT3, CONST_CS | CONST_PERSISTENT);
    REGISTER_LONG_CONSTANT("VIO_FLOAT4", VIO_FLOAT4, CONST_CS | CONST_PERSISTENT);
    REGISTER_LONG_CONSTANT("VIO_INT1", VIO_INT1, CONST_CS | CONST_PERSISTENT);
    REGISTER_LONG_CONSTANT("VIO_INT2", VIO_INT2, CONST_CS | CONST_PERSISTENT);
    REGISTER_LONG_CONSTANT("VIO_INT3", VIO_INT3, CONST_CS | CONST_PERSISTENT);
    REGISTER_LONG_CONSTANT("VIO_INT4", VIO_INT4, CONST_CS | CONST_PERSISTENT);

    /* Topology */
    REGISTER_LONG_CONSTANT("VIO_TRIANGLES", VIO_TRIANGLES, CONST_CS | CONST_PERSISTENT);
    REGISTER_LONG_CONSTANT("VIO_TRIANGLE_STRIP", VIO_TRIANGLE_STRIP, CONST_CS | CONST_PERSISTENT);
    REGISTER_LONG_CONSTANT("VIO_TRIANGLE_FAN", VIO_TRIANGLE_FAN, CONST_CS | CONST_PERSISTENT);
    REGISTER_LONG_CONSTANT("VIO_LINES", VIO_LINES, CONST_CS | CONST_PERSISTENT);
    REGISTER_LONG_CONSTANT("VIO_LINE_STRIP", VIO_LINE_STRIP, CONST_CS | CONST_PERSISTENT);
    REGISTER_LONG_CONSTANT("VIO_POINTS", VIO_POINTS, CONST_CS | CONST_PERSISTENT);

    /* Cull mode */
    REGISTER_LONG_CONSTANT("VIO_CULL_NONE", VIO_CULL_NONE, CONST_CS | CONST_PERSISTENT);
    REGISTER_LONG_CONSTANT("VIO_CULL_BACK", VIO_CULL_BACK, CONST_CS | CONST_PERSISTENT);
    REGISTER_LONG_CONSTANT("VIO_CULL_FRONT", VIO_CULL_FRONT, CONST_CS | CONST_PERSISTENT);

    /* Blend mode */
    REGISTER_LONG_CONSTANT("VIO_BLEND_NONE", VIO_BLEND_NONE, CONST_CS | CONST_PERSISTENT);
    REGISTER_LONG_CONSTANT("VIO_BLEND_ALPHA", VIO_BLEND_ALPHA, CONST_CS | CONST_PERSISTENT);
    REGISTER_LONG_CONSTANT("VIO_BLEND_ADDITIVE", VIO_BLEND_ADDITIVE, CONST_CS | CONST_PERSISTENT);

    /* Depth function */
    REGISTER_LONG_CONSTANT("VIO_DEPTH_LESS", VIO_DEPTH_LESS, CONST_CS | CONST_PERSISTENT);
    REGISTER_LONG_CONSTANT("VIO_DEPTH_LEQUAL", VIO_DEPTH_LEQUAL, CONST_CS | CONST_PERSISTENT);

    /* Cursor mode */
    REGISTER_LONG_CONSTANT("VIO_CURSOR_NORMAL", 0, CONST_CS | CONST_PERSISTENT);
    REGISTER_LONG_CONSTANT("VIO_CURSOR_DISABLED", 1, CONST_CS | CONST_PERSISTENT);
    REGISTER_LONG_CONSTANT("VIO_CURSOR_HIDDEN", 2, CONST_CS | CONST_PERSISTENT);

    /* Touch phase (vio_touch_get/inject) */
    REGISTER_LONG_CONSTANT("VIO_TOUCH_BEGAN", VIO_TOUCH_BEGAN, CONST_CS | CONST_PERSISTENT);
    REGISTER_LONG_CONSTANT("VIO_TOUCH_MOVED", VIO_TOUCH_MOVED, CONST_CS | CONST_PERSISTENT);
    REGISTER_LONG_CONSTANT("VIO_TOUCH_STATIONARY", VIO_TOUCH_STATIONARY, CONST_CS | CONST_PERSISTENT);
    REGISTER_LONG_CONSTANT("VIO_TOUCH_ENDED", VIO_TOUCH_ENDED, CONST_CS | CONST_PERSISTENT);
    REGISTER_LONG_CONSTANT("VIO_TOUCH_CANCELLED", VIO_TOUCH_CANCELLED, CONST_CS | CONST_PERSISTENT);

    /* Shader format */
    REGISTER_LONG_CONSTANT("VIO_SHADER_AUTO", VIO_SHADER_AUTO, CONST_CS | CONST_PERSISTENT);
    REGISTER_LONG_CONSTANT("VIO_SHADER_SPIRV", VIO_SHADER_SPIRV, CONST_CS | CONST_PERSISTENT);
    REGISTER_LONG_CONSTANT("VIO_SHADER_GLSL", VIO_SHADER_GLSL, CONST_CS | CONST_PERSISTENT);
    REGISTER_LONG_CONSTANT("VIO_SHADER_GLSL_RAW", VIO_SHADER_GLSL_RAW, CONST_CS | CONST_PERSISTENT);
    REGISTER_LONG_CONSTANT("VIO_SHADER_MSL", VIO_SHADER_MSL, CONST_CS | CONST_PERSISTENT);

    /* Filter */
    REGISTER_LONG_CONSTANT("VIO_FILTER_NEAREST", VIO_FILTER_NEAREST, CONST_CS | CONST_PERSISTENT);
    REGISTER_LONG_CONSTANT("VIO_FILTER_LINEAR", VIO_FILTER_LINEAR, CONST_CS | CONST_PERSISTENT);

    /* Wrap */
    REGISTER_LONG_CONSTANT("VIO_WRAP_REPEAT", VIO_WRAP_REPEAT, CONST_CS | CONST_PERSISTENT);
    REGISTER_LONG_CONSTANT("VIO_WRAP_CLAMP", VIO_WRAP_CLAMP, CONST_CS | CONST_PERSISTENT);
    REGISTER_LONG_CONSTANT("VIO_WRAP_MIRROR", VIO_WRAP_MIRROR, CONST_CS | CONST_PERSISTENT);

    /* Usage */
    REGISTER_LONG_CONSTANT("VIO_POSITION", VIO_POSITION, CONST_CS | CONST_PERSISTENT);
    REGISTER_LONG_CONSTANT("VIO_COLOR", VIO_COLOR, CONST_CS | CONST_PERSISTENT);
    REGISTER_LONG_CONSTANT("VIO_TEXCOORD", VIO_TEXCOORD, CONST_CS | CONST_PERSISTENT);
    REGISTER_LONG_CONSTANT("VIO_NORMAL", VIO_NORMAL, CONST_CS | CONST_PERSISTENT);
    REGISTER_LONG_CONSTANT("VIO_TANGENT", VIO_TANGENT, CONST_CS | CONST_PERSISTENT);

    /* Buffer type */
    REGISTER_LONG_CONSTANT("VIO_BUFFER_VERTEX", VIO_BUFFER_VERTEX, CONST_CS | CONST_PERSISTENT);
    REGISTER_LONG_CONSTANT("VIO_BUFFER_INDEX", VIO_BUFFER_INDEX, CONST_CS | CONST_PERSISTENT);
    REGISTER_LONG_CONSTANT("VIO_BUFFER_UNIFORM", VIO_BUFFER_UNIFORM, CONST_CS | CONST_PERSISTENT);
    REGISTER_LONG_CONSTANT("VIO_BUFFER_STORAGE", VIO_BUFFER_STORAGE, CONST_CS | CONST_PERSISTENT);

    /* Compute storage-buffer access (vio_compute_bind_buffer) */
    REGISTER_LONG_CONSTANT("VIO_COMPUTE_READ", 0, CONST_CS | CONST_PERSISTENT);
    REGISTER_LONG_CONSTANT("VIO_COMPUTE_WRITE", 1, CONST_CS | CONST_PERSISTENT);

    /* Features */
    REGISTER_LONG_CONSTANT("VIO_FEATURE_COMPUTE", VIO_FEATURE_COMPUTE, CONST_CS | CONST_PERSISTENT);
    REGISTER_LONG_CONSTANT("VIO_FEATURE_RAYTRACING", VIO_FEATURE_RAYTRACING, CONST_CS | CONST_PERSISTENT);
    REGISTER_LONG_CONSTANT("VIO_FEATURE_TESSELLATION", VIO_FEATURE_TESSELLATION, CONST_CS | CONST_PERSISTENT);
    REGISTER_LONG_CONSTANT("VIO_FEATURE_GEOMETRY", VIO_FEATURE_GEOMETRY, CONST_CS | CONST_PERSISTENT);
    REGISTER_LONG_CONSTANT("VIO_FEATURE_MULTIVIEW", VIO_FEATURE_MULTIVIEW, CONST_CS | CONST_PERSISTENT);
    REGISTER_LONG_CONSTANT("VIO_FEATURE_3D_PIPELINE", VIO_FEATURE_3D_PIPELINE, CONST_CS | CONST_PERSISTENT);
    REGISTER_LONG_CONSTANT("VIO_FEATURE_READ_PIXELS", VIO_FEATURE_READ_PIXELS, CONST_CS | CONST_PERSISTENT);
    REGISTER_LONG_CONSTANT("VIO_FEATURE_INSTANCED_DRAW", VIO_FEATURE_INSTANCED_DRAW, CONST_CS | CONST_PERSISTENT);
    REGISTER_LONG_CONSTANT("VIO_FEATURE_RENDER_TARGET", VIO_FEATURE_RENDER_TARGET, CONST_CS | CONST_PERSISTENT);
    REGISTER_LONG_CONSTANT("VIO_FEATURE_RENDER_TARGET_HDR", VIO_FEATURE_RENDER_TARGET_HDR, CONST_CS | CONST_PERSISTENT);
    REGISTER_LONG_CONSTANT("VIO_FEATURE_RENDER_TARGET_DEPTH", VIO_FEATURE_RENDER_TARGET_DEPTH, CONST_CS | CONST_PERSISTENT);
    REGISTER_LONG_CONSTANT("VIO_FEATURE_RENDER_TARGET_MSAA", VIO_FEATURE_RENDER_TARGET_MSAA, CONST_CS | CONST_PERSISTENT);
    REGISTER_LONG_CONSTANT("VIO_FEATURE_CUBEMAP", VIO_FEATURE_CUBEMAP, CONST_CS | CONST_PERSISTENT);
    REGISTER_LONG_CONSTANT("VIO_FEATURE_DEPTH_BIAS", VIO_FEATURE_DEPTH_BIAS, CONST_CS | CONST_PERSISTENT);
    REGISTER_LONG_CONSTANT("VIO_FEATURE_SCISSOR", VIO_FEATURE_SCISSOR, CONST_CS | CONST_PERSISTENT);
    REGISTER_LONG_CONSTANT("VIO_FEATURE_TEXTURE_SWIZZLE", VIO_FEATURE_TEXTURE_SWIZZLE, CONST_CS | CONST_PERSISTENT);
    REGISTER_LONG_CONSTANT("VIO_FEATURE_NATIVE_2D_BATCH", VIO_FEATURE_NATIVE_2D_BATCH, CONST_CS | CONST_PERSISTENT);
    REGISTER_LONG_CONSTANT("VIO_FEATURE_DEBUG_OUTPUT", VIO_FEATURE_DEBUG_OUTPUT, CONST_CS | CONST_PERSISTENT);
    REGISTER_LONG_CONSTANT("VIO_FEATURE_DSA", VIO_FEATURE_DSA, CONST_CS | CONST_PERSISTENT);
    REGISTER_LONG_CONSTANT("VIO_FEATURE_BUFFER_STORAGE", VIO_FEATURE_BUFFER_STORAGE, CONST_CS | CONST_PERSISTENT);
    REGISTER_LONG_CONSTANT("VIO_FEATURE_TEXTURE_STORAGE", VIO_FEATURE_TEXTURE_STORAGE, CONST_CS | CONST_PERSISTENT);
    REGISTER_LONG_CONSTANT("VIO_FEATURE_SEPARATE_SHADERS", VIO_FEATURE_SEPARATE_SHADERS, CONST_CS | CONST_PERSISTENT);
    REGISTER_LONG_CONSTANT("VIO_FEATURE_TEXTURE_3D", VIO_FEATURE_TEXTURE_3D, CONST_CS | CONST_PERSISTENT);

    /* Actions */
    REGISTER_LONG_CONSTANT("VIO_RELEASE", VIO_RELEASE, CONST_CS | CONST_PERSISTENT);
    REGISTER_LONG_CONSTANT("VIO_PRESS", VIO_PRESS, CONST_CS | CONST_PERSISTENT);
    REGISTER_LONG_CONSTANT("VIO_REPEAT", VIO_REPEAT, CONST_CS | CONST_PERSISTENT);

    /* Mouse buttons */
    REGISTER_LONG_CONSTANT("VIO_MOUSE_LEFT", VIO_MOUSE_LEFT, CONST_CS | CONST_PERSISTENT);
    REGISTER_LONG_CONSTANT("VIO_MOUSE_RIGHT", VIO_MOUSE_RIGHT, CONST_CS | CONST_PERSISTENT);
    REGISTER_LONG_CONSTANT("VIO_MOUSE_MIDDLE", VIO_MOUSE_MIDDLE, CONST_CS | CONST_PERSISTENT);

    /* Keys */
    REGISTER_LONG_CONSTANT("VIO_KEY_UNKNOWN", VIO_KEY_UNKNOWN, CONST_CS | CONST_PERSISTENT);
    REGISTER_LONG_CONSTANT("VIO_KEY_SPACE", VIO_KEY_SPACE, CONST_CS | CONST_PERSISTENT);
    REGISTER_LONG_CONSTANT("VIO_KEY_APOSTROPHE", VIO_KEY_APOSTROPHE, CONST_CS | CONST_PERSISTENT);
    REGISTER_LONG_CONSTANT("VIO_KEY_COMMA", VIO_KEY_COMMA, CONST_CS | CONST_PERSISTENT);
    REGISTER_LONG_CONSTANT("VIO_KEY_MINUS", VIO_KEY_MINUS, CONST_CS | CONST_PERSISTENT);
    REGISTER_LONG_CONSTANT("VIO_KEY_PERIOD", VIO_KEY_PERIOD, CONST_CS | CONST_PERSISTENT);
    REGISTER_LONG_CONSTANT("VIO_KEY_SLASH", VIO_KEY_SLASH, CONST_CS | CONST_PERSISTENT);
    REGISTER_LONG_CONSTANT("VIO_KEY_0", VIO_KEY_0, CONST_CS | CONST_PERSISTENT);
    REGISTER_LONG_CONSTANT("VIO_KEY_1", VIO_KEY_1, CONST_CS | CONST_PERSISTENT);
    REGISTER_LONG_CONSTANT("VIO_KEY_2", VIO_KEY_2, CONST_CS | CONST_PERSISTENT);
    REGISTER_LONG_CONSTANT("VIO_KEY_3", VIO_KEY_3, CONST_CS | CONST_PERSISTENT);
    REGISTER_LONG_CONSTANT("VIO_KEY_4", VIO_KEY_4, CONST_CS | CONST_PERSISTENT);
    REGISTER_LONG_CONSTANT("VIO_KEY_5", VIO_KEY_5, CONST_CS | CONST_PERSISTENT);
    REGISTER_LONG_CONSTANT("VIO_KEY_6", VIO_KEY_6, CONST_CS | CONST_PERSISTENT);
    REGISTER_LONG_CONSTANT("VIO_KEY_7", VIO_KEY_7, CONST_CS | CONST_PERSISTENT);
    REGISTER_LONG_CONSTANT("VIO_KEY_8", VIO_KEY_8, CONST_CS | CONST_PERSISTENT);
    REGISTER_LONG_CONSTANT("VIO_KEY_9", VIO_KEY_9, CONST_CS | CONST_PERSISTENT);
    REGISTER_LONG_CONSTANT("VIO_KEY_SEMICOLON", VIO_KEY_SEMICOLON, CONST_CS | CONST_PERSISTENT);
    REGISTER_LONG_CONSTANT("VIO_KEY_EQUAL", VIO_KEY_EQUAL, CONST_CS | CONST_PERSISTENT);
    REGISTER_LONG_CONSTANT("VIO_KEY_A", VIO_KEY_A, CONST_CS | CONST_PERSISTENT);
    REGISTER_LONG_CONSTANT("VIO_KEY_B", VIO_KEY_B, CONST_CS | CONST_PERSISTENT);
    REGISTER_LONG_CONSTANT("VIO_KEY_C", VIO_KEY_C, CONST_CS | CONST_PERSISTENT);
    REGISTER_LONG_CONSTANT("VIO_KEY_D", VIO_KEY_D, CONST_CS | CONST_PERSISTENT);
    REGISTER_LONG_CONSTANT("VIO_KEY_E", VIO_KEY_E, CONST_CS | CONST_PERSISTENT);
    REGISTER_LONG_CONSTANT("VIO_KEY_F", VIO_KEY_F, CONST_CS | CONST_PERSISTENT);
    REGISTER_LONG_CONSTANT("VIO_KEY_G", VIO_KEY_G, CONST_CS | CONST_PERSISTENT);
    REGISTER_LONG_CONSTANT("VIO_KEY_H", VIO_KEY_H, CONST_CS | CONST_PERSISTENT);
    REGISTER_LONG_CONSTANT("VIO_KEY_I", VIO_KEY_I, CONST_CS | CONST_PERSISTENT);
    REGISTER_LONG_CONSTANT("VIO_KEY_J", VIO_KEY_J, CONST_CS | CONST_PERSISTENT);
    REGISTER_LONG_CONSTANT("VIO_KEY_K", VIO_KEY_K, CONST_CS | CONST_PERSISTENT);
    REGISTER_LONG_CONSTANT("VIO_KEY_L", VIO_KEY_L, CONST_CS | CONST_PERSISTENT);
    REGISTER_LONG_CONSTANT("VIO_KEY_M", VIO_KEY_M, CONST_CS | CONST_PERSISTENT);
    REGISTER_LONG_CONSTANT("VIO_KEY_N", VIO_KEY_N, CONST_CS | CONST_PERSISTENT);
    REGISTER_LONG_CONSTANT("VIO_KEY_O", VIO_KEY_O, CONST_CS | CONST_PERSISTENT);
    REGISTER_LONG_CONSTANT("VIO_KEY_P", VIO_KEY_P, CONST_CS | CONST_PERSISTENT);
    REGISTER_LONG_CONSTANT("VIO_KEY_Q", VIO_KEY_Q, CONST_CS | CONST_PERSISTENT);
    REGISTER_LONG_CONSTANT("VIO_KEY_R", VIO_KEY_R, CONST_CS | CONST_PERSISTENT);
    REGISTER_LONG_CONSTANT("VIO_KEY_S", VIO_KEY_S, CONST_CS | CONST_PERSISTENT);
    REGISTER_LONG_CONSTANT("VIO_KEY_T", VIO_KEY_T, CONST_CS | CONST_PERSISTENT);
    REGISTER_LONG_CONSTANT("VIO_KEY_U", VIO_KEY_U, CONST_CS | CONST_PERSISTENT);
    REGISTER_LONG_CONSTANT("VIO_KEY_V", VIO_KEY_V, CONST_CS | CONST_PERSISTENT);
    REGISTER_LONG_CONSTANT("VIO_KEY_W", VIO_KEY_W, CONST_CS | CONST_PERSISTENT);
    REGISTER_LONG_CONSTANT("VIO_KEY_X", VIO_KEY_X, CONST_CS | CONST_PERSISTENT);
    REGISTER_LONG_CONSTANT("VIO_KEY_Y", VIO_KEY_Y, CONST_CS | CONST_PERSISTENT);
    REGISTER_LONG_CONSTANT("VIO_KEY_Z", VIO_KEY_Z, CONST_CS | CONST_PERSISTENT);
    REGISTER_LONG_CONSTANT("VIO_KEY_LEFT_BRACKET", VIO_KEY_LEFT_BRACKET, CONST_CS | CONST_PERSISTENT);
    REGISTER_LONG_CONSTANT("VIO_KEY_BACKSLASH", VIO_KEY_BACKSLASH, CONST_CS | CONST_PERSISTENT);
    REGISTER_LONG_CONSTANT("VIO_KEY_RIGHT_BRACKET", VIO_KEY_RIGHT_BRACKET, CONST_CS | CONST_PERSISTENT);
    REGISTER_LONG_CONSTANT("VIO_KEY_GRAVE_ACCENT", VIO_KEY_GRAVE_ACCENT, CONST_CS | CONST_PERSISTENT);
    REGISTER_LONG_CONSTANT("VIO_KEY_ESCAPE", VIO_KEY_ESCAPE, CONST_CS | CONST_PERSISTENT);
    REGISTER_LONG_CONSTANT("VIO_KEY_ENTER", VIO_KEY_ENTER, CONST_CS | CONST_PERSISTENT);
    REGISTER_LONG_CONSTANT("VIO_KEY_TAB", VIO_KEY_TAB, CONST_CS | CONST_PERSISTENT);
    REGISTER_LONG_CONSTANT("VIO_KEY_BACKSPACE", VIO_KEY_BACKSPACE, CONST_CS | CONST_PERSISTENT);
    REGISTER_LONG_CONSTANT("VIO_KEY_INSERT", VIO_KEY_INSERT, CONST_CS | CONST_PERSISTENT);
    REGISTER_LONG_CONSTANT("VIO_KEY_DELETE", VIO_KEY_DELETE, CONST_CS | CONST_PERSISTENT);
    REGISTER_LONG_CONSTANT("VIO_KEY_RIGHT", VIO_KEY_RIGHT, CONST_CS | CONST_PERSISTENT);
    REGISTER_LONG_CONSTANT("VIO_KEY_LEFT", VIO_KEY_LEFT, CONST_CS | CONST_PERSISTENT);
    REGISTER_LONG_CONSTANT("VIO_KEY_DOWN", VIO_KEY_DOWN, CONST_CS | CONST_PERSISTENT);
    REGISTER_LONG_CONSTANT("VIO_KEY_UP", VIO_KEY_UP, CONST_CS | CONST_PERSISTENT);
    REGISTER_LONG_CONSTANT("VIO_KEY_PAGE_UP", VIO_KEY_PAGE_UP, CONST_CS | CONST_PERSISTENT);
    REGISTER_LONG_CONSTANT("VIO_KEY_PAGE_DOWN", VIO_KEY_PAGE_DOWN, CONST_CS | CONST_PERSISTENT);
    REGISTER_LONG_CONSTANT("VIO_KEY_HOME", VIO_KEY_HOME, CONST_CS | CONST_PERSISTENT);
    REGISTER_LONG_CONSTANT("VIO_KEY_END", VIO_KEY_END, CONST_CS | CONST_PERSISTENT);
    REGISTER_LONG_CONSTANT("VIO_KEY_CAPS_LOCK", VIO_KEY_CAPS_LOCK, CONST_CS | CONST_PERSISTENT);
    REGISTER_LONG_CONSTANT("VIO_KEY_SCROLL_LOCK", VIO_KEY_SCROLL_LOCK, CONST_CS | CONST_PERSISTENT);
    REGISTER_LONG_CONSTANT("VIO_KEY_NUM_LOCK", VIO_KEY_NUM_LOCK, CONST_CS | CONST_PERSISTENT);
    REGISTER_LONG_CONSTANT("VIO_KEY_PRINT_SCREEN", VIO_KEY_PRINT_SCREEN, CONST_CS | CONST_PERSISTENT);
    REGISTER_LONG_CONSTANT("VIO_KEY_PAUSE", VIO_KEY_PAUSE, CONST_CS | CONST_PERSISTENT);
    REGISTER_LONG_CONSTANT("VIO_KEY_F1", VIO_KEY_F1, CONST_CS | CONST_PERSISTENT);
    REGISTER_LONG_CONSTANT("VIO_KEY_F2", VIO_KEY_F2, CONST_CS | CONST_PERSISTENT);
    REGISTER_LONG_CONSTANT("VIO_KEY_F3", VIO_KEY_F3, CONST_CS | CONST_PERSISTENT);
    REGISTER_LONG_CONSTANT("VIO_KEY_F4", VIO_KEY_F4, CONST_CS | CONST_PERSISTENT);
    REGISTER_LONG_CONSTANT("VIO_KEY_F5", VIO_KEY_F5, CONST_CS | CONST_PERSISTENT);
    REGISTER_LONG_CONSTANT("VIO_KEY_F6", VIO_KEY_F6, CONST_CS | CONST_PERSISTENT);
    REGISTER_LONG_CONSTANT("VIO_KEY_F7", VIO_KEY_F7, CONST_CS | CONST_PERSISTENT);
    REGISTER_LONG_CONSTANT("VIO_KEY_F8", VIO_KEY_F8, CONST_CS | CONST_PERSISTENT);
    REGISTER_LONG_CONSTANT("VIO_KEY_F9", VIO_KEY_F9, CONST_CS | CONST_PERSISTENT);
    REGISTER_LONG_CONSTANT("VIO_KEY_F10", VIO_KEY_F10, CONST_CS | CONST_PERSISTENT);
    REGISTER_LONG_CONSTANT("VIO_KEY_F11", VIO_KEY_F11, CONST_CS | CONST_PERSISTENT);
    REGISTER_LONG_CONSTANT("VIO_KEY_F12", VIO_KEY_F12, CONST_CS | CONST_PERSISTENT);
    REGISTER_LONG_CONSTANT("VIO_KEY_LEFT_SHIFT", VIO_KEY_LEFT_SHIFT, CONST_CS | CONST_PERSISTENT);
    REGISTER_LONG_CONSTANT("VIO_KEY_LEFT_CONTROL", VIO_KEY_LEFT_CONTROL, CONST_CS | CONST_PERSISTENT);
    REGISTER_LONG_CONSTANT("VIO_KEY_LEFT_ALT", VIO_KEY_LEFT_ALT, CONST_CS | CONST_PERSISTENT);
    REGISTER_LONG_CONSTANT("VIO_KEY_LEFT_SUPER", VIO_KEY_LEFT_SUPER, CONST_CS | CONST_PERSISTENT);
    REGISTER_LONG_CONSTANT("VIO_KEY_RIGHT_SHIFT", VIO_KEY_RIGHT_SHIFT, CONST_CS | CONST_PERSISTENT);
    REGISTER_LONG_CONSTANT("VIO_KEY_RIGHT_CONTROL", VIO_KEY_RIGHT_CONTROL, CONST_CS | CONST_PERSISTENT);
    REGISTER_LONG_CONSTANT("VIO_KEY_RIGHT_ALT", VIO_KEY_RIGHT_ALT, CONST_CS | CONST_PERSISTENT);
    REGISTER_LONG_CONSTANT("VIO_KEY_RIGHT_SUPER", VIO_KEY_RIGHT_SUPER, CONST_CS | CONST_PERSISTENT);
    REGISTER_LONG_CONSTANT("VIO_KEY_MENU", VIO_KEY_MENU, CONST_CS | CONST_PERSISTENT);

    /* Modifier keys */
    REGISTER_LONG_CONSTANT("VIO_MOD_SHIFT", VIO_MOD_SHIFT, CONST_CS | CONST_PERSISTENT);
    REGISTER_LONG_CONSTANT("VIO_MOD_CONTROL", VIO_MOD_CONTROL, CONST_CS | CONST_PERSISTENT);
    REGISTER_LONG_CONSTANT("VIO_MOD_ALT", VIO_MOD_ALT, CONST_CS | CONST_PERSISTENT);
    REGISTER_LONG_CONSTANT("VIO_MOD_SUPER", VIO_MOD_SUPER, CONST_CS | CONST_PERSISTENT);
    REGISTER_LONG_CONSTANT("VIO_MOD_CAPS_LOCK", VIO_MOD_CAPS_LOCK, CONST_CS | CONST_PERSISTENT);
    REGISTER_LONG_CONSTANT("VIO_MOD_NUM_LOCK", VIO_MOD_NUM_LOCK, CONST_CS | CONST_PERSISTENT);

    /* Gamepad buttons */
    REGISTER_LONG_CONSTANT("VIO_GAMEPAD_A", VIO_GAMEPAD_A, CONST_CS | CONST_PERSISTENT);
    REGISTER_LONG_CONSTANT("VIO_GAMEPAD_B", VIO_GAMEPAD_B, CONST_CS | CONST_PERSISTENT);
    REGISTER_LONG_CONSTANT("VIO_GAMEPAD_X", VIO_GAMEPAD_X, CONST_CS | CONST_PERSISTENT);
    REGISTER_LONG_CONSTANT("VIO_GAMEPAD_Y", VIO_GAMEPAD_Y, CONST_CS | CONST_PERSISTENT);
    REGISTER_LONG_CONSTANT("VIO_GAMEPAD_LEFT_BUMPER", VIO_GAMEPAD_LEFT_BUMPER, CONST_CS | CONST_PERSISTENT);
    REGISTER_LONG_CONSTANT("VIO_GAMEPAD_RIGHT_BUMPER", VIO_GAMEPAD_RIGHT_BUMPER, CONST_CS | CONST_PERSISTENT);
    REGISTER_LONG_CONSTANT("VIO_GAMEPAD_BACK", VIO_GAMEPAD_BACK, CONST_CS | CONST_PERSISTENT);
    REGISTER_LONG_CONSTANT("VIO_GAMEPAD_START", VIO_GAMEPAD_START, CONST_CS | CONST_PERSISTENT);
    REGISTER_LONG_CONSTANT("VIO_GAMEPAD_GUIDE", VIO_GAMEPAD_GUIDE, CONST_CS | CONST_PERSISTENT);
    REGISTER_LONG_CONSTANT("VIO_GAMEPAD_LEFT_THUMB", VIO_GAMEPAD_LEFT_THUMB, CONST_CS | CONST_PERSISTENT);
    REGISTER_LONG_CONSTANT("VIO_GAMEPAD_RIGHT_THUMB", VIO_GAMEPAD_RIGHT_THUMB, CONST_CS | CONST_PERSISTENT);
    REGISTER_LONG_CONSTANT("VIO_GAMEPAD_DPAD_UP", VIO_GAMEPAD_DPAD_UP, CONST_CS | CONST_PERSISTENT);
    REGISTER_LONG_CONSTANT("VIO_GAMEPAD_DPAD_RIGHT", VIO_GAMEPAD_DPAD_RIGHT, CONST_CS | CONST_PERSISTENT);
    REGISTER_LONG_CONSTANT("VIO_GAMEPAD_DPAD_DOWN", VIO_GAMEPAD_DPAD_DOWN, CONST_CS | CONST_PERSISTENT);
    REGISTER_LONG_CONSTANT("VIO_GAMEPAD_DPAD_LEFT", VIO_GAMEPAD_DPAD_LEFT, CONST_CS | CONST_PERSISTENT);

    /* Gamepad axes */
    REGISTER_LONG_CONSTANT("VIO_GAMEPAD_AXIS_LEFT_X", VIO_GAMEPAD_AXIS_LEFT_X, CONST_CS | CONST_PERSISTENT);
    REGISTER_LONG_CONSTANT("VIO_GAMEPAD_AXIS_LEFT_Y", VIO_GAMEPAD_AXIS_LEFT_Y, CONST_CS | CONST_PERSISTENT);
    REGISTER_LONG_CONSTANT("VIO_GAMEPAD_AXIS_RIGHT_X", VIO_GAMEPAD_AXIS_RIGHT_X, CONST_CS | CONST_PERSISTENT);
    REGISTER_LONG_CONSTANT("VIO_GAMEPAD_AXIS_RIGHT_Y", VIO_GAMEPAD_AXIS_RIGHT_Y, CONST_CS | CONST_PERSISTENT);
    REGISTER_LONG_CONSTANT("VIO_GAMEPAD_AXIS_LEFT_TRIGGER", VIO_GAMEPAD_AXIS_LEFT_TRIGGER, CONST_CS | CONST_PERSISTENT);
    REGISTER_LONG_CONSTANT("VIO_GAMEPAD_AXIS_RIGHT_TRIGGER", VIO_GAMEPAD_AXIS_RIGHT_TRIGGER, CONST_CS | CONST_PERSISTENT);

    /* Plugin types */
    REGISTER_LONG_CONSTANT("VIO_PLUGIN_TYPE_GENERIC", VIO_PLUGIN_TYPE_GENERIC, CONST_CS | CONST_PERSISTENT);
    REGISTER_LONG_CONSTANT("VIO_PLUGIN_TYPE_OUTPUT", VIO_PLUGIN_TYPE_OUTPUT, CONST_CS | CONST_PERSISTENT);
    REGISTER_LONG_CONSTANT("VIO_PLUGIN_TYPE_INPUT", VIO_PLUGIN_TYPE_INPUT, CONST_CS | CONST_PERSISTENT);
    REGISTER_LONG_CONSTANT("VIO_PLUGIN_TYPE_FILTER", VIO_PLUGIN_TYPE_FILTER, CONST_CS | CONST_PERSISTENT);
    REGISTER_LONG_CONSTANT("VIO_PLUGIN_API_VERSION", VIO_PLUGIN_API_VERSION, CONST_CS | CONST_PERSISTENT);

    /* 1 when the extension was built with HarfBuzz — complex-script shaping and
     * BiDi (Arabic, Thai, ligatures, mixed LTR/RTL) are active for all fonts.
     * 0 when built without it: text uses the legacy codepoint-per-glyph path,
     * which cannot render those scripts. */
#ifdef HAVE_HARFBUZZ
    REGISTER_LONG_CONSTANT("VIO_HAS_SHAPING", 1, CONST_CS | CONST_PERSISTENT);
#else
    REGISTER_LONG_CONSTANT("VIO_HAS_SHAPING", 0, CONST_CS | CONST_PERSISTENT);
#endif
}

/* ── Plugin functions ─────────────────────────────────────────────── */

ZEND_FUNCTION(vio_plugins)
{
    ZEND_PARSE_PARAMETERS_NONE();

    array_init(return_value);
    int count = vio_plugin_count();
    for (int i = 0; i < count; i++) {
        const char *name = vio_get_plugin_name(i);
        if (name) {
            add_next_index_string(return_value, name);
        }
    }
}

ZEND_FUNCTION(vio_plugin_info)
{
    char *name;
    size_t name_len;

    ZEND_PARSE_PARAMETERS_START(1, 1)
        Z_PARAM_STRING(name, name_len)
    ZEND_PARSE_PARAMETERS_END();

    const vio_plugin *plugin = vio_find_plugin(name);
    if (!plugin) {
        RETURN_FALSE;
    }

    array_init(return_value);
    add_assoc_string(return_value, "name", (char *)plugin->name);
    add_assoc_string(return_value, "description", plugin->description ? (char *)plugin->description : "");
    add_assoc_string(return_value, "version", plugin->version ? (char *)plugin->version : "");
    add_assoc_long(return_value, "api_version", plugin->api_version);
    add_assoc_long(return_value, "type", plugin->type);

    /* Type flags as readable array */
    zval types;
    array_init(&types);
    if (plugin->type & VIO_PLUGIN_TYPE_OUTPUT) {
        add_next_index_string(&types, "output");
    }
    if (plugin->type & VIO_PLUGIN_TYPE_INPUT) {
        add_next_index_string(&types, "input");
    }
    if (plugin->type & VIO_PLUGIN_TYPE_FILTER) {
        add_next_index_string(&types, "filter");
    }
    if (plugin->type == VIO_PLUGIN_TYPE_GENERIC) {
        add_next_index_string(&types, "generic");
    }
    add_assoc_zval(return_value, "types", &types);
}

/* ── Async texture loading ───────────────────────────────────────── */

static int le_vio_async_load;

typedef struct _vio_async_texture_load {
    char          *path;
    unsigned char *data;
    int            width;
    int            height;
    int            channels;
    int            done;
    int            failed;
    int            consumed;   /* poll already produced a result */
} vio_async_texture_load;

#ifdef PHP_WIN32
static unsigned __stdcall vio_texture_load_thread(void *arg)
#else
static void *vio_texture_load_thread(void *arg)
#endif
{
    vio_async_texture_load *load = (vio_async_texture_load *)arg;
    load->data = stbi_load(load->path, &load->width, &load->height, &load->channels, 4);
    if (!load->data) {
        load->failed = 1;
    }
    load->done = 1;
#ifdef PHP_WIN32
    return 0;
#else
    return NULL;
#endif
}

ZEND_FUNCTION(vio_texture_load_async)
{
    char *path;
    size_t path_len;

    ZEND_PARSE_PARAMETERS_START(1, 1)
        Z_PARAM_STRING(path, path_len)
    ZEND_PARSE_PARAMETERS_END();

    vio_async_texture_load *load = ecalloc(1, sizeof(vio_async_texture_load));
    load->path = estrndup(path, path_len);

#ifdef PHP_WIN32
    HANDLE thread = (HANDLE)_beginthreadex(NULL, 0, vio_texture_load_thread, load, 0, NULL);
    if (!thread) {
        efree(load->path);
        efree(load);
        php_error_docref(NULL, E_WARNING, "Failed to create loading thread");
        RETURN_FALSE;
    }
    CloseHandle(thread);
#else
    pthread_t thread;
    if (pthread_create(&thread, NULL, vio_texture_load_thread, load) != 0) {
        efree(load->path);
        efree(load);
        php_error_docref(NULL, E_WARNING, "Failed to create loading thread");
        RETURN_FALSE;
    }
    pthread_detach(thread);
#endif

    /* Return as opaque resource */
    zend_resource *res = zend_register_resource(load, le_vio_async_load);
    RETURN_RES(res);
}

ZEND_FUNCTION(vio_texture_load_poll)
{
    zval *res_zval;

    ZEND_PARSE_PARAMETERS_START(1, 1)
        Z_PARAM_RESOURCE(res_zval)
    ZEND_PARSE_PARAMETERS_END();

    vio_async_texture_load *load = (vio_async_texture_load *)zend_fetch_resource(
        Z_RES_P(res_zval), "vio_async_load", le_vio_async_load);
    if (!load) {
        RETURN_FALSE;
    }

    /* A previous poll already consumed the result; report failure without
     * touching the resource. */
    if (load->consumed) {
        RETURN_FALSE;
    }

    if (!load->done) {
        RETURN_NULL(); /* Still loading */
    }

    /* Mark consumed and free worker data eagerly rather than calling
     * zend_list_delete() from inside the poll. Deleting the resource here frees
     * it while the caller may still hold a PHP reference to the handle (e.g. in
     * an array passed by reference); that dangling resource is then double-freed
     * at request shutdown and corrupts the Zend heap. Letting PHP drop the last
     * reference naturally — with the dtor guarded by the freed pointers — is the
     * safe lifecycle. */
    load->consumed = 1;

    if (load->failed) {
        RETURN_FALSE;
    }

    /* Return array with image data info */
    array_init(return_value);
    add_assoc_long(return_value, "width", load->width);
    add_assoc_long(return_value, "height", load->height);
    add_assoc_stringl(return_value, "data", (char *)load->data, load->width * load->height * 4);
    stbi_image_free(load->data);
    load->data = NULL;
}

static void vio_async_load_dtor(zend_resource *res)
{
    vio_async_texture_load *load = (vio_async_texture_load *)res->ptr;
    if (load) {
        /* If still loading, we can't safely free — wait for it */
        while (!load->done) {
            usleep(1000);
        }
        if (load->data) {
            stbi_image_free(load->data);
        }
        if (load->path) {
            efree(load->path);
        }
        efree(load);
    }
}

/* ── Async font loading ──────────────────────────────────────────────
 *
 * Mirrors the async texture lifecycle above. The expensive part of loading a
 * large fallback font (e.g. the ~13 MB CJK NotoSansSC/KR) is rasterizing the
 * 4096x4096 multi-range glyph atlas with stb_truetype — a pure-CPU pack over
 * ~32k codepoints that previously froze the render thread for 20-25 s the
 * first time a CJK glyph hit the fallback chain.
 *
 * The split, identical in spirit to the texture path (decode off-thread, GPU
 * upload on the render thread at poll time):
 *
 *   Worker thread (vio_font_load_thread):
 *     - raw fopen/fread of the TTF (NOT php_stream — php streams are
 *       per-request and not thread-safe)
 *     - vio_font_pack_atlas_raw(): rasterize the atlas bitmap + produce a flat,
 *       malloc-backed glyph array. No Zend allocator, no GPU calls.
 *
 *   Render thread (vio_font_load_poll):
 *     - build the VioFont object, copy the TTF into emalloc memory, populate
 *       the Zend glyph_map via vio_font_finalize_glyphs()
 *     - vio_font_upload_atlas_to_gpu(): upload the atlas against the current
 *       GL / Metal / D3D / Vulkan context (must be the render thread)
 *
 * All worker-touched buffers use libc malloc/free so the worker never touches
 * the Zend heap. The backend pointer is captured at submit time and reused at
 * poll time. Until the poll succeeds the caller renders .notdef (the engine
 * simply skips the not-yet-ready fallback font in its chain). */

/* Cross-thread publication fences for the worker -> render-thread handoff.
 * The worker writes the result fields then sets `done`; the render thread reads
 * `done` then the result fields. A release fence on the producer paired with an
 * acquire fence on the consumer guarantees the reader sees all the producer's
 * writes once it observes done == 1. Falls back to a full compiler+memory
 * barrier where C11 atomics are unavailable (older MSVC). */
#if defined(__GNUC__) || defined(__clang__)
#  define VIO_ATOMIC_THREAD_FENCE_RELEASE() __atomic_thread_fence(__ATOMIC_RELEASE)
#  define VIO_ATOMIC_THREAD_FENCE_ACQUIRE() __atomic_thread_fence(__ATOMIC_ACQUIRE)
#elif defined(PHP_WIN32)
#  include <windows.h>
#  define VIO_ATOMIC_THREAD_FENCE_RELEASE() MemoryBarrier()
#  define VIO_ATOMIC_THREAD_FENCE_ACQUIRE() MemoryBarrier()
#else
#  define VIO_ATOMIC_THREAD_FENCE_RELEASE() do { } while (0)
#  define VIO_ATOMIC_THREAD_FENCE_ACQUIRE() do { } while (0)
#endif

static int le_vio_async_font;

typedef struct _vio_async_font_load {
    char                    *path;          /* malloc */
    float                    font_size;      /* atlas rasterization size (physical px) */
    float                    render_scale;   /* atlas-px -> logical-px divisor */
    const vio_backend       *backend;       /* captured at submit; render-thread use only */
    unsigned char           *ttf_data;      /* malloc — raw TTF bytes */
    size_t                   ttf_len;
    unsigned char           *atlas_bitmap;  /* malloc — R8 coverage atlas */
    int                      atlas_side;     /* dynamic atlas dimension (square) */
    vio_font_packed_glyph   *glyphs;        /* malloc — flat packed glyph array */
    int                      glyph_count;
    volatile int             done;          /* set last by the worker */
    volatile int             failed;
    int                      consumed;      /* poll already produced a result */
} vio_async_font_load;

#ifdef PHP_WIN32
static unsigned __stdcall vio_font_load_thread(void *arg)
#else
static void *vio_font_load_thread(void *arg)
#endif
{
    vio_async_font_load *load = (vio_async_font_load *)arg;

    /* Read the whole TTF with libc stdio — thread-safe, unlike php_stream. */
    FILE *fp = fopen(load->path, "rb");
    if (fp) {
        if (fseek(fp, 0, SEEK_END) == 0) {
            long sz = ftell(fp);
            if (sz > 0 && fseek(fp, 0, SEEK_SET) == 0) {
                unsigned char *buf = (unsigned char *)malloc((size_t)sz);
                if (buf && fread(buf, 1, (size_t)sz, fp) == (size_t)sz) {
                    load->ttf_data = buf;
                    load->ttf_len  = (size_t)sz;
                } else {
                    free(buf);
                }
            }
        }
        fclose(fp);
    }

    if (load->ttf_data) {
        /* Dynamically-sized atlas: the bitmap is malloc'd at the smallest size
         * that fits this font's glyphs (down to ~512² for small Latin sizes vs
         * the old fixed 4096²). A partially-packed atlas still produces a usable
         * font, so only a NULL bitmap / glyph array counts as failure. */
        vio_font_pack_atlas_dynamic(load->ttf_data, load->font_size,
                                    &load->atlas_bitmap, &load->atlas_side,
                                    &load->glyphs, &load->glyph_count);
        if (!load->atlas_bitmap || !load->glyphs) {
            load->failed = 1;
        }
    } else {
        load->failed = 1;
    }

    /* Release barrier: publish all the result writes (ttf_data, atlas_bitmap,
     * glyphs, glyph_count, failed) BEFORE the render thread observes done == 1.
     * Without this the compiler / CPU may make `done` visible while glyphs or
     * atlas_bitmap still hold stale/NULL values, and the poll would then read a
     * half-initialised struct — corrupting the Zend heap when it finalises the
     * glyph map from a garbage pointer. (volatile alone orders the volatile
     * stores but does not order the plain pointer stores against `done`.) */
    VIO_ATOMIC_THREAD_FENCE_RELEASE();
    load->done = 1;
#ifdef PHP_WIN32
    return 0;
#else
    return NULL;
#endif
}

ZEND_FUNCTION(vio_font_load_async)
{
    zval *ctx_zval;
    char *path;
    size_t path_len;
    double size = 24.0;
    double scale = 1.0;

    ZEND_PARSE_PARAMETERS_START(2, 4)
        Z_PARAM_OBJECT_OF_CLASS(ctx_zval, vio_context_ce)
        Z_PARAM_STRING(path, path_len)
        Z_PARAM_OPTIONAL
        Z_PARAM_DOUBLE(size)
        Z_PARAM_DOUBLE(scale)
    ZEND_PARSE_PARAMETERS_END();

    vio_context_object *ctx = Z_VIO_CONTEXT_P(ctx_zval);
    if (!ctx->initialized) {
        php_error_docref(NULL, E_WARNING, "Context is not initialized");
        RETURN_FALSE;
    }

    if (scale < 1.0) scale = 1.0;

    vio_async_font_load *load = ecalloc(1, sizeof(vio_async_font_load));
    /* path uses libc malloc — it is read on the worker thread */
    load->path = (char *)malloc(path_len + 1);
    if (!load->path) {
        efree(load);
        php_error_docref(NULL, E_WARNING, "Out of memory");
        RETURN_FALSE;
    }
    memcpy(load->path, path, path_len);
    load->path[path_len] = '\0';
    load->render_scale = (float)scale;
    load->font_size = (float)(size * scale);
    load->backend   = ctx->backend;

#ifdef PHP_WIN32
    HANDLE thread = (HANDLE)_beginthreadex(NULL, 0, vio_font_load_thread, load, 0, NULL);
    if (!thread) {
        free(load->path);
        efree(load);
        php_error_docref(NULL, E_WARNING, "Failed to create font loading thread");
        RETURN_FALSE;
    }
    CloseHandle(thread);
#else
    pthread_t thread;
    if (pthread_create(&thread, NULL, vio_font_load_thread, load) != 0) {
        free(load->path);
        efree(load);
        php_error_docref(NULL, E_WARNING, "Failed to create font loading thread");
        RETURN_FALSE;
    }
    pthread_detach(thread);
#endif

    zend_resource *res = zend_register_resource(load, le_vio_async_font);
    RETURN_RES(res);
}

ZEND_FUNCTION(vio_font_load_poll)
{
    zval *res_zval;

    ZEND_PARSE_PARAMETERS_START(1, 1)
        Z_PARAM_RESOURCE(res_zval)
    ZEND_PARSE_PARAMETERS_END();

    vio_async_font_load *load = (vio_async_font_load *)zend_fetch_resource(
        Z_RES_P(res_zval), "vio_async_font", le_vio_async_font);
    if (!load) {
        RETURN_FALSE;
    }

    /* A previous poll already produced a result for this handle. Report failure
     * (the result was returned once and the worker buffers are gone), but do
     * NOT touch the resource — see the buffer-freeing note below. */
    if (load->consumed) {
        RETURN_FALSE;
    }

    if (!load->done) {
        RETURN_NULL(); /* Still loading */
    }

    /* Acquire barrier paired with the worker's release fence: now that we have
     * observed done == 1, this guarantees the result fields below (failed,
     * ttf_data, atlas_bitmap, glyphs, glyph_count) reflect the worker's
     * completed writes rather than stale values. */
    VIO_ATOMIC_THREAD_FENCE_ACQUIRE();

    /* Mark consumed and release the worker buffers eagerly here rather than via
     * zend_list_delete(): deleting the resource from inside the poll frees it
     * while the caller may still hold a PHP reference to the handle (e.g. in an
     * array passed by reference), which leaves a dangling resource that is
     * double-freed at request shutdown and corrupts the Zend heap. Instead we
     * free the (libc-allocated) worker buffers now, flag the load consumed, and
     * let the resource die naturally when PHP drops its last reference — the
     * dtor then sees consumed and frees nothing. */
    load->consumed = 1;

    if (load->failed) {
        if (load->ttf_data)     { free(load->ttf_data);     load->ttf_data = NULL; }
        if (load->atlas_bitmap) { free(load->atlas_bitmap); load->atlas_bitmap = NULL; }
        if (load->glyphs)       { free(load->glyphs);       load->glyphs = NULL; }
        RETURN_FALSE;
    }

    /* Worker finished the CPU-only work. Build the VioFont on the render
     * thread: copy the TTF into Zend memory, populate the glyph map, and
     * upload the atlas to the GPU (current render context required). */
    zval font_zval;
    object_init_ex(&font_zval, vio_font_ce);
    vio_font_object *font = Z_VIO_FONT_P(&font_zval);

    font->font_size    = load->font_size;
    font->render_scale = load->render_scale;
    font->backend   = load->backend;
    font->ttf_len   = load->ttf_len;
    font->ttf_data  = emalloc(load->ttf_len);
    memcpy(font->ttf_data, load->ttf_data, load->ttf_len);

    font->atlas_w = font->atlas_h = load->atlas_side;
    vio_font_finalize_glyphs(font, load->glyphs, load->glyph_count);
    vio_font_upload_atlas_to_gpu(font, load->backend, load->atlas_bitmap);

    font->valid = 1;

    /* Release the worker buffers now — they have been fully consumed. The
     * resource itself stays alive until PHP releases the handle. */
    if (load->ttf_data)     { free(load->ttf_data);     load->ttf_data = NULL; }
    if (load->atlas_bitmap) { free(load->atlas_bitmap); load->atlas_bitmap = NULL; }
    if (load->glyphs)       { free(load->glyphs);       load->glyphs = NULL; }

    RETURN_COPY_VALUE(&font_zval);
}

static void vio_async_font_dtor(zend_resource *res)
{
    vio_async_font_load *load = (vio_async_font_load *)res->ptr;
    if (load) {
        /* If still loading, we can't safely free the worker buffers — wait. */
        while (!load->done) {
            usleep(1000);
        }
        if (load->ttf_data)     free(load->ttf_data);
        if (load->atlas_bitmap) free(load->atlas_bitmap);
        if (load->glyphs)       free(load->glyphs);
        if (load->path)         free(load->path);
        efree(load);
    }
}

ZEND_FUNCTION(vio_texture_size)
{
    zval *tex_zval;

    ZEND_PARSE_PARAMETERS_START(1, 1)
        Z_PARAM_OBJECT_OF_CLASS(tex_zval, vio_texture_ce)
    ZEND_PARSE_PARAMETERS_END();

    vio_texture_object *tex = Z_VIO_TEXTURE_P(tex_zval);

    array_init(return_value);
    add_index_long(return_value, 0, tex->width);
    add_index_long(return_value, 1, tex->height);
}

/* ── 3D: Render targets, cubemaps, instancing, viewport ──────────── */

ZEND_FUNCTION(vio_viewport)
{
    zval *ctx_zval;
    zend_long x, y, w, h;

    ZEND_PARSE_PARAMETERS_START(5, 5)
        Z_PARAM_OBJECT_OF_CLASS(ctx_zval, vio_context_ce)
        Z_PARAM_LONG(x)
        Z_PARAM_LONG(y)
        Z_PARAM_LONG(w)
        Z_PARAM_LONG(h)
    ZEND_PARSE_PARAMETERS_END();

    vio_context_object *ctx = Z_VIO_CONTEXT_P(ctx_zval);

    if (!ctx->initialized) {
        php_error_docref(NULL, E_WARNING, "Context is not initialized");
        return;
    }

    if (ctx->backend->set_viewport) {
        ctx->backend->set_viewport((int)x, (int)y, (int)w, (int)h);
    }
}

ZEND_FUNCTION(vio_draw_3d)
{
    zval *ctx_zval;

    ZEND_PARSE_PARAMETERS_START(1, 1)
        Z_PARAM_OBJECT_OF_CLASS(ctx_zval, vio_context_ce)
    ZEND_PARSE_PARAMETERS_END();

    vio_context_object *ctx = Z_VIO_CONTEXT_P(ctx_zval);

    if (!ctx->initialized || !ctx->in_frame) {
        php_error_docref(NULL, E_WARNING, "Must call vio_draw_3d between vio_begin and vio_end");
        return;
    }

    /* 3D draws are issued inline via vio_draw / vio_draw_instanced. This
     * function serves as a flush/sync point — reset bound VAO/program so
     * the 2D batcher (or a subsequent RT switch) starts from a clean slate. */
    if (ctx->backend->flush_draw_state) {
        ctx->backend->flush_draw_state();
    }
}

/* Helper: convert PHP array|string matrices to float buffer.
 * Returns mat_data (caller frees with efree, or NULL if string was used).
 * Sets *out_data to the float pointer and *out_size to byte count. */
static int vio_resolve_instance_data(zval *matrices_zval, zend_long instance_count,
                                      const float **out_data, size_t *out_size,
                                      float **out_allocated)
{
    size_t total_floats = (size_t)instance_count * 16;
    size_t byte_size = total_floats * sizeof(float);

    *out_allocated = NULL;

    if (Z_TYPE_P(matrices_zval) == IS_STRING) {
        /* Fast path: packed binary float data — zero iteration */
        zend_string *str = Z_STR_P(matrices_zval);
        if (ZSTR_LEN(str) < byte_size) {
            php_error_docref(NULL, E_WARNING,
                "matrices string has %zu bytes, need %zu (%ld instances * 64)",
                ZSTR_LEN(str), byte_size, (long)instance_count);
            return -1;
        }
        *out_data = (const float *)ZSTR_VAL(str);
        *out_size = byte_size;
        return 0;
    }

    if (Z_TYPE_P(matrices_zval) == IS_ARRAY) {
        /* Slow path: PHP array of floats — iterate and convert */
        HashTable *ht = Z_ARRVAL_P(matrices_zval);
        size_t arr_count = zend_hash_num_elements(ht);
        if (arr_count < total_floats) {
            php_error_docref(NULL, E_WARNING,
                "matrices array has %zu elements, need %zu (%ld instances * 16)",
                arr_count, total_floats, (long)instance_count);
            return -1;
        }

        float *mat_data = emalloc(byte_size);
        zval *val;
        size_t i = 0;
        ZEND_HASH_FOREACH_VAL(ht, val) {
            if (i >= total_floats) break;
            mat_data[i++] = (float)zval_get_double(val);
        } ZEND_HASH_FOREACH_END();

        *out_data = mat_data;
        *out_size = byte_size;
        *out_allocated = mat_data;
        return 0;
    }

    php_error_docref(NULL, E_WARNING, "matrices must be array or packed string");
    return -1;
}

ZEND_FUNCTION(vio_draw_instanced)
{
    zval *ctx_zval;
    zval *mesh_zval;
    zval *matrices_zval;
    zend_long instance_count;

    ZEND_PARSE_PARAMETERS_START(4, 4)
        Z_PARAM_OBJECT_OF_CLASS(ctx_zval, vio_context_ce)
        Z_PARAM_OBJECT_OF_CLASS(mesh_zval, vio_mesh_ce)
        Z_PARAM_ZVAL(matrices_zval)
        Z_PARAM_LONG(instance_count)
    ZEND_PARSE_PARAMETERS_END();

    vio_context_object *ctx = Z_VIO_CONTEXT_P(ctx_zval);
    vio_mesh_object *mesh = Z_VIO_MESH_P(mesh_zval);

    if (!ctx->initialized || !ctx->in_frame) {
        php_error_docref(NULL, E_WARNING, "Must call vio_draw_instanced between vio_begin and vio_end");
        return;
    }

    if (instance_count <= 0) {
        return;
    }

    /* Resolve matrix data (fast binary or slow array path) */
    const float *mat_data = NULL;
    size_t mat_size = 0;
    float *allocated = NULL;

    if (vio_resolve_instance_data(matrices_zval, instance_count, &mat_data, &mat_size, &allocated) != 0) {
        return;
    }

    if (ctx->backend->draw_mesh_instanced) {
        ctx->backend->draw_mesh_instanced(mesh, mat_data, (int)instance_count);
    }

    /* Backend instanced draw (D3D11/D3D12/Vulkan) */
    if (strcmp(ctx->backend->name, "opengl") != 0 && mesh->backend_vb) {

#ifdef HAVE_D3D11
        if (strcmp(ctx->backend->name, "d3d11") == 0 && vio_d3d11.initialized) {
            UINT byte_size = (UINT)mat_size;

            /* Reuse persistent DYNAMIC instance buffer (avoids CreateBuffer per frame) */
            static ID3D11Buffer *s_instance_buf = NULL;
            static UINT s_instance_buf_capacity = 0;

            if (!s_instance_buf || s_instance_buf_capacity < byte_size) {
                if (s_instance_buf) {
                    ID3D11Buffer_Release(s_instance_buf);
                    s_instance_buf = NULL;
                }

                D3D11_BUFFER_DESC bd = {0};
                bd.ByteWidth = byte_size;
                bd.Usage = D3D11_USAGE_DYNAMIC;
                bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
                bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;

                HRESULT hr = ID3D11Device_CreateBuffer(vio_d3d11.device, &bd, NULL, &s_instance_buf);
                if (FAILED(hr)) {
                    if (allocated) efree(allocated);
                    return;
                }
                s_instance_buf_capacity = byte_size;
            }

            /* Upload via Map/Unmap WRITE_DISCARD */
            D3D11_MAPPED_SUBRESOURCE mapped = {0};
            HRESULT hr = ID3D11DeviceContext_Map(vio_d3d11.context,
                (ID3D11Resource *)s_instance_buf, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
            if (SUCCEEDED(hr)) {
                memcpy(mapped.pData, mat_data, byte_size);
                ID3D11DeviceContext_Unmap(vio_d3d11.context, (ID3D11Resource *)s_instance_buf, 0);

                /* Bind mesh VB to slot 0, instance VB to slot 1 */
                vio_d3d11_buffer *vb = (vio_d3d11_buffer *)mesh->backend_vb;
                ID3D11Buffer *buffers[2] = { vb->buffer, s_instance_buf };
                UINT strides[2] = { (UINT)mesh->stride, 64 };  /* 64 = sizeof(mat4) */
                UINT offsets[2] = { 0, 0 };
                ID3D11DeviceContext_IASetVertexBuffers(vio_d3d11.context, 0, 2, buffers, strides, offsets);

                /* Flush cbuffers */
                if (ctx->bound_shader_object) {
                    vio_shader_object *sh = (vio_shader_object *)ctx->bound_shader_object;
                    if (sh->cbuffer_dirty && sh->cbuffer_backend && ctx->backend->update_buffer) {
                        ctx->backend->update_buffer(sh->cbuffer_backend, sh->cbuffer_data, sh->cbuffer_total_size);
                        sh->cbuffer_dirty = 0;
                    }
                    if (sh->frag_cbuffer_dirty && sh->frag_cbuffer_backend && ctx->backend->update_buffer) {
                        ctx->backend->update_buffer(sh->frag_cbuffer_backend, sh->frag_cbuffer_data, sh->frag_cbuffer_total_size);
                        sh->frag_cbuffer_dirty = 0;
                    }
                    if (sh->cbuffer_backend) {
                        vio_d3d11_buffer *cb = (vio_d3d11_buffer *)sh->cbuffer_backend;
                        ID3D11DeviceContext_VSSetConstantBuffers(vio_d3d11.context, 0, 1, &cb->buffer);
                        if (!sh->frag_cbuffer_backend)
                            ID3D11DeviceContext_PSSetConstantBuffers(vio_d3d11.context, 0, 1, &cb->buffer);
                    }
                    if (sh->frag_cbuffer_backend) {
                        vio_d3d11_buffer *fcb = (vio_d3d11_buffer *)sh->frag_cbuffer_backend;
                        ID3D11DeviceContext_PSSetConstantBuffers(vio_d3d11.context, 0, 1, &fcb->buffer);
                    }
                }

                /* Draw */
                if (mesh->index_count > 0 && mesh->backend_ib) {
                    vio_d3d11_buffer *ib = (vio_d3d11_buffer *)mesh->backend_ib;
                    ID3D11DeviceContext_IASetIndexBuffer(vio_d3d11.context, ib->buffer,
                                                         DXGI_FORMAT_R32_UINT, 0);
                    ID3D11DeviceContext_DrawIndexedInstanced(vio_d3d11.context,
                        mesh->index_count, (UINT)instance_count, 0, 0, 0);
                } else {
                    ID3D11DeviceContext_DrawInstanced(vio_d3d11.context,
                        mesh->vertex_count, (UINT)instance_count, 0, 0);
                }
            }
        } else
#endif
#ifdef HAVE_D3D12
        if (strcmp(ctx->backend->name, "d3d12") == 0 && vio_d3d12.initialized) {
            UINT byte_size = (UINT)mat_size;

            /* Per-draw instance slice from the persistently-mapped per-frame
             * instance ring (vio_d3d12.instance_heap*). Mirrors the cbuffer
             * heap exactly: each instanced draw in a frame gets its OWN stable
             * 256-byte-aligned slice inside THIS frame's region, so the slot-1
             * VBV GPU VA recorded now still points at THIS draw's matrices when
             * the command list executes at Present. The previous code shared one
             * function-static buffer across all instanced draws -> every draw
             * aliased the same VA -> only the last upload survived (district
             * buildings vanished/displaced) and a mid-frame overflow Released the
             * buffer under already-recorded draws -> GPU use-after-free ->
             * intermittent DEVICE_REMOVED. The ring NEVER frees mid-frame: an
             * overflow just skips this draw (the grow happens at next
             * begin_frame, after a full GPU sync). */
            D3D12_GPU_VIRTUAL_ADDRESS instance_gpu = 0;
            if (vio_d3d12.instance_heap_mapped && byte_size > 0) {
                UINT aligned = (byte_size + 255u) & ~255u;
                UINT offset = vio_d3d12.instance_heap_offset;
                /* Never spill past this frame's slice — the bytes beyond it
                 * belong to the other in-flight frame's instance data. */
                if (offset + aligned <= vio_d3d12.instance_frame_end) {
                    memcpy(vio_d3d12.instance_heap_mapped + offset, mat_data, byte_size);
                    instance_gpu = vio_d3d12.instance_heap_gpu + offset;
                    vio_d3d12.instance_heap_offset = offset + aligned;
                }
            }
            if (instance_gpu == 0) {
                /* Slice exhausted (or heap unavailable): skip rather than draw
                 * with another draw's matrices. Heap grows at next begin_frame. */
                if (allocated) efree(allocated);
                return;
            }

            /* Flush cbuffers */
            if (ctx->bound_shader_object) {
                vio_shader_object *sh = (vio_shader_object *)ctx->bound_shader_object;
                if (sh->cbuffer_dirty && sh->cbuffer_backend && ctx->backend->update_buffer) {
                    ctx->backend->update_buffer(sh->cbuffer_backend, sh->cbuffer_data, sh->cbuffer_total_size);
                    sh->cbuffer_dirty = 0;
                }
                if (sh->frag_cbuffer_dirty && sh->frag_cbuffer_backend && ctx->backend->update_buffer) {
                    ctx->backend->update_buffer(sh->frag_cbuffer_backend, sh->frag_cbuffer_data, sh->frag_cbuffer_total_size);
                    sh->frag_cbuffer_dirty = 0;
                }
                /* Allocate per-draw cbuffer slices from linear allocator.
                 * Bound against THIS frame's slice end (cbuffer_frame_end), not
                 * heap capacity — spilling past the slice would clobber the
                 * other in-flight frame's root CBVs (same aliasing class). */
                if (sh->cbuffer_total_size > 0 && vio_d3d12.cbuffer_heap_mapped) {
                    UINT aligned = (sh->cbuffer_total_size + 255) & ~255;
                    UINT offset = vio_d3d12.cbuffer_heap_offset;
                    if (offset + aligned <= vio_d3d12.cbuffer_frame_end) {
                        memcpy(vio_d3d12.cbuffer_heap_mapped + offset,
                               sh->cbuffer_data, sh->cbuffer_total_size);
                        ID3D12GraphicsCommandList_SetGraphicsRootConstantBufferView(
                            vio_d3d12.cmd_list, 0,
                            vio_d3d12.cbuffer_heap_gpu + offset);
                        vio_d3d12.cbuffer_heap_offset = offset + aligned;
                    }
                }
                if (sh->frag_cbuffer_total_size > 0 && vio_d3d12.cbuffer_heap_mapped) {
                    UINT aligned = (sh->frag_cbuffer_total_size + 255) & ~255;
                    UINT offset = vio_d3d12.cbuffer_heap_offset;
                    if (offset + aligned <= vio_d3d12.cbuffer_frame_end) {
                        memcpy(vio_d3d12.cbuffer_heap_mapped + offset,
                               sh->frag_cbuffer_data, sh->frag_cbuffer_total_size);
                        ID3D12GraphicsCommandList_SetGraphicsRootConstantBufferView(
                            vio_d3d12.cmd_list, 1,
                            vio_d3d12.cbuffer_heap_gpu + offset);
                        vio_d3d12.cbuffer_heap_offset = offset + aligned;
                    }
                }
            }

            /* Bind vertex buffers: slot 0 = mesh, slot 1 = instances.
             * instance_gpu is this draw's OWN slice in the per-frame ring,
             * stable until the command list executes at Present. */
            vio_d3d12_buffer *vb = (vio_d3d12_buffer *)mesh->backend_vb;
            D3D12_VERTEX_BUFFER_VIEW vbvs[2];
            vbvs[0].BufferLocation = vb->gpu_address;
            vbvs[0].SizeInBytes = (UINT)vb->size;
            vbvs[0].StrideInBytes = (UINT)mesh->stride;
            vbvs[1].BufferLocation = instance_gpu;
            vbvs[1].SizeInBytes = byte_size;
            vbvs[1].StrideInBytes = 64; /* sizeof(mat4) */
            ID3D12GraphicsCommandList_IASetVertexBuffers(vio_d3d12.cmd_list, 0, 2, vbvs);

            /* Draw */
            if (mesh->index_count > 0 && mesh->backend_ib) {
                vio_d3d12_buffer *ib = (vio_d3d12_buffer *)mesh->backend_ib;
                D3D12_INDEX_BUFFER_VIEW ibv = {0};
                ibv.BufferLocation = ib->gpu_address;
                ibv.SizeInBytes = (UINT)ib->size;
                ibv.Format = DXGI_FORMAT_R32_UINT;
                ID3D12GraphicsCommandList_IASetIndexBuffer(vio_d3d12.cmd_list, &ibv);
                vio_d3d12_flush_srv_table();
                ID3D12GraphicsCommandList_DrawIndexedInstanced(vio_d3d12.cmd_list,
                    mesh->index_count, (UINT)instance_count, 0, 0, 0);
            } else {
                vio_d3d12_flush_srv_table();
                ID3D12GraphicsCommandList_DrawInstanced(vio_d3d12.cmd_list,
                    mesh->vertex_count, (UINT)instance_count, 0, 0);
            }
        } else
#endif
        {
            /* Fallback for other backends */
            if (mesh->index_count > 0 && mesh->backend_ib && ctx->backend->draw_indexed) {
                vio_draw_indexed_cmd cmd = {0};
                cmd.vertex_buffer = mesh->backend_vb;
                cmd.index_buffer = mesh->backend_ib;
                cmd.index_count = mesh->index_count;
                cmd.instance_count = (int)instance_count;
                cmd.vertex_stride = mesh->stride;
                ctx->backend->draw_indexed(&cmd);
            } else if (ctx->backend->draw) {
                vio_draw_cmd cmd = {0};
                cmd.vertex_buffer = mesh->backend_vb;
                cmd.vertex_count = mesh->vertex_count;
                cmd.instance_count = (int)instance_count;
                cmd.vertex_stride = mesh->stride;
                ctx->backend->draw(&cmd);
            }
        }

    }

    if (allocated) efree(allocated);
}

ZEND_FUNCTION(vio_render_target)
{
    zval *ctx_zval;
    HashTable *config_ht;

    ZEND_PARSE_PARAMETERS_START(2, 2)
        Z_PARAM_OBJECT_OF_CLASS(ctx_zval, vio_context_ce)
        Z_PARAM_ARRAY_HT(config_ht)
    ZEND_PARSE_PARAMETERS_END();

    vio_context_object *ctx = Z_VIO_CONTEXT_P(ctx_zval);

    if (!ctx->initialized) {
        php_error_docref(NULL, E_WARNING, "Context is not initialized");
        RETURN_FALSE;
    }

    /* Parse config */
    zval *val;
    int width = 1024, height = 1024;
    int depth_only = 0;

    if ((val = zend_hash_str_find(config_ht, "width", sizeof("width") - 1)) != NULL) {
        width = (int)zval_get_long(val);
    }
    if ((val = zend_hash_str_find(config_ht, "height", sizeof("height") - 1)) != NULL) {
        height = (int)zval_get_long(val);
    }
    if ((val = zend_hash_str_find(config_ht, "depth_only", sizeof("depth_only") - 1)) != NULL) {
        depth_only = zend_is_true(val);
    }
    int hdr = 0;
    if ((val = zend_hash_str_find(config_ht, "hdr", sizeof("hdr") - 1)) != NULL) {
        hdr = zend_is_true(val);
    }

    /* Create VioRenderTarget object */
    zval rt_zval;
    object_init_ex(&rt_zval, vio_render_target_ce);
    vio_render_target_object *rt = Z_VIO_RENDER_TARGET_P(&rt_zval);

    rt->width      = width;
    rt->height     = height;
    rt->depth_only = depth_only;
    rt->backend    = ctx->backend;

    /* OpenGL + Metal go through the vtable; D3D11/D3D12 still inline below
     * until their backends implement create_render_target. */
    if (ctx->backend->create_render_target &&
        (strcmp(ctx->backend->name, "opengl") == 0 ||
         strcmp(ctx->backend->name, "metal") == 0)) {
        if (ctx->backend->create_render_target(rt, width, height, hdr, depth_only) != 0) {
            zval_ptr_dtor(&rt_zval);
            RETURN_FALSE;
        }
    }

#ifdef HAVE_D3D11
    if (strcmp(ctx->backend->name, "d3d11") == 0 && vio_d3d11.initialized) {
        HRESULT hr;

        /* Depth texture */
        D3D11_TEXTURE2D_DESC depth_desc = {0};
        depth_desc.Width = width;
        depth_desc.Height = height;
        depth_desc.MipLevels = 1;
        depth_desc.ArraySize = 1;
        depth_desc.Format = DXGI_FORMAT_R24G8_TYPELESS;
        depth_desc.SampleDesc.Count = 1;
        depth_desc.Usage = D3D11_USAGE_DEFAULT;
        depth_desc.BindFlags = D3D11_BIND_DEPTH_STENCIL | D3D11_BIND_SHADER_RESOURCE;

        ID3D11Texture2D *depth_tex = NULL;
        hr = ID3D11Device_CreateTexture2D(vio_d3d11.device, &depth_desc, NULL, &depth_tex);
        if (FAILED(hr)) {
            php_error_docref(NULL, E_WARNING, "D3D11: Failed to create depth texture (0x%08lx)", hr);
            zval_ptr_dtor(&rt_zval);
            RETURN_FALSE;
        }

        /* DSV */
        D3D11_DEPTH_STENCIL_VIEW_DESC dsv_desc = {0};
        dsv_desc.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
        dsv_desc.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;

        ID3D11DepthStencilView *dsv = NULL;
        hr = ID3D11Device_CreateDepthStencilView(vio_d3d11.device, (ID3D11Resource *)depth_tex,
                                                  &dsv_desc, &dsv);
        if (FAILED(hr)) {
            ID3D11Texture2D_Release(depth_tex);
            php_error_docref(NULL, E_WARNING, "D3D11: Failed to create DSV (0x%08lx)", hr);
            zval_ptr_dtor(&rt_zval);
            RETURN_FALSE;
        }

        /* SRV for depth (for shadow map sampling) */
        D3D11_SHADER_RESOURCE_VIEW_DESC srv_desc = {0};
        srv_desc.Format = DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
        srv_desc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        srv_desc.Texture2D.MipLevels = 1;

        ID3D11ShaderResourceView *depth_srv = NULL;
        ID3D11Device_CreateShaderResourceView(vio_d3d11.device, (ID3D11Resource *)depth_tex,
                                               &srv_desc, &depth_srv);

        rt->d3d11_dsv = dsv;
        rt->d3d11_depth_tex = depth_tex;
        rt->d3d11_depth_srv = depth_srv;

        if (!depth_only) {
            /* Color texture */
            D3D11_TEXTURE2D_DESC color_desc = {0};
            color_desc.Width = width;
            color_desc.Height = height;
            color_desc.MipLevels = 1;
            color_desc.ArraySize = 1;
            color_desc.Format = hdr ? DXGI_FORMAT_R16G16B16A16_FLOAT : DXGI_FORMAT_R8G8B8A8_UNORM;
            color_desc.SampleDesc.Count = 1;
            color_desc.Usage = D3D11_USAGE_DEFAULT;
            color_desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;

            ID3D11Texture2D *color_tex = NULL;
            hr = ID3D11Device_CreateTexture2D(vio_d3d11.device, &color_desc, NULL, &color_tex);
            if (FAILED(hr)) {
                ID3D11DepthStencilView_Release(dsv);
                ID3D11Texture2D_Release(depth_tex);
                if (depth_srv) ID3D11ShaderResourceView_Release(depth_srv);
                php_error_docref(NULL, E_WARNING, "D3D11: Failed to create color texture (0x%08lx)", hr);
                zval_ptr_dtor(&rt_zval);
                RETURN_FALSE;
            }

            ID3D11RenderTargetView *rtv = NULL;
            hr = ID3D11Device_CreateRenderTargetView(vio_d3d11.device, (ID3D11Resource *)color_tex,
                                                      NULL, &rtv);
            if (FAILED(hr)) {
                ID3D11Texture2D_Release(color_tex);
                ID3D11DepthStencilView_Release(dsv);
                ID3D11Texture2D_Release(depth_tex);
                if (depth_srv) ID3D11ShaderResourceView_Release(depth_srv);
                php_error_docref(NULL, E_WARNING, "D3D11: Failed to create RTV (0x%08lx)", hr);
                zval_ptr_dtor(&rt_zval);
                RETURN_FALSE;
            }

            rt->d3d11_rtv = rtv;
            rt->d3d11_color_tex = color_tex;

            /* Create SRV for color texture (for sampling in post-process passes) */
            {
                ID3D11ShaderResourceView *color_srv = NULL;
                D3D11_SHADER_RESOURCE_VIEW_DESC color_srv_desc = {0};
                color_srv_desc.Format = hdr ? DXGI_FORMAT_R16G16B16A16_FLOAT : DXGI_FORMAT_R8G8B8A8_UNORM;
                color_srv_desc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
                color_srv_desc.Texture2D.MipLevels = 1;
                ID3D11Device_CreateShaderResourceView(vio_d3d11.device, (ID3D11Resource *)color_tex, &color_srv_desc, &color_srv);
                rt->d3d11_color_srv = color_srv;
            }
        }

        rt->backend_type = VIO_RT_BACKEND_D3D11;
    }
#endif

#ifdef HAVE_D3D12
    if (strcmp(ctx->backend->name, "d3d12") == 0 && vio_d3d12.initialized) {
        HRESULT hr;

        /* Create dedicated RTV descriptor heap (1 descriptor) */
        if (!depth_only) {
            D3D12_DESCRIPTOR_HEAP_DESC rtv_heap_desc = {0};
            rtv_heap_desc.NumDescriptors = 1;
            rtv_heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;

            ID3D12DescriptorHeap *rtv_heap = NULL;
            hr = ID3D12Device_CreateDescriptorHeap(vio_d3d12.device, &rtv_heap_desc,
                                                    &IID_ID3D12DescriptorHeap, (void **)&rtv_heap);
            if (FAILED(hr)) {
                php_error_docref(NULL, E_WARNING, "D3D12: Failed to create RTV heap (0x%08lx)", hr);
                zval_ptr_dtor(&rt_zval);
                RETURN_FALSE;
            }
            rt->d3d12_rtv_heap = rtv_heap;

            /* Color resource */
            D3D12_HEAP_PROPERTIES heap_props = {0};
            heap_props.Type = D3D12_HEAP_TYPE_DEFAULT;

            D3D12_RESOURCE_DESC res_desc = {0};
            res_desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
            res_desc.Width = width;
            res_desc.Height = height;
            res_desc.DepthOrArraySize = 1;
            res_desc.MipLevels = 1;
            res_desc.Format = hdr ? DXGI_FORMAT_R16G16B16A16_FLOAT : DXGI_FORMAT_R8G8B8A8_UNORM;
            res_desc.SampleDesc.Count = 1;
            res_desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;

            D3D12_CLEAR_VALUE clear_val = {0};
            clear_val.Format = hdr ? DXGI_FORMAT_R16G16B16A16_FLOAT : DXGI_FORMAT_R8G8B8A8_UNORM;

            ID3D12Resource *color_res = NULL;
            hr = ID3D12Device_CreateCommittedResource(vio_d3d12.device, &heap_props,
                D3D12_HEAP_FLAG_NONE, &res_desc, D3D12_RESOURCE_STATE_RENDER_TARGET,
                &clear_val, &IID_ID3D12Resource, (void **)&color_res);
            if (FAILED(hr)) {
                ID3D12DescriptorHeap_Release(rtv_heap);
                php_error_docref(NULL, E_WARNING, "D3D12: Failed to create color resource (0x%08lx)", hr);
                zval_ptr_dtor(&rt_zval);
                RETURN_FALSE;
            }
            rt->d3d12_color_resource = color_res;

            /* Create RTV */
            D3D12_CPU_DESCRIPTOR_HANDLE rtv_handle;
            ID3D12DescriptorHeap_GetCPUDescriptorHandleForHeapStart(rtv_heap, &rtv_handle);
            ID3D12Device_CreateRenderTargetView(vio_d3d12.device, color_res, NULL, rtv_handle);
        }

        /* DSV descriptor heap */
        D3D12_DESCRIPTOR_HEAP_DESC dsv_heap_desc = {0};
        dsv_heap_desc.NumDescriptors = 1;
        dsv_heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;

        ID3D12DescriptorHeap *dsv_heap = NULL;
        hr = ID3D12Device_CreateDescriptorHeap(vio_d3d12.device, &dsv_heap_desc,
                                                &IID_ID3D12DescriptorHeap, (void **)&dsv_heap);
        if (FAILED(hr)) {
            if (rt->d3d12_color_resource) {
                ID3D12Resource_Release((ID3D12Resource *)rt->d3d12_color_resource);
                rt->d3d12_color_resource = NULL;
            }
            if (rt->d3d12_rtv_heap) {
                ID3D12DescriptorHeap_Release((ID3D12DescriptorHeap *)rt->d3d12_rtv_heap);
                rt->d3d12_rtv_heap = NULL;
            }
            php_error_docref(NULL, E_WARNING, "D3D12: Failed to create DSV heap (0x%08lx)", hr);
            zval_ptr_dtor(&rt_zval);
            RETURN_FALSE;
        }
        rt->d3d12_dsv_heap = dsv_heap;

        /* Depth resource */
        D3D12_HEAP_PROPERTIES depth_heap_props = {0};
        depth_heap_props.Type = D3D12_HEAP_TYPE_DEFAULT;

        D3D12_RESOURCE_DESC depth_res_desc = {0};
        depth_res_desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        depth_res_desc.Width = width;
        depth_res_desc.Height = height;
        depth_res_desc.DepthOrArraySize = 1;
        depth_res_desc.MipLevels = 1;
        depth_res_desc.Format = depth_only ? DXGI_FORMAT_R24G8_TYPELESS : DXGI_FORMAT_D24_UNORM_S8_UINT;
        depth_res_desc.SampleDesc.Count = 1;
        depth_res_desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;

        D3D12_CLEAR_VALUE depth_clear = {0};
        depth_clear.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
        depth_clear.DepthStencil.Depth = 1.0f;

        ID3D12Resource *depth_res = NULL;
        hr = ID3D12Device_CreateCommittedResource(vio_d3d12.device, &depth_heap_props,
            D3D12_HEAP_FLAG_NONE, &depth_res_desc, D3D12_RESOURCE_STATE_DEPTH_WRITE,
            &depth_clear, &IID_ID3D12Resource, (void **)&depth_res);
        if (FAILED(hr)) {
            ID3D12DescriptorHeap_Release(dsv_heap);
            if (rt->d3d12_color_resource) {
                ID3D12Resource_Release((ID3D12Resource *)rt->d3d12_color_resource);
                rt->d3d12_color_resource = NULL;
            }
            if (rt->d3d12_rtv_heap) {
                ID3D12DescriptorHeap_Release((ID3D12DescriptorHeap *)rt->d3d12_rtv_heap);
                rt->d3d12_rtv_heap = NULL;
            }
            php_error_docref(NULL, E_WARNING, "D3D12: Failed to create depth resource (0x%08lx)", hr);
            zval_ptr_dtor(&rt_zval);
            RETURN_FALSE;
        }
        rt->d3d12_depth_resource = depth_res;

        /* Create DSV (explicit format for typeless resources) */
        D3D12_DEPTH_STENCIL_VIEW_DESC dsv_view_desc = {0};
        dsv_view_desc.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
        dsv_view_desc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
        D3D12_CPU_DESCRIPTOR_HANDLE dsv_handle;
        ID3D12DescriptorHeap_GetCPUDescriptorHandleForHeapStart(dsv_heap, &dsv_handle);
        ID3D12Device_CreateDepthStencilView(vio_d3d12.device, depth_res,
            depth_only ? &dsv_view_desc : NULL, dsv_handle);

        /* For depth-only targets: pre-create SRV for shadow map sampling.
         * SRV must live in the staging (non-shader-visible) heap so it can
         * serve as the source operand of CopyDescriptorsSimple inside
         * vio_d3d12_flush_srv_table. The GPU handle still indexes into the
         * matching slot of the shader-visible heap. */
        if (depth_only && vio_d3d12.srv_heap.count < vio_d3d12.srv_heap.capacity) {
            UINT srv_idx = vio_d3d12.srv_heap.capacity - 1 - vio_d3d12.srv_heap.count;
            vio_d3d12.srv_heap.count++;
            D3D12_CPU_DESCRIPTOR_HANDLE staging_cpu;
            D3D12_GPU_DESCRIPTOR_HANDLE srv_gpu;
            ID3D12DescriptorHeap_GetCPUDescriptorHandleForHeapStart(vio_d3d12.srv_staging_heap, &staging_cpu);
            ID3D12DescriptorHeap_GetGPUDescriptorHandleForHeapStart(vio_d3d12.srv_heap.heap, &srv_gpu);
            staging_cpu.ptr += srv_idx * vio_d3d12.srv_heap.descriptor_size;
            srv_gpu.ptr     += srv_idx * vio_d3d12.srv_heap.descriptor_size;

            D3D12_SHADER_RESOURCE_VIEW_DESC srv_desc = {0};
            srv_desc.Format = DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
            srv_desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            srv_desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            srv_desc.Texture2D.MipLevels = 1;
            ID3D12Device_CreateShaderResourceView(vio_d3d12.device, depth_res, &srv_desc, staging_cpu);

            rt->d3d12_depth_srv_gpu = srv_gpu.ptr;
            rt->d3d12_depth_srv_cpu = staging_cpu.ptr;
        }

        /* For color targets: pre-create SRV for color texture sampling.
         * Same staging-heap pattern as the depth-only branch above. */
        if (!depth_only && rt->d3d12_color_resource && vio_d3d12.srv_heap.count < vio_d3d12.srv_heap.capacity) {
            UINT color_srv_idx = vio_d3d12.srv_heap.capacity - 1 - vio_d3d12.srv_heap.count;
            vio_d3d12.srv_heap.count++;
            D3D12_CPU_DESCRIPTOR_HANDLE color_staging_cpu;
            D3D12_GPU_DESCRIPTOR_HANDLE color_srv_gpu;
            ID3D12DescriptorHeap_GetCPUDescriptorHandleForHeapStart(vio_d3d12.srv_staging_heap, &color_staging_cpu);
            ID3D12DescriptorHeap_GetGPUDescriptorHandleForHeapStart(vio_d3d12.srv_heap.heap, &color_srv_gpu);
            color_staging_cpu.ptr += color_srv_idx * vio_d3d12.srv_heap.descriptor_size;
            color_srv_gpu.ptr     += color_srv_idx * vio_d3d12.srv_heap.descriptor_size;

            D3D12_SHADER_RESOURCE_VIEW_DESC color_srv_desc = {0};
            color_srv_desc.Format = hdr ? DXGI_FORMAT_R16G16B16A16_FLOAT : DXGI_FORMAT_R8G8B8A8_UNORM;
            color_srv_desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            color_srv_desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            color_srv_desc.Texture2D.MipLevels = 1;
            ID3D12Device_CreateShaderResourceView(vio_d3d12.device, (ID3D12Resource *)rt->d3d12_color_resource, &color_srv_desc, color_staging_cpu);

            rt->d3d12_color_srv_gpu = color_srv_gpu.ptr;
            rt->d3d12_color_srv_cpu = color_staging_cpu.ptr;
        }

        rt->backend_type = VIO_RT_BACKEND_D3D12;
    }
#endif

#ifdef HAVE_VULKAN
    if (strcmp(ctx->backend->name, "vulkan") == 0 && vio_vk.initialized) {
        /* Parallel to the d3d12 inline block: create the per-target Vulkan
         * resources (color image+view, optional depth, a render-pass-compatible
         * VkRenderPass, framebuffer, sampler) and tag the backend type. */
        if (vulkan_create_render_target(rt, width, height, hdr, depth_only) != 0) {
            zval_ptr_dtor(&rt_zval);
            RETURN_FALSE;
        }
        rt->backend_type = VIO_RT_BACKEND_VULKAN;
    }
#endif

    rt->valid = 1;
    RETURN_COPY_VALUE(&rt_zval);
}

#ifdef HAVE_D3D12
/* Record the D3D12 commands that make `rt` the active render target.
 * Caller MUST ensure the command list is open (vio_d3d12.in_frame == 1) —
 * every call here lands on vio_d3d12.cmd_list, which only accepts commands
 * between d3d12_begin_frame()'s Reset() and d3d12_end_frame()'s Close().
 * Out-of-frame binds are deferred via vio_d3d12.pending_bound_rt and applied
 * from vio_begin(). */
static void d3d12_record_bind_render_target(vio_render_target_object *rt)
{
    /* Barrier: transition the OUTGOING render target's depth back to a samplable
     * state before we bind the new one. Without this, only the final
     * vio_unbind_render_target transitions one target (the last-bound) to
     * PIXEL_SHADER_RESOURCE — so when the engine renders multiple depth-only
     * targets in sequence (e.g. the CSM cascade loop binds cascade 0, 1, 2 in
     * turn with a single unbind afterwards), cascades 0 and 1 are left in
     * DEPTH_WRITE and read back as NULL/garbage when the mesh pass samples them.
     * Moving the DEPTH_WRITE->PIXEL_SHADER_RESOURCE transition here makes every
     * previously-bound depth target samplable as soon as a new one is bound,
     * regardless of how many targets the engine cycles through per unbind. */
    if (vio_d3d12.current_bound_rt && vio_d3d12.current_bound_rt != rt) {
        vio_render_target_object *prev = (vio_render_target_object *)vio_d3d12.current_bound_rt;
        if (prev->d3d12_depth_resource && prev->depth_only && !prev->d3d12_depth_is_srv) {
            D3D12_RESOURCE_BARRIER barrier = {0};
            barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            barrier.Transition.pResource = (ID3D12Resource *)prev->d3d12_depth_resource;
            barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_DEPTH_WRITE;
            barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
            barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            ID3D12GraphicsCommandList_ResourceBarrier(vio_d3d12.cmd_list, 1, &barrier);
            prev->d3d12_depth_is_srv = 1;
        }
    }

    /* Barrier: if color resource was used as SRV, transition back to RENDER_TARGET */
    if (rt->d3d12_color_resource && rt->d3d12_color_is_srv) {
        D3D12_RESOURCE_BARRIER barrier = {0};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource = (ID3D12Resource *)rt->d3d12_color_resource;
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        ID3D12GraphicsCommandList_ResourceBarrier(vio_d3d12.cmd_list, 1, &barrier);
        rt->d3d12_color_is_srv = 0;
    }

    /* Barrier: if depth resource was used as SRV, transition back to DEPTH_WRITE */
    if (rt->d3d12_depth_resource && rt->d3d12_depth_is_srv) {
        D3D12_RESOURCE_BARRIER barrier = {0};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource = (ID3D12Resource *)rt->d3d12_depth_resource;
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_DEPTH_WRITE;
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        ID3D12GraphicsCommandList_ResourceBarrier(vio_d3d12.cmd_list, 1, &barrier);
        rt->d3d12_depth_is_srv = 0;
    }

    D3D12_CPU_DESCRIPTOR_HANDLE dsv_handle;
    ID3D12DescriptorHeap_GetCPUDescriptorHandleForHeapStart(
        (ID3D12DescriptorHeap *)rt->d3d12_dsv_heap, &dsv_handle);

    if (rt->depth_only) {
        ID3D12GraphicsCommandList_OMSetRenderTargets(vio_d3d12.cmd_list, 0, NULL, FALSE, &dsv_handle);
        vio_d3d12.current_has_rtv = 0;
    } else {
        D3D12_CPU_DESCRIPTOR_HANDLE rtv_handle;
        ID3D12DescriptorHeap_GetCPUDescriptorHandleForHeapStart(
            (ID3D12DescriptorHeap *)rt->d3d12_rtv_heap, &rtv_handle);
        ID3D12GraphicsCommandList_OMSetRenderTargets(vio_d3d12.cmd_list, 1, &rtv_handle, FALSE, &dsv_handle);
        vio_d3d12.current_rtv = rtv_handle;
        vio_d3d12.current_has_rtv = 1;
    }
    vio_d3d12.current_dsv = dsv_handle;
    vio_d3d12.current_rt_width = rt->width;
    vio_d3d12.current_rt_height = rt->height;

    D3D12_VIEWPORT vp = {0};
    vp.Width = (float)rt->width;
    vp.Height = (float)rt->height;
    vp.MinDepth = 0.0f;
    vp.MaxDepth = 1.0f;
    ID3D12GraphicsCommandList_RSSetViewports(vio_d3d12.cmd_list, 1, &vp);

    D3D12_RECT scissor = {0, 0, rt->width, rt->height};
    ID3D12GraphicsCommandList_RSSetScissorRects(vio_d3d12.cmd_list, 1, &scissor);

    vio_d3d12.current_bound_rt = rt;
}
#endif

#ifdef HAVE_D3D11
/* Make `rt` the active D3D11 render target immediately on the (single,
 * immediate) device context: unbind SRVs to clear any read-as-SRV hazard,
 * OMSetRenderTargets to the offscreen RTV/DSV (or DSV-only for depth_only),
 * track current_rtv/current_dsv/current_rt_width/height, and set the viewport
 * to the RT extent. Caller MUST have verified rt->valid and
 * rt->backend_type == VIO_RT_BACKEND_D3D11.
 *
 * Used in two places: (a) the in-frame / render-to-texture path, called
 * directly from vio_bind_render_target while ctx->in_frame; and (b) the
 * out-of-frame warm-render path, deferred via vio_d3d11.pending_bound_rt and
 * re-applied from vio_begin() AFTER begin_frame() (which would otherwise reset
 * current_rtv = rtv and clobber a pre-begin bind). */
static void d3d11_apply_render_target_bind(vio_render_target_object *rt)
{
    ID3D11RenderTargetView *rtv = (ID3D11RenderTargetView *)rt->d3d11_rtv;
    ID3D11DepthStencilView *dsv = (ID3D11DepthStencilView *)rt->d3d11_dsv;

    /* Unbind all SRVs to avoid D3D11 resource hazard — a resource cannot be
     * bound as SRV and RTV/DSV simultaneously. This is critical for post-process
     * passes that read from one render target while writing to another. */
    {
        ID3D11ShaderResourceView *null_srvs[8] = {NULL};
        ID3D11DeviceContext_PSSetShaderResources(vio_d3d11.context, 0, 8, null_srvs);
    }

    if (rt->depth_only) {
        ID3D11DeviceContext_OMSetRenderTargets(vio_d3d11.context, 0, NULL, dsv);
        vio_d3d11.current_rtv = NULL;
    } else {
        ID3D11DeviceContext_OMSetRenderTargets(vio_d3d11.context, 1, &rtv, dsv);
        vio_d3d11.current_rtv = rtv;
    }
    vio_d3d11.current_dsv = dsv;
    vio_d3d11.current_rt_width = rt->width;
    vio_d3d11.current_rt_height = rt->height;

    D3D11_VIEWPORT vp = {0};
    vp.Width = (float)rt->width;
    vp.Height = (float)rt->height;
    vp.MinDepth = 0.0f;
    vp.MaxDepth = 1.0f;
    ID3D11DeviceContext_RSSetViewports(vio_d3d11.context, 1, &vp);
}
#endif

ZEND_FUNCTION(vio_bind_render_target)
{
    zval *ctx_zval;
    zval *rt_zval;

    ZEND_PARSE_PARAMETERS_START(2, 2)
        Z_PARAM_OBJECT_OF_CLASS(ctx_zval, vio_context_ce)
        Z_PARAM_OBJECT_OF_CLASS(rt_zval, vio_render_target_ce)
    ZEND_PARSE_PARAMETERS_END();

    vio_context_object *ctx = Z_VIO_CONTEXT_P(ctx_zval);

    if (!ctx->initialized) {
        php_error_docref(NULL, E_WARNING, "Context is not initialized");
        return;
    }

    vio_render_target_object *rt = Z_VIO_RENDER_TARGET_P(rt_zval);
    if (!rt->valid) {
        php_error_docref(NULL, E_WARNING, "Render target is not valid");
        return;
    }

    if (ctx->backend->bind_render_target &&
        (rt->backend_type == VIO_RT_BACKEND_OPENGL ||
         rt->backend_type == VIO_RT_BACKEND_METAL)) {
        ctx->backend->bind_render_target(rt);
    }

#ifdef HAVE_D3D11
    if (rt->backend_type == VIO_RT_BACKEND_D3D11 && vio_d3d11.initialized) {
        if (ctx->in_frame) {
            /* In-frame / render-to-texture path: apply the bind on the
             * immediate context now, exactly as before. The current frame's
             * draws will land on the offscreen target. */
            d3d11_apply_render_target_bind(rt);
        } else {
            /* Called before vio_begin() (the warm-render "bind then begin"
             * order): applying now is pointless because d3d11_begin_frame()
             * unconditionally resets current_rtv = rtv and re-binds the
             * backbuffer at the start of the frame, clobbering this bind. Defer
             * it — vio_begin() re-applies the pending bind AFTER begin_frame(),
             * so the offscreen redirect survives. Storing the rt (not applying)
             * is the strict no-op the normal path relies on; only a non-NULL
             * pending_bound_rt makes vio_begin act. */
            vio_d3d11.pending_bound_rt = rt;
        }
    }
#endif

#ifdef HAVE_D3D12
    if (rt->backend_type == VIO_RT_BACKEND_D3D12 && vio_d3d12.initialized) {
        if (vio_d3d12.in_frame) {
            d3d12_record_bind_render_target(rt);
        } else {
            /* Called before vio_begin(): the command list is closed, so the
             * bind cannot be recorded yet (the D3D12 debug layer would report
             * "This API cannot be called on a closed command list" at the next
             * begin_frame InfoQueue drain). Defer it — vio_begin() applies the
             * pending bind once the frame's list is open. This is what makes
             * the warm-render "bind then begin" order work on D3D12. */
            vio_d3d12.pending_bound_rt = rt;
        }
    }
#endif

#ifdef HAVE_VULKAN
    if (rt->backend_type == VIO_RT_BACKEND_VULKAN && vio_vk.initialized) {
        if (vio_vk.in_frame) {
            /* A swapchain pass is open on this frame's command buffer: switch to
             * the offscreen pass now (end swapchain pass -> begin offscreen pass
             * -> RT-extent viewport/scissor -> current_bound_rt = rt). */
            vulkan_record_bind_render_target(rt);
        } else {
            /* Called before vio_begin() (the warm-render "bind then begin"
             * order): no command buffer is recording and no pass is open, so we
             * cannot record the switch yet. Stash it; vio_begin() applies the
             * pending bind once begin_frame() has opened the swapchain pass.
             * Recording nothing here is the strict no-op the normal path relies
             * on — only a non-NULL pending_bound_rt makes vio_begin act. */
            vio_vk.pending_bound_rt = rt;
        }
    }
#endif
}

ZEND_FUNCTION(vio_unbind_render_target)
{
    zval *ctx_zval;

    ZEND_PARSE_PARAMETERS_START(1, 1)
        Z_PARAM_OBJECT_OF_CLASS(ctx_zval, vio_context_ce)
    ZEND_PARSE_PARAMETERS_END();

    vio_context_object *ctx = Z_VIO_CONTEXT_P(ctx_zval);

    if (!ctx->initialized) {
        php_error_docref(NULL, E_WARNING, "Context is not initialized");
        return;
    }

    if (ctx->backend->unbind_render_target &&
        strcmp(ctx->backend->name, "opengl") == 0) {
        /* For OpenGL the "default" target is the headless FBO when present;
         * otherwise FBO 0 with the current window's framebuffer dimensions. */
        unsigned int default_fbo = ctx->headless_fbo;
        int w = ctx->config.width;
        int h = ctx->config.height;
        if (!default_fbo) {
#ifdef HAVE_GLFW
            if (ctx->window) {
                glfwGetFramebufferSize(ctx->window, &w, &h);
            }
#endif
        }
        ctx->backend->unbind_render_target(default_fbo, w, h);
    }

    if (ctx->backend->unbind_render_target &&
        strcmp(ctx->backend->name, "metal") == 0) {
        /* Metal manages its swapchain via CAMetalLayer internally; the
         * default_fbo/width/height parameters are ignored on this backend. */
        int w = ctx->config.width;
        int h = ctx->config.height;
#ifdef HAVE_GLFW
        if (ctx->window) {
            glfwGetFramebufferSize(ctx->window, &w, &h);
        }
#endif
        ctx->backend->unbind_render_target(0, w, h);
    }

#ifdef HAVE_D3D11
    if (strcmp(ctx->backend->name, "d3d11") == 0 && vio_d3d11.initialized) {
        /* Drop any deferred (pre-begin) bind. In the warm-render path the caller
         * does vio_bind_render_target -> vio_begin -> ... -> vio_end ->
         * vio_unbind_render_target. By the time we get here vio_begin() has
         * already consumed the pending bind (cleared it to NULL), so this is
         * normally a no-op. But if unbind is called WITHOUT an intervening
         * vio_begin (a bind-then-unbind with no frame), clearing it here ensures
         * a stale pending RT can't leak into a later, unrelated frame. */
        vio_d3d11.pending_bound_rt = NULL;

        /* Restore main backbuffer RTV + DSV */
        vio_d3d11.current_rtv = vio_d3d11.rtv;
        vio_d3d11.current_dsv = vio_d3d11.dsv;
        vio_d3d11.current_rt_width = vio_d3d11.width;
        vio_d3d11.current_rt_height = vio_d3d11.height;

        ID3D11DeviceContext_OMSetRenderTargets(vio_d3d11.context, 1,
                                               &vio_d3d11.rtv, vio_d3d11.dsv);

        D3D11_VIEWPORT vp = {0};
        vp.Width = (float)vio_d3d11.width;
        vp.Height = (float)vio_d3d11.height;
        vp.MinDepth = 0.0f;
        vp.MaxDepth = 1.0f;
        ID3D11DeviceContext_RSSetViewports(vio_d3d11.context, 1, &vp);
    }
#endif

#ifdef HAVE_D3D12
    if (strcmp(ctx->backend->name, "d3d12") == 0 && vio_d3d12.initialized) {
        if (!vio_d3d12.in_frame) {
            /* Command list is closed (vio_unbind_render_target called after
             * vio_end — e.g. VioRenderer2D::endFrame / endOffscreenFrame in the
             * warm-render path). Recording the restore-swapchain barrier +
             * OMSetRenderTargets here would hit the closed list and surface as
             * "This API cannot be called on a closed command list" at the next
             * begin_frame drain. It's also unnecessary: the next
             * d3d12_begin_frame() unconditionally rebinds the swapchain target.
             * Just drop tracked + pending binding so stale state can't leak.
             *
             * NOTE: this skips the RENDER_TARGET->PIXEL_SHADER_RESOURCE barrier
             * the in-frame path records, so the offscreen color stays in
             * RENDER_TARGET state. That's correct for the only out-of-frame
             * unbind caller (Engine::warmRender, which discards its offscreen
             * target without ever sampling it). A render-to-texture caller that
             * wants to SAMPLE the result must unbind while a frame is open so the
             * SRV transition is recorded. */
            vio_d3d12.pending_bound_rt = NULL;
            vio_d3d12.current_bound_rt = NULL;
            return;
        }

        /* Barrier: transition the offscreen depth resource to SRV for shadow sampling.
         * We find the currently bound RT's depth resource from the DSV heap.
         * Since we track the RT object via current_dsv, and the depth_resource is stored
         * on the render target object, we use a flag approach:
         * If the render target was depth-only, its depth resource needs the barrier. */
        /* Barrier: transition shadow map depth from DEPTH_WRITE → SRV for sampling */
        if (vio_d3d12.current_bound_rt) {
            vio_render_target_object *bound_rt = (vio_render_target_object *)vio_d3d12.current_bound_rt;
            /* Transition depth to SRV if depth-only target (skip if a subsequent
             * bind already moved it to PIXEL_SHADER_RESOURCE — see the outgoing-RT
             * transition in d3d12_record_bind_render_target). */
            if (bound_rt->d3d12_depth_resource && bound_rt->depth_only && !bound_rt->d3d12_depth_is_srv) {
                D3D12_RESOURCE_BARRIER barrier = {0};
                barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                barrier.Transition.pResource = (ID3D12Resource *)bound_rt->d3d12_depth_resource;
                barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_DEPTH_WRITE;
                barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
                barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
                ID3D12GraphicsCommandList_ResourceBarrier(vio_d3d12.cmd_list, 1, &barrier);
                bound_rt->d3d12_depth_is_srv = 1;
            }
            /* Transition color to SRV if color target */
            if (bound_rt->d3d12_color_resource && !bound_rt->depth_only) {
                D3D12_RESOURCE_BARRIER barrier = {0};
                barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                barrier.Transition.pResource = (ID3D12Resource *)bound_rt->d3d12_color_resource;
                barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
                barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
                barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
                ID3D12GraphicsCommandList_ResourceBarrier(vio_d3d12.cmd_list, 1, &barrier);
                bound_rt->d3d12_color_is_srv = 1;
            }
            vio_d3d12.current_bound_rt = NULL;
        }

        /* Restore main swapchain render target */
        vio_d3d12_frame *frame = &vio_d3d12.frames[vio_d3d12.frame_index];

        D3D12_CPU_DESCRIPTOR_HANDLE dsv_handle;
        ID3D12DescriptorHeap_GetCPUDescriptorHandleForHeapStart(vio_d3d12.dsv_heap, &dsv_handle);
        ID3D12GraphicsCommandList_OMSetRenderTargets(vio_d3d12.cmd_list, 1,
                                                      &frame->rtv_handle, FALSE, &dsv_handle);

        vio_d3d12.current_rtv = frame->rtv_handle;
        vio_d3d12.current_dsv = dsv_handle;
        vio_d3d12.current_rt_width = vio_d3d12.width;
        vio_d3d12.current_rt_height = vio_d3d12.height;
        vio_d3d12.current_has_rtv = 1;

        D3D12_VIEWPORT vp = {0};
        vp.Width = (float)vio_d3d12.width;
        vp.Height = (float)vio_d3d12.height;
        vp.MinDepth = 0.0f;
        vp.MaxDepth = 1.0f;
        ID3D12GraphicsCommandList_RSSetViewports(vio_d3d12.cmd_list, 1, &vp);

        D3D12_RECT scissor = {0, 0, vio_d3d12.width, vio_d3d12.height};
        ID3D12GraphicsCommandList_RSSetScissorRects(vio_d3d12.cmd_list, 1, &scissor);
    }
#endif

#ifdef HAVE_VULKAN
    if (strcmp(ctx->backend->name, "vulkan") == 0 && vio_vk.initialized) {
        if (vio_vk.in_frame && vio_vk.current_bound_rt) {
            /* In-frame unbind: end the offscreen pass (its finalLayout leaves the
             * color image SHADER_READ_ONLY, so it's immediately samplable) and
             * re-open the swapchain pass with loadOp=LOAD so prior swapchain
             * draws survive and later draws composite on top. */
            vulkan_record_unbind_render_target();
        } else {
            /* Out-of-frame unbind (warm-render "unbind after end"): the command
             * buffer is already submitted, so there is nothing to record. Just
             * drop the tracked / pending binding. Mirrors the d3d12 closed-list
             * branch. */
            vio_vk.pending_bound_rt = NULL;
            vio_vk.current_bound_rt = NULL;
        }
    }
#endif
}

ZEND_FUNCTION(vio_render_target_texture)
{
    zval *rt_zval;

    ZEND_PARSE_PARAMETERS_START(1, 1)
        Z_PARAM_OBJECT_OF_CLASS(rt_zval, vio_render_target_ce)
    ZEND_PARSE_PARAMETERS_END();

    vio_render_target_object *rt = Z_VIO_RENDER_TARGET_P(rt_zval);

    if (!rt->valid) {
        php_error_docref(NULL, E_WARNING, "Render target is not valid");
        RETURN_FALSE;
    }

    /* Create a VioTexture that references the render target's depth or color texture */
    zval tex_zval;
    object_init_ex(&tex_zval, vio_texture_ce);
    vio_texture_object *tex = Z_VIO_TEXTURE_P(&tex_zval);
    tex->backend = rt->backend;

    tex->width    = rt->width;
    tex->height   = rt->height;
    tex->channels = rt->depth_only ? 1 : 4;
    tex->filter   = VIO_FILTER_NEAREST;
    tex->wrap     = VIO_WRAP_CLAMP;

    /* Return depth texture for depth-only targets, color texture otherwise */
    tex->texture_id = rt->depth_only ? rt->depth_texture : rt->color_texture;
    tex->valid    = 1;
    tex->borrowed = 1;  /* GL resource owned by render target, don't double-delete */

#ifdef HAVE_D3D11
    /* For D3D11: hand out a cached backend-texture wrapper owned by the
     * render target. Building a fresh wrapper on every call was the
     * original implementation but it leaked one vio_d3d11_texture, one
     * sampler state and one SRV AddRef per frame whenever the offscreen
     * blit path queried the texture — within a few frames the renderer
     * starved D3D11's sampler-state pool (cap: 4096) and the third
     * vio_render_target_texture call started returning garbage SRV
     * handles, which crashed the next sampler bind. */
    if (rt->backend_type == VIO_RT_BACKEND_D3D11 && vio_d3d11.initialized) {
        vio_d3d11_texture **cache_slot = rt->depth_only
            ? (vio_d3d11_texture **)&rt->d3d11_depth_backend_texture
            : (vio_d3d11_texture **)&rt->d3d11_color_backend_texture;

        if (*cache_slot == NULL) {
            ID3D11ShaderResourceView *srv = rt->depth_only
                ? (ID3D11ShaderResourceView *)rt->d3d11_depth_srv
                : (ID3D11ShaderResourceView *)rt->d3d11_color_srv;
            if (srv) {
                vio_d3d11_texture *d3d_tex = calloc(1, sizeof(vio_d3d11_texture));
                d3d_tex->texture = NULL;  /* owned by render target */
                d3d_tex->srv = srv;       /* borrowed — RT owns the lifetime */
                d3d_tex->width = rt->width;
                d3d_tex->height = rt->height;

                if (rt->depth_only) {
                    /* Regular sampler for sampler2D + texture() (manual shadow comparison) */
                    D3D11_SAMPLER_DESC sampler_desc = {0};
                    sampler_desc.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
                    sampler_desc.AddressU = D3D11_TEXTURE_ADDRESS_BORDER;
                    sampler_desc.AddressV = D3D11_TEXTURE_ADDRESS_BORDER;
                    sampler_desc.AddressW = D3D11_TEXTURE_ADDRESS_BORDER;
                    sampler_desc.MaxAnisotropy = 1;
                    sampler_desc.MaxLOD = D3D11_FLOAT32_MAX;
                    sampler_desc.BorderColor[0] = 1.0f;
                    sampler_desc.BorderColor[1] = 1.0f;
                    sampler_desc.BorderColor[2] = 1.0f;
                    sampler_desc.BorderColor[3] = 1.0f;
                    ID3D11Device_CreateSamplerState(vio_d3d11.device, &sampler_desc, &d3d_tex->sampler);

                    /* Comparison sampler for sampler2DShadow + SampleCmp (hardware PCF) */
                    D3D11_SAMPLER_DESC cmp_desc = sampler_desc;
                    cmp_desc.Filter = D3D11_FILTER_COMPARISON_MIN_MAG_LINEAR_MIP_POINT;
                    cmp_desc.ComparisonFunc = D3D11_COMPARISON_LESS_EQUAL;
                    ID3D11Device_CreateSamplerState(vio_d3d11.device, &cmp_desc, &d3d_tex->sampler_cmp);
                    d3d_tex->is_depth = 1;
                } else {
                    /* Linear clamp sampler for color texture sampling */
                    D3D11_SAMPLER_DESC sampler_desc = {0};
                    sampler_desc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
                    sampler_desc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
                    sampler_desc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
                    sampler_desc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
                    sampler_desc.MaxAnisotropy = 1;
                    sampler_desc.MaxLOD = D3D11_FLOAT32_MAX;
                    ID3D11Device_CreateSamplerState(vio_d3d11.device, &sampler_desc, &d3d_tex->sampler);
                }

                *cache_slot = d3d_tex;
            }
        }

        if (*cache_slot != NULL) {
            tex->backend_texture = *cache_slot;
            tex->borrowed = 1; /* the cache slot owns the wrapper; don't free in tex dtor */
        }
    }
#endif

#ifdef HAVE_D3D12
    /* For D3D12: use pre-created SRV from render target (allocated once at RT creation) */
    /* Same cache-the-wrapper rationale as the D3D11 branch above: building
     * a fresh vio_d3d12_texture every frame leaks the struct and inflates
     * the descriptor-table churn. The wrapper has no heap-owned resources
     * here (the SRV descriptors are owned by the RT), so the cached
     * wrapper is freed in the RT's free handler alongside its descriptors. */
    if (rt->backend_type == VIO_RT_BACKEND_D3D12 && vio_d3d12.initialized) {
        vio_d3d12_texture **cache_slot = rt->depth_only
            ? (vio_d3d12_texture **)&rt->d3d12_depth_backend_texture
            : (vio_d3d12_texture **)&rt->d3d12_color_backend_texture;

        if (*cache_slot == NULL) {
            if (rt->depth_only && rt->d3d12_depth_srv_gpu) {
                vio_d3d12_texture *d3d_tex = calloc(1, sizeof(vio_d3d12_texture));
                d3d_tex->resource = NULL;
                d3d_tex->width = rt->width;
                d3d_tex->height = rt->height;
                d3d_tex->srv_gpu.ptr = rt->d3d12_depth_srv_gpu;
                d3d_tex->srv_cpu.ptr = rt->d3d12_depth_srv_cpu;
                *cache_slot = d3d_tex;
            } else if (!rt->depth_only && rt->d3d12_color_srv_gpu) {
                vio_d3d12_texture *d3d_tex = calloc(1, sizeof(vio_d3d12_texture));
                d3d_tex->resource = NULL;
                d3d_tex->width = rt->width;
                d3d_tex->height = rt->height;
                d3d_tex->srv_gpu.ptr = rt->d3d12_color_srv_gpu;
                d3d_tex->srv_cpu.ptr = rt->d3d12_color_srv_cpu;
                *cache_slot = d3d_tex;
            }
        }

        if (*cache_slot != NULL) {
            tex->backend_texture = *cache_slot;
            tex->borrowed = 1;
        }
    }
#endif

#ifdef HAVE_METAL
    /* For Metal: register the RT's MTLTexture into the texture registry so the
     * 2D batch can bind it via texture_id, then store that id on the texture
     * wrapper. The registry slot is cleared lazily by other deletes; the actual
     * MTLTexture lifetime stays with the RT (CFBridgingRelease in destroy). */
    if (rt->backend_type == VIO_RT_BACKEND_METAL) {
        void *cf_tex = rt->depth_only ? rt->metal_depth_texture : rt->metal_color_texture;
        if (cf_tex) {
            tex->texture_id = vio_metal_register_external_texture(cf_tex);
            tex->borrowed = 1;
        }
    }
#endif

#ifdef HAVE_VULKAN
    /* For Vulkan: hand out a vio_vulkan_texture wrapper around the RT's color
     * view + sampler, cached on the RT (built once, reused every call) — the
     * same cache-the-wrapper rationale as the D3D11/D3D12 branches (a fresh
     * wrapper per call would leak a struct per frame). The 2D flush textured
     * path reads wrapper->view + wrapper->sampler and allocates a per-frame
     * combined-image-sampler descriptor from the 2D pool ring, so no persistent
     * descriptor set is needed here (the per-frame ring already handles the
     * in-flight hazard correctly).
     *
     * The wrapper BORROWS the RT-owned view+sampler (it must NOT add itself to
     * vio_vk.live_textures and must NOT free those handles), and the RT's free
     * handler (vulkan_destroy_render_target) frees the cached struct. To stop
     * the returned VioTexture's free handler from running vulkan_destroy_texture
     * on the borrowed handles, we leave tex->backend = NULL (the texture free
     * handler short-circuits when backend is NULL) and mark it borrowed. */
    if (rt->backend_type == VIO_RT_BACKEND_VULKAN && vio_vk.initialized && !rt->depth_only) {
        vio_vulkan_texture **cache_slot =
            (vio_vulkan_texture **)&rt->vulkan_color_backend_texture;

        if (*cache_slot == NULL && rt->vulkan_color_view && rt->vulkan_sampler) {
            vio_vulkan_texture *w = calloc(1, sizeof(vio_vulkan_texture));
            if (w) {
                w->image      = (VkImage)rt->vulkan_color_image; /* borrowed (RT-owned) */
                w->allocation = NULL;                            /* RT owns the allocation */
                w->view       = (VkImageView)rt->vulkan_color_view;   /* borrowed */
                w->sampler    = (VkSampler)rt->vulkan_sampler;        /* borrowed */
                w->width      = rt->width;
                w->height     = rt->height;
                /* NOT linked into vio_vk.live_textures: the shutdown sweep frees
                 * GPU objects, but these are owned by the RT, not this wrapper. */
                w->next = w->prev = NULL;
                *cache_slot = w;
            }
        }

        if (*cache_slot != NULL) {
            tex->backend_texture = *cache_slot;
            tex->backend         = NULL; /* prevent the tex dtor from freeing borrowed handles */
            tex->borrowed        = 1;
        }
    }
#endif

    RETURN_COPY_VALUE(&tex_zval);
}

ZEND_FUNCTION(vio_cubemap)
{
    zval *ctx_zval;
    HashTable *config_ht;

    ZEND_PARSE_PARAMETERS_START(2, 2)
        Z_PARAM_OBJECT_OF_CLASS(ctx_zval, vio_context_ce)
        Z_PARAM_ARRAY_HT(config_ht)
    ZEND_PARSE_PARAMETERS_END();

    vio_context_object *ctx = Z_VIO_CONTEXT_P(ctx_zval);

    if (!ctx->initialized) {
        php_error_docref(NULL, E_WARNING, "Context is not initialized");
        RETURN_FALSE;
    }

    zval *faces_zval = zend_hash_str_find(config_ht, "faces", sizeof("faces") - 1);
    zval *pixels_zval = zend_hash_str_find(config_ht, "pixels", sizeof("pixels") - 1);

    /* Create VioCubemap object */
    zval cm_zval;
    object_init_ex(&cm_zval, vio_cubemap_ce);
    vio_cubemap_object *cm = Z_VIO_CUBEMAP_P(&cm_zval);
    cm->backend = ctx->backend;

    /* OpenGL goes through the vtable; D3D11/D3D12 stay inline below. */
    if (ctx->backend->upload_cubemap && strcmp(ctx->backend->name, "opengl") == 0) {
        /* Marshal source data: 6 RGBA8 buffers of (face_w, face_h). The
         * vtable assumes uniform face dimensions — file-based loads use
         * the first face's size, pixel-based reads w/h from config. */
        unsigned char *owned[6] = {NULL};
        int  owned_kind[6] = {0};  /* 0=none, 1=stbi, 2=emalloc */
        const void *faces[6] = {NULL};
        int face_w = 0, face_h = 0;
        int ok = 1;

        if (faces_zval && Z_TYPE_P(faces_zval) == IS_ARRAY) {
            HashTable *faces_ht = Z_ARRVAL_P(faces_zval);
            if (zend_hash_num_elements(faces_ht) != 6) {
                php_error_docref(NULL, E_WARNING, "cubemap 'faces' must have exactly 6 entries");
                ok = 0;
            }
            int face_idx = 0;
            zval *face_path;
            if (ok) ZEND_HASH_FOREACH_VAL(faces_ht, face_path) {
                if (Z_TYPE_P(face_path) != IS_STRING) {
                    php_error_docref(NULL, E_WARNING, "cubemap face %d must be a string path", face_idx);
                    ok = 0;
                    break;
                }
                int w, h, ch;
                unsigned char *data = stbi_load(Z_STRVAL_P(face_path), &w, &h, &ch, 4);
                if (!data) {
                    php_error_docref(NULL, E_WARNING, "Failed to load cubemap face %d: %s",
                        face_idx, stbi_failure_reason());
                    ok = 0;
                    break;
                }
                if (face_idx == 0) { face_w = w; face_h = h; cm->resolution = w; }
                owned[face_idx] = data;
                owned_kind[face_idx] = 1;
                faces[face_idx] = data;
                face_idx++;
            } ZEND_HASH_FOREACH_END();
        } else if (pixels_zval && Z_TYPE_P(pixels_zval) == IS_ARRAY) {
            zval *width_zval = zend_hash_str_find(config_ht, "width", sizeof("width") - 1);
            zval *height_zval = zend_hash_str_find(config_ht, "height", sizeof("height") - 1);
            HashTable *pixels_ht = Z_ARRVAL_P(pixels_zval);

            if (!width_zval || !height_zval) {
                php_error_docref(NULL, E_WARNING, "cubemap with 'pixels' requires 'width' and 'height'");
                ok = 0;
            } else if (zend_hash_num_elements(pixels_ht) != 6) {
                php_error_docref(NULL, E_WARNING, "cubemap 'pixels' must have exactly 6 face arrays");
                ok = 0;
            } else {
                face_w = (int)zval_get_long(width_zval);
                face_h = (int)zval_get_long(height_zval);
                cm->resolution = face_w;
                size_t face_size = (size_t)face_w * (size_t)face_h * 4;
                int face_idx = 0;
                zval *face_arr;
                ZEND_HASH_FOREACH_VAL(pixels_ht, face_arr) {
                    if (Z_TYPE_P(face_arr) != IS_ARRAY) {
                        php_error_docref(NULL, E_WARNING, "cubemap pixel face %d must be an array", face_idx);
                        ok = 0;
                        break;
                    }
                    unsigned char *buf = emalloc(face_size);
                    size_t j = 0;
                    zval *pixel_val;
                    ZEND_HASH_FOREACH_VAL(Z_ARRVAL_P(face_arr), pixel_val) {
                        if (j >= face_size) break;
                        buf[j++] = (unsigned char)zval_get_long(pixel_val);
                    } ZEND_HASH_FOREACH_END();
                    while (j < face_size) buf[j++] = 0;  /* zero-fill short faces */

                    owned[face_idx] = buf;
                    owned_kind[face_idx] = 2;
                    faces[face_idx] = buf;
                    face_idx++;
                } ZEND_HASH_FOREACH_END();
            }
        } else {
            php_error_docref(NULL, E_WARNING, "vio_cubemap requires 'faces' or 'pixels' array");
            ok = 0;
        }

        if (ok) {
            ctx->backend->upload_cubemap(cm, face_w, face_h, faces);
            cm->backend_type = 1;  /* VIO_CM_BACKEND_OPENGL — see vio_cubemap.h */
        }

        for (int i = 0; i < 6; i++) {
            if (owned_kind[i] == 1) stbi_image_free(owned[i]);
            else if (owned_kind[i] == 2) efree(owned[i]);
        }

        if (!ok) {
            zval_ptr_dtor(&cm_zval);
            RETURN_FALSE;
        }
    }

#ifdef HAVE_D3D11
    if (strcmp(ctx->backend->name, "d3d11") == 0 && vio_d3d11.initialized) {
        int res_w = 0, res_h = 0;
        unsigned char *face_data[6] = {NULL};
        int face_allocated[6] = {0};

        if (faces_zval && Z_TYPE_P(faces_zval) == IS_ARRAY) {
            HashTable *faces_ht = Z_ARRVAL_P(faces_zval);
            if (zend_hash_num_elements(faces_ht) != 6) {
                php_error_docref(NULL, E_WARNING, "cubemap 'faces' must have exactly 6 entries");
                zval_ptr_dtor(&cm_zval);
                RETURN_FALSE;
            }
            int face_idx = 0;
            zval *face_path;
            ZEND_HASH_FOREACH_VAL(faces_ht, face_path) {
                if (Z_TYPE_P(face_path) != IS_STRING) goto d3d11_cm_fail;
                int w, h, ch;
                face_data[face_idx] = stbi_load(Z_STRVAL_P(face_path), &w, &h, &ch, 4);
                if (!face_data[face_idx]) goto d3d11_cm_fail;
                face_allocated[face_idx] = 1;
                if (face_idx == 0) { res_w = w; res_h = h; }
                face_idx++;
            } ZEND_HASH_FOREACH_END();
        } else if (pixels_zval && Z_TYPE_P(pixels_zval) == IS_ARRAY) {
            HashTable *pixels_ht = Z_ARRVAL_P(pixels_zval);
            zval *w_zval = zend_hash_str_find(config_ht, "width", sizeof("width") - 1);
            zval *h_zval = zend_hash_str_find(config_ht, "height", sizeof("height") - 1);
            if (!w_zval || !h_zval || zend_hash_num_elements(pixels_ht) != 6) goto d3d11_cm_fail;
            res_w = (int)zval_get_long(w_zval);
            res_h = (int)zval_get_long(h_zval);
            size_t face_size = (size_t)res_w * res_h * 4;
            int face_idx = 0;
            zval *face_arr;
            ZEND_HASH_FOREACH_VAL(pixels_ht, face_arr) {
                if (Z_TYPE_P(face_arr) != IS_ARRAY) goto d3d11_cm_fail;
                face_data[face_idx] = emalloc(face_size);
                face_allocated[face_idx] = 2;
                size_t j = 0;
                zval *pv;
                ZEND_HASH_FOREACH_VAL(Z_ARRVAL_P(face_arr), pv) {
                    if (j >= face_size) break;
                    face_data[face_idx][j++] = (unsigned char)zval_get_long(pv);
                } ZEND_HASH_FOREACH_END();
                while (j < face_size) face_data[face_idx][j++] = 0;
                face_idx++;
            } ZEND_HASH_FOREACH_END();
        } else {
            goto d3d11_cm_fail;
        }

        cm->resolution = res_w;

        D3D11_TEXTURE2D_DESC tex_desc = {0};
        tex_desc.Width = res_w;
        tex_desc.Height = res_h;
        tex_desc.MipLevels = 1;
        tex_desc.ArraySize = 6;
        tex_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        tex_desc.SampleDesc.Count = 1;
        tex_desc.Usage = D3D11_USAGE_DEFAULT;
        tex_desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        tex_desc.MiscFlags = D3D11_RESOURCE_MISC_TEXTURECUBE;

        D3D11_SUBRESOURCE_DATA init_data[6];
        for (int i = 0; i < 6; i++) {
            init_data[i].pSysMem = face_data[i];
            init_data[i].SysMemPitch = res_w * 4;
            init_data[i].SysMemSlicePitch = 0;
        }

        ID3D11Texture2D *tex = NULL;
        HRESULT hr = ID3D11Device_CreateTexture2D(vio_d3d11.device, &tex_desc, init_data, &tex);

        for (int i = 0; i < 6; i++) {
            if (face_allocated[i] == 1 && face_data[i]) stbi_image_free(face_data[i]);
            else if (face_allocated[i] == 2 && face_data[i]) efree(face_data[i]);
        }

        if (FAILED(hr)) {
            php_error_docref(NULL, E_WARNING, "D3D11: Failed to create cubemap texture (0x%08lx)", hr);
            zval_ptr_dtor(&cm_zval);
            RETURN_FALSE;
        }

        cm->d3d11_texture = tex;

        D3D11_SHADER_RESOURCE_VIEW_DESC srv_desc = {0};
        srv_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        srv_desc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURECUBE;
        srv_desc.TextureCube.MostDetailedMip = 0;
        srv_desc.TextureCube.MipLevels = 1;

        ID3D11ShaderResourceView *srv = NULL;
        hr = ID3D11Device_CreateShaderResourceView(vio_d3d11.device, (ID3D11Resource *)tex, &srv_desc, &srv);
        if (FAILED(hr)) {
            php_error_docref(NULL, E_WARNING, "D3D11: Failed to create cubemap SRV (0x%08lx)", hr);
            ID3D11Texture2D_Release(tex);
            cm->d3d11_texture = NULL;
            zval_ptr_dtor(&cm_zval);
            RETURN_FALSE;
        }
        cm->d3d11_srv = srv;

        D3D11_SAMPLER_DESC sampler_desc = {0};
        sampler_desc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
        sampler_desc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
        sampler_desc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
        sampler_desc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        sampler_desc.MaxAnisotropy = 1;
        sampler_desc.MaxLOD = D3D11_FLOAT32_MAX;

        ID3D11SamplerState *sampler = NULL;
        ID3D11Device_CreateSamplerState(vio_d3d11.device, &sampler_desc, &sampler);
        cm->d3d11_sampler = sampler;

        cm->backend_type = 2; /* D3D11 */

        if (0) {
d3d11_cm_fail:
            for (int i = 0; i < 6; i++) {
                if (face_allocated[i] == 1 && face_data[i]) stbi_image_free(face_data[i]);
                else if (face_allocated[i] == 2 && face_data[i]) efree(face_data[i]);
            }
            php_error_docref(NULL, E_WARNING, "D3D11: cubemap creation failed");
            zval_ptr_dtor(&cm_zval);
            RETURN_FALSE;
        }
    }
#endif

    cm->valid = 1;
    RETURN_COPY_VALUE(&cm_zval);
}

ZEND_FUNCTION(vio_bind_cubemap)
{
    zval *ctx_zval;
    zval *cm_zval;
    zend_long slot = 0;

    ZEND_PARSE_PARAMETERS_START(2, 3)
        Z_PARAM_OBJECT_OF_CLASS(ctx_zval, vio_context_ce)
        Z_PARAM_OBJECT_OF_CLASS(cm_zval, vio_cubemap_ce)
        Z_PARAM_OPTIONAL
        Z_PARAM_LONG(slot)
    ZEND_PARSE_PARAMETERS_END();

    vio_context_object *ctx = Z_VIO_CONTEXT_P(ctx_zval);

    if (!ctx->initialized || !ctx->in_frame) {
        php_error_docref(NULL, E_WARNING, "Must call vio_bind_cubemap between vio_begin and vio_end");
        return;
    }

    vio_cubemap_object *cm = Z_VIO_CUBEMAP_P(cm_zval);
    if (!cm->valid) {
        php_error_docref(NULL, E_WARNING, "Cubemap is not valid");
        return;
    }

    if (ctx->backend->bind_cubemap_id) {
        ctx->backend->bind_cubemap_id(cm->texture_id, (int)slot);
    }

#ifdef HAVE_D3D11
    if (strcmp(ctx->backend->name, "d3d11") == 0 && vio_d3d11.initialized &&
        cm->d3d11_srv && cm->d3d11_sampler) {
        int hlsl_slot = (int)slot;
        if (ctx->bound_shader_object) {
            vio_shader_object *sh = (vio_shader_object *)ctx->bound_shader_object;
            if (slot >= 0 && slot < 16 && sh->gl_to_hlsl_sampler[slot] >= 0) {
                hlsl_slot = sh->gl_to_hlsl_sampler[slot];
            }
        }
        ID3D11ShaderResourceView *srv = (ID3D11ShaderResourceView *)cm->d3d11_srv;
        ID3D11SamplerState *sampler = (ID3D11SamplerState *)cm->d3d11_sampler;
        ID3D11DeviceContext_PSSetShaderResources(vio_d3d11.context, (UINT)hlsl_slot, 1, &srv);
        ID3D11DeviceContext_PSSetSamplers(vio_d3d11.context, (UINT)hlsl_slot, 1, &sampler);
    }
#endif

#ifdef HAVE_D3D12
    if (strcmp(ctx->backend->name, "d3d12") == 0 && vio_d3d12.initialized &&
        cm->d3d12_srv_gpu) {
        int hlsl_slot = (int)slot;
        if (ctx->bound_shader_object) {
            vio_shader_object *sh = (vio_shader_object *)ctx->bound_shader_object;
            if (slot >= 0 && slot < 16 && sh->gl_to_hlsl_sampler[slot] >= 0) {
                hlsl_slot = sh->gl_to_hlsl_sampler[slot];
            }
        }
        D3D12_CPU_DESCRIPTOR_HANDLE srv_cpu;
        srv_cpu.ptr = cm->d3d12_srv_cpu;
        vio_d3d12.pending_srvs[hlsl_slot] = srv_cpu;
        vio_d3d12.pending_srv_valid[hlsl_slot] = 1;
    }
#endif
}

ZEND_FUNCTION(vio_set_window_size)
{
    zval *ctx_zval;
    zend_long width, height;

    ZEND_PARSE_PARAMETERS_START(3, 3)
        Z_PARAM_OBJECT_OF_CLASS(ctx_zval, vio_context_ce)
        Z_PARAM_LONG(width)
        Z_PARAM_LONG(height)
    ZEND_PARSE_PARAMETERS_END();

    vio_context_object *ctx = Z_VIO_CONTEXT_P(ctx_zval);

    if (!ctx->initialized) {
        php_error_docref(NULL, E_WARNING, "Context is not initialized");
        return;
    }

#ifdef HAVE_GLFW
    if (ctx->window) {
        glfwSetWindowSize(ctx->window, (int)width, (int)height);
    }
#endif

    ctx->config.width  = (int)width;
    ctx->config.height = (int)height;
}

/* ── Backend capability query ─────────────────────────────────────── */

ZEND_FUNCTION(vio_supports_feature)
{
    zval *ctx_zval;
    zend_long feature;

    ZEND_PARSE_PARAMETERS_START(2, 2)
        Z_PARAM_OBJECT_OF_CLASS(ctx_zval, vio_context_ce)
        Z_PARAM_LONG(feature)
    ZEND_PARSE_PARAMETERS_END();

    vio_context_object *ctx = Z_VIO_CONTEXT_P(ctx_zval);
    if (!ctx->backend || !ctx->backend->supports_feature) {
        RETURN_FALSE;
    }
    RETURN_BOOL(ctx->backend->supports_feature((vio_feature)feature) != 0);
}

/* ── OpenGL diagnostics (Issue #3 part 3) ─────────────────────────── */

ZEND_FUNCTION(vio_gl_info)
{
    zval *ctx_zval;
    ZEND_PARSE_PARAMETERS_START(1, 1)
        Z_PARAM_OBJECT_OF_CLASS(ctx_zval, vio_context_ce)
    ZEND_PARSE_PARAMETERS_END();

    vio_context_object *ctx = Z_VIO_CONTEXT_P(ctx_zval);
    if (!ctx->initialized || !ctx->backend ||
        strcmp(ctx->backend->name, "opengl") != 0) {
        RETURN_FALSE;
    }

#ifdef HAVE_GLFW
    if (!vio_gl.initialized) {
        RETURN_FALSE;
    }

    array_init(return_value);

    char version_buf[32];
    snprintf(version_buf, sizeof(version_buf), "%d.%d", vio_gl.gl_major, vio_gl.gl_minor);
    add_assoc_string(return_value, "version", version_buf);
    add_assoc_long(return_value, "glsl", vio_gl.glsl_version);

    add_assoc_string(return_value, "renderer", vio_gl.renderer ? vio_gl.renderer : "");
    add_assoc_string(return_value, "vendor",   vio_gl.vendor   ? vio_gl.vendor   : "");
    add_assoc_string(return_value, "profile",  "core");

    zval extensions;
    array_init(&extensions);
    for (int i = 0; i < vio_gl.extension_count; i++) {
        if (vio_gl.extensions[i]) {
            add_next_index_string(&extensions, vio_gl.extensions[i]);
        }
    }
    add_assoc_zval(return_value, "extensions", &extensions);

    zval features;
    array_init(&features);
    add_assoc_bool(&features, "compute_shader",          vio_gl.caps.has_compute_shader);
    add_assoc_bool(&features, "tessellation",            vio_gl.caps.has_tessellation);
    add_assoc_bool(&features, "separate_shader_objects", vio_gl.caps.has_separate_shaders);
    add_assoc_bool(&features, "debug_output",            vio_gl.caps.has_debug_output);
    add_assoc_bool(&features, "dsa",                     vio_gl.caps.has_dsa);
    add_assoc_bool(&features, "buffer_storage",          vio_gl.caps.has_buffer_storage);
    add_assoc_bool(&features, "texture_storage",         vio_gl.caps.has_texture_storage);
    add_assoc_bool(&features, "texture_swizzle",         vio_gl.caps.has_texture_swizzle);
    add_assoc_zval(return_value, "features", &features);
    return;
#else
    RETURN_FALSE;
#endif
}

/* ── Render-target API surface (Issue #4) ─────────────────────────── */
/* Convenience wrappers around vio_render_target / vio_bind_render_target /
 * vio_unbind_render_target. The dispatcher functions live further up; these
 * just re-marshal arguments and call them. */

ZEND_FUNCTION(vio_create_render_target)
{
    zval *ctx_zval;
    zend_long width, height;
    HashTable *options = NULL;

    ZEND_PARSE_PARAMETERS_START(3, 4)
        Z_PARAM_OBJECT_OF_CLASS(ctx_zval, vio_context_ce)
        Z_PARAM_LONG(width)
        Z_PARAM_LONG(height)
        Z_PARAM_OPTIONAL
        Z_PARAM_ARRAY_HT(options)
    ZEND_PARSE_PARAMETERS_END();

    /* Marshal the explicit (w, h, options) form into the array-config form
     * that vio_render_target() expects, then call it. */
    zval config_arr;
    array_init(&config_arr);
    add_assoc_long(&config_arr, "width", width);
    add_assoc_long(&config_arr, "height", height);
    if (options) {
        zval *v;
        if ((v = zend_hash_str_find(options, "format", sizeof("format") - 1)) != NULL &&
            Z_TYPE_P(v) == IS_STRING) {
            if (strcmp(Z_STRVAL_P(v), "rgba16f") == 0) {
                add_assoc_bool(&config_arr, "hdr", 1);
            }
        }
        if ((v = zend_hash_str_find(options, "depth", sizeof("depth") - 1)) != NULL) {
            /* depth=true means "include a depth attachment" → not depth_only.
             * depth=false (or missing) leaves depth_only at default. */
            if (!zend_is_true(v)) {
                add_assoc_bool(&config_arr, "depth_only", 1);
            }
        }
        /* samples > 1 not yet wired through the vtable — silently dropped. */
    }

    zval args[2];
    ZVAL_COPY_VALUE(&args[0], ctx_zval);
    ZVAL_COPY_VALUE(&args[1], &config_arr);

    zval result;
    zend_call_known_function(
        zend_hash_str_find_ptr(EG(function_table), "vio_render_target", sizeof("vio_render_target") - 1),
        NULL, NULL, &result, 2, args, NULL);
    zval_ptr_dtor(&config_arr);

    RETURN_COPY_VALUE(&result);
}

ZEND_FUNCTION(vio_set_render_target)
{
    zval *ctx_zval, *rt_zval;
    ZEND_PARSE_PARAMETERS_START(2, 2)
        Z_PARAM_OBJECT_OF_CLASS(ctx_zval, vio_context_ce)
        Z_PARAM_OBJECT_OF_CLASS_OR_NULL(rt_zval, vio_render_target_ce)
    ZEND_PARSE_PARAMETERS_END();

    zval result;
    if (rt_zval) {
        zval args[2];
        ZVAL_COPY_VALUE(&args[0], ctx_zval);
        ZVAL_COPY_VALUE(&args[1], rt_zval);
        zend_call_known_function(
            zend_hash_str_find_ptr(EG(function_table), "vio_bind_render_target", sizeof("vio_bind_render_target") - 1),
            NULL, NULL, &result, 2, args, NULL);
    } else {
        zval args[1];
        ZVAL_COPY_VALUE(&args[0], ctx_zval);
        zend_call_known_function(
            zend_hash_str_find_ptr(EG(function_table), "vio_unbind_render_target", sizeof("vio_unbind_render_target") - 1),
            NULL, NULL, &result, 1, args, NULL);
    }
    zval_ptr_dtor(&result);
}

ZEND_FUNCTION(vio_destroy_render_target)
{
    zval *rt_zval;
    ZEND_PARSE_PARAMETERS_START(1, 1)
        Z_PARAM_OBJECT_OF_CLASS(rt_zval, vio_render_target_ce)
    ZEND_PARSE_PARAMETERS_END();

    /* Walk through the same destruction path the GC would take eventually,
     * but right now. The Zend object stays alive (we only release the GPU
     * resources); subsequent binds emit a warning because rt->valid is 0. */
    vio_render_target_object *rt = Z_VIO_RENDER_TARGET_P(rt_zval);
    if (rt->backend && rt->backend->destroy_render_target) {
        rt->backend->destroy_render_target(rt);
    }
    rt->valid = 0;
}

ZEND_FUNCTION(vio_push_render_target)
{
    zval *ctx_zval, *rt_zval;
    ZEND_PARSE_PARAMETERS_START(2, 2)
        Z_PARAM_OBJECT_OF_CLASS(ctx_zval, vio_context_ce)
        Z_PARAM_OBJECT_OF_CLASS(rt_zval, vio_render_target_ce)
    ZEND_PARSE_PARAMETERS_END();

    vio_context_object *ctx = Z_VIO_CONTEXT_P(ctx_zval);

    if (ctx->rt_stack_depth >= (int)(sizeof(ctx->rt_stack) / sizeof(ctx->rt_stack[0]))) {
        php_error_docref(NULL, E_WARNING,
            "vio_push_render_target: stack depth exceeded (max %d); replacing top",
            (int)(sizeof(ctx->rt_stack) / sizeof(ctx->rt_stack[0])));
        ctx->rt_stack_depth--;  /* replace top with new push */
    }

    /* Store a refcounted zval copy so the RT survives GC while on the stack. */
    zval *slot = (zval *)emalloc(sizeof(zval));
    ZVAL_COPY(slot, rt_zval);
    ctx->rt_stack[ctx->rt_stack_depth++] = slot;

    /* Bind the new target. */
    zval args[2];
    ZVAL_COPY_VALUE(&args[0], ctx_zval);
    ZVAL_COPY_VALUE(&args[1], rt_zval);
    zval result;
    zend_call_known_function(
        zend_hash_str_find_ptr(EG(function_table), "vio_bind_render_target", sizeof("vio_bind_render_target") - 1),
        NULL, NULL, &result, 2, args, NULL);
    zval_ptr_dtor(&result);
}

ZEND_FUNCTION(vio_pop_render_target)
{
    zval *ctx_zval;
    ZEND_PARSE_PARAMETERS_START(1, 1)
        Z_PARAM_OBJECT_OF_CLASS(ctx_zval, vio_context_ce)
    ZEND_PARSE_PARAMETERS_END();

    vio_context_object *ctx = Z_VIO_CONTEXT_P(ctx_zval);

    if (ctx->rt_stack_depth == 0) {
        php_error_docref(NULL, E_WARNING,
            "vio_pop_render_target: stack is empty; restoring default target");
        zval args[1];
        ZVAL_COPY_VALUE(&args[0], ctx_zval);
        zval result;
        zend_call_known_function(
            zend_hash_str_find_ptr(EG(function_table), "vio_unbind_render_target", sizeof("vio_unbind_render_target") - 1),
            NULL, NULL, &result, 1, args, NULL);
        zval_ptr_dtor(&result);
        return;
    }

    /* Drop the top entry. */
    zval *top = (zval *)ctx->rt_stack[--ctx->rt_stack_depth];
    zval_ptr_dtor(top);
    efree(top);

    /* Re-bind whatever's underneath (or the default target if empty). */
    zval args[2];
    zval result;
    if (ctx->rt_stack_depth > 0) {
        zval *new_top = (zval *)ctx->rt_stack[ctx->rt_stack_depth - 1];
        ZVAL_COPY_VALUE(&args[0], ctx_zval);
        ZVAL_COPY_VALUE(&args[1], new_top);
        zend_call_known_function(
            zend_hash_str_find_ptr(EG(function_table), "vio_bind_render_target", sizeof("vio_bind_render_target") - 1),
            NULL, NULL, &result, 2, args, NULL);
    } else {
        ZVAL_COPY_VALUE(&args[0], ctx_zval);
        zend_call_known_function(
            zend_hash_str_find_ptr(EG(function_table), "vio_unbind_render_target", sizeof("vio_unbind_render_target") - 1),
            NULL, NULL, &result, 1, args, NULL);
    }
    zval_ptr_dtor(&result);
}

/* ── Module lifecycle ─────────────────────────────────────────────── */

PHP_MINIT_FUNCTION(vio)
{
    REGISTER_INI_ENTRIES();
    le_vio_async_load = zend_register_list_destructors_ex(vio_async_load_dtor, NULL, "vio_async_load", module_number);
    le_vio_async_font = zend_register_list_destructors_ex(vio_async_font_dtor, NULL, "vio_async_font", module_number);
    vio_plugin_registry_init();
    vio_backend_registry_init();
    vio_backend_null_register();
#ifdef HAVE_GLFW
    vio_backend_opengl_register();
#endif
#ifdef HAVE_VULKAN
    vio_backend_vulkan_register();
#endif
#ifdef HAVE_METAL
    vio_backend_metal_register();
#endif
#ifdef HAVE_D3D11
    vio_backend_d3d11_register();
#endif
#ifdef HAVE_D3D12
    vio_backend_d3d12_register();
#endif
    vio_resource_init();
    vio_context_register();
    vio_mesh_register();
    vio_shader_register();
    vio_pipeline_register();
    vio_texture_register();
    vio_buffer_register();
    vio_compute_pipeline_register();
    vio_font_register();
    vio_sound_register();
    vio_render_target_register();
    vio_cubemap_register();
#ifdef HAVE_FFMPEG
    vio_recorder_register();
    vio_stream_register();
#endif
    vio_register_constants(module_number);
    vio_shader_compiler_init();
    vio_window_init();
    return SUCCESS;
}

PHP_MSHUTDOWN_FUNCTION(vio)
{
    UNREGISTER_INI_ENTRIES();
    vio_plugin_registry_shutdown();
    vio_audio_engine_shutdown();
    vio_shader_compiler_shutdown();
    vio_window_shutdown();
    vio_resource_shutdown();
    vio_backend_registry_shutdown();
    return SUCCESS;
}

PHP_RINIT_FUNCTION(vio)
{
#if defined(ZTS) && defined(COMPILE_DL_VIO)
    ZEND_TSRMLS_CACHE_UPDATE();
#endif
    return SUCCESS;
}

PHP_RSHUTDOWN_FUNCTION(vio)
{
    return SUCCESS;
}

PHP_MINFO_FUNCTION(vio)
{
    char buf[16];
    snprintf(buf, sizeof(buf), "%d", vio_backend_count());

    php_info_print_table_start();
    php_info_print_table_header(2, "vio support", "enabled");
    php_info_print_table_row(2, "Version", PHP_VIO_VERSION);
    php_info_print_table_row(2, "Backends registered", buf);
#ifdef HAVE_GLFW
    php_info_print_table_row(2, "GLFW", "available");
#else
    php_info_print_table_row(2, "GLFW", "not available");
#endif
#ifdef HAVE_GLSLANG
    php_info_print_table_row(2, "glslang (GLSL->SPIR-V)", "available");
#else
    php_info_print_table_row(2, "glslang (GLSL->SPIR-V)", "not available");
#endif
#ifdef HAVE_SPIRV_CROSS
    php_info_print_table_row(2, "SPIRV-Cross", "available");
#else
    php_info_print_table_row(2, "SPIRV-Cross", "not available");
#endif
#ifdef HAVE_VULKAN
    php_info_print_table_row(2, "Vulkan", "available");
#else
    php_info_print_table_row(2, "Vulkan", "not available");
#endif
#ifdef HAVE_METAL
    php_info_print_table_row(2, "Metal", "available");
#else
    php_info_print_table_row(2, "Metal", "not available");
#endif
#ifdef HAVE_D3D11
    php_info_print_table_row(2, "Direct3D 11", "available");
#else
    php_info_print_table_row(2, "Direct3D 11", "not available");
#endif
#ifdef HAVE_D3D12
    php_info_print_table_row(2, "Direct3D 12", "available");
#else
    php_info_print_table_row(2, "Direct3D 12", "not available");
#endif
#ifdef HAVE_FFMPEG
    php_info_print_table_row(2, "FFmpeg (video recording)", "available");
#else
    php_info_print_table_row(2, "FFmpeg (video recording)", "not available");
#endif
    php_info_print_table_end();

    DISPLAY_INI_ENTRIES();
}

/* ── Module entry ─────────────────────────────────────────────────── */

zend_module_entry vio_module_entry = {
    STANDARD_MODULE_HEADER,
    PHP_VIO_EXTNAME,
    ext_functions,
    PHP_MINIT(vio),
    PHP_MSHUTDOWN(vio),
    PHP_RINIT(vio),
    PHP_RSHUTDOWN(vio),
    PHP_MINFO(vio),
    PHP_VIO_VERSION,
    PHP_MODULE_GLOBALS(vio),
    NULL,  /* globals ctor */
    NULL,  /* globals dtor */
    NULL,  /* post deactivate */
    STANDARD_MODULE_PROPERTIES_EX
};

#ifdef COMPILE_DL_VIO
ZEND_GET_MODULE(vio)
#endif

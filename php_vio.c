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
#include "src/vio_2d.h"
#include "src/vio_font.h"
#include "src/vio_shader_compiler.h"
#include "src/vio_shader_reflect.h"
#include "src/vio_audio.h"
#include "src/vio_render_target.h"
#include "src/vio_cubemap.h"
#include "src/vio_recorder.h"
#include "src/vio_stream.h"
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

            /* Headless: create FBO for offscreen rendering */
            if (ctx->config.headless) {
                int w = ctx->config.width;
                int h = ctx->config.height;

                glGenFramebuffers(1, &ctx->headless_fbo);
                glBindFramebuffer(GL_FRAMEBUFFER, ctx->headless_fbo);

                glGenRenderbuffers(1, &ctx->headless_color_rb);
                glBindRenderbuffer(GL_RENDERBUFFER, ctx->headless_color_rb);
                glRenderbufferStorage(GL_RENDERBUFFER, GL_RGBA8, w, h);
                glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, ctx->headless_color_rb);

                glGenRenderbuffers(1, &ctx->headless_depth_rb);
                glBindRenderbuffer(GL_RENDERBUFFER, ctx->headless_depth_rb);
                glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, w, h);
                glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, ctx->headless_depth_rb);

                if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
                    php_error_docref(NULL, E_WARNING, "Headless FBO is not complete");
                    glBindFramebuffer(GL_FRAMEBUFFER, 0);
                    vio_window_destroy(ctx->window);
                    ctx->window = NULL;
                    zval_ptr_dtor(&obj);
                    RETURN_FALSE;
                }
                /* Keep FBO bound for all subsequent rendering */
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
#ifdef HAVE_GLFW
        /* Cleanup headless FBO before destroying GL context */
        if (ctx->headless_fbo) {
            glDeleteFramebuffers(1, &ctx->headless_fbo);
            glDeleteRenderbuffers(1, &ctx->headless_color_rb);
            glDeleteRenderbuffers(1, &ctx->headless_depth_rb);
            ctx->headless_fbo = 0;
        }
#endif
        if (ctx->backend->shutdown) {
            ctx->backend->shutdown();
        }
#ifdef HAVE_GLFW
        if (ctx->window) {
            vio_window_destroy(ctx->window);
            ctx->window = NULL;
        }
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
}

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
    /* Sync 2D projection and viewport to current window size (handles resize + maximize) */
    if (ctx->window && vio_gl.initialized) {
        int win_w, win_h;
        glfwGetWindowSize(ctx->window, &win_w, &win_h);
        int fb_w, fb_h;
        glfwGetFramebufferSize(ctx->window, &fb_w, &fb_h);
        if (win_w > 0 && win_h > 0 &&
            (win_w != ctx->state_2d.width || win_h != ctx->state_2d.height)) {
            vio_2d_set_size(&ctx->state_2d, win_w, win_h);
        }
        /* Always sync viewport and scissor scale to framebuffer size (Retina + resize) */
        glViewport(0, 0, fb_w, fb_h);
        ctx->state_2d.fb_width = fb_w;
        ctx->state_2d.fb_height = fb_h;
    }
#endif

#ifdef HAVE_METAL
    /* Sync 2D projection to current window size for Metal backend */
    if (ctx->window && strcmp(ctx->backend->name, "metal") == 0) {
        int win_w, win_h;
        glfwGetWindowSize(ctx->window, &win_w, &win_h);
        int fb_w, fb_h;
        glfwGetFramebufferSize(ctx->window, &fb_w, &fb_h);
        if (win_w > 0 && win_h > 0 &&
            (win_w != ctx->state_2d.width || win_h != ctx->state_2d.height)) {
            vio_2d_set_size(&ctx->state_2d, win_w, win_h);
        }
        ctx->state_2d.fb_width = fb_w;
        ctx->state_2d.fb_height = fb_h;
    }
#endif

#if (defined(HAVE_D3D11) || defined(HAVE_D3D12)) && defined(HAVE_GLFW)
    /* Sync 2D projection to current window size for D3D backends */
    if (ctx->window && (strcmp(ctx->backend->name, "d3d11") == 0
                     || strcmp(ctx->backend->name, "d3d12") == 0)) {
        int win_w, win_h;
        glfwGetWindowSize(ctx->window, &win_w, &win_h);
        int fb_w, fb_h;
        glfwGetFramebufferSize(ctx->window, &fb_w, &fb_h);
        if (win_w > 0 && win_h > 0 &&
            (win_w != ctx->state_2d.width || win_h != ctx->state_2d.height)) {
            vio_2d_set_size(&ctx->state_2d, win_w, win_h);
        }
        ctx->state_2d.fb_width = fb_w;
        ctx->state_2d.fb_height = fb_h;
    }
#endif

    vio_2d_begin(&ctx->state_2d);

#ifdef HAVE_GLFW
    /* Bind headless FBO before begin_frame so clear/draw go to offscreen target */
    if (ctx->headless_fbo && vio_gl.initialized) {
        glBindFramebuffer(GL_FRAMEBUFFER, ctx->headless_fbo);
        glViewport(0, 0, ctx->config.width, ctx->config.height);
    }
#endif

    if (ctx->backend->begin_frame) {
        ctx->backend->begin_frame();
    }

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

ZEND_FUNCTION(vio_mouse_position)
{
    zval *ctx_zval;

    ZEND_PARSE_PARAMETERS_START(1, 1)
        Z_PARAM_OBJECT_OF_CLASS(ctx_zval, vio_context_ce)
    ZEND_PARSE_PARAMETERS_END();

    vio_context_object *ctx = Z_VIO_CONTEXT_P(ctx_zval);
    array_init(return_value);
    add_next_index_double(return_value, ctx->input.mouse_x);
    add_next_index_double(return_value, ctx->input.mouse_y);
}

ZEND_FUNCTION(vio_mouse_delta)
{
    zval *ctx_zval;

    ZEND_PARSE_PARAMETERS_START(1, 1)
        Z_PARAM_OBJECT_OF_CLASS(ctx_zval, vio_context_ce)
    ZEND_PARSE_PARAMETERS_END();

    vio_context_object *ctx = Z_VIO_CONTEXT_P(ctx_zval);
    array_init(return_value);
    add_next_index_double(return_value, ctx->input.mouse_x - ctx->input.mouse_prev_x);
    add_next_index_double(return_value, ctx->input.mouse_y - ctx->input.mouse_prev_y);
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

    glfwRestoreWindow(ctx->window);
    glfwSetWindowAttrib(ctx->window, GLFW_DECORATED, GLFW_TRUE);
#endif
}

ZEND_FUNCTION(vio_set_fullscreen)
{
    zval *ctx_zval;

    ZEND_PARSE_PARAMETERS_START(1, 1)
        Z_PARAM_OBJECT_OF_CLASS(ctx_zval, vio_context_ce)
    ZEND_PARSE_PARAMETERS_END();

#ifdef HAVE_GLFW
    vio_context_object *ctx = Z_VIO_CONTEXT_P(ctx_zval);
    if (!ctx->window) return;

    GLFWmonitor *monitor = glfwGetPrimaryMonitor();
    const GLFWvidmode *mode = glfwGetVideoMode(monitor);
    glfwSetWindowMonitor(ctx->window, monitor,
        0, 0, mode->width, mode->height, mode->refreshRate);
#endif
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
        int w = 0, h = 0;
        glfwGetWindowSize(ctx->window, &w, &h);
        add_next_index_long(return_value, w);
        add_next_index_long(return_value, h);
        return;
    }
#endif
    add_next_index_long(return_value, ctx->config.width > 0 ? ctx->config.width : 800);
    add_next_index_long(return_value, ctx->config.height > 0 ? ctx->config.height : 600);
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

#ifdef HAVE_GLFW
    /* Create OpenGL objects (only if OpenGL backend is active) */
    if (strcmp(ctx->backend->name, "opengl") == 0 && vio_gl.initialized) {
    glGenVertexArrays(1, &mesh->vao);
    glGenBuffers(1, &mesh->vbo);

    glBindVertexArray(mesh->vao);

    glBindBuffer(GL_ARRAY_BUFFER, mesh->vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(float) * vertex_data_count, data, GL_STATIC_DRAW);

    if (has_explicit_layout && parsed_layout_count > 0) {
        /* Explicit dict-style layout: set up each attribute from the parsed layout */
        int offset = 0;
        for (int a = 0; a < parsed_layout_count; a++) {
            glVertexAttribPointer(
                parsed_layout[a].location,
                parsed_layout[a].components,
                GL_FLOAT,
                GL_FALSE,
                mesh->stride,
                (void *)(intptr_t)(offset * sizeof(float))
            );
            glEnableVertexAttribArray(parsed_layout[a].location);
            offset += parsed_layout[a].components;
        }
    } else if (has_colors && floats_per_vertex >= 7) {
        /* Legacy: Position (location 0, vec3) + Color (location 1, vec4) */
        glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, mesh->stride, (void *)0);
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(1, 4, GL_FLOAT, GL_FALSE, mesh->stride, (void *)(3 * sizeof(float)));
        glEnableVertexAttribArray(1);
    } else {
        /* Legacy: Position only (location 0, vec3) */
        glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, mesh->stride, (void *)0);
        glEnableVertexAttribArray(0);
    }

    if (indices && index_count > 0) {
        glGenBuffers(1, &mesh->ebo);
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, mesh->ebo);
        glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof(unsigned int) * index_count, indices, GL_STATIC_DRAW);
    }

    glBindVertexArray(0);
    } /* end if opengl */
#endif

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

#ifdef HAVE_GLFW
    if (strcmp(ctx->backend->name, "opengl") == 0 && vio_gl.initialized) {
        /* Use pipeline-bound shader if available, otherwise fall back to defaults */
        if (ctx->bound_shader_program) {
            /* Pipeline already called glUseProgram; just bind VAO and draw */
        } else {
            unsigned int shader = mesh->has_colors
                ? vio_gl.default_shader_program
                : vio_gl.default_shader_pos_only;
            glUseProgram(shader);
        }

        glBindVertexArray(mesh->vao);

        if (mesh->index_count > 0) {
            glDrawElements(GL_TRIANGLES, mesh->index_count, GL_UNSIGNED_INT, 0);
        } else {
            glDrawArrays(GL_TRIANGLES, 0, mesh->vertex_count);
        }

        glBindVertexArray(0);

        if (!ctx->bound_shader_program) {
            glUseProgram(0);
        }
    }
#endif

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

    /* Create VioShader object */
    zval shader_zval;
    object_init_ex(&shader_zval, vio_shader_ce);
    vio_shader_object *shader = Z_VIO_SHADER_P(&shader_zval);
    shader->format = format;

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
            /* Transpile SPIR-V to GLSL 410 */
            char *error_msg = NULL;

            char *vert_glsl = vio_spirv_to_glsl(shader->vert_spirv, shader->vert_spirv_size, 410, &error_msg);
            if (!vert_glsl) {
                php_error_docref(NULL, E_WARNING, "Vertex SPIR-V to GLSL transpilation failed: %s",
                    error_msg ? error_msg : "unknown error");
                free(error_msg);
                zval_ptr_dtor(&shader_zval);
                RETURN_FALSE;
            }

            char *frag_glsl = vio_spirv_to_glsl(shader->frag_spirv, shader->frag_spirv_size, 410, &error_msg);
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
                    for (int s = 0; s < shader->sampler_count; s++) {
                        strncpy(shader->sampler_names[s], frag_reflect.textures[s].name,
                                sizeof(shader->sampler_names[s]) - 1);
                        shader->sampler_is_depth[s] = frag_reflect.textures[s].is_depth ? 1 : 0;
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

    /* Store backend shader reference for lazy pipeline creation */
    pipe->backend_shader = shader->backend_shader;
    pipe->shader_ref = shader;

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

#ifdef HAVE_GLFW
    if (strcmp(ctx->backend->name, "opengl") == 0 && vio_gl.initialized) {
        glUseProgram(pipe->shader_program);

        /* Apply pipeline state */
        GLenum gl_topology;
        switch (pipe->topology) {
            case VIO_TRIANGLES:      gl_topology = GL_TRIANGLES; break;
            case VIO_TRIANGLE_STRIP: gl_topology = GL_TRIANGLE_STRIP; break;
            case VIO_TRIANGLE_FAN:   gl_topology = GL_TRIANGLE_FAN; break;
            case VIO_LINES:          gl_topology = GL_LINES; break;
            case VIO_LINE_STRIP:     gl_topology = GL_LINE_STRIP; break;
            case VIO_POINTS:         gl_topology = GL_POINTS; break;
            default:                 gl_topology = GL_TRIANGLES; break;
        }
        (void)gl_topology; /* stored in context for draw calls */

        /* Cull mode */
        if (pipe->cull_mode == VIO_CULL_NONE) {
            glDisable(GL_CULL_FACE);
        } else {
            glEnable(GL_CULL_FACE);
            glCullFace(pipe->cull_mode == VIO_CULL_BACK ? GL_BACK : GL_FRONT);
        }

        /* Depth test */
        if (pipe->depth_test) {
            glEnable(GL_DEPTH_TEST);
            glDepthFunc(pipe->depth_func == VIO_DEPTH_LEQUAL ? GL_LEQUAL : GL_LESS);
        } else {
            glDisable(GL_DEPTH_TEST);
        }

        /* Depth bias (polygon offset for shadow mapping) */
        if (pipe->depth_bias != 0.0f || pipe->slope_scaled_depth_bias != 0.0f) {
            glEnable(GL_POLYGON_OFFSET_FILL);
            glPolygonOffset(pipe->slope_scaled_depth_bias, pipe->depth_bias);
        } else {
            glDisable(GL_POLYGON_OFFSET_FILL);
        }

        /* Blend */
        switch (pipe->blend) {
            case VIO_BLEND_NONE:
                glDisable(GL_BLEND);
                break;
            case VIO_BLEND_ALPHA:
                glEnable(GL_BLEND);
                glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
                break;
            case VIO_BLEND_ADDITIVE:
                glEnable(GL_BLEND);
                glBlendFunc(GL_SRC_ALPHA, GL_ONE);
                break;
        }
    }
#endif

    /* Backend pipeline binding (D3D11/D3D12/Vulkan) */
    if (strcmp(ctx->backend->name, "opengl") != 0 &&
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
        pixels = (unsigned char *)Z_STRVAL_P(data_zval);
    } else {
        php_error_docref(NULL, E_WARNING, "vio_texture requires 'file' or 'data'+'width'+'height'");
        zval_ptr_dtor(&tex_zval);
        RETURN_FALSE;
    }

    tex->width    = w;
    tex->height   = h;
    tex->channels = channels;

#ifdef HAVE_GLFW
    if (strcmp(ctx->backend->name, "opengl") == 0 && vio_gl.initialized) {
        glGenTextures(1, &tex->texture_id);
        glBindTexture(GL_TEXTURE_2D, tex->texture_id);

        /* Wrap mode */
        GLint gl_wrap;
        switch (tex->wrap) {
            case VIO_WRAP_CLAMP:  gl_wrap = GL_CLAMP_TO_EDGE; break;
            case VIO_WRAP_MIRROR: gl_wrap = GL_MIRRORED_REPEAT; break;
            default:              gl_wrap = GL_REPEAT; break;
        }
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, gl_wrap);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, gl_wrap);

        /* Filter mode */
        GLint gl_filter = (tex->filter == VIO_FILTER_NEAREST) ? GL_NEAREST : GL_LINEAR;
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, gl_filter);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, gl_filter);

        /* Upload pixel data */
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels);

        /* Generate mipmaps if requested */
        zval *mipmap_zval = zend_hash_str_find(config_ht, "mipmaps", sizeof("mipmaps") - 1);
        if (mipmap_zval && zend_is_true(mipmap_zval)) {
            glGenerateMipmap(GL_TEXTURE_2D);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER,
                (tex->filter == VIO_FILTER_NEAREST) ? GL_NEAREST_MIPMAP_NEAREST : GL_LINEAR_MIPMAP_LINEAR);
        }

        glBindTexture(GL_TEXTURE_2D, 0);
    }
#endif

    /* Backend texture creation (D3D11/D3D12/Vulkan) */
    if (strcmp(ctx->backend->name, "opengl") != 0 && ctx->backend->create_texture) {
        zval *mipmap_zval = zend_hash_str_find(config_ht, "mipmaps", sizeof("mipmaps") - 1);

        vio_texture_desc desc = {0};
        desc.data = pixels;
        desc.data_size = (size_t)(w * h * channels);
        desc.width = w;
        desc.height = h;
        desc.filter = tex->filter;
        desc.wrap = tex->wrap;
        desc.mipmaps = (mipmap_zval && zend_is_true(mipmap_zval)) ? 1 : 0;
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

#ifdef HAVE_GLFW
    if (strcmp(ctx->backend->name, "opengl") == 0 && vio_gl.initialized) {
        glActiveTexture(GL_TEXTURE0 + (GLenum)slot);
        glBindTexture(GL_TEXTURE_2D, tex->texture_id);
    }
#endif

    /* Backend texture binding (D3D11/D3D12/Vulkan) */
    if (strcmp(ctx->backend->name, "opengl") != 0 &&
        tex->backend_texture && ctx->backend->bind_texture) {
        int hlsl_slot = (int)slot;
        int shader_wants_depth = 0;
        /* Remap GL texture unit to HLSL register using sampler map */
        if (ctx->bound_shader_object) {
            vio_shader_object *sh = (vio_shader_object *)ctx->bound_shader_object;
            if (slot >= 0 && slot < 16 && sh->gl_to_hlsl_sampler[slot] >= 0) {
                int sampler_idx = sh->gl_to_hlsl_sampler[slot];
                hlsl_slot = sampler_idx;
                if (sampler_idx >= 0 && sampler_idx < sh->sampler_count) {
                    shader_wants_depth = sh->sampler_is_depth[sampler_idx];
                }
            }
        }
        /* D3D12: depth/shadow samplers are remapped to registers 4+ by SPIRV-Cross.
         * The reflection-based hlsl_slot doesn't account for this, so add the offset. */
#ifdef HAVE_D3D12
        if (strcmp(ctx->backend->name, "d3d12") == 0 && shader_wants_depth) {
            hlsl_slot += 4;
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

#ifdef HAVE_GLFW
    if (strcmp(ctx->backend->name, "opengl") == 0 && vio_gl.initialized) {
        glGenBuffers(1, &buf->buffer_id);
        glBindBuffer(GL_UNIFORM_BUFFER, buf->buffer_id);
        glBufferData(GL_UNIFORM_BUFFER, size, init_data, GL_DYNAMIC_DRAW);
        glBindBufferBase(GL_UNIFORM_BUFFER, binding, buf->buffer_id);
        glBindBuffer(GL_UNIFORM_BUFFER, 0);
    }
#endif

    /* Backend uniform buffer creation */
    if (strcmp(ctx->backend->name, "opengl") != 0 && ctx->backend->create_buffer) {
        vio_buffer_desc desc = {0};
        desc.type = VIO_BUFFER_UNIFORM;
        desc.data = init_data;
        desc.size = size;
        desc.binding = binding;
        buf->backend_buffer = ctx->backend->create_buffer(&desc);
        buf->backend = ctx->backend;
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

#ifdef HAVE_GLFW
    if (buf->buffer_id) {
        glBindBuffer(GL_UNIFORM_BUFFER, buf->buffer_id);
        glBufferSubData(GL_UNIFORM_BUFFER, offset, data_len, data);
        glBindBuffer(GL_UNIFORM_BUFFER, 0);
    }
#endif

    /* Backend buffer update */
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

#ifdef HAVE_GLFW
    if (strcmp(ctx->backend->name, "opengl") == 0 && vio_gl.initialized) {
        glBindBufferBase(GL_UNIFORM_BUFFER, bind_point, buf->buffer_id);
    }
#endif

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

#ifdef HAVE_GLFW
    if (strcmp(ctx->backend->name, "opengl") == 0 && vio_gl.initialized && ctx->bound_shader_program) {
        GLint loc = glGetUniformLocation(ctx->bound_shader_program, name);
        if (loc < 0) {
            return; /* silently ignore missing uniforms */
        }

        if (Z_TYPE_P(value_zval) == IS_LONG) {
            glUniform1i(loc, (GLint)Z_LVAL_P(value_zval));
        } else if (Z_TYPE_P(value_zval) == IS_DOUBLE) {
            glUniform1f(loc, (GLfloat)Z_DVAL_P(value_zval));
        } else if (Z_TYPE_P(value_zval) == IS_ARRAY) {
            HashTable *ht = Z_ARRVAL_P(value_zval);
            int count = zend_hash_num_elements(ht);
            if (count <= 0) return;

            /* Check if first element is array (matrix) */
            zval *first = zend_hash_index_find(ht, 0);
            if (first && Z_TYPE_P(first) == IS_ARRAY) {
                /* Matrix: array of 16 floats packed row/column major */
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
                if (i == 16) glUniformMatrix4fv(loc, 1, GL_FALSE, mat);
                else if (i == 9) glUniformMatrix3fv(loc, 1, GL_FALSE, mat);
            } else {
                /* Flat array: vec2/3/4 or flat mat4 */
                float vals[16];
                int i = 0;
                zval *elem;
                ZEND_HASH_FOREACH_VAL(ht, elem) {
                    if (i < 16) vals[i++] = (float)zval_get_double(elem);
                } ZEND_HASH_FOREACH_END();
                switch (i) {
                    case 2:  glUniform2fv(loc, 1, vals); break;
                    case 3:  glUniform3fv(loc, 1, vals); break;
                    case 4:  glUniform4fv(loc, 1, vals); break;
                    case 9:  glUniformMatrix3fv(loc, 1, GL_FALSE, vals); break;
                    case 16: glUniformMatrix4fv(loc, 1, GL_FALSE, vals); break;
                }
            }
        }
    }
#endif

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

        /* Search both vertex and fragment uniform arrays */
        unsigned char *dst = NULL;
        int max_size = 0;
        int is_frag = 0;

        for (int u = 0; u < sh->uniform_count; u++) {
            if (strcmp(sh->uniforms[u].name, name) != 0) continue;
            int offset = sh->uniforms[u].offset;
            max_size = sh->uniforms[u].size;
            if (offset >= 0 && offset + max_size <= VIO_CBUFFER_SIZE) {
                dst = sh->cbuffer_data + offset;
            }
            break;
        }

        if (!dst) {
            for (int u = 0; u < sh->frag_uniform_count; u++) {
                if (strcmp(sh->frag_uniforms[u].name, name) != 0) continue;
                int offset = sh->frag_uniforms[u].offset;
                max_size = sh->frag_uniforms[u].size;
                if (offset >= 0 && offset + max_size <= VIO_CBUFFER_SIZE) {
                    dst = sh->frag_cbuffer_data + offset;
                    is_frag = 1;
                }
                break;
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

ZEND_FUNCTION(vio_font)
{
    zval *ctx_zval;
    char *path;
    size_t path_len;
    double size = 24.0;

    ZEND_PARSE_PARAMETERS_START(2, 3)
        Z_PARAM_OBJECT_OF_CLASS(ctx_zval, vio_context_ce)
        Z_PARAM_STRING(path, path_len)
        Z_PARAM_OPTIONAL
        Z_PARAM_DOUBLE(size)
    ZEND_PARSE_PARAMETERS_END();

    vio_context_object *ctx = Z_VIO_CONTEXT_P(ctx_zval);
    if (!ctx->initialized) {
        php_error_docref(NULL, E_WARNING, "Context is not initialized");
        RETURN_FALSE;
    }

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

    font->font_size = (float)size;
    font->ttf_len = ZSTR_LEN(contents);
    font->ttf_data = emalloc(font->ttf_len);
    memcpy(font->ttf_data, ZSTR_VAL(contents), font->ttf_len);
    zend_string_release(contents);

    /* Multi-range atlas packing (Latin, Cyrillic, Greek, CJK, Hangul, etc.) */
    int atlas_size = VIO_FONT_ATLAS_SIZE;
    unsigned char *atlas_bitmap = ecalloc(1, atlas_size * atlas_size);

    vio_font_pack_atlas(font, atlas_bitmap, atlas_size);

#ifdef HAVE_GLFW
    if (strcmp(ctx->backend->name, "opengl") == 0 && vio_gl.initialized) {
        glGenTextures(1, &font->atlas_texture);
        glBindTexture(GL_TEXTURE_2D, font->atlas_texture);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RED, atlas_size, atlas_size,
            0, GL_RED, GL_UNSIGNED_BYTE, atlas_bitmap);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        /* Swizzle: red channel -> alpha for proper text blending */
        GLint swizzle[] = {GL_ONE, GL_ONE, GL_ONE, GL_RED};
        glTexParameteriv(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_RGBA, swizzle);
        glBindTexture(GL_TEXTURE_2D, 0);
    }
#endif

#ifdef HAVE_D3D11
    if (strcmp(ctx->backend->name, "d3d11") == 0 && vio_d3d11.initialized) {
        /* Convert R8 atlas to R8G8B8A8 with white RGB and alpha from bitmap.
         * HLSL shader samples .a, so the glyph coverage goes into alpha. */
        unsigned char *rgba = emalloc(VIO_FONT_ATLAS_SIZE * VIO_FONT_ATLAS_SIZE * 4);
        for (int p = 0; p < VIO_FONT_ATLAS_SIZE * VIO_FONT_ATLAS_SIZE; p++) {
            rgba[p * 4 + 0] = 255;
            rgba[p * 4 + 1] = 255;
            rgba[p * 4 + 2] = 255;
            rgba[p * 4 + 3] = atlas_bitmap[p];
        }

        vio_texture_desc desc = {0};
        desc.width  = VIO_FONT_ATLAS_SIZE;
        desc.height = VIO_FONT_ATLAS_SIZE;
        desc.data   = rgba;
        desc.filter = VIO_FILTER_LINEAR;
        desc.wrap   = VIO_WRAP_CLAMP;
        desc.mipmaps = 0;
        font->atlas_backend_texture = ctx->backend->create_texture(&desc);
        efree(rgba);
    }
#endif

#ifdef HAVE_D3D12
    if (strcmp(ctx->backend->name, "d3d12") == 0 && vio_d3d12.initialized) {
        unsigned char *rgba = emalloc(VIO_FONT_ATLAS_SIZE * VIO_FONT_ATLAS_SIZE * 4);
        for (int p = 0; p < VIO_FONT_ATLAS_SIZE * VIO_FONT_ATLAS_SIZE; p++) {
            rgba[p * 4 + 0] = 255;
            rgba[p * 4 + 1] = 255;
            rgba[p * 4 + 2] = 255;
            rgba[p * 4 + 3] = atlas_bitmap[p];
        }

        vio_texture_desc desc = {0};
        desc.width  = VIO_FONT_ATLAS_SIZE;
        desc.height = VIO_FONT_ATLAS_SIZE;
        desc.data   = rgba;
        desc.filter = VIO_FILTER_LINEAR;
        desc.wrap   = VIO_WRAP_CLAMP;
        desc.mipmaps = 0;
        font->atlas_backend_texture = ctx->backend->create_texture(&desc);
        efree(rgba);
    }
#endif

#ifdef HAVE_METAL
    if (strcmp(ctx->backend->name, "metal") == 0) {
        font->atlas_texture = vio_metal_create_font_atlas(atlas_size, atlas_size, atlas_bitmap);
    }
#endif

    efree(atlas_bitmap);

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

    if (opts_ht) {
        zval *val;
        if ((val = zend_hash_str_find(opts_ht, "color", sizeof("color") - 1)) != NULL) {
            vio_argb_unpack((uint32_t)zval_get_long(val), &cr, &cg, &cb, &ca);
        }
        if ((val = zend_hash_str_find(opts_ht, "z", sizeof("z") - 1)) != NULL) {
            z = (float)zval_get_double(val);
        }
    }

    float fx = (float)x, fy = (float)y;
    float inv_w = 1.0f / (float)font->atlas_w;
    float inv_h = 1.0f / (float)font->atlas_h;

    /* Generate quads for each character (UTF-8 aware, hashmap lookup) */
    for (size_t i = 0; i < text_len; i++) {
        uint32_t cp = vio_utf8_decode(text, text_len, &i);
        zval *entry = zend_hash_index_find(&font->glyph_map, (zend_long)cp);
        if (!entry) continue;
        vio_stbtt_packedchar *b = (vio_stbtt_packedchar *)Z_STRVAL_P(entry);

        float px = fx + b->xoff;
        float py = fy + b->yoff;
        float pw = (float)(b->x1 - b->x0);
        float ph = (float)(b->y1 - b->y0);

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

        fx += b->xadvance;
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

    ZEND_PARSE_PARAMETERS_START(2, 2)
        Z_PARAM_OBJECT_OF_CLASS(font_zval, vio_font_ce)
        Z_PARAM_STRING(text, text_len)
    ZEND_PARSE_PARAMETERS_END();

    vio_font_object *font = Z_VIO_FONT_P(font_zval);
    if (!font->valid) {
        php_error_docref(NULL, E_WARNING, "Font is not valid");
        RETURN_FALSE;
    }

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

    array_init(return_value);
    add_assoc_double(return_value, "width", (double)width);
    add_assoc_double(return_value, "height", (double)height);
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

#ifdef HAVE_GLFW
    if (strcmp(ctx->backend->name, "opengl") == 0) {
        size_t size = (size_t)w * h * 4;
        zend_string *buf = zend_string_alloc(size, 0);

        if (ctx->headless_fbo) {
            glBindFramebuffer(GL_READ_FRAMEBUFFER, ctx->headless_fbo);
        }
        glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, ZSTR_VAL(buf));
        ZSTR_VAL(buf)[size] = '\0';

        /* Flip vertically (OpenGL reads bottom-up) */
        int stride = w * 4;
        unsigned char *tmp = emalloc(stride);
        unsigned char *data = (unsigned char *)ZSTR_VAL(buf);
        for (int y = 0; y < h / 2; y++) {
            unsigned char *top = data + y * stride;
            unsigned char *bot = data + (h - 1 - y) * stride;
            memcpy(tmp, top, stride);
            memcpy(top, bot, stride);
            memcpy(bot, tmp, stride);
        }
        efree(tmp);

        RETURN_NEW_STR(buf);
    }
#endif

#ifdef HAVE_D3D11
    if (strcmp(ctx->backend->name, "d3d11") == 0 && vio_d3d11.initialized) {
        /* d3d11_end_frame copies the rendered backbuffer into
         * vio_d3d11.readback_staging. Reading from staging avoids the
         * FLIP_DISCARD race where the live RTV has undefined content
         * after Present().
         *
         * If end_frame hasn't run yet (pre-first-end call from headless
         * tests that read before any vio_end), fall back to a one-shot
         * copy from the live RTV — still valid at that point. */
        ID3D11Texture2D *staging = vio_d3d11.readback_staging;
        ID3D11Texture2D *one_shot = NULL;

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
        /* Wait for any in-flight GPU work to complete. */
        vio_d3d12_wait_for_gpu();

        /* After Present(), frame_index has rotated to the NEXT backbuffer
         * (whose contents are undefined with FLIP_DISCARD). We want the
         * just-rendered frame — d3d12_present() stashes that index in
         * last_presented_frame_idx. Before the very first Present, both
         * indices are 0 so reading from frame_index is also correct. */
        UINT read_idx = vio_d3d12.last_presented_frame_idx;
        ID3D12Resource *src = vio_d3d12.frames[read_idx].render_target;
        if (!src) {
            php_error_docref(NULL, E_WARNING, "vio_read_pixels: no D3D12 render target");
            RETURN_FALSE;
        }

        D3D12_RESOURCE_DESC src_desc;
        ID3D12Resource_GetDesc(src, &src_desc);
        w = (int)src_desc.Width;
        h = (int)src_desc.Height;

        /* Get required layout of a readback copy */
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint = {0};
        UINT num_rows = 0;
        UINT64 row_size = 0, total_bytes = 0;
        ID3D12Device_GetCopyableFootprints(vio_d3d12.device, &src_desc, 0, 1, 0,
                                            &footprint, &num_rows, &row_size, &total_bytes);

        /* Create readback buffer */
        D3D12_HEAP_PROPERTIES hp = {0};
        hp.Type = D3D12_HEAP_TYPE_READBACK;
        D3D12_RESOURCE_DESC rb_desc = {0};
        rb_desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        rb_desc.Width = total_bytes;
        rb_desc.Height = 1;
        rb_desc.DepthOrArraySize = 1;
        rb_desc.MipLevels = 1;
        rb_desc.SampleDesc.Count = 1;
        rb_desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

        ID3D12Resource *readback = NULL;
        HRESULT hr = ID3D12Device_CreateCommittedResource(vio_d3d12.device, &hp,
            D3D12_HEAP_FLAG_NONE, &rb_desc, D3D12_RESOURCE_STATE_COPY_DEST, NULL,
            &IID_ID3D12Resource, (void **)&readback);
        if (FAILED(hr) || !readback) {
            php_error_docref(NULL, E_WARNING,
                "vio_read_pixels: Failed to create readback buffer (0x%08lx)", hr);
            RETURN_FALSE;
        }

        /* Transient command allocator + list — don't interfere with frame cmd_list */
        ID3D12CommandAllocator *alloc = NULL;
        hr = ID3D12Device_CreateCommandAllocator(vio_d3d12.device,
            D3D12_COMMAND_LIST_TYPE_DIRECT, &IID_ID3D12CommandAllocator, (void **)&alloc);
        if (FAILED(hr)) {
            ID3D12Resource_Release(readback);
            RETURN_FALSE;
        }

        ID3D12GraphicsCommandList *list = NULL;
        hr = ID3D12Device_CreateCommandList(vio_d3d12.device, 0,
            D3D12_COMMAND_LIST_TYPE_DIRECT, alloc, NULL,
            &IID_ID3D12GraphicsCommandList, (void **)&list);
        if (FAILED(hr)) {
            ID3D12CommandAllocator_Release(alloc);
            ID3D12Resource_Release(readback);
            RETURN_FALSE;
        }

        /* Transition RT: PRESENT -> COPY_SOURCE */
        D3D12_RESOURCE_BARRIER barrier = {0};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource = src;
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        ID3D12GraphicsCommandList_ResourceBarrier(list, 1, &barrier);

        /* Copy RT into readback buffer using placed footprint */
        D3D12_TEXTURE_COPY_LOCATION src_loc = {0};
        src_loc.pResource = src;
        src_loc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        src_loc.SubresourceIndex = 0;

        D3D12_TEXTURE_COPY_LOCATION dst_loc = {0};
        dst_loc.pResource = readback;
        dst_loc.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        dst_loc.PlacedFootprint = footprint;

        ID3D12GraphicsCommandList_CopyTextureRegion(list, &dst_loc, 0, 0, 0,
                                                     &src_loc, NULL);

        /* Transition back: COPY_SOURCE -> PRESENT */
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
        ID3D12GraphicsCommandList_ResourceBarrier(list, 1, &barrier);

        ID3D12GraphicsCommandList_Close(list);
        ID3D12CommandList *lists[] = { (ID3D12CommandList *)list };
        ID3D12CommandQueue_ExecuteCommandLists(vio_d3d12.cmd_queue, 1, lists);

        /* Wait for copy to finish */
        vio_d3d12_wait_for_gpu();

        /* Map readback buffer and copy rows out (RowPitch may include padding) */
        D3D12_RANGE read_range = {0, (SIZE_T)total_bytes};
        void *mapped_ptr = NULL;
        hr = ID3D12Resource_Map(readback, 0, &read_range, &mapped_ptr);
        if (FAILED(hr) || !mapped_ptr) {
            ID3D12GraphicsCommandList_Release(list);
            ID3D12CommandAllocator_Release(alloc);
            ID3D12Resource_Release(readback);
            php_error_docref(NULL, E_WARNING, "vio_read_pixels: Failed to map readback buffer");
            RETURN_FALSE;
        }

        size_t out_size = (size_t)w * h * 4;
        zend_string *buf = zend_string_alloc(out_size, 0);
        unsigned char *dst = (unsigned char *)ZSTR_VAL(buf);
        const unsigned char *srcp = (const unsigned char *)mapped_ptr;
        for (int y = 0; y < h; y++) {
            memcpy(dst + (size_t)y * w * 4,
                   srcp + (size_t)y * footprint.Footprint.RowPitch,
                   (size_t)w * 4);
        }
        ZSTR_VAL(buf)[out_size] = '\0';

        ID3D12Resource_Unmap(readback, 0, NULL);
        ID3D12GraphicsCommandList_Release(list);
        ID3D12CommandAllocator_Release(alloc);
        ID3D12Resource_Release(readback);

        RETURN_NEW_STR(buf);
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

#ifdef HAVE_GLFW
    if (strcmp(ctx->backend->name, "opengl") == 0) {
        size_t size = (size_t)w * h * 4;
        unsigned char *pixels = emalloc(size);

        if (ctx->headless_fbo) {
            glBindFramebuffer(GL_READ_FRAMEBUFFER, ctx->headless_fbo);
        }
        glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, pixels);

        /* Flip vertically */
        int stride = w * 4;
        unsigned char *tmp = emalloc(stride);
        for (int y = 0; y < h / 2; y++) {
            unsigned char *top = pixels + y * stride;
            unsigned char *bot = pixels + (h - 1 - y) * stride;
            memcpy(tmp, top, stride);
            memcpy(top, bot, stride);
            memcpy(bot, tmp, stride);
        }
        efree(tmp);

        int ok = stbi_write_png(path, w, h, 4, pixels, stride);
        efree(pixels);

        RETURN_BOOL(ok);
    }
#endif

#ifdef HAVE_METAL
    if (strcmp(ctx->backend->name, "metal") == 0) {
        RETURN_BOOL(vio_metal_save_screenshot(path, w, h) == 0);
    }
#endif

#if defined(HAVE_D3D11) || defined(HAVE_D3D12)
    {
        /* Fallback for D3D backends: delegate to vio_read_pixels and write PNG.
         * Calls vio_read_pixels() via the Zend API to avoid duplicating the readback
         * logic for D3D11/D3D12. */
        int is_d3d = 0;
#ifdef HAVE_D3D11
        if (strcmp(ctx->backend->name, "d3d11") == 0) is_d3d = 1;
#endif
#ifdef HAVE_D3D12
        if (strcmp(ctx->backend->name, "d3d12") == 0) is_d3d = 1;
#endif
        if (is_d3d) {
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

#ifdef HAVE_GLFW
    if (strcmp(ctx->backend->name, "opengl") == 0) {
        size_t size = (size_t)w * h * 4;
        unsigned char *pixels = emalloc(size);

        if (ctx->headless_fbo) {
            glBindFramebuffer(GL_READ_FRAMEBUFFER, ctx->headless_fbo);
        }
        glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, pixels);

        /* Flip vertically */
        int stride = w * 4;
        unsigned char *tmp = emalloc(stride);
        for (int y = 0; y < h / 2; y++) {
            unsigned char *top = pixels + y * stride;
            unsigned char *bot = pixels + (h - 1 - y) * stride;
            memcpy(tmp, top, stride);
            memcpy(top, bot, stride);
            memcpy(bot, tmp, stride);
        }
        efree(tmp);

        int ret = vio_recorder_write_rgba(rec, pixels);
        efree(pixels);

        if (ret < 0) {
            php_error_docref(NULL, E_WARNING, "Failed to encode frame (error %d)", ret);
            RETURN_FALSE;
        }
        RETURN_TRUE;
    }
#endif

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

#ifdef HAVE_GLFW
    if (strcmp(ctx->backend->name, "opengl") == 0) {
        size_t size = (size_t)w * h * 4;
        unsigned char *pixels = emalloc(size);

        if (ctx->headless_fbo) {
            glBindFramebuffer(GL_READ_FRAMEBUFFER, ctx->headless_fbo);
        }
        glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, pixels);

        /* Flip vertically */
        int stride = w * 4;
        unsigned char *tmp = emalloc(stride);
        for (int y = 0; y < h / 2; y++) {
            unsigned char *top = pixels + y * stride;
            unsigned char *bot = pixels + (h - 1 - y) * stride;
            memcpy(tmp, top, stride);
            memcpy(top, bot, stride);
            memcpy(bot, tmp, stride);
        }
        efree(tmp);

        int ret = vio_stream_write_rgba(st, pixels);
        efree(pixels);

        if (ret < 0) {
            php_error_docref(NULL, E_WARNING, "Failed to encode/send frame (error %d)", ret);
            RETURN_FALSE;
        }
        RETURN_TRUE;
    }
#endif

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

    /* Features */
    REGISTER_LONG_CONSTANT("VIO_FEATURE_COMPUTE", VIO_FEATURE_COMPUTE, CONST_CS | CONST_PERSISTENT);
    REGISTER_LONG_CONSTANT("VIO_FEATURE_RAYTRACING", VIO_FEATURE_RAYTRACING, CONST_CS | CONST_PERSISTENT);
    REGISTER_LONG_CONSTANT("VIO_FEATURE_TESSELLATION", VIO_FEATURE_TESSELLATION, CONST_CS | CONST_PERSISTENT);
    REGISTER_LONG_CONSTANT("VIO_FEATURE_GEOMETRY", VIO_FEATURE_GEOMETRY, CONST_CS | CONST_PERSISTENT);
    REGISTER_LONG_CONSTANT("VIO_FEATURE_MULTIVIEW", VIO_FEATURE_MULTIVIEW, CONST_CS | CONST_PERSISTENT);

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

    if (!load->done) {
        RETURN_NULL(); /* Still loading */
    }

    if (load->failed) {
        zend_list_delete(Z_RES_P(res_zval));
        RETURN_FALSE;
    }

    /* Return array with image data info */
    array_init(return_value);
    add_assoc_long(return_value, "width", load->width);
    add_assoc_long(return_value, "height", load->height);
    add_assoc_stringl(return_value, "data", (char *)load->data, load->width * load->height * 4);
    stbi_image_free(load->data);
    load->data = NULL;
    zend_list_delete(Z_RES_P(res_zval));
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

#ifdef HAVE_GLFW
    if (strcmp(ctx->backend->name, "opengl") == 0) {
        glViewport((GLint)x, (GLint)y, (GLsizei)w, (GLsizei)h);
    }
#endif

    /* Backend viewport */
    if (strcmp(ctx->backend->name, "opengl") != 0 && ctx->backend->set_viewport) {
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

    /* 3D draws are issued inline via vio_draw / vio_draw_instanced.
     * This function serves as a flush/sync point - ensure all GL state is clean. */
#ifdef HAVE_GLFW
    if (strcmp(ctx->backend->name, "opengl") == 0 && vio_gl.initialized) {
        glBindVertexArray(0);
        glUseProgram(0);
    }
#endif
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

#ifdef HAVE_GLFW
    if (strcmp(ctx->backend->name, "opengl") == 0 && vio_gl.initialized) {
        size_t total_floats = (size_t)instance_count * 16;

        /* Create instance VBO with model matrices */
        unsigned int instance_vbo;
        glGenBuffers(1, &instance_vbo);
        glBindBuffer(GL_ARRAY_BUFFER, instance_vbo);
        glBufferData(GL_ARRAY_BUFFER, mat_size, mat_data, GL_STREAM_DRAW);

        /* Bind mesh VAO and set up instance attribute pointers (locations 3-6 for mat4) */
        glBindVertexArray(mesh->vao);

        for (int col = 0; col < 4; col++) {
            unsigned int loc = 3 + col;
            glEnableVertexAttribArray(loc);
            glVertexAttribPointer(loc, 4, GL_FLOAT, GL_FALSE,
                sizeof(float) * 16, (void *)(sizeof(float) * 4 * col));
            glVertexAttribDivisor(loc, 1);
        }

        /* Draw instanced */
        if (mesh->index_count > 0) {
            glDrawElementsInstanced(GL_TRIANGLES, mesh->index_count,
                GL_UNSIGNED_INT, 0, (GLsizei)instance_count);
        } else {
            glDrawArraysInstanced(GL_TRIANGLES, 0, mesh->vertex_count,
                (GLsizei)instance_count);
        }

        /* Clean up instance attributes */
        for (int col = 0; col < 4; col++) {
            glVertexAttribDivisor(3 + col, 0);
            glDisableVertexAttribArray(3 + col);
        }

        glBindVertexArray(0);
        glDeleteBuffers(1, &instance_vbo);
    }
#endif

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

            /* Persistent UPLOAD heap instance buffer */
            static ID3D12Resource *s_d3d12_instance_buf = NULL;
            static UINT s_d3d12_instance_capacity = 0;
            static D3D12_GPU_VIRTUAL_ADDRESS s_d3d12_instance_gpu = 0;

            if (!s_d3d12_instance_buf || s_d3d12_instance_capacity < byte_size) {
                if (s_d3d12_instance_buf) {
                    ID3D12Resource_Release(s_d3d12_instance_buf);
                    s_d3d12_instance_buf = NULL;
                }
                D3D12_HEAP_PROPERTIES heap_props = {0};
                heap_props.Type = D3D12_HEAP_TYPE_UPLOAD;
                D3D12_RESOURCE_DESC res_desc = {0};
                res_desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
                res_desc.Width = byte_size;
                res_desc.Height = 1;
                res_desc.DepthOrArraySize = 1;
                res_desc.MipLevels = 1;
                res_desc.SampleDesc.Count = 1;
                res_desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

                HRESULT hr = ID3D12Device_CreateCommittedResource(vio_d3d12.device,
                    &heap_props, D3D12_HEAP_FLAG_NONE, &res_desc,
                    D3D12_RESOURCE_STATE_GENERIC_READ, NULL,
                    &IID_ID3D12Resource, (void **)&s_d3d12_instance_buf);
                if (FAILED(hr)) { if (allocated) efree(allocated); return; }
                s_d3d12_instance_capacity = byte_size;
                s_d3d12_instance_gpu = ID3D12Resource_GetGPUVirtualAddress(s_d3d12_instance_buf);
            }

            /* Upload */
            void *mapped = NULL;
            D3D12_RANGE read_range = {0, 0};
            if (SUCCEEDED(ID3D12Resource_Map(s_d3d12_instance_buf, 0, &read_range, &mapped))) {
                memcpy(mapped, mat_data, byte_size);
                ID3D12Resource_Unmap(s_d3d12_instance_buf, 0, NULL);
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
                /* Allocate per-draw cbuffer slices from linear allocator */
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

            /* Bind vertex buffers: slot 0 = mesh, slot 1 = instances */
            vio_d3d12_buffer *vb = (vio_d3d12_buffer *)mesh->backend_vb;
            D3D12_VERTEX_BUFFER_VIEW vbvs[2];
            vbvs[0].BufferLocation = vb->gpu_address;
            vbvs[0].SizeInBytes = (UINT)vb->size;
            vbvs[0].StrideInBytes = (UINT)mesh->stride;
            vbvs[1].BufferLocation = s_d3d12_instance_gpu;
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

#ifdef HAVE_GLFW
    if (strcmp(ctx->backend->name, "opengl") == 0 && vio_gl.initialized) {
        glGenFramebuffers(1, &rt->fbo);
        glBindFramebuffer(GL_FRAMEBUFFER, rt->fbo);

        /* Depth texture (always created) */
        glGenTextures(1, &rt->depth_texture);
        glBindTexture(GL_TEXTURE_2D, rt->depth_texture);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT24, width, height,
            0, GL_DEPTH_COMPONENT, GL_FLOAT, NULL);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_BORDER);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_BORDER);
        float border_color[] = {1.0f, 1.0f, 1.0f, 1.0f};
        glTexParameterfv(GL_TEXTURE_2D, GL_TEXTURE_BORDER_COLOR, border_color);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D,
            rt->depth_texture, 0);

        if (depth_only) {
            /* No color attachment */
            glDrawBuffer(GL_NONE);
            glReadBuffer(GL_NONE);
        } else {
            /* Color texture */
            glGenTextures(1, &rt->color_texture);
            glBindTexture(GL_TEXTURE_2D, rt->color_texture);
            glTexImage2D(GL_TEXTURE_2D, 0, hdr ? GL_RGBA16F : GL_RGBA8, width, height,
                0, GL_RGBA, hdr ? GL_FLOAT : GL_UNSIGNED_BYTE, NULL);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
            glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                rt->color_texture, 0);
        }

        /* Verify completeness */
        GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glBindTexture(GL_TEXTURE_2D, 0);

        if (status != GL_FRAMEBUFFER_COMPLETE) {
            php_error_docref(NULL, E_WARNING, "Render target FBO is not complete (status: 0x%04x)", status);
            zval_ptr_dtor(&rt_zval);
            RETURN_FALSE;
        }

        rt->backend_type = VIO_RT_BACKEND_OPENGL;
    }
#endif

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

        /* For depth-only targets: pre-create SRV for shadow map sampling */
        if (depth_only && vio_d3d12.srv_heap.count < vio_d3d12.srv_heap.capacity) {
            UINT srv_idx = vio_d3d12.srv_heap.count++;
            D3D12_CPU_DESCRIPTOR_HANDLE srv_cpu;
            D3D12_GPU_DESCRIPTOR_HANDLE srv_gpu;
            ID3D12DescriptorHeap_GetCPUDescriptorHandleForHeapStart(vio_d3d12.srv_heap.heap, &srv_cpu);
            ID3D12DescriptorHeap_GetGPUDescriptorHandleForHeapStart(vio_d3d12.srv_heap.heap, &srv_gpu);
            srv_cpu.ptr += srv_idx * vio_d3d12.srv_heap.descriptor_size;
            srv_gpu.ptr += srv_idx * vio_d3d12.srv_heap.descriptor_size;

            D3D12_SHADER_RESOURCE_VIEW_DESC srv_desc = {0};
            srv_desc.Format = DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
            srv_desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            srv_desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            srv_desc.Texture2D.MipLevels = 1;
            ID3D12Device_CreateShaderResourceView(vio_d3d12.device, depth_res, &srv_desc, srv_cpu);

            rt->d3d12_depth_srv_gpu = srv_gpu.ptr;
            rt->d3d12_depth_srv_cpu = srv_cpu.ptr;
        }

        /* For color targets: pre-create SRV for color texture sampling */
        if (!depth_only && rt->d3d12_color_resource && vio_d3d12.srv_heap.count < vio_d3d12.srv_heap.capacity) {
            UINT color_srv_idx = vio_d3d12.srv_heap.count++;
            D3D12_CPU_DESCRIPTOR_HANDLE color_srv_cpu;
            D3D12_GPU_DESCRIPTOR_HANDLE color_srv_gpu;
            ID3D12DescriptorHeap_GetCPUDescriptorHandleForHeapStart(vio_d3d12.srv_heap.heap, &color_srv_cpu);
            ID3D12DescriptorHeap_GetGPUDescriptorHandleForHeapStart(vio_d3d12.srv_heap.heap, &color_srv_gpu);
            color_srv_cpu.ptr += color_srv_idx * vio_d3d12.srv_heap.descriptor_size;
            color_srv_gpu.ptr += color_srv_idx * vio_d3d12.srv_heap.descriptor_size;

            D3D12_SHADER_RESOURCE_VIEW_DESC color_srv_desc = {0};
            color_srv_desc.Format = hdr ? DXGI_FORMAT_R16G16B16A16_FLOAT : DXGI_FORMAT_R8G8B8A8_UNORM;
            color_srv_desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            color_srv_desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            color_srv_desc.Texture2D.MipLevels = 1;
            ID3D12Device_CreateShaderResourceView(vio_d3d12.device, (ID3D12Resource *)rt->d3d12_color_resource, &color_srv_desc, color_srv_cpu);

            rt->d3d12_color_srv_gpu = color_srv_gpu.ptr;
            rt->d3d12_color_srv_cpu = color_srv_cpu.ptr;
        }

        rt->backend_type = VIO_RT_BACKEND_D3D12;
    }
#endif

    rt->valid = 1;
    RETURN_COPY_VALUE(&rt_zval);
}

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

#ifdef HAVE_GLFW
    if (rt->backend_type == VIO_RT_BACKEND_OPENGL && vio_gl.initialized) {
        glBindFramebuffer(GL_FRAMEBUFFER, rt->fbo);
        glViewport(0, 0, rt->width, rt->height);
    }
#endif

#ifdef HAVE_D3D11
    if (rt->backend_type == VIO_RT_BACKEND_D3D11 && vio_d3d11.initialized) {
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

#ifdef HAVE_D3D12
    if (rt->backend_type == VIO_RT_BACKEND_D3D12 && vio_d3d12.initialized) {
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

#ifdef HAVE_GLFW
    if (strcmp(ctx->backend->name, "opengl") == 0) {
        /* Restore default framebuffer (or headless FBO if applicable) */
        if (ctx->headless_fbo) {
            glBindFramebuffer(GL_FRAMEBUFFER, ctx->headless_fbo);
            glViewport(0, 0, ctx->config.width, ctx->config.height);
        } else {
            glBindFramebuffer(GL_FRAMEBUFFER, 0);
            if (ctx->window) {
                int fb_w, fb_h;
                glfwGetFramebufferSize(ctx->window, &fb_w, &fb_h);
                glViewport(0, 0, fb_w, fb_h);
            }
        }
    }
#endif

#ifdef HAVE_D3D11
    if (strcmp(ctx->backend->name, "d3d11") == 0 && vio_d3d11.initialized) {
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
        /* Barrier: transition the offscreen depth resource to SRV for shadow sampling.
         * We find the currently bound RT's depth resource from the DSV heap.
         * Since we track the RT object via current_dsv, and the depth_resource is stored
         * on the render target object, we use a flag approach:
         * If the render target was depth-only, its depth resource needs the barrier. */
        /* Barrier: transition shadow map depth from DEPTH_WRITE → SRV for sampling */
        if (vio_d3d12.current_bound_rt) {
            vio_render_target_object *bound_rt = (vio_render_target_object *)vio_d3d12.current_bound_rt;
            /* Transition depth to SRV if depth-only target */
            if (bound_rt->d3d12_depth_resource && bound_rt->depth_only) {
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
    /* For D3D11: create a backend texture wrapper with the SRV from the render target */
    if (rt->backend_type == VIO_RT_BACKEND_D3D11 && vio_d3d11.initialized) {
        ID3D11ShaderResourceView *srv = NULL;
        if (rt->depth_only) {
            srv = (ID3D11ShaderResourceView *)rt->d3d11_depth_srv;
        } else if (rt->d3d11_color_srv) {
            srv = (ID3D11ShaderResourceView *)rt->d3d11_color_srv;
        }
        if (srv) {
            vio_d3d11_texture *d3d_tex = calloc(1, sizeof(vio_d3d11_texture));
            d3d_tex->texture = NULL;  /* owned by render target */
            d3d_tex->srv = srv;
            ID3D11ShaderResourceView_AddRef(srv);  /* prevent premature release */
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

            tex->backend_texture = d3d_tex;
        }
    }
#endif

#ifdef HAVE_D3D12
    /* For D3D12: use pre-created SRV from render target (allocated once at RT creation) */
    if (rt->backend_type == VIO_RT_BACKEND_D3D12 && vio_d3d12.initialized) {
        if (rt->depth_only && rt->d3d12_depth_srv_gpu) {
            vio_d3d12_texture *d3d_tex = calloc(1, sizeof(vio_d3d12_texture));
            d3d_tex->resource = NULL; /* owned by render target, don't release */
            d3d_tex->width = rt->width;
            d3d_tex->height = rt->height;
            d3d_tex->srv_gpu.ptr = rt->d3d12_depth_srv_gpu;
            d3d_tex->srv_cpu.ptr = rt->d3d12_depth_srv_cpu;
            tex->backend_texture = d3d_tex;
        } else if (!rt->depth_only && rt->d3d12_color_srv_gpu) {
            vio_d3d12_texture *d3d_tex = calloc(1, sizeof(vio_d3d12_texture));
            d3d_tex->resource = NULL; /* owned by render target, don't release */
            d3d_tex->width = rt->width;
            d3d_tex->height = rt->height;
            d3d_tex->srv_gpu.ptr = rt->d3d12_color_srv_gpu;
            d3d_tex->srv_cpu.ptr = rt->d3d12_color_srv_cpu;
            tex->backend_texture = d3d_tex;
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

#ifdef HAVE_GLFW
    if (strcmp(ctx->backend->name, "opengl") == 0 && vio_gl.initialized) {
        glGenTextures(1, &cm->texture_id);
        glBindTexture(GL_TEXTURE_CUBE_MAP, cm->texture_id);

        if (faces_zval && Z_TYPE_P(faces_zval) == IS_ARRAY) {
            /* File-based: 6 image file paths (+X, -X, +Y, -Y, +Z, -Z) */
            HashTable *faces_ht = Z_ARRVAL_P(faces_zval);
            if (zend_hash_num_elements(faces_ht) != 6) {
                php_error_docref(NULL, E_WARNING, "cubemap 'faces' must have exactly 6 entries");
                glDeleteTextures(1, &cm->texture_id);
                zval_ptr_dtor(&cm_zval);
                RETURN_FALSE;
            }

            int face_idx = 0;
            zval *face_path;
            ZEND_HASH_FOREACH_VAL(faces_ht, face_path) {
                if (Z_TYPE_P(face_path) != IS_STRING) {
                    php_error_docref(NULL, E_WARNING, "cubemap face %d must be a string path", face_idx);
                    glDeleteTextures(1, &cm->texture_id);
                    zval_ptr_dtor(&cm_zval);
                    RETURN_FALSE;
                }

                int w, h, channels;
                unsigned char *data = stbi_load(Z_STRVAL_P(face_path), &w, &h, &channels, 4);
                if (!data) {
                    php_error_docref(NULL, E_WARNING, "Failed to load cubemap face %d: %s",
                        face_idx, stbi_failure_reason());
                    glDeleteTextures(1, &cm->texture_id);
                    zval_ptr_dtor(&cm_zval);
                    RETURN_FALSE;
                }

                if (face_idx == 0) {
                    cm->resolution = w;
                }

                glTexImage2D(GL_TEXTURE_CUBE_MAP_POSITIVE_X + face_idx, 0, GL_RGBA,
                    w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, data);
                stbi_image_free(data);
                face_idx++;
            } ZEND_HASH_FOREACH_END();

        } else if (pixels_zval && Z_TYPE_P(pixels_zval) == IS_ARRAY) {
            /* Procedural: 6 arrays of RGBA pixel data */
            HashTable *pixels_ht = Z_ARRVAL_P(pixels_zval);

            zval *width_zval = zend_hash_str_find(config_ht, "width", sizeof("width") - 1);
            zval *height_zval = zend_hash_str_find(config_ht, "height", sizeof("height") - 1);

            if (!width_zval || !height_zval) {
                php_error_docref(NULL, E_WARNING, "cubemap with 'pixels' requires 'width' and 'height'");
                glDeleteTextures(1, &cm->texture_id);
                zval_ptr_dtor(&cm_zval);
                RETURN_FALSE;
            }

            int res_w = (int)zval_get_long(width_zval);
            int res_h = (int)zval_get_long(height_zval);
            cm->resolution = res_w;

            if (zend_hash_num_elements(pixels_ht) != 6) {
                php_error_docref(NULL, E_WARNING, "cubemap 'pixels' must have exactly 6 face arrays");
                glDeleteTextures(1, &cm->texture_id);
                zval_ptr_dtor(&cm_zval);
                RETURN_FALSE;
            }

            size_t face_size = (size_t)res_w * (size_t)res_h * 4;
            unsigned char *face_buf = emalloc(face_size);

            int face_idx = 0;
            zval *face_arr;
            ZEND_HASH_FOREACH_VAL(pixels_ht, face_arr) {
                if (Z_TYPE_P(face_arr) != IS_ARRAY) {
                    php_error_docref(NULL, E_WARNING, "cubemap pixel face %d must be an array", face_idx);
                    efree(face_buf);
                    glDeleteTextures(1, &cm->texture_id);
                    zval_ptr_dtor(&cm_zval);
                    RETURN_FALSE;
                }

                HashTable *face_data_ht = Z_ARRVAL_P(face_arr);
                size_t j = 0;
                zval *pixel_val;
                ZEND_HASH_FOREACH_VAL(face_data_ht, pixel_val) {
                    if (j >= face_size) break;
                    face_buf[j++] = (unsigned char)zval_get_long(pixel_val);
                } ZEND_HASH_FOREACH_END();

                /* Zero-fill if data is short */
                while (j < face_size) {
                    face_buf[j++] = 0;
                }

                glTexImage2D(GL_TEXTURE_CUBE_MAP_POSITIVE_X + face_idx, 0, GL_RGBA,
                    res_w, res_h, 0, GL_RGBA, GL_UNSIGNED_BYTE, face_buf);
                face_idx++;
            } ZEND_HASH_FOREACH_END();

            efree(face_buf);

        } else {
            php_error_docref(NULL, E_WARNING, "vio_cubemap requires 'faces' or 'pixels' array");
            glDeleteTextures(1, &cm->texture_id);
            zval_ptr_dtor(&cm_zval);
            RETURN_FALSE;
        }

        /* Cubemap filtering */
        glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_WRAP_R, GL_CLAMP_TO_EDGE);

        glBindTexture(GL_TEXTURE_CUBE_MAP, 0);
    }
#endif

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

#ifdef HAVE_GLFW
    if (strcmp(ctx->backend->name, "opengl") == 0 && vio_gl.initialized) {
        glActiveTexture(GL_TEXTURE0 + (GLenum)slot);
        glBindTexture(GL_TEXTURE_CUBE_MAP, cm->texture_id);
    }
#endif

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

/* ── Module lifecycle ─────────────────────────────────────────────── */

PHP_MINIT_FUNCTION(vio)
{
    REGISTER_INI_ENTRIES();
    le_vio_async_load = zend_register_list_destructors_ex(vio_async_load_dtor, NULL, "vio_async_load", module_number);
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

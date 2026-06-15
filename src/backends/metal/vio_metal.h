/*
 * php-vio - Metal Backend (macOS)
 */

#ifndef VIO_METAL_H
#define VIO_METAL_H

#include "../../../include/vio_backend.h"
#include "../../vio_2d.h"

void vio_backend_metal_register(void);

/*
 * Setup Metal context against an externally-provided CAMetalLayer.
 *
 * The caller owns the layer's parent view (NSView on macOS, UIView on iOS).
 * This is the native entry-point used by all platforms. The Metal backend
 * never touches AppKit, UIKit or GLFW directly - it only sees the layer
 * and a starting framebuffer size.
 *
 *   cf_metal_layer  - CAMetalLayer* (passed as void to keep headers ObjC-free).
 *                     Retained by the backend for the lifetime of the context.
 *   width, height   - initial framebuffer size in pixels.
 *   cfg             - config (only vsync is consumed today).
 *
 * Returns 0 on success, -1 on failure.
 */
int vio_metal_setup_context_native(void *cf_metal_layer, int width, int height,
                                   vio_config *cfg);

#ifdef HAVE_GLFW
/*
 * GLFW convenience wrapper (macOS-only). Extracts NSWindow -> contentView,
 * attaches a freshly-created CAMetalLayer, then delegates to
 * vio_metal_setup_context_native(). Pulls framebuffer size via GLFW and
 * registers GLFW for pull-based resize polling in metal_begin_frame.
 *
 * Returns 0 on success, -1 on failure.
 */
int vio_metal_setup_context(void *glfw_window, vio_config *cfg);
#endif

/*
 * Push-based resize notification used by platforms without a polling-friendly
 * windowing API (iOS UIView, AppKit-via-callback). The GLFW wrapper does not
 * call this - it polls framebuffer size each frame. Safe to call between
 * frames; not safe to call while an encoder is open.
 */
void vio_metal_handle_resize(int width, int height);

/*
 * Shutdown Metal context and release all resources.
 */
void vio_metal_shutdown_context(void);

/* ── Metal 2D rendering pipeline ─────────────────────────────────── */

/*
 * Initialize Metal 2D pipeline: compile MSL shaders, create pipeline states.
 * Called after vio_metal_setup_context().
 */
int  vio_metal_2d_init(int width, int height);

/*
 * Returns 1 if the Metal 2D pipeline is active and ready to render.
 */
int  vio_metal_2d_is_active(void);

/*
 * Flush batched 2D items using Metal. Called from vio_2d_flush() when
 * the Metal backend is active. Items must already be sorted.
 */
void vio_metal_2d_flush(vio_2d_state *state);

/*
 * Update Metal 2D projection and viewport when window resizes.
 */
void vio_metal_2d_set_size(int width, int height, int fb_width, int fb_height);

/* ── Metal texture management ────────────────────────────────────── */

/*
 * Create an RGBA texture. Returns a texture ID (>0) or 0 on failure.
 * The ID can be used as texture_id in 2D batch items.
 */
unsigned int vio_metal_create_texture_rgba(int width, int height,
    const unsigned char *pixels, int filter_linear, int wrap_clamp);

/*
 * Create a 3D / volume RGBA8 texture (Fieldtracing SDF). pixels is
 * width*height*depth*4 bytes, Z-slices ascending. Returns texture ID or 0.
 * NOTE: the Metal 3D draw pipeline is still stubbed, so this texture cannot be
 * sampled by a mesh shader yet — it is wired for forward-compatibility so the
 * vio 3D-texture API is uniform across backends.
 */
unsigned int vio_metal_create_texture_3d_rgba(int width, int height, int depth,
    const unsigned char *pixels, int filter_linear, int wrap_clamp);

/*
 * Create a single-channel font atlas texture with swizzle (R -> alpha).
 * Returns texture ID or 0 on failure.
 */
unsigned int vio_metal_create_font_atlas(int width, int height,
    const unsigned char *bitmap);

/*
 * Delete a Metal texture by ID.
 */
void vio_metal_delete_texture(unsigned int texture_id);

/*
 * Register an externally-owned MTLTexture (passed as a CFBridgingRetain'd
 * opaque pointer matching the metal_color_texture / metal_depth_texture
 * slots on vio_render_target_object) into the texture registry so the 2D
 * batch renderer can bind it via item->texture_id. The texture remains
 * owned by the caller; the registry entry is cleared on
 * vio_metal_delete_texture. Returns 0 on failure.
 */
unsigned int vio_metal_register_external_texture(void *cf_retained_texture);

/* ── Metal pixel readback ────────────────────────────────────────── */

/*
 * Read the current framebuffer into an RGBA pixel buffer.
 * Caller must provide a buffer of size width*height*4.
 * Returns 0 on success, -1 on failure.
 */
int vio_metal_read_pixels(int width, int height, unsigned char *out_rgba);

/*
 * Save the current framebuffer as a PNG file.
 * Returns 0 on success, -1 on failure.
 */
int vio_metal_save_screenshot(const char *path, int width, int height);

#endif /* VIO_METAL_H */

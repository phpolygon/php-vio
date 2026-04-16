/*
 * php-vio - Metal Backend (macOS)
 */

#ifndef VIO_METAL_H
#define VIO_METAL_H

#include "../../../include/vio_backend.h"
#include "../../vio_2d.h"

void vio_backend_metal_register(void);

/*
 * Setup Metal context: create device, command queue, CAMetalLayer on GLFW window.
 * Called from vio_create() after window creation.
 * Returns 0 on success, -1 on failure.
 */
int vio_metal_setup_context(void *glfw_window, vio_config *cfg);

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
 * Create a single-channel font atlas texture with swizzle (R -> alpha).
 * Returns texture ID or 0 on failure.
 */
unsigned int vio_metal_create_font_atlas(int width, int height,
    const unsigned char *bitmap);

/*
 * Delete a Metal texture by ID.
 */
void vio_metal_delete_texture(unsigned int texture_id);

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

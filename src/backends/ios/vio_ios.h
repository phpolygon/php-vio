/*
 * php-vio - iOS / iPadOS backend
 *
 * The iOS backend is the "Window + Input" half of what GLFW provides on
 * desktop. Rendering is unchanged - it goes through the Metal backend's
 * GLFW-agnostic native entry point (vio_metal_setup_context_native).
 *
 * Threading: all UIKit / CAMetalLayer interaction must happen on the
 * main thread. This matches how a typical Xcode wrapper boots PHP -
 * AppDelegate startup, then PHP runs inline on the main thread driven
 * by a CADisplayLink callback.
 */

#ifndef VIO_IOS_H
#define VIO_IOS_H

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#ifdef HAVE_IOS

#include "../../../include/vio_backend.h"
#include "../../../include/vio_types.h"

/*
 * Setup the iOS context:
 *   1. Find the foreground UIWindow created by the hosting Xcode wrapper.
 *   2. Create a VioRenderView (UIView with CAMetalLayer) sized to the window.
 *   3. Hand the CAMetalLayer to vio_metal_setup_context_native().
 *   4. Wire UITouch -> vio_input_touch_* via the provided input state.
 *
 * The hosting Xcode wrapper owns the UIApplication / UIWindow lifecycle.
 * This function expects a key window to already exist - call it from a
 * point in startup where UIScene activation has happened (e.g. after
 * viewDidAppear, or from a CADisplayLink callback).
 *
 *   width, height - requested logical size from the game config. iOS
 *                   ignores them and uses the actual screen bounds * scale;
 *                   they are accepted only for API parity with the GLFW
 *                   setup path.
 *   cfg           - vio_config (only vsync is consumed today).
 *   input_state   - vio_input_state* the touch dispatcher should feed.
 *                   Passed as void to keep this header free of vio_input.h.
 *
 * Returns 0 on success, -1 on failure.
 */
int vio_ios_setup_context(int width, int height, vio_config *cfg,
                          void *input_state);

/*
 * Teardown: remove the render view, drop the CAMetalLayer reference.
 * Metal context shutdown is handled separately by vio_metal_shutdown_context.
 */
void vio_ios_shutdown_context(void);

/*
 * Return the current backbuffer size in pixels. Updated by the
 * VioRenderView's layoutSubviews when the device rotates or the user
 * enters / leaves split-screen / stage manager. Useful from PHP if the
 * game needs to react to size changes outside the resize callback.
 */
void vio_ios_get_framebuffer_size(int *out_w, int *out_h);

/*
 * Content scale (physical pixels per logical point), e.g. 3.0 on an
 * iPhone Pro. Used to derive the logical window size (framebuffer / scale)
 * that the game lays out against. Render-thread safe (reads a cached value).
 */
float vio_ios_get_content_scale(void);

/*
 * Show / hide the on-screen keyboard by toggling the render view's
 * first-responder status. Call when a game text field gains / loses focus.
 * Hops to the UIKit main thread internally; safe to call from the render
 * thread. Typed text + backspaces arrive via vio_input_push_text /
 * vio_input_ime_backspace.
 */
void vio_ios_keyboard_show(void);
void vio_ios_keyboard_hide(void);

#endif /* HAVE_IOS */

#endif /* VIO_IOS_H */

/*
 * php-vio - Null platform: no window system (iOS, builds without GLFW, tests)
 *
 * Every slot answers like "there is no window": no windows are created, sizes
 * stay as asked, no monitors and no joysticks. The GPU backends then render
 * offscreen (headless) as they did behind the old #ifdef HAVE_GLFW guards.
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <string.h>
#include "../../../include/vio_platform.h"

static int  null_init(void) { return 1; }
static void null_shutdown(void) {}

static vio_window_handle null_create_window(vio_config *cfg, const char *backend_name)
{
    (void)cfg; (void)backend_name;
    return NULL;
}

static void null_destroy_window(vio_window_handle w) { (void)w; }
static int  null_should_close(vio_window_handle w) { (void)w; return 0; }
static void null_set_should_close(vio_window_handle w, int v) { (void)w; (void)v; }
static void null_poll_events(void) {}
static void null_swap_buffers(vio_window_handle w) { (void)w; }
static void null_size(vio_window_handle w, int *a, int *b) { (void)w; if (a) *a = 0; if (b) *b = 0; }
static void null_content_scale(vio_window_handle w, float *sx, float *sy) { (void)w; if (sx) *sx = 1.0f; if (sy) *sy = 1.0f; }
static void null_cursor_pos(vio_window_handle w, double *x, double *y) { (void)w; if (x) *x = 0.0; if (y) *y = 0.0; }
static void null_set_title(vio_window_handle w, const char *t) { (void)w; (void)t; }
static void null_set_size(vio_window_handle w, int a, int b) { (void)w; (void)a; (void)b; }
static int  null_get_attrib(vio_window_handle w, vio_window_attrib a) { (void)w; return a == VIO_WINDOW_AUTO_ICONIFY || a == VIO_WINDOW_DECORATED; }
static void null_set_attrib(vio_window_handle w, vio_window_attrib a, int v) { (void)w; (void)a; (void)v; }
static void null_window_op(vio_window_handle w) { (void)w; }
static void null_set_cursor_mode(vio_window_handle w, int mode) { (void)w; (void)mode; }
static int  null_window_monitor(vio_window_handle w) { (void)w; return -1; }
static void null_set_window_monitor(vio_window_handle w, int m, int x, int y, int cw, int ch, int r)
{
    (void)w; (void)m; (void)x; (void)y; (void)cw; (void)ch; (void)r;
}
static int  null_monitor_count(void) { return 0; }
static int  null_primary_monitor(void) { return -1; }
static int  null_monitor_desc(int i, vio_monitor_desc *out) { (void)i; (void)out; return -1; }
static int  null_video_modes(int i, vio_video_mode *out, int max) { (void)i; (void)out; (void)max; return 0; }
static int  null_joystick(int id) { (void)id; return 0; }
static const char *null_name(int id) { (void)id; return NULL; }
static int  null_gamepad_state(int id, unsigned char b[15], float a[6]) { (void)id; (void)b; (void)a; return 0; }
static const unsigned char *null_buttons(int id, int *n) { (void)id; if (n) *n = 0; return NULL; }
static const float *null_axes(int id, int *n) { (void)id; if (n) *n = 0; return NULL; }
static void *null_native(vio_window_handle w, vio_native_kind k) { (void)w; (void)k; return NULL; }
static void null_install_input(vio_window_handle w, struct _vio_input_state *s) { (void)w; (void)s; }

static const vio_platform vio_platform_null = {
    .name                 = "null",
    .init                 = null_init,
    .shutdown             = null_shutdown,
    .create_window        = null_create_window,
    .destroy_window       = null_destroy_window,
    .should_close         = null_should_close,
    .set_should_close     = null_set_should_close,
    .poll_events          = null_poll_events,
    .wait_events          = null_poll_events,
    .swap_buffers         = null_swap_buffers,
    .get_framebuffer_size = null_size,
    .get_window_size      = null_size,
    .get_content_scale    = null_content_scale,
    .get_cursor_pos       = null_cursor_pos,
    .set_title            = null_set_title,
    .set_window_size      = null_set_size,
    .get_window_pos       = null_size,
    .set_window_pos       = null_set_size,
    .get_attrib           = null_get_attrib,
    .set_attrib           = null_set_attrib,
    .maximize             = null_window_op,
    .restore              = null_window_op,
    .set_cursor_mode      = null_set_cursor_mode,
    .window_monitor       = null_window_monitor,
    .set_window_monitor   = null_set_window_monitor,
    .monitor_count        = null_monitor_count,
    .primary_monitor      = null_primary_monitor,
    .monitor_desc         = null_monitor_desc,
    .video_modes          = null_video_modes,
    .joystick_present     = null_joystick,
    .joystick_is_gamepad  = null_joystick,
    .joystick_name        = null_name,
    .gamepad_name         = null_name,
    .gamepad_state        = null_gamepad_state,
    .joystick_buttons     = null_buttons,
    .joystick_axes        = null_axes,
    .native_handle        = null_native,
    .install_input        = null_install_input,
};

void vio_platform_null_register(void)
{
    vio_register_platform(&vio_platform_null);
}

--TEST--
vio_read_pixels / vio_save_screenshot capture the whole framebuffer after it grew past the creation size
--EXTENSIONS--
vio
--SKIPIF--
<?php
if (!extension_loaded('vio')) die('skip vio not loaded');
?>
--ENV--
VIO_FORCE_CONTENT_SCALE=1.25
--FILE--
<?php
/* Both readback functions took their size from the context's configured size -
 * the size the window was created with, updated by vio_set_window_size in
 * LOGICAL units. The pixels they read come from the framebuffer, which is
 * physical. The two part as soon as they differ, and they differ all the time
 * in a real game: a maximised or fullscreen window, a window dragged bigger, or
 * any desktop scaled above 100 %.
 *
 * Found through a game's bug-report screenshot. After maximising, only D3D12
 * captured the window; D3D11 wrote a 640x360 PNG out of a 3840x1032 frame, and
 * OpenGL and Vulkan read a 640x360 corner. On a Linux desktop at 125 % that
 * corner is what every screenshot would have been from the first frame on.
 *
 * The scale is forced so the mismatch is reproducible without a monitor or a
 * window manager: a 640x360 logical window at 1.25 is an 800x450 framebuffer,
 * while the configured size stays 640x360. The shrinking case is test 131. */
require __DIR__ . '/readback_framebuffer.inc';

foreach (['opengl', 'd3d11', 'd3d12', 'vulkan', 'metal'] as $b) {
    echo "$b: ", readback_follows_framebuffer($b, 640, 360), "\n";
}
echo "DONE\n";
?>
--EXPECTREGEX--
opengl: (ok|unavailable)
d3d11: (ok|unavailable)
d3d12: (ok|unavailable)
vulkan: (ok|unavailable)
metal: (ok|unavailable)
DONE

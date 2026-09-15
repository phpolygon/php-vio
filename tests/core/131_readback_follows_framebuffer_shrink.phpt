--TEST--
vio_read_pixels / vio_save_screenshot stay inside the framebuffer after it shrank below the creation size
--EXTENSIONS--
vio
--SKIPIF--
<?php
if (!extension_loaded('vio')) die('skip vio not loaded');
?>
--ENV--
VIO_FORCE_CONTENT_SCALE=0.5
--FILE--
<?php
/* The other direction of test 130, and the dangerous one. With the configured
 * size larger than the framebuffer, a PNG writer told "640x360" walks off the
 * end of a 320x180 pixel buffer - D3D11's vio_save_screenshot did exactly that,
 * passing the configured size alongside the smaller readback. A window made
 * smaller than it started must still produce a correct, complete image.
 *
 * A logical 640x360 window at a forced 0.5 is a 320x180 framebuffer. */
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

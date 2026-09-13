--TEST--
vio_set_window_size round-trips through vio_window_size on a scaled monitor
--EXTENSIONS--
vio
--SKIPIF--
<?php
require __DIR__ . '/../skipif_gl.inc';
?>
--ENV--
VIO_FORCE_CONTENT_SCALE=1.5
--FILE--
<?php
/* vio_window_size reports framebuffer/contentScale, and vio_create takes that
 * same logical size. vio_set_window_size used to forward its arguments straight
 * to glfwSetWindowSize, which speaks screen coordinates - physical pixels on
 * Windows and X11. The two spaces coincide at scale 1.0, which is why this
 * survived every developer machine and CI runner, but at 1.5x a request for
 * 1920x1080 read back as 1280x720: a resolution picker sets a size, sees a
 * smaller one, and looks like it jumped back to the previous entry.
 *
 * The scale is forced here rather than taken from the monitor, so the scaled
 * path is exercised on an unscaled machine. */
$ctx = vio_create('opengl', ['width' => 640, 'height' => 480, 'title' => 'round-trip']);
if (!$ctx) { echo "SKIP\n"; exit; }

/* Let the window manager settle the initial map before measuring. */
for ($i = 0; $i < 20; $i++) { vio_poll_events($ctx); }

var_dump(vio_content_scale($ctx) === [1.5, 1.5]);

/* The invariant: what you set is what you read back. */
foreach ([[800, 600], [1024, 576], [640, 480]] as [$w, $h]) {
    vio_set_window_size($ctx, $w, $h);
    for ($i = 0; $i < 20; $i++) { vio_poll_events($ctx); }
    var_dump(vio_window_size($ctx) === [$w, $h]);
}

/* The logical size stays consistent with the physical surface, so a caller that
 * sizes a render target from the framebuffer and lays out from the window size
 * does not see the two disagree about the scale factor. */
$scale = vio_content_scale($ctx);
$fb = vio_framebuffer_size($ctx);
$win = vio_window_size($ctx);
var_dump(abs($fb[0] - (int) round($win[0] * $scale[0])) <= 2);
var_dump(abs($fb[1] - (int) round($win[1] * $scale[1])) <= 2);

vio_destroy($ctx);
echo "OK\n";
?>
--EXPECT--
bool(true)
bool(true)
bool(true)
bool(true)
bool(true)
bool(true)
OK

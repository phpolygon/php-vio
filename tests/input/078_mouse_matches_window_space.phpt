--TEST--
vio_mouse_position reports the cursor in the space vio_window_size describes
--EXTENSIONS--
vio
--SKIPIF--
<?php
require __DIR__ . '/../skipif_gl.inc';
?>
--ENV--
VIO_FORCE_CONTENT_SCALE=1.25
--FILE--
<?php
/* A caller lays its UI out against vio_window_size and hit-tests it with
 * vio_mouse_position. The two therefore have to describe the SAME space, and
 * they did not.
 *
 * vio_window_size returns the logical size - framebuffer divided by content
 * scale - on every platform. vio_mouse_position divided by that scale only
 * under _WIN32, on the reasoning that GLFW elsewhere reports the cursor in the
 * same units as the window. That is true of glfwGetWindowSize, but
 * vio_window_size is not glfwGetWindowSize: on an X11 desktop at 125 % the
 * window is 1600x900 physical pixels with a content scale of 1.25, so
 * vio_window_size says 1280x720 while the cursor still arrives in 1600x900.
 * Every click then lands 1.25x too far right and down - a selection that
 * drifts further from the pointer the closer you get to the edge. Reported from
 * a Linux/Steam build: "game selection is off set of the mouse cursor".
 *
 * The invariant: a cursor at the far corner of the drawable surface must read
 * as the far corner of the window that vio_window_size reports.
 *
 * The scale is forced here rather than taken from the monitor, so the scaled
 * path runs on an unscaled machine. On Windows this test passed before the fix
 * as well - there the two spaces already agreed. It fails on a platform that
 * skipped the division, which is the point. */
$ctx = vio_create('opengl', ['width' => 640, 'height' => 480, 'title' => 'mouse space']);
if (!$ctx) { echo "SKIP\n"; exit; }

for ($i = 0; $i < 20; $i++) { vio_poll_events($ctx); }

[$winW, $winH] = vio_window_size($ctx);
[$fbW, $fbH]   = vio_framebuffer_size($ctx);

var_dump(vio_content_scale($ctx) === [1.25, 1.25]);

/* Injection writes the raw cursor, exactly where the GLFW callback puts it, so
 * this exercises the same conversion a real pointer goes through. */
vio_inject_mouse_move($ctx, (float) $fbW, (float) $fbH);
[$mx, $my] = vio_mouse_position($ctx);

/* Rounded: the conversion is a float divide and the corner is what matters. */
var_dump((int) round($mx) === $winW);
var_dump((int) round($my) === $winH);

/* The origin has to survive too - a scale bug that only shifted the corner
 * would still be a scale bug, but one that moved 0,0 would be an offset bug. */
vio_inject_mouse_move($ctx, 0.0, 0.0);
var_dump(vio_mouse_position($ctx) === [0.0, 0.0]);

/* And the middle, so the mapping is linear rather than merely endpoint-correct. */
vio_inject_mouse_move($ctx, $fbW / 2.0, $fbH / 2.0);
[$mx, $my] = vio_mouse_position($ctx);
var_dump((int) round($mx) === (int) round($winW / 2));
var_dump((int) round($my) === (int) round($winH / 2));

vio_destroy($ctx);
?>
--EXPECT--
bool(true)
bool(true)
bool(true)
bool(true)
bool(true)
bool(true)

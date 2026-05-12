--TEST--
Display metrics: vio_framebuffer_size, vio_content_scale, vio_pixel_ratio
--EXTENSIONS--
vio
--FILE--
<?php
$ctx = vio_create("opengl", ["width" => 200, "height" => 100, "headless" => true]);
if (!$ctx) { echo "SKIP\n"; exit; }

// framebuffer_size: physical pixel dimensions of the drawing surface.
// On a headless context this equals the requested logical size; on a
// HiDPI display it is requested-size × content-scale. Either way it
// must return a 2-element array of positive ints.
$fb = vio_framebuffer_size($ctx);
var_dump(is_array($fb));
var_dump(count($fb) === 2);
var_dump(is_int($fb[0]) && $fb[0] > 0);
var_dump(is_int($fb[1]) && $fb[1] > 0);

// content_scale: per-axis DPI factor. Two floats, both > 0.
// Equals 1.0 on a standard display, 2.0 on retina, 1.5 on 150% Windows,
// 1.875 on a 1.875× monitor. Headless contexts can report 1.0 or the
// physical monitor's value depending on backend — we only assert positivity.
$cs = vio_content_scale($ctx);
var_dump(is_array($cs));
var_dump(count($cs) === 2);
var_dump(is_float($cs[0]) && $cs[0] > 0.0);
var_dump(is_float($cs[1]) && $cs[1] > 0.0);

// pixel_ratio: scalar variant of content_scale. Must equal one of the
// content_scale axes (most backends return the X axis).
$pr = vio_pixel_ratio($ctx);
var_dump(is_float($pr) && $pr > 0.0);

// Invariant: framebuffer_size ≈ requested × content_scale, modulo rounding.
// We give it a generous tolerance because headless backends and OS
// quirks can off-by-one. The contract is "physical pixels".
$expected_w = (int)round(200 * $cs[0]);
$expected_h = (int)round(100 * $cs[1]);
var_dump(abs($fb[0] - $expected_w) <= 2);
var_dump(abs($fb[1] - $expected_h) <= 2);

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
bool(true)
bool(true)
bool(true)
bool(true)
bool(true)
OK

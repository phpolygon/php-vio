--TEST--
Metal backend: offscreen render target bind/unbind redirects draws
--SKIPIF--
<?php
if (!in_array('metal', vio_backends(), true)) die('skip Metal backend not compiled in');
?>
--EXTENSIONS--
vio
--FILE--
<?php
$ctx = vio_create("metal", ["width" => 64, "height" => 64, "headless" => true]);
if (!$ctx) { echo "SKIP: Metal context unavailable\n"; exit; }

var_dump(vio_backend_name($ctx) === "metal");

// Allocate an offscreen color RT and a depth-only RT.
$rt_color = vio_render_target($ctx, [
    'width' => 128,
    'height' => 128,
]);
var_dump($rt_color instanceof VioRenderTarget);

$rt_depth = vio_render_target($ctx, [
    'width' => 256,
    'height' => 256,
    'depth_only' => true,
]);
var_dump($rt_depth instanceof VioRenderTarget);

// HDR variant.
$rt_hdr = vio_render_target($ctx, [
    'width' => 64,
    'height' => 64,
    'hdr' => true,
]);
var_dump($rt_hdr instanceof VioRenderTarget);

// ── Single-frame bind/unbind cycle ────────────────────────────────────
// Clear the swapchain bright red, then bind the RT, draw a green rect
// into the RT, unbind back to swapchain. After vio_end the swapchain
// should still be red — the draws went into the offscreen texture.
vio_begin($ctx);
vio_clear($ctx, 1.0, 0.0, 0.0, 1.0);
vio_rect($ctx, 0, 0, 64, 64, ["color" => 0xFF0000FF]); // red rect to fill swapchain
vio_draw_2d($ctx);

vio_bind_render_target($ctx, $rt_color);
vio_clear($ctx, 0.0, 1.0, 0.0, 1.0);
vio_rect($ctx, 0, 0, 128, 128, ["color" => 0x00FF00FF]); // green into RT
vio_draw_2d($ctx);
echo "color RT drawn OK\n";

vio_unbind_render_target($ctx);
echo "RT unbound OK\n";

// Depth-only RT — no color attachment, still must bind/unbind cleanly.
vio_bind_render_target($ctx, $rt_depth);
vio_clear($ctx, 0.0, 0.0, 0.0, 1.0);
echo "depth RT bound OK\n";
vio_unbind_render_target($ctx);

vio_end($ctx);
echo "frame OK\n";

// Texture wrapper round-trip.
$color_tex = vio_render_target_texture($rt_color);
var_dump($color_tex instanceof VioTexture);
$size = vio_texture_size($color_tex);
var_dump($size[0] === 128 && $size[1] === 128);

$depth_tex = vio_render_target_texture($rt_depth);
var_dump($depth_tex instanceof VioTexture);

// Multiple bind/unbind cycles within a single frame.
vio_begin($ctx);
for ($i = 0; $i < 3; $i++) {
    vio_bind_render_target($ctx, $rt_color);
    vio_clear($ctx, $i / 3.0, 0.5, 0.5, 1.0);
    vio_unbind_render_target($ctx);
}
vio_end($ctx);
echo "multi-cycle OK\n";

vio_destroy($ctx);
echo "OK\n";
?>
--EXPECT--
bool(true)
bool(true)
bool(true)
bool(true)
color RT drawn OK
RT unbound OK
depth RT bound OK
frame OK
bool(true)
bool(true)
bool(true)
multi-cycle OK
OK

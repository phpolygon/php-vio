--TEST--
vio_render_target creates FBO and allows offscreen rendering
--EXTENSIONS--
vio
--FILE--
<?php
$ctx = vio_create("opengl", ["width" => 64, "height" => 64, "headless" => true]);
if (!$ctx) { echo "SKIP\n"; exit; }

// Depth-only render target (shadow map use case)
$rt_depth = vio_render_target($ctx, [
    'width' => 256,
    'height' => 256,
    'depth_only' => true,
]);
var_dump($rt_depth instanceof VioRenderTarget);

// Color + depth render target
$rt_color = vio_render_target($ctx, [
    'width' => 128,
    'height' => 128,
]);
var_dump($rt_color instanceof VioRenderTarget);

// Bind, render, unbind
vio_begin($ctx);

vio_bind_render_target($ctx, $rt_depth);
vio_clear($ctx, 0.0, 0.0, 0.0, 1.0);
echo "depth RT bound OK\n";
vio_unbind_render_target($ctx);
echo "RT unbound OK\n";

vio_bind_render_target($ctx, $rt_color);
vio_clear($ctx, 1.0, 0.0, 0.0, 1.0);
echo "color RT bound OK\n";
vio_unbind_render_target($ctx);

vio_end($ctx);

// Get textures from render targets
$depth_tex = vio_render_target_texture($rt_depth);
var_dump($depth_tex instanceof VioTexture);
$size = vio_texture_size($depth_tex);
var_dump($size[0] === 256 && $size[1] === 256);

$color_tex = vio_render_target_texture($rt_color);
var_dump($color_tex instanceof VioTexture);
$size = vio_texture_size($color_tex);
var_dump($size[0] === 128 && $size[1] === 128);

vio_destroy($ctx);
echo "OK\n";
?>
--EXPECT--
bool(true)
bool(true)
depth RT bound OK
RT unbound OK
color RT bound OK
bool(true)
bool(true)
bool(true)
bool(true)
OK

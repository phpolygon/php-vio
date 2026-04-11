--TEST--
vio_sprite 2D sprite rendering with null backend
--EXTENSIONS--
vio
--FILE--
<?php
$ctx = vio_create('null');

// Create a small texture
$pixels = str_repeat("\xff\x00\x00\xff", 4);
$tex = vio_texture($ctx, ['data' => $pixels, 'width' => 2, 'height' => 2]);

vio_begin($ctx);

// Basic sprite
vio_sprite($ctx, $tex, ['x' => 100, 'y' => 100]);
echo "sprite ok\n";

// Sprite with scale and tint
vio_sprite($ctx, $tex, [
    'x' => 200, 'y' => 200,
    'scale_x' => 2.0, 'scale_y' => 2.0,
    'color' => 0x80FFFFFF,
    'z' => 1.0,
]);
echo "sprite scaled ok\n";

vio_draw_2d($ctx);
echo "draw_2d ok\n";

vio_end($ctx);
vio_destroy($ctx);
echo "OK\n";
?>
--EXPECT--
sprite ok
sprite scaled ok
draw_2d ok
OK

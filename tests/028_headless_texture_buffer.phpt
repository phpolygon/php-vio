--TEST--
Headless texture and uniform buffer with real OpenGL context
--EXTENSIONS--
vio
--FILE--
<?php
$ctx = vio_create("opengl", ["width" => 32, "height" => 32, "headless" => true]);
if (!$ctx) { echo "SKIP\n"; exit; }

// Create 2x2 texture from raw RGBA data
$data = str_repeat("\xff\x00\x00\xff", 4); // 2x2 red pixels
$tex = vio_texture($ctx, [
    'data' => $data,
    'width' => 2,
    'height' => 2,
    'filter' => VIO_FILTER_NEAREST,
    'wrap' => VIO_WRAP_CLAMP,
]);
var_dump($tex instanceof VioTexture);

// Bind texture in frame
vio_begin($ctx);
vio_bind_texture($ctx, $tex, 0);
echo "texture bound OK\n";
vio_end($ctx);

// Create uniform buffer
$buf = vio_uniform_buffer($ctx, ['size' => 64, 'binding' => 0]);
var_dump($buf instanceof VioBuffer);

// Update buffer with raw float binary data (4 floats = 16 bytes)
$float_data = pack('f4', 1.0, 0.0, 0.0, 1.0);
vio_update_buffer($buf, $float_data);
echo "buffer updated OK\n";

// Update buffer with offset
$float_data2 = pack('f2', 0.5, 0.5);
vio_update_buffer($buf, $float_data2, 16);
echo "buffer updated with offset OK\n";

// Bind buffer in frame
vio_begin($ctx);
vio_bind_buffer($ctx, $buf);
echo "buffer bound OK\n";
vio_end($ctx);

vio_destroy($ctx);
echo "OK\n";
?>
--EXPECT--
bool(true)
texture bound OK
bool(true)
buffer updated OK
buffer updated with offset OK
buffer bound OK
OK

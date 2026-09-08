--TEST--
vio_texture creation from raw data and error handling
--EXTENSIONS--
vio
--FILE--
<?php
$ctx = vio_create('null');

// Create texture from raw RGBA data (2x2 red pixels)
$pixels = str_repeat("\xff\x00\x00\xff", 4);
$tex = vio_texture($ctx, [
    'data'   => $pixels,
    'width'  => 2,
    'height' => 2,
    'filter' => VIO_FILTER_NEAREST,
    'wrap'   => VIO_WRAP_CLAMP,
]);
var_dump($tex instanceof VioTexture);

// Bind texture in frame
vio_begin($ctx);
vio_bind_texture($ctx, $tex, 0);
echo "bind_texture ok\n";
vio_end($ctx);

// Error: missing required fields
$bad = vio_texture($ctx, []);
var_dump($bad);

vio_destroy($ctx);
echo "OK\n";
?>
--EXPECTF--
bool(true)
bind_texture ok

Warning: vio_texture(): vio_texture requires 'file' or 'data'+'width'+'height' in %s on line %d
bool(false)
OK

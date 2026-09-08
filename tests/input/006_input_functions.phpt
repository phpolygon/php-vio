--TEST--
vio_key_pressed / vio_key_just_pressed / vio_key_released with null backend
--EXTENSIONS--
vio
--FILE--
<?php
$ctx = vio_create('null');

// All keys should be unpressed initially
var_dump(vio_key_pressed($ctx, VIO_KEY_W));
var_dump(vio_key_just_pressed($ctx, VIO_KEY_SPACE));
var_dump(vio_key_released($ctx, VIO_KEY_ESCAPE));

// After a frame cycle, still unpressed
vio_begin($ctx);
vio_end($ctx);
var_dump(vio_key_pressed($ctx, VIO_KEY_A));

vio_destroy($ctx);
echo "OK\n";
?>
--EXPECT--
bool(false)
bool(false)
bool(false)
bool(false)
OK

--TEST--
vio_mouse_position / vio_mouse_delta / vio_mouse_button / vio_mouse_scroll with null backend
--EXTENSIONS--
vio
--FILE--
<?php
$ctx = vio_create('null');

// Mouse position defaults to origin
$pos = vio_mouse_position($ctx);
var_dump($pos === [0.0, 0.0]);

// Mouse delta should be zero
$delta = vio_mouse_delta($ctx);
var_dump($delta === [0.0, 0.0]);

// No mouse buttons pressed
var_dump(vio_mouse_button($ctx, VIO_MOUSE_LEFT));
var_dump(vio_mouse_button($ctx, VIO_MOUSE_RIGHT));

// No scroll
$scroll = vio_mouse_scroll($ctx);
var_dump($scroll === [0.0, 0.0]);

vio_destroy($ctx);
echo "OK\n";
?>
--EXPECT--
bool(true)
bool(true)
bool(false)
bool(false)
bool(true)
OK

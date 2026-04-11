--TEST--
Input injection: vio_inject_key, vio_inject_mouse_move, vio_inject_mouse_button
--EXTENSIONS--
vio
--FILE--
<?php
$ctx = vio_create('null');

// Initially no key pressed
var_dump(vio_key_pressed($ctx, VIO_KEY_W));

// Inject key press
vio_inject_key($ctx, VIO_KEY_W, VIO_PRESS);
var_dump(vio_key_pressed($ctx, VIO_KEY_W));

// Inject key release
vio_inject_key($ctx, VIO_KEY_W, VIO_RELEASE);
var_dump(vio_key_pressed($ctx, VIO_KEY_W));

// Mouse move injection
vio_inject_mouse_move($ctx, 123.5, 456.7);
$pos = vio_mouse_position($ctx);
var_dump($pos[0] === 123.5);
var_dump($pos[1] === 456.7);

// Mouse button injection
var_dump(vio_mouse_button($ctx, VIO_MOUSE_LEFT));
vio_inject_mouse_button($ctx, VIO_MOUSE_LEFT, VIO_PRESS);
var_dump(vio_mouse_button($ctx, VIO_MOUSE_LEFT));
vio_inject_mouse_button($ctx, VIO_MOUSE_LEFT, VIO_RELEASE);
var_dump(vio_mouse_button($ctx, VIO_MOUSE_LEFT));

vio_destroy($ctx);
echo "OK\n";
?>
--EXPECT--
bool(false)
bool(true)
bool(false)
bool(true)
bool(true)
bool(false)
bool(true)
bool(false)
OK

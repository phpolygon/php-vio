--TEST--
Headless input injection with GPU context verification
--EXTENSIONS--
vio
--FILE--
<?php
$ctx = vio_create("opengl", ["width" => 32, "height" => 32, "headless" => true]);
if (!$ctx) { echo "SKIP\n"; exit; }

// Key not pressed initially
var_dump(vio_key_pressed($ctx, VIO_KEY_W) === false);

// Inject key press
vio_inject_key($ctx, VIO_KEY_W, VIO_PRESS);
vio_begin($ctx); vio_end($ctx);
var_dump(vio_key_pressed($ctx, VIO_KEY_W) === true);

// Inject key release
vio_inject_key($ctx, VIO_KEY_W, VIO_RELEASE);
vio_begin($ctx); vio_end($ctx);
var_dump(vio_key_pressed($ctx, VIO_KEY_W) === false);

// Mouse injection
vio_inject_mouse_move($ctx, 15.5, 20.3);
$pos = vio_mouse_position($ctx);
var_dump(abs($pos[0] - 15.5) < 0.01);
var_dump(abs($pos[1] - 20.3) < 0.01);

// Mouse button injection
var_dump(vio_mouse_button($ctx, VIO_MOUSE_LEFT) === false);
vio_inject_mouse_button($ctx, VIO_MOUSE_LEFT, VIO_PRESS);
vio_begin($ctx); vio_end($ctx);
var_dump(vio_mouse_button($ctx, VIO_MOUSE_LEFT) === true);

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
OK

--TEST--
Gamepad API functions and constants
--EXTENSIONS--
vio
--FILE--
<?php
// All gamepad functions exist
$funcs = [
    'vio_gamepads', 'vio_gamepad_connected', 'vio_gamepad_name',
    'vio_gamepad_buttons', 'vio_gamepad_axes', 'vio_gamepad_triggers',
];
foreach ($funcs as $fn) {
    var_dump(function_exists($fn));
}

// Gamepad constants are defined
var_dump(defined('VIO_GAMEPAD_A'));
var_dump(defined('VIO_GAMEPAD_B'));
var_dump(defined('VIO_GAMEPAD_X'));
var_dump(defined('VIO_GAMEPAD_Y'));
var_dump(defined('VIO_GAMEPAD_DPAD_UP'));
var_dump(defined('VIO_GAMEPAD_AXIS_LEFT_X'));
var_dump(defined('VIO_GAMEPAD_AXIS_LEFT_TRIGGER'));
var_dump(defined('VIO_GAMEPAD_AXIS_RIGHT_TRIGGER'));

// vio_gamepads returns an array (may be empty if no controllers connected)
$pads = vio_gamepads();
var_dump(is_array($pads));

// vio_gamepad_connected with invalid ID returns false
var_dump(vio_gamepad_connected(99));

// vio_gamepad_buttons/axes/triggers return arrays even with no gamepad
var_dump(is_array(vio_gamepad_buttons(99)));
var_dump(is_array(vio_gamepad_axes(99)));
var_dump(is_array(vio_gamepad_triggers(99)));

// Triggers always have left/right keys
$triggers = vio_gamepad_triggers(99);
var_dump(array_key_exists('left', $triggers));
var_dump(array_key_exists('right', $triggers));

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
bool(true)
bool(true)
bool(true)
bool(true)
bool(false)
bool(true)
bool(true)
bool(true)
bool(true)
bool(true)
OK

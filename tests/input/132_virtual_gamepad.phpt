--TEST--
Virtual gamepads: connect, inject buttons/axes, override the vio_gamepad_* readers
--EXTENSIONS--
vio
--FILE--
<?php
vio_virtual_gamepad_connect(3, 'Bot Pad');
var_dump(in_array(3, vio_gamepads(), true));
var_dump(vio_gamepad_connected(3), vio_gamepad_name(3));

// Default state: released, sticks centred, triggers at -1 (GLFW convention)
var_dump(count(vio_gamepad_buttons(3)), array_sum(vio_gamepad_buttons(3)));
echo implode(',', vio_gamepad_axes(3)), "\n";
echo json_encode(vio_gamepad_triggers(3)), "\n";

vio_inject_gamepad_button(3, VIO_GAMEPAD_A, VIO_PRESS);
vio_inject_gamepad_button(3, VIO_GAMEPAD_DPAD_LEFT, VIO_PRESS);
vio_inject_gamepad_axis(3, VIO_GAMEPAD_AXIS_LEFT_X, 0.5);
vio_inject_gamepad_axis(3, VIO_GAMEPAD_AXIS_LEFT_Y, -3.0);           // clamped
vio_inject_gamepad_axis(3, VIO_GAMEPAD_AXIS_RIGHT_TRIGGER, 1.0);
$b = vio_gamepad_buttons(3);
var_dump($b[VIO_GAMEPAD_A], $b[VIO_GAMEPAD_B], $b[VIO_GAMEPAD_DPAD_LEFT]);
echo implode(',', vio_gamepad_axes(3)), "\n";
echo json_encode(vio_gamepad_triggers(3)), "\n";

vio_inject_gamepad_button(3, VIO_GAMEPAD_A, VIO_RELEASE);
var_dump(vio_gamepad_buttons(3)[VIO_GAMEPAD_A]);

// Reconnecting resets the pad
vio_virtual_gamepad_connect(3);
var_dump(vio_gamepad_name(3), array_sum(vio_gamepad_buttons(3)));

// Errors
foreach ([
    fn() => vio_virtual_gamepad_connect(16),
    fn() => vio_inject_gamepad_button(3, 15, VIO_PRESS),
    fn() => vio_inject_gamepad_button(3, VIO_GAMEPAD_A, 5),
    fn() => vio_inject_gamepad_axis(3, 6, 0.0),
    fn() => vio_inject_gamepad_button(4, VIO_GAMEPAD_A, VIO_PRESS),
    fn() => vio_inject_gamepad_axis(4, VIO_GAMEPAD_AXIS_LEFT_X, 0.0),
] as $call) {
    try {
        $call();
    } catch (Throwable $e) {
        echo get_class($e), ': ', $e->getMessage(), "\n";
    }
}

vio_virtual_gamepad_disconnect(3);
var_dump(in_array(3, vio_gamepads(), true));
var_dump(vio_gamepad_buttons(3) === [] || !vio_gamepad_connected(3));
echo "OK\n";
?>
--EXPECT--
bool(true)
bool(true)
string(7) "Bot Pad"
int(15)
int(0)
0,0,0,0,-1,-1
{"left":-1,"right":-1}
bool(true)
bool(false)
bool(true)
0.5,-1,0,0,-1,1
{"left":-1,"right":1}
bool(false)
string(15) "Virtual Gamepad"
int(0)
ValueError: vio_virtual_gamepad_connect(): Argument #1 ($id) must be between 0 and 15
ValueError: vio_inject_gamepad_button(): Argument #2 ($button) must be a VIO_GAMEPAD_* button constant
ValueError: vio_inject_gamepad_button(): Argument #3 ($action) must be VIO_RELEASE, VIO_PRESS or VIO_REPEAT
ValueError: vio_inject_gamepad_axis(): Argument #2 ($axis) must be a VIO_GAMEPAD_AXIS_* constant
Error: No virtual gamepad connected in slot 4
Error: No virtual gamepad connected in slot 4
bool(false)
bool(true)
OK

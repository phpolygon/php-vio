--TEST--
Input injection takes the OS event path: on_key/on_char callbacks, scroll, text, edges
--EXTENSIONS--
vio
--FILE--
<?php
$ctx = vio_create('null');

// Keys: state + on_key callback with action and mods, like a GLFW event
$events = [];
vio_on_key($ctx, function (int $key, int $action, int $mods) use (&$events) {
    $events[] = "$key/$action/$mods";
});
vio_inject_key($ctx, VIO_KEY_A, VIO_PRESS, VIO_MOD_SHIFT);
vio_inject_key($ctx, VIO_KEY_A, VIO_REPEAT);
var_dump(vio_key_pressed($ctx, VIO_KEY_A));
vio_inject_key($ctx, VIO_KEY_A, VIO_RELEASE);
var_dump(vio_key_pressed($ctx, VIO_KEY_A));
echo implode(' ', $events), "\n";

// Unknown key: dropped, no callback
$events = [];
vio_inject_key($ctx, -1, VIO_PRESS);
var_dump($events === []);

// Invalid action throws and changes nothing
try {
    vio_inject_key($ctx, VIO_KEY_B, 7);
} catch (ValueError $e) {
    echo $e->getMessage(), "\n";
}
var_dump(vio_key_pressed($ctx, VIO_KEY_B));
try {
    vio_inject_mouse_button($ctx, VIO_MOUSE_LEFT, -1);
} catch (ValueError $e) {
    echo $e->getMessage(), "\n";
}

// Edge timing: injected between frames (where vio_poll_events runs) the game
// sees exactly one just_pressed edge
vio_begin($ctx); vio_end($ctx);
vio_inject_key($ctx, VIO_KEY_SPACE, VIO_PRESS);
var_dump(vio_key_just_pressed($ctx, VIO_KEY_SPACE));
vio_begin($ctx); vio_end($ctx);
var_dump(vio_key_just_pressed($ctx, VIO_KEY_SPACE), vio_key_pressed($ctx, VIO_KEY_SPACE));
vio_inject_key($ctx, VIO_KEY_SPACE, VIO_RELEASE);
var_dump(vio_key_released($ctx, VIO_KEY_SPACE));

// Mouse move round-trips and yields a delta against the last vio_begin
vio_inject_mouse_move($ctx, 10.0, 20.0);
vio_begin($ctx); vio_end($ctx);
vio_inject_mouse_move($ctx, 13.5, 16.0);
var_dump(vio_mouse_position($ctx), vio_mouse_delta($ctx));

// Scroll accumulates until vio_begin resets it
vio_inject_scroll($ctx, 0.0, 1.0);
vio_inject_scroll($ctx, 0.5, 2.0);
var_dump(vio_mouse_scroll($ctx));
vio_begin($ctx); vio_end($ctx);
var_dump(vio_mouse_scroll($ctx));

// Text: codepoint or UTF-8 string, char buffer + on_char callback
$cps = [];
vio_on_char($ctx, function (int $cp) use (&$cps) { $cps[] = sprintf('U+%04X', $cp); });
var_dump(vio_inject_char($ctx, 0x41));
var_dump(vio_inject_char($ctx, "é€😀"));
var_dump(vio_chars_typed($ctx) === "Aé€😀");
echo implode(' ', $cps), "\n";

// Control characters and malformed UTF-8 throw and emit nothing
foreach ([10, "a\nb", "ok\xC3", "\xED\xA0\x80", 0x110000] as $bad) {
    try {
        vio_inject_char($ctx, $bad);
    } catch (ValueError $e) {
        echo $e->getMessage(), "\n";
    }
}
var_dump(count($cps));

vio_begin($ctx); vio_end($ctx);
var_dump(vio_chars_typed($ctx));

vio_destroy($ctx);
echo "OK\n";
?>
--EXPECT--
bool(true)
bool(false)
65/1/1 65/2/0 65/0/0
bool(true)
vio_inject_key(): Argument #3 ($action) must be VIO_RELEASE, VIO_PRESS or VIO_REPEAT
bool(false)
vio_inject_mouse_button(): Argument #3 ($action) must be VIO_RELEASE, VIO_PRESS or VIO_REPEAT
bool(true)
bool(false)
bool(true)
bool(true)
array(2) {
  [0]=>
  float(13.5)
  [1]=>
  float(16)
}
array(2) {
  [0]=>
  float(3.5)
  [1]=>
  float(-4)
}
array(2) {
  [0]=>
  float(0.5)
  [1]=>
  float(3)
}
array(2) {
  [0]=>
  float(0)
  [1]=>
  float(0)
}
int(1)
int(3)
bool(true)
U+0041 U+00E9 U+20AC U+1F600
vio_inject_char(): Argument #2 ($input) must be a printable Unicode codepoint (control characters are injected as keys)
vio_inject_char(): Argument #2 ($input) must not contain control characters (inject Enter, Tab, Backspace as keys)
vio_inject_char(): Argument #2 ($input) must be valid UTF-8
vio_inject_char(): Argument #2 ($input) must be valid UTF-8
vio_inject_char(): Argument #2 ($input) must be a printable Unicode codepoint (control characters are injected as keys)
int(4)
string(0) ""
OK

--TEST--
Input: vio_chars_typed buffer + vio_on_char callback registration
--EXTENSIONS--
vio
--SKIPIF--
<?php
require __DIR__ . '/skipif_gl.inc';
?>
--FILE--
<?php
$ctx = vio_create("opengl", ["width" => 64, "height" => 64, "headless" => true]);
if (!$ctx) { echo "SKIP\n"; exit; }

// chars_typed returns the codepoints typed since the last poll. With
// no input it must return an empty string, never null/false/error.
$initial = vio_chars_typed($ctx);
var_dump(is_string($initial));
var_dump($initial === "");

// Calling chars_typed multiple times should be safe (each call drains
// the buffer; subsequent calls without input also return "").
$again = vio_chars_typed($ctx);
var_dump($again === "");

// Register a char callback. The runtime stores the callable and invokes
// it on each typed character. We just verify registration succeeds —
// triggering the callback requires actual keyboard input which a phpt
// test cannot synthesise from PHP.
$callback_fired = 0;
vio_on_char($ctx, function (int $codepoint) use (&$callback_fired) {
    $callback_fired++;
});
echo "on_char closure OK\n";

// Re-registering replaces the previous callback (typical event-handler
// contract). Must not leak the prior closure.
vio_on_char($ctx, function (int $codepoint) use (&$callback_fired) {
    $callback_fired += 2;
});
echo "on_char replacement OK\n";

// Plain function (string callable).
function vio_test_on_char(int $cp): void {}
vio_on_char($ctx, 'vio_test_on_char');
echo "on_char string callable OK\n";

// Static method.
class VioTestCharSink { public static function handle(int $cp): void {} }
vio_on_char($ctx, ['VioTestCharSink', 'handle']);
echo "on_char static method OK\n";

// Poll events — fires queued callbacks. With no actual input, nothing
// should fire. Must not crash even with no registered handler before.
vio_poll_events($ctx);
var_dump($callback_fired === 0);

vio_destroy($ctx);
echo "OK\n";
?>
--EXPECT--
bool(true)
bool(true)
bool(true)
on_char closure OK
on_char replacement OK
on_char string callable OK
on_char static method OK
bool(true)
OK

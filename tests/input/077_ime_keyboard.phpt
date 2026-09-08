--TEST--
vio_keyboard_show / vio_keyboard_hide / vio_ime_backspaces (on-screen keyboard, no-op on desktop)
--EXTENSIONS--
vio
--FILE--
<?php
$ctx = vio_create("null", ["width" => 64, "height" => 64, "headless" => true]);

// No-ops on desktop — must be callable without error
vio_keyboard_show($ctx);
vio_keyboard_hide($ctx);
echo "keyboard toggled\n";

// read-and-clear counter; 0 on desktop (physical Backspace uses the key API)
$n = vio_ime_backspaces($ctx);
var_dump(is_int($n));
var_dump($n === 0);

vio_destroy($ctx);
echo "OK\n";
?>
--EXPECT--
keyboard toggled
bool(true)
bool(true)
OK

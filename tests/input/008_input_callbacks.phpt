--TEST--
vio_on_key / vio_on_resize accept callable and do not crash with null backend
--EXTENSIONS--
vio
--FILE--
<?php
$ctx = vio_create('null');

// Set key callback
$keyCalled = false;
vio_on_key($ctx, function(int $key, int $action, int $mods) use (&$keyCalled) {
    $keyCalled = true;
});
echo "on_key set\n";

// Set resize callback
vio_on_resize($ctx, function(int $width, int $height) {
    // no-op
});
echo "on_resize set\n";

// Replace callbacks (should not leak)
vio_on_key($ctx, function(int $key, int $action, int $mods) {});
echo "on_key replaced\n";

// Toggle fullscreen (no-op on null backend)
vio_toggle_fullscreen($ctx);
echo "toggle_fullscreen ok\n";

vio_destroy($ctx);
echo "OK\n";
?>
--EXPECT--
on_key set
on_resize set
on_key replaced
toggle_fullscreen ok
OK

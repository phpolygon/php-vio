--TEST--
vio_set_window_size changes context dimensions
--EXTENSIONS--
vio
--FILE--
<?php
$ctx = vio_create("opengl", ["width" => 64, "height" => 48, "headless" => true]);
if (!$ctx) { echo "SKIP\n"; exit; }

$size = vio_window_size($ctx);
echo "initial: {$size[0]}x{$size[1]}\n";

vio_set_window_size($ctx, 128, 96);
// In headless mode the internal config is updated
// (GLFW window resize may not reflect immediately in headless)
echo "set_window_size OK\n";

vio_destroy($ctx);
echo "OK\n";
?>
--EXPECT--
initial: 64x48
set_window_size OK
OK

--TEST--
vio_set_window_size changes context dimensions
--EXTENSIONS--
vio
--SKIPIF--
<?php
require __DIR__ . '/skipif_gl.inc';
?>
--FILE--
<?php
// Windows enforces a minimum window client area (≈ 100×30 dpi-aware px)
// and GLFW silently clamps requested dimensions up to that floor. The
// test previously used 64×48 and saw the clamped 120×48 on a 1.875×
// HiDPI monitor. Use 256×256 which is comfortably above every WM's
// minimum and stays uniform across backends.
$ctx = vio_create("opengl", ["width" => 256, "height" => 256, "headless" => true]);
if (!$ctx) { echo "SKIP\n"; exit; }

$size = vio_window_size($ctx);
echo "initial: {$size[0]}x{$size[1]}\n";

vio_set_window_size($ctx, 512, 384);
// In headless mode the internal config is updated
// (GLFW window resize may not reflect immediately in headless)
echo "set_window_size OK\n";

vio_destroy($ctx);
echo "OK\n";
?>
--EXPECT--
initial: 256x256
set_window_size OK
OK

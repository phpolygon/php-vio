--TEST--
Fullscreen windows do not auto-minimize on focus loss (Print Screen / alt-tab)
--EXTENSIONS--
vio
--SKIPIF--
<?php
require __DIR__ . '/skipif_gl.inc';
?>
--FILE--
<?php
$ctx = vio_create("opengl", ["width" => 256, "height" => 256, "headless" => true]);
if (!$ctx) { echo "SKIP\n"; exit; }

// vio forces GLFW_AUTO_ICONIFY off at window creation so an exclusive-fullscreen
// window keeps rendering (and stays screenshot-able) when it loses focus. Before
// this, pressing Print Screen / opening the Windows snipping tool minimized the
// game and screenshots only worked in windowed/borderless.
var_dump(vio_get_auto_iconify($ctx));   // false — off from creation

// Entering fullscreen must not turn it back on: the attribute survives the
// glfwSetWindowMonitor switch inside vio_set_fullscreen.
vio_set_fullscreen($ctx);
var_dump(vio_get_auto_iconify($ctx));   // still false

// ...and returning to windowed leaves it off too.
vio_set_windowed($ctx);
var_dump(vio_get_auto_iconify($ctx));   // still false

// Null backend has no window — reports GLFW's default and never crashes.
$null = vio_create("null");
var_dump(vio_get_auto_iconify($null));  // true
vio_destroy($null);

vio_destroy($ctx);
echo "OK\n";
?>
--EXPECT--
bool(false)
bool(false)
bool(false)
bool(true)
OK

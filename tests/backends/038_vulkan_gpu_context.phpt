--TEST--
Vulkan backend: headless context creation, frame lifecycle, input
--EXTENSIONS--
vio
--ENV--
VK_DRIVER_FILES=/usr/local/etc/vulkan/icd.d/MoltenVK_icd.json
DYLD_LIBRARY_PATH=/usr/local/lib
--SKIPIF--
<?php
$ctx = @vio_create("vulkan", ["width" => 8, "height" => 8, "headless" => true]);
if (!$ctx) die("skip Vulkan context unavailable (MoltenVK not found)");
vio_destroy($ctx);
?>
--FILE--
<?php
$ctx = vio_create("vulkan", ["width" => 32, "height" => 32, "headless" => true]);

var_dump(vio_backend_name($ctx) === "vulkan");

// Frame lifecycle
vio_begin($ctx);
vio_clear($ctx, 1.0, 0.0, 0.0, 1.0);
vio_end($ctx);
echo "frame OK\n";

// Multiple frames
for ($i = 0; $i < 5; $i++) {
    vio_begin($ctx);
    vio_clear($ctx, $i / 5.0, 0.0, 0.0, 1.0);
    vio_end($ctx);
}
echo "multi-frame OK\n";

// Input
vio_inject_key($ctx, VIO_KEY_ESCAPE, VIO_PRESS);
vio_begin($ctx); vio_end($ctx);
var_dump(vio_key_pressed($ctx, VIO_KEY_ESCAPE) === true);

vio_inject_mouse_move($ctx, 10.0, 20.0);
$pos = vio_mouse_position($ctx);
var_dump(abs($pos[0] - 10.0) < 0.01);

// Close
var_dump(vio_should_close($ctx) === false);
vio_close($ctx);
var_dump(vio_should_close($ctx) === true);

vio_destroy($ctx);
echo "OK\n";
?>
--EXPECT--
bool(true)
frame OK
multi-frame OK
bool(true)
bool(true)
bool(true)
bool(true)
OK

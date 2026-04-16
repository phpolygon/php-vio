<?php
$ctx = vio_create("metal", ["width" => 32, "height" => 32, "headless" => true]);
if (!$ctx) { echo "SKIP: Metal context unavailable\n"; exit; }

var_dump(vio_backend_name($ctx) === "metal");

// Frame lifecycle
vio_clear($ctx, 1.0, 0.0, 0.0, 1.0);
vio_begin($ctx);
vio_end($ctx);
echo "frame OK\n";

// Multiple frames
for ($i = 0; $i < 5; $i++) {
    vio_begin($ctx);
    vio_clear($ctx, $i / 5.0, 0.0, 0.0, 1.0);
    vio_end($ctx);
}
echo "multi-frame OK\n";

// Input works on Metal too
vio_inject_key($ctx, VIO_KEY_ESCAPE, VIO_PRESS);
vio_begin($ctx); vio_end($ctx);
var_dump(vio_key_pressed($ctx, VIO_KEY_ESCAPE) === true);

vio_inject_mouse_move($ctx, 10.0, 20.0);
$pos = vio_mouse_position($ctx);
var_dump(abs($pos[0] - 10.0) < 0.01);

// Should close
var_dump(vio_should_close($ctx) === false);

// Cleanup
vio_close($ctx);
var_dump(vio_should_close($ctx) === true);

vio_destroy($ctx);
echo "OK\n";
?>

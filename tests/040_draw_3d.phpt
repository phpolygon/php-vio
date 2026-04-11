--TEST--
vio_draw_3d flushes 3D state without error
--EXTENSIONS--
vio
--FILE--
<?php
$ctx = vio_create("opengl", ["width" => 32, "height" => 32, "headless" => true]);
if (!$ctx) { echo "SKIP\n"; exit; }

vio_begin($ctx);
vio_clear($ctx, 0.0, 0.0, 0.0, 1.0);
vio_draw_3d($ctx);
echo "draw_3d OK\n";
vio_end($ctx);
vio_destroy($ctx);
echo "OK\n";
?>
--EXPECT--
draw_3d OK
OK

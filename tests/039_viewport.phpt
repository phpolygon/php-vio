--TEST--
vio_viewport sets GL viewport without error
--EXTENSIONS--
vio
--FILE--
<?php
$ctx = vio_create("opengl", ["width" => 64, "height" => 64, "headless" => true]);
if (!$ctx) { echo "SKIP\n"; exit; }

vio_begin($ctx);
vio_viewport($ctx, 0, 0, 32, 32);
echo "viewport set OK\n";
vio_viewport($ctx, 16, 16, 48, 48);
echo "viewport changed OK\n";
vio_end($ctx);
vio_destroy($ctx);
echo "OK\n";
?>
--EXPECT--
viewport set OK
viewport changed OK
OK

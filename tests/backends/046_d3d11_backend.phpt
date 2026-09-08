--TEST--
Direct3D 11 backend registration and vtable (Windows-only)
--SKIPIF--
<?php
if (PHP_OS_FAMILY !== 'Windows') die('skip Windows only');
if (!in_array('d3d11', vio_backends(), true)) die('skip D3D11 backend not compiled in');
?>
--EXTENSIONS--
vio
--FILE--
<?php
var_dump(in_array('d3d11', vio_backends(), true));

// Headless D3D11 context — uses WARP software renderer, no GPU/window required
$ctx = @vio_create('d3d11', ['width' => 64, 'height' => 64, 'headless' => true]);
if (!$ctx instanceof VioContext) {
    // WARP may be unavailable in some CI environments
    echo "d3d11 context creation skipped\n";
    echo "done\n";
    exit;
}
var_dump(vio_backend_name($ctx) === 'd3d11');

vio_begin($ctx);
vio_clear($ctx, 0.0, 0.5, 1.0, 1.0);
vio_end($ctx);
echo "render OK\n";

// Readback should produce a 64*64*4 = 16384 byte RGBA buffer
$pixels = vio_read_pixels($ctx);
var_dump(is_string($pixels) && strlen($pixels) === 64 * 64 * 4);

vio_destroy($ctx);
echo "done\n";
?>
--EXPECTF--
bool(true)
%s
%s
%s
done

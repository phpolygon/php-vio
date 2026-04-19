--TEST--
Direct3D 12 backend registration and vtable (Windows-only)
--SKIPIF--
<?php
if (PHP_OS_FAMILY !== 'Windows') die('skip Windows only');
if (!in_array('d3d12', vio_backends(), true)) die('skip D3D12 backend not compiled in');
?>
--EXTENSIONS--
vio
--FILE--
<?php
var_dump(in_array('d3d12', vio_backends(), true));

// D3D12 needs a swapchain and therefore a window — this test can't render
// headless. Just assert that the backend is registered and findable.
echo "done\n";
?>
--EXPECT--
bool(true)
done

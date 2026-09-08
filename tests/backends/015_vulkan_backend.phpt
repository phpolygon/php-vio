--TEST--
Vulkan backend registration
--EXTENSIONS--
vio
--SKIPIF--
<?php
$backends = vio_backends();
if (!in_array('vulkan', $backends)) die('skip Vulkan backend not compiled');
?>
--FILE--
<?php
// Vulkan should be in the backend list
$backends = vio_backends();
var_dump(in_array('vulkan', $backends));

// Backend count should include vulkan (null + opengl + vulkan >= 3)
var_dump(vio_backend_count() >= 3);

echo "OK\n";
?>
--EXPECT--
bool(true)
bool(true)
OK

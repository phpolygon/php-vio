--TEST--
Metal backend registration
--SKIPIF--
<?php
// Metal is only compiled in on macOS builds.
if (!in_array('metal', vio_backends(), true)) die('skip Metal backend not compiled in');
?>
--EXTENSIONS--
vio
--FILE--
<?php
$backends = vio_backends();
var_dump(in_array('metal', $backends));

// Metal backend count should be at least 4 (null, opengl, vulkan, metal)
var_dump(vio_backend_count() >= 4);

echo "OK\n";
?>
--EXPECT--
bool(true)
bool(true)
OK

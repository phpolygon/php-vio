--TEST--
Async texture loading API
--EXTENSIONS--
vio
--FILE--
<?php
// Function exists
var_dump(function_exists('vio_texture_load_async'));
var_dump(function_exists('vio_texture_load_poll'));

// Loading nonexistent file - should start async, then poll returns false
$handle = vio_texture_load_async('/nonexistent/path/image.png');
var_dump(is_resource($handle));

// Poll until done (should be very fast for nonexistent file)
$result = null;
for ($i = 0; $i < 100; $i++) {
    $result = vio_texture_load_poll($handle);
    if ($result !== null) break;
    usleep(1000);
}
// Should return false (file not found)
var_dump($result === false);

echo "OK\n";
?>
--EXPECT--
bool(true)
bool(true)
bool(true)
bool(true)
OK

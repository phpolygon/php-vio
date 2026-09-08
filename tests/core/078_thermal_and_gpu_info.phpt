--TEST--
vio_thermal_state returns a valid token and vio_gpu_info returns the documented shape
--EXTENSIONS--
vio
--FILE--
<?php
// Global queries — no context required.
$state = vio_thermal_state();
$valid = ["nominal", "fair", "serious", "critical", "unknown"];
var_dump(in_array($state, $valid, true));

$g = vio_gpu_info();
var_dump(is_array($g));
var_dump(array_key_exists("name", $g));
var_dump(array_key_exists("vram_bytes", $g));
var_dump(array_key_exists("ram_bytes", $g));
var_dump(is_string($g["name"]));
var_dump(is_int($g["vram_bytes"]));
var_dump(is_int($g["ram_bytes"]));

echo "OK\n";
?>
--EXPECT--
bool(true)
bool(true)
bool(true)
bool(true)
bool(true)
bool(true)
bool(true)
bool(true)
OK

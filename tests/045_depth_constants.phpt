--TEST--
VIO_DEPTH_LESS and VIO_DEPTH_LEQUAL constants are defined
--EXTENSIONS--
vio
--FILE--
<?php
var_dump(defined('VIO_DEPTH_LESS'));
var_dump(VIO_DEPTH_LESS === 0);
var_dump(defined('VIO_DEPTH_LEQUAL'));
var_dump(VIO_DEPTH_LEQUAL === 1);
echo "OK\n";
?>
--EXPECT--
bool(true)
bool(true)
bool(true)
bool(true)
OK

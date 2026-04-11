--TEST--
Check if vio extension is loaded
--EXTENSIONS--
vio
--FILE--
<?php
var_dump(extension_loaded('vio'));
?>
--EXPECT--
bool(true)

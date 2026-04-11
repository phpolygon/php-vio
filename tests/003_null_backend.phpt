--TEST--
Null backend: create, begin/end, close, destroy
--EXTENSIONS--
vio
--FILE--
<?php
// Null backend is always registered
var_dump(vio_backend_count() >= 1);

$ctx = vio_create('null');
var_dump($ctx instanceof VioContext);
var_dump(vio_backend_name($ctx));

// Should not close initially
var_dump(vio_should_close($ctx));

// Begin/end frame
vio_begin($ctx);
vio_end($ctx);
echo "frame OK\n";

// Close
vio_close($ctx);
var_dump(vio_should_close($ctx));

// Destroy
vio_destroy($ctx);
echo "destroy OK\n";
?>
--EXPECT--
bool(true)
bool(true)
string(4) "null"
bool(false)
frame OK
bool(true)
destroy OK

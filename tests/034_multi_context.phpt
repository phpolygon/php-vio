--TEST--
Multiple contexts and resource isolation
--EXTENSIONS--
vio
--FILE--
<?php
// Create two headless contexts
$ctx1 = vio_create("opengl", ["width" => 16, "height" => 16, "headless" => true]);
$ctx2 = vio_create("null", []);

var_dump($ctx1 instanceof VioContext);
var_dump($ctx2 instanceof VioContext);

// Different backends
var_dump(vio_backend_name($ctx1) === "opengl");
var_dump(vio_backend_name($ctx2) === "null");

// Both can begin/end independently
vio_begin($ctx1);
vio_begin($ctx2);
vio_end($ctx2);
vio_end($ctx1);
echo "independent frames OK\n";

// Input state is isolated
vio_inject_key($ctx1, VIO_KEY_W, VIO_PRESS);
vio_begin($ctx1); vio_end($ctx1);
vio_begin($ctx2); vio_end($ctx2);
var_dump(vio_key_pressed($ctx1, VIO_KEY_W) === true);
var_dump(vio_key_pressed($ctx2, VIO_KEY_W) === false); // not injected into ctx2

// Destroy in reverse order
vio_destroy($ctx2);
vio_destroy($ctx1);
echo "OK\n";
?>
--EXPECT--
bool(true)
bool(true)
bool(true)
bool(true)
independent frames OK
bool(true)
bool(true)
OK

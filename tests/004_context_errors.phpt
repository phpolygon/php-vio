--TEST--
Context error handling: double begin, end without begin, invalid backend
--EXTENSIONS--
vio
--FILE--
<?php
// Invalid backend name
$ctx = @vio_create('nonexistent');
var_dump($ctx);

// Create valid context
$ctx = vio_create('null');

// End without begin
@vio_end($ctx);

// Begin twice
vio_begin($ctx);
@vio_begin($ctx);

// End normally
vio_end($ctx);
echo "error handling OK\n";

vio_destroy($ctx);
?>
--EXPECT--
bool(false)
error handling OK

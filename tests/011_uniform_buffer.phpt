--TEST--
vio_uniform_buffer / vio_update_buffer / vio_bind_buffer with null backend
--EXTENSIONS--
vio
--FILE--
<?php
$ctx = vio_create('null');

// Create uniform buffer
$ubo = vio_uniform_buffer($ctx, [
    'size'    => 64,
    'binding' => 0,
]);
var_dump($ubo instanceof VioBuffer);

// Update buffer with float data
$data = pack('f4', 1.0, 0.0, 0.0, 1.0);
vio_update_buffer($ubo, $data);
echo "update ok\n";

// Update with offset
vio_update_buffer($ubo, pack('f', 0.5), 16);
echo "update offset ok\n";

// Bind buffer in frame
vio_begin($ctx);
vio_bind_buffer($ctx, $ubo, 0);
echo "bind ok\n";
vio_end($ctx);

// Error: missing size
$bad = vio_uniform_buffer($ctx, []);
var_dump($bad);

// Error: data exceeds size
vio_update_buffer($ubo, str_repeat('x', 100));

vio_destroy($ctx);
echo "OK\n";
?>
--EXPECTF--
bool(true)
update ok
update offset ok
bind ok

Warning: vio_uniform_buffer(): vio_uniform_buffer requires 'size' in %s on line %d
bool(false)

Warning: vio_update_buffer(): Data exceeds buffer size in %s on line %d
OK

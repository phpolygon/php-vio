--TEST--
Network streaming API and VioStream class
--EXTENSIONS--
vio
--FILE--
<?php
// VioStream class exists
var_dump(class_exists('VioStream'));

// All streaming functions exist
var_dump(function_exists('vio_stream'));
var_dump(function_exists('vio_stream_push'));
var_dump(function_exists('vio_stream_stop'));

// Attempting to stream to invalid URL should fail gracefully
$ctx = vio_create('null');
$st = @vio_stream($ctx, ['url' => 'rtmp://invalid.local/test', 'fps' => 30]);
// Expected to fail (can't connect to invalid host)
var_dump($st === false);
vio_destroy($ctx);

echo "OK\n";
?>
--EXPECTF--
bool(true)
bool(true)
bool(true)
bool(true)
%Abool(true)
OK

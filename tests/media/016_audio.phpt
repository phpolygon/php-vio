--TEST--
VioSound class and audio API basics
--EXTENSIONS--
vio
--FILE--
<?php
// VioSound class should exist
var_dump(class_exists('VioSound'));

// Loading a non-existent file should return false
$result = vio_audio_load('/tmp/nonexistent_audio_file_12345.wav');
var_dump($result);

// All audio functions should exist
$funcs = [
    'vio_audio_load', 'vio_audio_play', 'vio_audio_stop',
    'vio_audio_pause', 'vio_audio_resume', 'vio_audio_volume',
    'vio_audio_playing', 'vio_audio_position', 'vio_audio_listener',
];
foreach ($funcs as $fn) {
    var_dump(function_exists($fn));
}

echo "OK\n";
?>
--EXPECTF--
bool(true)

Warning: vio_audio_load(): Failed to load audio file %s in %s on line %d
bool(false)
bool(true)
bool(true)
bool(true)
bool(true)
bool(true)
bool(true)
bool(true)
bool(true)
bool(true)
OK

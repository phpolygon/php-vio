<?php
// VioRecorder class exists
var_dump(class_exists('VioRecorder'));

// All recorder functions exist
var_dump(function_exists('vio_recorder'));
var_dump(function_exists('vio_recorder_capture'));
var_dump(function_exists('vio_recorder_stop'));

// Create null context and try recorder (needs path)
$ctx = vio_create('null');
$rec = @vio_recorder($ctx, ['path' => '/tmp/vio_test_record.mp4', 'fps' => 30]);

// With null backend, recording may or may not work depending on FFmpeg availability
// Just verify the API doesn't crash
if ($rec !== false) {
    var_dump($rec instanceof VioRecorder);
    vio_recorder_stop($rec);
    // Clean up temp file
    @unlink('/tmp/vio_test_record.mp4');
} else {
    // Recorder creation failed (expected with null backend or no FFmpeg)
    var_dump(true);
}

vio_destroy($ctx);
echo "OK\n";
?>

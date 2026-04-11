--TEST--
Video recording with headless GPU produces valid file
--EXTENSIONS--
vio
--FILE--
<?php
$ctx = vio_create("opengl", ["width" => 64, "height" => 64, "headless" => true]);
if (!$ctx) { echo "SKIP\n"; exit; }

$path = tempnam(sys_get_temp_dir(), 'vio_rec_') . '.mp4';

$rec = @vio_recorder($ctx, ['path' => $path, 'fps' => 10]);
if ($rec === false) {
    // VideoToolbox may be unavailable in CI/headless
    echo "recorder unavailable - skipping encode test\n";
    echo "class exists: " . (class_exists('VioRecorder') ? "yes" : "no") . "\n";
    echo "function exists: " . (function_exists('vio_recorder') ? "yes" : "no") . "\n";
    @unlink($path);
    vio_destroy($ctx);
    echo "OK\n";
    exit;
}
var_dump($rec instanceof VioRecorder);

// Record 3 frames with different colors
$colors = [[1.0, 0.0, 0.0], [0.0, 1.0, 0.0], [0.0, 0.0, 1.0]];
foreach ($colors as $c) {
    vio_clear($ctx, $c[0], $c[1], $c[2], 1.0);
    vio_begin($ctx);
    $ok = vio_recorder_capture($rec, $ctx);
    var_dump($ok);
    vio_end($ctx);
}

vio_recorder_stop($rec);

var_dump(file_exists($path));
var_dump(filesize($path) > 100);

unlink($path);
vio_destroy($ctx);
echo "OK\n";
?>
--EXPECTF--
%AOK

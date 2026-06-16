--TEST--
vio_texture_3d creates a 3D/volume texture and rejects malformed input
--SKIPIF--
<?php
$c = @vio_create("auto", ["headless" => true, "width" => 8, "height" => 8]);
if (!$c) die("skip no headless GPU context");
if (!vio_supports_feature($c, VIO_FEATURE_TEXTURE_3D)) die("skip backend lacks 3D textures");
?>
--EXTENSIONS--
vio
--FILE--
<?php
$ctx = vio_create("auto", ["width" => 16, "height" => 16, "headless" => true]);

// 2x2x2 RGBA8 volume = 32 bytes
$vox = str_repeat("\xFF", 2 * 2 * 2 * 4);
$tex = vio_texture_3d($ctx, [
    "data"   => $vox,
    "width"  => 2,
    "height" => 2,
    "depth"  => 2,
    "filter" => VIO_FILTER_LINEAR,
    "wrap"   => VIO_WRAP_CLAMP,
]);
$isTexture = $tex instanceof VioTexture;

// binding samplers must happen inside a frame
vio_begin($ctx);
vio_bind_texture($ctx, $tex, 3);
vio_end($ctx);

// data smaller than width*height*depth*4 -> false (graceful, no fatal)
$badSize = (@vio_texture_3d($ctx, ["data" => "x", "width" => 2, "height" => 2, "depth" => 2]) === false);

// missing 'depth' -> false
$badKeys = (@vio_texture_3d($ctx, ["data" => $vox, "width" => 2, "height" => 2]) === false);

vio_destroy($ctx);

// Deterministic result block last, so any backend banner/validation noise on
// the stream is absorbed by the leading %A.
echo "RESULTS\n";
var_dump($isTexture);
var_dump($badSize);
var_dump($badKeys);
echo "OK\n";
?>
--EXPECTF--
%ARESULTS
bool(true)
bool(true)
bool(true)
OK

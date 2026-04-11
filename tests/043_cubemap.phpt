--TEST--
vio_cubemap creates cubemap from procedural pixel data and binds it
--EXTENSIONS--
vio
--FILE--
<?php
$ctx = vio_create("opengl", ["width" => 32, "height" => 32, "headless" => true]);
if (!$ctx) { echo "SKIP\n"; exit; }

// Create a 2x2 procedural cubemap (6 faces, each 2x2 RGBA)
$res = 2;
$face_size = $res * $res * 4;
$faces = [];
$colors = [
    [255, 0, 0, 255],   // +X red
    [0, 255, 0, 255],   // -X green
    [0, 0, 255, 255],   // +Y blue
    [255, 255, 0, 255], // -Y yellow
    [255, 0, 255, 255], // +Z magenta
    [0, 255, 255, 255], // -Z cyan
];
for ($f = 0; $f < 6; $f++) {
    $face = [];
    for ($p = 0; $p < $res * $res; $p++) {
        $face[] = $colors[$f][0];
        $face[] = $colors[$f][1];
        $face[] = $colors[$f][2];
        $face[] = $colors[$f][3];
    }
    $faces[] = $face;
}

$cm = vio_cubemap($ctx, [
    'pixels' => $faces,
    'width' => $res,
    'height' => $res,
]);
var_dump($cm instanceof VioCubemap);

// Bind cubemap in frame
vio_begin($ctx);
vio_bind_cubemap($ctx, $cm, 0);
echo "cubemap bound OK\n";
vio_end($ctx);

vio_destroy($ctx);
echo "OK\n";
?>
--EXPECT--
bool(true)
cubemap bound OK
OK

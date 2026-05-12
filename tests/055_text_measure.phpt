--TEST--
vio_text_measure: returns width/height array, handles edge inputs
--SKIPIF--
<?php
$candidates = [
    '/Library/Fonts/Arial Unicode.ttf',
    '/System/Library/Fonts/Helvetica.ttc',
    '/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf',
    '/usr/share/fonts/dejavu/DejaVuSans.ttf',
    'C:\\Windows\\Fonts\\arial.ttf',
];
foreach ($candidates as $c) { if (is_file($c)) exit; }
die("skip no system font available for vio_text_measure");
?>
--EXTENSIONS--
vio
--FILE--
<?php
$candidates = [
    '/Library/Fonts/Arial Unicode.ttf',
    '/System/Library/Fonts/Helvetica.ttc',
    '/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf',
    '/usr/share/fonts/dejavu/DejaVuSans.ttf',
    'C:\\Windows\\Fonts\\arial.ttf',
];
$font_path = null;
foreach ($candidates as $c) { if (is_file($c)) { $font_path = $c; break; } }

$ctx = vio_create("opengl", ["width" => 64, "height" => 64, "headless" => true]);
if (!$ctx) { echo "SKIP\n"; exit; }

$font = vio_font($ctx, $font_path, 16.0);
var_dump($font instanceof VioFont);

// Basic text measurement: returns associative array with positive
// width/height. Wider text must measure wider than narrower text.
$short = vio_text_measure($font, "X");
$long  = vio_text_measure($font, "MMMMMMMMMM");

var_dump(is_array($short));
var_dump(is_array($long));
var_dump(isset($short['width']) && isset($short['height']));
var_dump($short['width'] > 0);
var_dump($short['height'] > 0);
var_dump($long['width'] > $short['width']);

// Same text at different sizes: larger font ⇒ larger measurement.
$font_big = vio_font($ctx, $font_path, 32.0);
$big = vio_text_measure($font_big, "X");
var_dump($big['width'] > $short['width']);
var_dump($big['height'] > $short['height']);

// Empty string measures to zero width (or close to it). Height may
// still be the font's line-height — backends differ here, so we only
// assert width.
$empty = vio_text_measure($font, "");
var_dump(is_array($empty));
var_dump($empty['width'] === 0 || $empty['width'] === 0.0);

// Unicode codepoint outside ASCII: the font may not have a glyph for
// every codepoint, but the function must not crash and must return
// a non-negative width.
$unicode = vio_text_measure($font, "Héllo Wörld");
var_dump(is_array($unicode));
var_dump($unicode['width'] >= 0);

vio_destroy($ctx);
echo "OK\n";
?>
--EXPECT--
bool(true)
bool(true)
bool(true)
bool(true)
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

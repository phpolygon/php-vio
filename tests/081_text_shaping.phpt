--TEST--
Text shaping (HarfBuzz): Latin still renders, measure accumulates advances
--SKIPIF--
<?php
require __DIR__ . '/skipif_gl.inc';
if (!defined('VIO_HAS_SHAPING') || VIO_HAS_SHAPING !== 1) {
    die("skip extension built without HarfBuzz (VIO_HAS_SHAPING=0)");
}
$candidates = [
    '/Library/Fonts/Arial Unicode.ttf',
    '/System/Library/Fonts/Helvetica.ttc',
    '/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf',
    '/usr/share/fonts/dejavu/DejaVuSans.ttf',
    'C:\\Windows\\Fonts\\arial.ttf',
];
foreach ($candidates as $c) { if (is_file($c)) exit; }
die("skip no system font available");
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

$ctx = vio_create("opengl", ["width" => 256, "height" => 64, "headless" => true]);
if (!$ctx) { echo "SKIP\n"; exit; }

$font = vio_font($ctx, $font_path, 24.0);
var_dump($font instanceof VioFont);

// Regression: with shaping active, plain Latin text must still rasterize.
vio_clear($ctx, 0.0, 0.0, 0.0, 1.0);
vio_begin($ctx);
vio_text($ctx, $font, "Shaping", 8.0, 8.0, ["color" => 0xFFFFFFFF]);
vio_draw_2d($ctx);
$pixels = vio_read_pixels($ctx);
vio_end($ctx);

$non_black = 0;
for ($i = 0; $i < strlen($pixels); $i += 4) {
    if (ord($pixels[$i]) > 10) { $non_black++; }
}
var_dump($non_black > 0);                       // text rendered

// Measure: advances accumulate, longer string is wider, empty is zero width.
$one  = vio_text_measure($font, "i");
$many = vio_text_measure($font, "iiiiiiiiii");
$empty = vio_text_measure($font, "");
var_dump($one['width'] > 0);
var_dump($many['width'] > $one['width']);
var_dump($empty['width'] === 0 || $empty['width'] === 0.0);

// Mixed script string must not crash and measures positive.
$mix = vio_text_measure($font, "Café Ω 日本");
var_dump(is_array($mix));
var_dump($mix['width'] >= 0);

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
OK

--TEST--
Text shaping: line wrapping (hard '\n' + soft word wrap at max_width)
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

$ctx = vio_create("opengl", ["width" => 256, "height" => 128, "headless" => true]);
if (!$ctx) { echo "SKIP\n"; exit; }
$font = vio_font($ctx, $font_path, 20.0);

// Hard break: '\n' splits into two lines regardless of width.
$hard = vio_text_measure($font, "one\ntwo");
var_dump($hard['lines'] === 2);
var_dump($hard['height'] > 0);

// A single line reports one line.
$single = vio_text_measure($font, "one two three");
var_dump($single['lines'] === 1);

// Soft wrap: the same text with a small max_width breaks into >1 line, and the
// widest wrapped line is no wider than the unwrapped measurement.
$full = $single['width'];
$wrapped = vio_text_measure($font, "one two three four five six", ["max_width" => $full / 2.0]);
var_dump($wrapped['lines'] > 1);
var_dump($wrapped['width'] <= $single['width'] + 0.5);
var_dump($wrapped['height'] > $single['height']);

// A single word longer than max_width must not loop/crash: it overflows on one
// line (no mid-word break in v1).
$oneword = vio_text_measure($font, "Supercalifragilistic", ["max_width" => 10.0]);
var_dump($oneword['lines'] === 1);

// Render wrapped multi-line text: pixels on more than one row.
vio_clear($ctx, 0.0, 0.0, 0.0, 1.0);
vio_begin($ctx);
vio_text($ctx, $font, "alpha beta gamma delta epsilon", 6.0, 20.0,
         ["color" => 0xFFFFFFFF, "max_width" => 110.0]);
vio_text($ctx, $font, "hard\nbreak", 6.0, 90.0, ["color" => 0xFFFFFFFF]);
vio_draw_2d($ctx);
$px = vio_read_pixels($ctx);
vio_end($ctx);

// Count distinct rows (of 128) that contain any lit pixel — wrapping should
// spread text across several rows.
$w = 256; $h = 128; $rows = 0;
for ($y = 0; $y < $h; $y++) {
    $lit = false;
    for ($x = 0; $x < $w; $x++) {
        if (ord($px[($y * $w + $x) * 4]) > 10) { $lit = true; break; }
    }
    if ($lit) $rows++;
}
var_dump($rows > 20); // multiple text rows across the image

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
OK

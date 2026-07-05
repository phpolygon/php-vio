--TEST--
Text shaping: CJK + Thai line breaking (UAX #14-lite, no spaces needed)
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

$ctx = vio_create("opengl", ["width" => 128, "height" => 128, "headless" => true]);
if (!$ctx) { echo "SKIP\n"; exit; }
$font = vio_font($ctx, $font_path, 20.0);

// Line breaking is driven by Unicode class, not glyph coverage, so these hold
// even if the chosen font renders .notdef boxes for CJK/Thai.

// CJK: a spaceless run of 24 ideographs must wrap into several lines.
$cjk = str_repeat("\u{65E5}\u{672C}\u{8A9E}", 8);         // 日本語 × 8 = 24 chars
$one = vio_text_measure($font, "\u{65E5}")['width'];       // width of one ideograph
$mc  = vio_text_measure($font, $cjk, ["max_width" => $one * 5.0]);
var_dump($mc['lines'] > 1);                                // wrapped
var_dump($mc['width'] <= $one * 6.0 + 1.0);                // no line runs far past the cap

// Kinsoku: a closing bracket must not start a line — the break falls before the
// preceding ideograph instead. Just assert it still wraps without crashing.
$kin = str_repeat("\u{6587}\u{5B57}\u{3002}", 8);          // 文字。 × 8
$mk = vio_text_measure($font, $kin, ["max_width" => $one * 4.0]);
var_dump($mk['lines'] > 1);

// Thai: spaceless clusters (consonant + SARA AM) wrap at cluster boundaries.
$thai = str_repeat("\u{0E01}\u{0E33}", 20);                // กำ × 20
$mt = vio_text_measure($font, $thai, ["max_width" => 40.0]);
var_dump($mt['lines'] > 1);

// Latin is unaffected: still breaks at spaces, never mid-word.
$lat = vio_text_measure($font, "wrapping words here now", ["max_width" => 1.0]);
var_dump($lat['lines'] === 4);                             // one word per line

// Render CJK + Thai wrapped without crashing.
vio_clear($ctx, 0.0, 0.0, 0.0, 1.0);
vio_begin($ctx);
vio_text($ctx, $font, $cjk,  4.0, 18.0, ["color" => 0xFFFFFFFF, "max_width" => 110.0]);
vio_text($ctx, $font, $thai, 4.0, 90.0, ["color" => 0xFFFFFFFF, "max_width" => 110.0]);
vio_draw_2d($ctx);
$px = vio_read_pixels($ctx);
vio_end($ctx);
$nonblack = 0;
for ($i = 0; $i < strlen($px); $i += 4) { if (ord($px[$i]) > 10) $nonblack++; }
var_dump($nonblack > 0);

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
OK

--TEST--
Text shaping: Arabic / BiDi (RTL) path does not crash and renders
--SKIPIF--
<?php
require __DIR__ . '/skipif_gl.inc';
if (!defined('VIO_HAS_SHAPING') || VIO_HAS_SHAPING !== 1) {
    die("skip extension built without HarfBuzz (VIO_HAS_SHAPING=0)");
}
// Needs a font that actually carries Arabic glyphs (joining/shaping). DejaVu
// (the usual Linux CI font) has no real Arabic shaping, so it is excluded.
$candidates = [
    'C:\\Windows\\Fonts\\arial.ttf',                        // Windows: has Arabic
    'C:\\Windows\\Fonts\\tahoma.ttf',
    '/System/Library/Fonts/Supplemental/Arial.ttf',        // macOS
    '/Library/Fonts/Arial Unicode.ttf',
    '/System/Library/Fonts/GeezaPro.ttc',
    '/usr/share/fonts/truetype/noto/NotoNaskhArabic-Regular.ttf',
    '/usr/share/fonts/noto/NotoNaskhArabic-Regular.ttf',
    '/usr/share/fonts/truetype/noto/NotoSansArabic-Regular.ttf',
];
foreach ($candidates as $c) { if (is_file($c)) exit; }
die("skip no Arabic-capable font available");
?>
--EXTENSIONS--
vio
--FILE--
<?php
$candidates = [
    'C:\\Windows\\Fonts\\arial.ttf',
    'C:\\Windows\\Fonts\\tahoma.ttf',
    '/System/Library/Fonts/Supplemental/Arial.ttf',
    '/Library/Fonts/Arial Unicode.ttf',
    '/System/Library/Fonts/GeezaPro.ttc',
    '/usr/share/fonts/truetype/noto/NotoNaskhArabic-Regular.ttf',
    '/usr/share/fonts/noto/NotoNaskhArabic-Regular.ttf',
    '/usr/share/fonts/truetype/noto/NotoSansArabic-Regular.ttf',
];
$font_path = null;
foreach ($candidates as $c) { if (is_file($c)) { $font_path = $c; break; } }

$ctx = vio_create("opengl", ["width" => 256, "height" => 64, "headless" => true]);
if (!$ctx) { echo "SKIP\n"; exit; }

$font = vio_font($ctx, $font_path, 28.0);
var_dump($font instanceof VioFont);

// "مرحبا" (marhaba / hello). Pure-RTL run: must shape + render without crash.
$arabic = "\u{0645}\u{0631}\u{062D}\u{0628}\u{0627}";
$m = vio_text_measure($font, $arabic);
var_dump(is_array($m));
var_dump($m['width'] > 0);   // Arabic glyphs present -> positive advance

vio_clear($ctx, 0.0, 0.0, 0.0, 1.0);
vio_begin($ctx);
vio_text($ctx, $font, $arabic, 8.0, 8.0, ["color" => 0xFFFFFFFF]);
// Mixed direction (LTR digits + RTL word) exercises the BiDi run reordering.
vio_text($ctx, $font, "abc " . $arabic . " 123", 8.0, 40.0, ["color" => 0xFFFFFFFF]);
vio_draw_2d($ctx);
$pixels = vio_read_pixels($ctx);
vio_end($ctx);

$non_black = 0;
for ($i = 0; $i < strlen($pixels); $i += 4) {
    if (ord($pixels[$i]) > 10) { $non_black++; }
}
var_dump($non_black > 0);    // something rendered

vio_destroy($ctx);
echo "OK\n";
?>
--EXPECT--
bool(true)
bool(true)
bool(true)
bool(true)
OK

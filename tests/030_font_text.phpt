--TEST--
Font loading and text rendering with headless GPU context
--SKIPIF--
<?php
require __DIR__ . '/skipif_gl.inc';
// Locate a system font that's almost certainly present on the host.
// Test was previously hard-coded to /Library/Fonts/Arial Unicode.ttf
// which only exists on macOS.
$candidates = [
    '/Library/Fonts/Arial Unicode.ttf',                 // macOS
    '/System/Library/Fonts/Helvetica.ttc',              // macOS fallback
    '/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf',  // Debian/Ubuntu
    '/usr/share/fonts/dejavu/DejaVuSans.ttf',           // Fedora
    'C:\\Windows\\Fonts\\arial.ttf',                    // Windows
];
$found = false;
foreach ($candidates as $c) { if (is_file($c)) { $found = true; break; } }
if (!$found) die("skip no system font found for vio_font test");
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

$ctx = vio_create("opengl", ["width" => 128, "height" => 64, "headless" => true]);
if (!$ctx) { echo "SKIP\n"; exit; }

// Load the located system TTF font.
$font = vio_font($ctx, $font_path, 32.0);
var_dump($font instanceof VioFont);

// Loading nonexistent font should fail.
$bad = @vio_font($ctx, "/nonexistent/font.ttf");
var_dump($bad === false);

// Clear to black, draw white text, verify pixels changed.
vio_clear($ctx, 0.0, 0.0, 0.0, 1.0);
vio_begin($ctx);
vio_text($ctx, $font, "Hi", 10.0, 10.0, ["color" => 0xFFFFFFFF]);
vio_draw_2d($ctx);
$pixels = vio_read_pixels($ctx);
vio_end($ctx);

// Check that at least some pixels are non-black (text was rendered).
$non_black = 0;
for ($i = 0; $i < strlen($pixels); $i += 4) {
    if (ord($pixels[$i]) > 10 || ord($pixels[$i+1]) > 10 || ord($pixels[$i+2]) > 10) {
        $non_black++;
    }
}
echo "text rendered: " . ($non_black > 0 ? "yes" : "no") . "\n";

// Render with a smaller size — exercises a second vio_font allocation
// against the same context.
$font_small = vio_font($ctx, $font_path, 12.0);
var_dump($font_small instanceof VioFont);

vio_destroy($ctx);
echo "OK\n";
?>
--EXPECT--
bool(true)
bool(true)
text rendered: yes
bool(true)
OK

--TEST--
Async font loading API (vio_font_load_async / vio_font_load_poll)
--SKIPIF--
<?php
$candidates = [
    '/Library/Fonts/Arial Unicode.ttf',                 // macOS
    '/System/Library/Fonts/Helvetica.ttc',              // macOS fallback
    '/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf',  // Debian/Ubuntu
    '/usr/share/fonts/dejavu/DejaVuSans.ttf',           // Fedora
    'C:\\Windows\\Fonts\\arial.ttf',                    // Windows
];
$found = false;
foreach ($candidates as $c) { if (is_file($c)) { $found = true; break; } }
if (!$found) die("skip no system font found for vio async font test");
?>
--EXTENSIONS--
vio
--FILE--
<?php
// API surface exists.
var_dump(function_exists('vio_font_load_async'));
var_dump(function_exists('vio_font_load_poll'));

$ctx = vio_create("opengl", ["width" => 128, "height" => 64, "headless" => true]);
if (!$ctx) { echo "SKIP\n"; exit; }

// --- Error path: nonexistent file loads async, then poll resolves to false.
$bad = vio_font_load_async($ctx, "/nonexistent/font.ttf", 24.0);
var_dump(is_resource($bad));
$result = null;
for ($i = 0; $i < 5000; $i++) {
    $result = vio_font_load_poll($bad);
    if ($result !== null) { break; }
    usleep(1000); // yield so the worker thread can run and set done
}
var_dump($result === false);

// --- Happy path: load a real system font on a worker thread.
$candidates = [
    '/Library/Fonts/Arial Unicode.ttf',
    '/System/Library/Fonts/Helvetica.ttc',
    '/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf',
    '/usr/share/fonts/dejavu/DejaVuSans.ttf',
    'C:\\Windows\\Fonts\\arial.ttf',
];
$font_path = null;
foreach ($candidates as $c) { if (is_file($c)) { $font_path = $c; break; } }

$handle = vio_font_load_async($ctx, $font_path, 32.0);
var_dump(is_resource($handle));

$font = null;
for ($i = 0; $i < 5000; $i++) {
    $font = vio_font_load_poll($handle);
    if ($font !== null) break;   // null = still loading
    usleep(1000);
}
var_dump($font instanceof VioFont);

// The async-loaded font must be a fully usable VioFont: render with it and
// confirm pixels changed, exactly like a synchronous vio_font() result.
vio_clear($ctx, 0.0, 0.0, 0.0, 1.0);
vio_begin($ctx);
vio_text($ctx, $font, "Hi", 10.0, 10.0, ["color" => 0xFFFFFFFF]);
vio_draw_2d($ctx);
$pixels = vio_read_pixels($ctx);
vio_end($ctx);

$non_black = 0;
for ($i = 0; $i < strlen($pixels); $i += 4) {
    if (ord($pixels[$i]) > 10 || ord($pixels[$i+1]) > 10 || ord($pixels[$i+2]) > 10) {
        $non_black++;
    }
}
echo "text rendered: " . ($non_black > 0 ? "yes" : "no") . "\n";

// Measuring text on the async font works (glyph map was populated).
$m = vio_text_measure($font, "Hi");
var_dump(is_array($m) && $m['width'] > 0.0);

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
text rendered: yes
bool(true)
OK

--TEST--
Glyph atlas fills on demand (HarfBuzz path): vio_font rasterizes nothing up front, vio_text rasterizes and uploads only the new glyphs (sub-image upload), measuring rasterizes nothing; text whose glyphs arrived over several frames renders like text drawn by a fresh font in one go, on every backend
--SKIPIF--
<?php
if (!defined('VIO_HAS_SHAPING') || VIO_HAS_SHAPING !== 1) die("skip extension built without HarfBuzz (VIO_HAS_SHAPING=0)");
foreach (['/Library/Fonts/Arial Unicode.ttf', '/System/Library/Fonts/Helvetica.ttc', '/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf',
          '/usr/share/fonts/dejavu/DejaVuSans.ttf', 'C:\\Windows\\Fonts\\arial.ttf'] as $c) if (is_file($c)) exit;
die("skip no system font available");
?>
--EXTENSIONS--
vio
--FILE--
<?php
/* OPEN-ITEMS-PLAN A33. */
$fontPath = null;
foreach (['/Library/Fonts/Arial Unicode.ttf', '/System/Library/Fonts/Helvetica.ttc', '/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf',
          '/usr/share/fonts/dejavu/DejaVuSans.ttf', 'C:\\Windows\\Fonts\\arial.ttf'] as $c) if (is_file($c)) { $fontPath = $c; break; }
$W = 256; $H = 64;
function frame($ctx, $font, string $text): string {
    vio_clear($ctx, 0, 0, 0, 1);
    vio_begin($ctx);
    vio_text($ctx, $font, $text, 4.0, 40.0, ['color' => 0xFFFFFFFF]);
    vio_draw_2d($ctx);
    vio_end($ctx);
    return vio_read_pixels($ctx);
}
function diff(string $a, string $b): int {
    $n = 0;
    for ($i = 0; $i < strlen($a); $i += 4) if (abs(ord($a[$i]) - ord($b[$i])) > 8) $n++;
    return $n;
}
function lit(string $p): int { $n = 0; for ($i = 0; $i < strlen($p); $i += 4) if (ord($p[$i]) > 128) $n++; return $n; }

$info0 = null;
foreach (['opengl', 'd3d11', 'd3d12', 'vulkan', 'metal'] as $b) {
    $ctx = @vio_create($b, ['width' => $W, 'height' => $H, 'headless' => true, 'vsync' => false]);
    if ($ctx && vio_backend_name($ctx) !== $b) { vio_destroy($ctx); $ctx = null; }
    if (!$ctx) { echo "$b: skip (unavailable)\n"; continue; }
    $fail = [];
    $font = vio_font($ctx, $fontPath, 32.0);
    $i = vio_font_info($font);
    if (array_keys($i) !== ['glyphs', 'rasterized', 'atlas_size', 'lazy']) $fail[] = "keys " . json_encode(array_keys($i));
    if (!$i['lazy'] || $i['rasterized'] !== 0 || $i['glyphs'] < 100) $fail[] = "after vio_font " . json_encode($i);
    vio_text_measure($font, "Measured only: XYZ");
    if (vio_font_info($font)['rasterized'] !== 0) $fail[] = "measure rasterized";
    $p1 = frame($ctx, $font, "Hi");
    if (($r = vio_font_info($font)['rasterized']) !== 2) $fail[] = "after 'Hi' rasterized $r";
    frame($ctx, $font, "Hi there");
    $pLate = frame($ctx, $font, "Hi there, 123!");
    $r = vio_font_info($font)['rasterized'];
    if ($r !== count(array_unique(str_split(str_replace(' ', '', "Hi there, 123!"))))) $fail[] = "rasterized $r";
    $fresh = vio_font($ctx, $fontPath, 32.0);
    $pFresh = frame($ctx, $fresh, "Hi there, 123!");
    if (lit($pFresh) < 50) $fail[] = "fresh font drew nothing";
    if (($d = diff($pLate, $pFresh)) > 0) $fail[] = "late glyphs differ from a fresh font in $d px";
    // Back to the first frame's text: still identical (old glyphs untouched by later uploads).
    if (($d = diff(frame($ctx, $font, "Hi"), $p1)) > 0) $fail[] = "'Hi' changed in $d px";
    echo "$b: ", $fail ? "FAIL " . implode('; ', $fail) : "OK", "\n";
    vio_destroy($ctx);
}
echo "DONE\n";
?>
--EXPECTF--
opengl: %r(OK|skip \(.*\))%r
d3d11: %r(OK|skip \(.*\))%r
d3d12: %r(OK|skip \(.*\))%r
vulkan: %r(OK|skip \(.*\))%r
metal: %r(OK|skip \(.*\))%r
DONE

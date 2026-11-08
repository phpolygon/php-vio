--TEST--
vio_font 'sdf': a signed-distance-field atlas keeps glyph edges sharp when a transform magnifies the text, on every backend
--DESCRIPTION--
The same text from a coverage atlas and from an SDF atlas, magnified 8x by a 2D
transform: the coverage glyphs smear over several pixels, the SDF glyphs keep a
one-to-two-pixel edge. Ink area and vio_text_measure() match the coverage font;
vio_font_info() reports the spread.
--EXTENSIONS--
vio
--SKIPIF--
<?php
if (!extension_loaded('vio')) die('skip vio not loaded');
$found = false;
foreach (['/Library/Fonts/Arial Unicode.ttf', '/System/Library/Fonts/Helvetica.ttc', '/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf', '/usr/share/fonts/dejavu/DejaVuSans.ttf', 'C:\\Windows\\Fonts\\arial.ttf'] as $c) {
    if (is_file($c)) { $found = true; break; }
}
if (!$found) die('skip no system font found');
?>
--FILE--
<?php
$path = null;
foreach (['/Library/Fonts/Arial Unicode.ttf', '/System/Library/Fonts/Helvetica.ttc', '/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf', '/usr/share/fonts/dejavu/DejaVuSans.ttf', 'C:\\Windows\\Fonts\\arial.ttf'] as $c) {
    if (is_file($c)) { $path = $c; break; }
}
const W = 256;
const H = 160;

/** @return array{0: int, 1: int, 2: float} [fully lit pixels, edge pixels, summed brightness] of the left / right half */
function ink(string $px, int $x0): array
{
    $full = 0; $edge = 0; $sum = 0.0;
    for ($y = 0; $y < H; $y++) {
        for ($x = $x0; $x < $x0 + W / 2; $x++) {
            $l = ord($px[($y * W + $x) * 4 + 1]);
            $sum += $l / 255.0;
            if ($l >= 215) $full++;
            elseif ($l > 40) $edge++;
        }
    }
    return [$full, $edge, $sum];
}

$seen = [];
foreach (['auto', 'metal', 'opengl', 'd3d11', 'd3d12', 'vulkan'] as $candidate) {
    $ctx = @vio_create($candidate, ['width' => W, 'height' => H, 'headless' => true]);
    if (!$ctx) continue;
    $name = vio_backend_name($ctx);
    if (in_array($name, $seen, true)) { vio_destroy($ctx); continue; }
    $seen[] = $name;
    $bmp = vio_font($ctx, $path, 24.0);
    $sdf = vio_font($ctx, $path, 24.0, 1.0, ['sdf' => true]);
    if (!$bmp || !$sdf) { echo "$name: font failed\n"; vio_destroy($ctx); continue; }
    $spread = vio_font_info($sdf)['sdf'];
    if ($spread === 0) {
        // Coverage only without the shaping atlas (no HarfBuzz): nothing to compare.
        echo "$name: OK\n";
        vio_destroy($ctx);
        continue;
    }
    $mb = vio_text_measure($bmp, 'Ag');
    $ms = vio_text_measure($sdf, 'Ag');
    for ($f = 0; $f < 2; $f++) {
        vio_begin($ctx);
        vio_clear($ctx, 0.0, 0.0, 0.0, 1.0);
        vio_push_transform($ctx, 4, 0, 0, 4, 4, 20);
        vio_text($ctx, $bmp, 'Ag', 0, 24, ['color' => 0xFFFFFFFF]);
        vio_pop_transform($ctx);
        vio_push_transform($ctx, 4, 0, 0, 4, W / 2 + 4, 20);
        vio_text($ctx, $sdf, 'Ag', 0, 24, ['color' => 0xFFFFFFFF]);
        vio_pop_transform($ctx);
        vio_draw_2d($ctx);
        vio_end($ctx);
    }
    $px = (string) vio_read_pixels($ctx);
    [$bFull, $bEdge, $bInk] = ink($px, 0);
    [$sFull, $sEdge, $sInk] = ink($px, W / 2);
    $problems = [];
    if ($spread < 2) $problems[] = "spread $spread";
    if (abs($mb['width'] - $ms['width']) > 1.0 || abs($mb['height'] - $ms['height']) > 1.0) {
        $problems[] = sprintf('measure %.1fx%.1f vs %.1fx%.1f', $mb['width'], $mb['height'], $ms['width'], $ms['height']);
    }
    if ($sFull < 300) $problems[] = "sdf glyphs missing ($sFull lit)";
    // Same ink (summed brightness survives the blur), within 15 %.
    if ($bInk <= 0 || abs($sInk - $bInk) / $bInk > 0.15) $problems[] = sprintf('ink %.0f vs %.0f', $bInk, $sInk);
    // Sharp: the SDF edge band is far narrower relative to the glyph.
    $bRatio = $bEdge / max(1, $bFull); $sRatio = $sEdge / max(1, $sFull);
    if ($sRatio > $bRatio * 0.5) $problems[] = sprintf('edge ratio %.2f vs bitmap %.2f', $sRatio, $bRatio);
    echo "$name: ", $problems ? 'FAIL ' . implode(', ', $problems) : 'OK', "\n";
    vio_destroy($ctx);
}
if (!$seen) echo "none: OK\n";
?>
--EXPECTREGEX--
(\w+: OK)(\n\w+: OK)*

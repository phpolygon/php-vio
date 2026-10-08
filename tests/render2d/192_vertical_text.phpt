--TEST--
Vertical text (['vertical' => true], HarfBuzz top-to-bottom): glyphs stack downward from (x = right edge, y = top), '\n' starts the next column to the left, vio_text_measure swaps the axes (width = columns x line height, height = longest column, lines = columns)
--SKIPIF--
<?php
if (!defined('VIO_HAS_SHAPING') || VIO_HAS_SHAPING !== 1) die("skip extension built without HarfBuzz (VIO_HAS_SHAPING=0)");
foreach (['C:\\Windows\\Fonts\\msgothic.ttc', 'C:\\Windows\\Fonts\\msyh.ttc', 'C:\\Windows\\Fonts\\YuGothM.ttc',
          '/System/Library/Fonts/Hiragino Sans GB.ttc', '/System/Library/Fonts/ヒラギノ角ゴシック W3.ttc',
          '/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc', '/usr/share/fonts/noto-cjk/NotoSansCJK-Regular.ttc',
          '/usr/share/fonts/truetype/droid/DroidSansFallbackFull.ttf'] as $c) if (is_file($c)) exit;
die("skip no CJK font available");
?>
--EXTENSIONS--
vio
--FILE--
<?php
/* OPEN-ITEMS-PLAN A34. */
$fontPath = null;
foreach (['C:\\Windows\\Fonts\\msgothic.ttc', 'C:\\Windows\\Fonts\\msyh.ttc', 'C:\\Windows\\Fonts\\YuGothM.ttc',
          '/System/Library/Fonts/Hiragino Sans GB.ttc', '/System/Library/Fonts/ヒラギノ角ゴシック W3.ttc',
          '/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc', '/usr/share/fonts/noto-cjk/NotoSansCJK-Regular.ttc',
          '/usr/share/fonts/truetype/droid/DroidSansFallbackFull.ttf'] as $c) if (is_file($c)) { $fontPath = $c; break; }
$W = 160; $H = 160;
$ctx = vio_create('auto', ['width' => $W, 'height' => $H, 'headless' => true, 'vsync' => false]);
$font = vio_font($ctx, $fontPath, 24.0);

// Lit bounding box of a frame.
function bbox($ctx, $font, string $text, float $x, float $y, array $opts, int $W, int $H): ?array {
    vio_clear($ctx, 0, 0, 0, 1);
    vio_begin($ctx);
    vio_text($ctx, $font, $text, $x, $y, $opts + ['color' => 0xFFFFFFFF]);
    vio_draw_2d($ctx);
    vio_end($ctx);
    $p = vio_read_pixels($ctx);
    $x0 = $W; $y0 = $H; $x1 = -1; $y1 = -1;
    for ($j = 0; $j < $H; $j++) for ($i = 0; $i < $W; $i++) if (ord($p[($j * $W + $i) * 4]) > 100) {
        $x0 = min($x0, $i); $x1 = max($x1, $i); $y0 = min($y0, $j); $y1 = max($y1, $j);
    }
    return $x1 < 0 ? null : [$x0, $y0, $x1, $y1];
}
$text = "日本語の";
$h = bbox($ctx, $font, $text, 10, 40, [], $W, $H);
$v = bbox($ctx, $font, $text, 120, 10, ['vertical' => true], $W, $H);
$v2 = bbox($ctx, $font, "日本\n語の", 120, 10, ['vertical' => true], $W, $H);
$fail = [];
if (!$h || !$v || !$v2) $fail[] = "nothing drawn " . json_encode([$h, $v, $v2]);
else {
    [$hw, $hh] = [$h[2] - $h[0], $h[3] - $h[1]];
    [$vw, $vh] = [$v[2] - $v[0], $v[3] - $v[1]];
    if (!($hw > 2 * $hh)) $fail[] = "horizontal not wide " . json_encode($h);
    if (!($vh > 2 * $vw)) $fail[] = "vertical not tall " . json_encode($v);
    if ($v[2] > 120 || $v[0] < 120 - 40) $fail[] = "first column not left of x " . json_encode($v);
    if ($v[1] < 8 || $v[1] > 20) $fail[] = "column top not at y " . json_encode($v);
    // Two columns: the block grows to the left, and is about half as tall.
    if (!($v2[0] < $v[0] - 15)) $fail[] = "second column not to the left " . json_encode($v2);
    if (!($v2[3] - $v2[1] < 0.7 * $vh)) $fail[] = "two columns not shorter " . json_encode($v2);
}
$mh = vio_text_measure($font, $text);
$mv = vio_text_measure($font, $text, ['vertical' => true]);
$mv2 = vio_text_measure($font, "日本\n語の", ['vertical' => true]);
if (!($mv['height'] > 2 * $mv['width'])) $fail[] = "measure vertical " . json_encode($mv);
if (abs($mv['height'] - $mh['width']) > $mh['width'] * 0.25) $fail[] = "vertical length " . json_encode([$mh, $mv]);
if ($mv2['lines'] !== 2 || !($mv2['width'] > 1.5 * $mv['width'])) $fail[] = "measure two columns " . json_encode($mv2);
echo $fail ? "FAIL " . implode('; ', $fail) : "OK", "\n";
vio_destroy($ctx);
?>
--EXPECT--
OK

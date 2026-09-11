--TEST--
vio_font_face + vio_text_bitmap: shaped text as a coverage bitmap on the CPU, with a font fallback chain and no context
--DESCRIPTION--
VioFontFace parses a font without building a GPU atlas. vio_text_bitmap() lays a
line out through a fallback chain (first covering face wins, a segment keeps its
face while it covers the next codepoint), shapes it with HarfBuzz on SheenBidi
runs when available and rasterizes it into width*height coverage bytes. Checked:
the bitmap and its metrics, measure-only mode, scaling, empty text, argument
errors, routing to the covering fallback face and an Arabic lam-alef ligature.
--EXTENSIONS--
vio
--SKIPIF--
<?php
if (!extension_loaded('vio')) die('skip vio not loaded');
if (!function_exists('vio_text_bitmap')) die('skip vio_text_bitmap missing');
$__latin = [
    'C:\\Windows\\Fonts\\arial.ttf',
    '/System/Library/Fonts/Supplemental/Arial.ttf',
    '/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf',
    '/usr/share/fonts/dejavu/DejaVuSans.ttf',
    '/usr/share/fonts/TTF/DejaVuSans.ttf',
];
foreach ($__latin as $__f) { if (is_file($__f)) exit; }
die('skip no Latin font available');
?>
--FILE--
<?php
function first_file(array $candidates): ?string
{
    foreach ($candidates as $c) { if (is_file($c)) return $c; }
    return null;
}

$latinPath = first_file([
    'C:\\Windows\\Fonts\\arial.ttf',
    '/System/Library/Fonts/Supplemental/Arial.ttf',
    '/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf',
    '/usr/share/fonts/dejavu/DejaVuSans.ttf',
    '/usr/share/fonts/TTF/DejaVuSans.ttf',
]);
$latin = vio_font_face($latinPath);
var_dump($latin instanceof VioFontFace);
var_dump(vio_font_face_has_glyph($latin, ord('A')));
var_dump(vio_font_face_has_glyph($latin, 0x10FFFD));

// --- Bitmap and metrics.
$b = vio_text_bitmap($latin, 'Hello', 32.0);
$w = $b['width']; $h = $b['height'];
echo 'size: ', ($w > 40 && $h > 15 && strlen($b['data']) === $w * $h) ? 'ok' : "bad {$w}x{$h}", "\n";
$ink = 0; $max = 0;
foreach (count_chars($b['data'], 1) as $byte => $n) { if ($byte > 128) $ink += $n; $max = max($max, $byte); }
echo 'ink: ', ($ink > 50 && $max === 255) ? 'ok' : "bad ink={$ink} max={$max}", "\n";
echo 'origin: ', ($b['origin_x'] >= -2 && $b['origin_x'] <= 8 && $b['baseline'] > 0 && $b['baseline'] <= $h) ? 'ok' : "bad {$b['origin_x']}/{$b['baseline']}", "\n";
echo 'advance: ', ($b['advance'] >= $w - $b['origin_x'] - 4) ? 'ok' : "bad {$b['advance']}", "\n";

// --- Measure-only returns the same metrics without pixels.
$m = vio_text_bitmap($latin, 'Hello', 32.0, ['measure' => true]);
echo 'measure: ', ($m['width'] === $w && $m['height'] === $h && $m['baseline'] === $b['baseline']
    && $m['advance'] === $b['advance'] && !array_key_exists('data', $m)) ? 'ok' : 'bad', "\n";

// --- Scale is pixels per em.
$b2 = vio_text_bitmap($latin, 'Hello', 64.0, ['measure' => true]);
echo 'scale: ', abs($b2['advance'] - 2 * $b['advance']) < 2.0 ? 'ok' : "bad {$b2['advance']} vs {$b['advance']}", "\n";

// --- Nothing to draw.
$e = vio_text_bitmap($latin, ' ', 32.0);
echo 'blank: ', ($e['width'] === 0 && $e['height'] === 0 && $e['data'] === '' && $e['advance'] > 0) ? 'ok' : 'bad', "\n";
$e = vio_text_bitmap($latin, '', 32.0);
echo 'empty: ', ($e['width'] === 0 && $e['advance'] == 0) ? 'ok' : 'bad', "\n";

// --- Errors.
var_dump(@vio_font_face(__DIR__ . '/does-not-exist.ttf'));
foreach ([[[], 'x', 10.0], [['nope'], 'x', 10.0], [$latin, 'x', 0.0]] as [$faces, $text, $size]) {
    try {
        vio_text_bitmap($faces, $text, $size);
        echo "no error\n";
    } catch (\Throwable $t) {
        echo get_class($t), "\n";
    }
}

// --- Fallback: a codepoint the first face lacks renders exactly like the face that has it.
$cjkPath = first_file([
    'C:\\Windows\\Fonts\\msyh.ttc',
    'C:\\Windows\\Fonts\\YuGothM.ttc',
    'C:\\Windows\\Fonts\\msgothic.ttc',
    '/System/Library/Fonts/PingFang.ttc',
    '/System/Library/Fonts/Hiragino Sans GB.ttc',
    '/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc',
    '/usr/share/fonts/noto-cjk/NotoSansCJK-Regular.ttc',
]);
$cjk = $cjkPath !== null ? vio_font_face($cjkPath) : false;
if ($cjk && !vio_font_face_has_glyph($latin, 0x6F22) && vio_font_face_has_glyph($cjk, 0x6F22)) {
    $alone = vio_text_bitmap($cjk, '漢字', 32.0);
    $chain = vio_text_bitmap([$latin, $cjk], '漢字', 32.0);
    echo 'fallback: ', ($alone['data'] === $chain['data'] && $alone['width'] === $chain['width']) ? 'ok' : 'bad', "\n";
    $mixed = vio_text_bitmap([$latin, $cjk], 'A漢', 32.0, ['measure' => true]);
    $a = vio_text_bitmap($latin, 'A', 32.0, ['measure' => true]);
    $k = vio_text_bitmap($cjk, '漢', 32.0, ['measure' => true]);
    echo 'mixed advance: ', abs($mixed['advance'] - ($a['advance'] + $k['advance'])) < 1.5 ? 'ok' : "bad {$mixed['advance']}", "\n";
} else {
    echo "fallback: skipped\nmixed advance: skipped\n";
}

// --- Shaping: lam + alef form one ligature, narrower than the two letters apart.
$arabicPath = first_file([
    'C:\\Windows\\Fonts\\arial.ttf',
    '/System/Library/Fonts/Supplemental/Arial.ttf',
    '/usr/share/fonts/truetype/noto/NotoSansArabic-Regular.ttf',
    '/usr/share/fonts/truetype/noto/NotoNaskhArabic-Regular.ttf',
]);
if (defined('VIO_HAS_SHAPING') && VIO_HAS_SHAPING === 1 && $arabicPath !== null) {
    $ar = vio_font_face($arabicPath);
    if (vio_font_face_has_glyph($ar, 0x0644)) {
        $lig = vio_text_bitmap($ar, "\u{0644}\u{0627}", 32.0, ['measure' => true]);
        $lam = vio_text_bitmap($ar, "\u{0644}", 32.0, ['measure' => true]);
        $alef = vio_text_bitmap($ar, "\u{0627}", 32.0, ['measure' => true]);
        echo 'ligature: ', $lig['advance'] < $lam['advance'] + $alef['advance'] - 1.0 ? 'ok' : "bad {$lig['advance']}", "\n";
    } else {
        echo "ligature: skipped\n";
    }
} else {
    echo "ligature: skipped\n";
}
?>
--EXPECTREGEX--
bool\(true\)
bool\(true\)
bool\(false\)
size: ok
ink: ok
origin: ok
advance: ok
measure: ok
scale: ok
blank: ok
empty: ok
bool\(false\)
ValueError
TypeError
ValueError
fallback: (ok|skipped)
mixed advance: (ok|skipped)
ligature: (ok|skipped)

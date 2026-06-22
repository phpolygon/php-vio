--TEST--
vio_rounded_rect: rendering parameters and edge cases
--EXTENSIONS--
vio
--SKIPIF--
<?php
require __DIR__ . '/skipif_gl.inc';
?>
--FILE--
<?php
$ctx = vio_create("opengl", ["width" => 64, "height" => 64, "headless" => true]);
if (!$ctx) { echo "SKIP\n"; exit; }

vio_clear($ctx, 0.0, 0.0, 0.0, 1.0);
vio_begin($ctx);

// Standard rounded fill
vio_rounded_rect($ctx, 4.0, 4.0, 56.0, 56.0, 8.0, ['fill' => 0xFFFFFFFF]);
echo "rounded fill OK\n";

// Outline
vio_rounded_rect($ctx, 8.0, 8.0, 48.0, 48.0, 4.0, ['color' => 0xFF00FF00, 'outline' => true]);
echo "rounded outline OK\n";

// Zero radius — degenerates to vio_rect's rendering
vio_rounded_rect($ctx, 0.0, 0.0, 16.0, 16.0, 0.0, ['fill' => 0xFFFF0000]);
echo "zero radius OK\n";

// Radius larger than half the shortest side — should clamp internally to
// at most min(w, h)/2 (effectively producing a circle/pill shape).
vio_rounded_rect($ctx, 0.0, 0.0, 20.0, 10.0, 999.0, ['fill' => 0xFF0000FF]);
echo "oversized radius OK\n";

// No options array — must use sensible defaults, never crash.
vio_rounded_rect($ctx, 30.0, 30.0, 10.0, 10.0, 2.0);
echo "no options OK\n";

vio_draw_2d($ctx);
$pixels = vio_read_pixels($ctx);
vio_end($ctx);

// Sanity: at least one non-black pixel must be present somewhere.
$any_white = false;
for ($i = 0; $i < strlen($pixels); $i += 4) {
    if (ord($pixels[$i]) > 200) { $any_white = true; break; }
}
var_dump($any_white);

vio_destroy($ctx);
echo "OK\n";
?>
--EXPECT--
rounded fill OK
rounded outline OK
zero radius OK
oversized radius OK
no options OK
bool(true)
OK

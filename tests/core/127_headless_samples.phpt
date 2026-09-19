--TEST--
Headless surfaces honour 'samples': offscreen edges are as smooth as on screen
--EXTENSIONS--
vio
--SKIPIF--
<?php require __DIR__ . '/../skipif_gl.inc'; ?>
--FILE--
<?php
/**
 * A window asks GLFW for multisampling; a headless surface used to draw into
 * a plain single-sampled FBO, so the same frame came back with jagged edges -
 * enough to unsettle a screenshot comparison. Count the pixels that are
 * neither background nor fill: a diagonal edge blends only when the surface
 * is multisampled.
 */
function blendedPixels(int $samples): int
{
    $ctx = vio_create('opengl', [
        'width'    => 64,
        'height'   => 64,
        'headless' => true,
        'samples'  => $samples,
    ]);
    if (!$ctx) {
        return -1;
    }

    vio_begin($ctx);
    vio_clear($ctx, 0.0, 0.0, 0.0, 1.0);
    // A circle: its edge cuts across the pixel grid at every angle.
    vio_circle($ctx, 32, 32, 22, ['fill' => 0xFFFFFFFF]);
    vio_draw_2d($ctx);
    vio_end($ctx);

    $pixels = vio_read_pixels($ctx);
    vio_destroy($ctx);
    if ($pixels === false) {
        return -1;
    }

    $blended = 0;
    for ($i = 0, $n = strlen($pixels); $i < $n; $i += 4) {
        $r = ord($pixels[$i]);
        if ($r > 8 && $r < 247) {
            $blended++;
        }
    }
    return $blended;
}

$aliased = blendedPixels(1);
$smooth  = blendedPixels(4);

echo 'single-sampled has hard edges: ', $aliased <= 8 ? 'yes' : "no ({$aliased})", "\n";
echo 'multisampled blends its edges: ', $smooth > $aliased + 20 ? 'yes' : "no ({$smooth} vs {$aliased})", "\n";
echo "OK\n";
?>
--EXPECT--
single-sampled has hard edges: yes
multisampled blends its edges: yes
OK

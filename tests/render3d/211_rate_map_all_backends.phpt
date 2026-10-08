--TEST--
Rate maps (vio_render_target(['rate_map' => ...]), VIO_FEATURE_RASTER_RATE_MAP, OPEN-ITEMS A16) on every backend: with the feature the target renders into a smaller physical size and the full-quality zones still match the full-rate picture after the resolve; without it the option renders at full rate; malformed maps throw
--EXTENSIONS--
vio
--FILE--
<?php
/* A 192x192 target, quality 1 / 0.25 / 1 in both directions: the centre zone
 * is rendered with a quarter of the samples. Fine diagonal stripes from NDC,
 * the same scene rendered into a plain target as the reference. */
$S = 192;
$VS = "#version 450\nlayout(location=0) in vec2 aPos;\nlayout(location=0) out vec2 ndc;\n"
    . "void main(){ ndc = aPos; gl_Position = vec4(aPos, 0.0, 1.0); }";
$FS = "#version 450\nlayout(location=0) in vec2 ndc;\nlayout(location=0) out vec4 o;\n"
    . "void main(){ float s = step(0.5, fract((ndc.x + ndc.y) * 12.0)); o = vec4(mix(vec3(0.1, 0.2, 0.7), vec3(0.95, 0.9, 0.2), s), 1.0); }";

function region_mae(string $a, string $b, int $w, int $x0, int $y0, int $x1, int $y1): float {
    $s = 0; $n = 0;
    for ($y = $y0; $y < $y1; $y++) for ($x = $x0; $x < $x1; $x++) {
        $i = ($y * $w + $x) * 4;
        for ($c = 0; $c < 3; $c++) { $s += abs(ord($a[$i + $c]) - ord($b[$i + $c])); $n++; }
    }
    return $s / $n;
}

function run_backend(string $name): string {
    global $S, $VS, $FS;
    $ctx = @vio_create($name, ['width' => 32, 'height' => 32, 'headless' => true, 'vsync' => false]);
    if ($ctx && vio_backend_name($ctx) !== $name) { vio_destroy($ctx); $ctx = null; }
    if (!$ctx) return "skip (unavailable)";
    if (!vio_supports_feature($ctx, VIO_FEATURE_3D_PIPELINE) || !vio_supports_feature($ctx, VIO_FEATURE_RENDER_TARGET)) {
        vio_destroy($ctx); return "skip (no 3D pipeline)";
    }
    $fail = [];
    $has = vio_supports_feature($ctx, VIO_FEATURE_RASTER_RATE_MAP);
    $quad = vio_mesh($ctx, ['vertices' => [-1,-1, 1,-1, 1,1, -1,1], 'indices' => [0,1,2, 0,2,3], 'layout' => [VIO_FLOAT2]]);
    $pipe = vio_pipeline($ctx, ['shader' => vio_shader($ctx, ['vertex' => $VS, 'fragment' => $FS]), 'depth_test' => false, 'cull_mode' => VIO_CULL_NONE]);
    $ref = vio_render_target($ctx, ['width' => $S, 'height' => $S]);
    $rm = vio_render_target($ctx, ['width' => $S, 'height' => $S, 'rate_map' => ['x' => [1.0, 0.25, 1.0], 'y' => [1.0, 0.25, 1.0]]]);
    if (!$rm) { vio_destroy($ctx); return "FAIL\n  rate-mapped target not created"; }
    $sz = vio_render_target_size($rm);
    if ($sz['width'] !== $S || $sz['height'] !== $S) $fail[] = "logical size " . json_encode($sz);
    if ($has && (!$sz['rate_map'] || $sz['physical_width'] >= $S || $sz['physical_height'] >= $S)) $fail[] = "feature reported, but " . json_encode($sz);
    if (!$has && ($sz['rate_map'] || $sz['physical_width'] !== $S || $sz['physical_height'] !== $S)) $fail[] = "no feature, but " . json_encode($sz);
    $plain = vio_render_target_size($ref);
    if ($plain['rate_map'] || $plain['physical_width'] !== $S) $fail[] = "plain target " . json_encode($plain);

    foreach ([$ref, $rm] as $rt) {
        vio_begin($ctx);
        vio_bind_render_target($ctx, $rt);
        vio_viewport($ctx, 0, 0, $S, $S);
        vio_clear($ctx, 0, 0, 0, 1);
        vio_bind_pipeline($ctx, $pipe);
        vio_draw($ctx, $quad);
        vio_unbind_render_target($ctx);
        vio_end($ctx);
    }
    $a = vio_read_render_target($ref);
    $b = vio_read_render_target($rm);
    $z = intdiv($S, 3);
    $corner = region_mae($a, $b, $S, 4, 4, $z - 4, $z - 4);          /* quality 1 x 1 */
    $centre = region_mae($a, $b, $S, $z + 4, $z + 4, 2 * $z - 4, 2 * $z - 4);   /* quality 0.25 x 0.25 */
    if (getenv('VIO_RATE_MAP_DEBUG')) fprintf(STDERR, "$name: feature %d corner %.2f centre %.2f size %s\n", $has, $corner, $centre, json_encode($sz));
    if ($has) {
        if ($corner > 12.0) $fail[] = sprintf("full-quality corner differs from the full-rate picture (%.2f)", $corner);
        if ($centre <= $corner) $fail[] = sprintf("the low-quality centre is not coarser (centre %.2f, corner %.2f)", $centre, $corner);
    } elseif ($a !== $b) {
        $fail[] = sprintf("without the feature the picture differs (corner %.2f, centre %.2f)", $corner, $centre);
    }

    foreach ([['x' => [1.0]], ['x' => [1.0], 'y' => []], ['x' => [0.0], 'y' => [1.0]], ['x' => [1.5], 'y' => [1.0]], 'zones'] as $k => $bad) {
        try { vio_render_target($ctx, ['width' => 16, 'height' => 16, 'rate_map' => $bad]); $fail[] = "bad rate map $k accepted"; } catch (ValueError $e) {}
    }
    try { vio_render_target($ctx, ['width' => 16, 'height' => 16, 'depth_only' => true, 'rate_map' => ['x' => [1.0], 'y' => [1.0]]]); $fail[] = "rate map on depth_only accepted"; } catch (ValueError $e) {}
    vio_destroy($ctx);
    return $fail ? "FAIL\n  " . implode("\n  ", $fail) : "OK";
}

foreach (['opengl', 'd3d11', 'd3d12', 'vulkan', 'metal'] as $b) echo "$b: ", run_backend($b), "\n";
echo "DONE\n";
?>
--EXPECTF--
opengl: %r(OK|skip \(.*\))%r
d3d11: %r(OK|skip \(.*\))%r
d3d12: %r(OK|skip \(.*\))%r
vulkan: %r(OK|skip \(.*\))%r
metal: %r(OK|skip \(.*\))%r
DONE

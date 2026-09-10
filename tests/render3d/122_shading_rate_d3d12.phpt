--TEST--
Variable rate shading (vio_set_shading_rate, VIO_FEATURE_SHADING_RATE): 2X2 shades pixel blocks once, 1X1 per pixel; unsupported backends answer false
--EXTENSIONS--
vio
--SKIPIF--
<?php
$ok = false;
foreach (vio_backends() as $b) {
    if ($b === 'null' || $b === 'vulkan') continue;
    $c = @vio_create($b, ["width" => 8, "height" => 8, "headless" => true, "vsync" => false]);
    if (!$c) continue;
    if (vio_supports_feature($c, VIO_FEATURE_3D_PIPELINE)) $ok = true;
    vio_destroy($c);
}
if (!$ok) die("skip no 3D backend");
?>
--FILE--
<?php
/* GAP-PHASE5-PLAN Block 12. The fragment shader writes its own pixel
 * coordinate into the colour. With a 2X2 shading rate the shader runs once per
 * 2x2 block, so the two pixels of a block carry the same value; at 1X1 they
 * differ by one step. Backends without VIO_FEATURE_SHADING_RATE return false
 * from vio_set_shading_rate and draw at full rate. */
$W = 16;
function px(string $p, int $x, int $y, int $w): array { $o = ($y*$w+$x)*4; return [ord($p[$o]), ord($p[$o+1]), ord($p[$o+2])]; }

function run_backend(string $name): string {
    global $W;
    $ctx = @vio_create($name, ["width" => $W, "height" => $W, "headless" => true, "vsync" => false]);
    if (!$ctx) return "skip (unavailable)";
    if (!vio_supports_feature($ctx, VIO_FEATURE_3D_PIPELINE)) { vio_destroy($ctx); return "skip (no 3D pipeline)"; }
    $fail = [];
    $fmt = $name === 'opengl' ? VIO_SHADER_GLSL_RAW : VIO_SHADER_GLSL;
    $vs = "#version 330 core\nlayout(location=0) in vec3 aPos;\nvoid main(){ gl_Position = vec4(aPos, 1.0); }";
    $fs = "#version 330 core\nlayout(location=0) out vec4 o;\nvoid main(){ o = vec4(floor(gl_FragCoord.x) / 16.0, floor(gl_FragCoord.y) / 16.0, 0.0, 1.0); }";
    $pipe = vio_pipeline($ctx, ['shader' => vio_shader($ctx, ['vertex' => $vs, 'fragment' => $fs, 'format' => $fmt]), 'depth_test' => false]);
    $quad = vio_mesh($ctx, ['vertices' => [-1,-1,0, 1,-1,0, 1,1,0, -1,1,0], 'indices' => [0,1,2, 0,2,3], 'layout' => [VIO_FLOAT3]]);

    $draw = static function () use ($ctx, $pipe, $quad, $W): string {
        vio_clear($ctx, 0, 0, 0, 1);
        vio_begin($ctx);
        vio_bind_pipeline($ctx, $pipe);
        vio_draw($ctx, $quad);
        vio_end($ctx);
        return vio_read_pixels($ctx);
    };

    $hasVrs = vio_supports_feature($ctx, VIO_FEATURE_SHADING_RATE);
    /* Full rate: neighbouring pixels differ by one 1/16 step in red. */
    $p = $draw();
    $a = px($p, 4, 8, $W); $b = px($p, 5, 8, $W);
    if (abs($a[0] - $b[0]) < 10) $fail[] = "1X1: neighbours equal " . json_encode([$a, $b]);

    if (!$hasVrs) {
        if (vio_set_shading_rate($ctx, VIO_SHADING_RATE_2X2) !== false) $fail[] = "set_shading_rate succeeded without the feature";
        vio_destroy($ctx);
        return $fail ? "FAIL\n  " . implode("\n  ", $fail) : "OK (no VRS)";
    }

    if (!vio_set_shading_rate($ctx, VIO_SHADING_RATE_2X2)) $fail[] = "set_shading_rate(2X2) returned false";
    $p = $draw();
    $a = px($p, 4, 8, $W); $b = px($p, 5, 8, $W); $c = px($p, 6, 8, $W);
    if ($a !== $b) $fail[] = "2X2: pixels 4 and 5 differ " . json_encode([$a, $b]);
    if (abs($a[0] - $c[0]) < 10) $fail[] = "2X2: pixels 4 and 6 equal " . json_encode([$a, $c]);
    /* Sticky across frames: a second frame still shades coarsely. */
    $p = $draw();
    if (px($p, 4, 8, $W) !== px($p, 5, 8, $W)) $fail[] = "2X2 not sticky across frames";
    /* Setting the rate inside a frame takes effect for the following draws. */
    vio_clear($ctx, 0, 0, 0, 1);
    vio_begin($ctx);
    vio_bind_pipeline($ctx, $pipe);
    vio_set_shading_rate($ctx, VIO_SHADING_RATE_1X1);
    vio_draw($ctx, $quad);
    vio_end($ctx);
    $p = vio_read_pixels($ctx);
    $a = px($p, 4, 8, $W); $b = px($p, 5, 8, $W);
    if (abs($a[0] - $b[0]) < 10) $fail[] = "back at 1X1: neighbours equal " . json_encode([$a, $b]);
    /* Invalid rates are refused. */
    if (@vio_set_shading_rate($ctx, 99) !== false) $fail[] = "rate 99 accepted";

    vio_destroy($ctx);
    return $fail ? "FAIL\n  " . implode("\n  ", $fail) : "OK";
}

foreach (['opengl', 'd3d11', 'd3d12', 'metal'] as $b) {
    echo "$b: ", run_backend($b), "\n";
}
echo "DONE\n";
?>
--EXPECTF--
opengl: %s
d3d11: %s
d3d12: %s
metal: %s
DONE

--TEST--
Stencil in array, cube and depth_only render targets: the stencil plane exists on every target kind (Metal used to keep cube / array / depth_only targets without one)
--EXTENSIONS--
vio
--SKIPIF--
<?php
$any = false;
foreach (vio_backends() as $b) {
    if ($b === 'null') continue;
    $c = @vio_create($b, ["width" => 8, "height" => 8, "headless" => true, "vsync" => false]);
    if (!$c) continue;
    if (vio_supports_feature($c, VIO_FEATURE_STENCIL) && vio_supports_feature($c, VIO_FEATURE_RENDER_TARGET_LAYERED)) $any = true;
    vio_destroy($c);
}
if (!$any) die("skip no backend with stencil + layered targets");
?>
--FILE--
<?php
/* Same three-draw scheme as 113, per target kind:
 *   1. LEFT half, colour masked off, stencil ALWAYS / ref 1 / REPLACE
 *   2. fullscreen green, stencil EQUAL 1    -> left green
 *   3. fullscreen red,   stencil NOTEQUAL 1 -> right red
 * A: layer 1 of a 2-layer array target, B: face 2 of a cube target, both read
 * back per layer / face. C: a depth_only target - step 2 writes depth (z = 0)
 * only where stencil == 1, so the left half reads back nearer than the right
 * (still at the cleared 1.0). */
$W = 32;
function px(string $p, int $x, int $y, int $w): array { $o = ($y*$w+$x)*4; return [ord($p[$o]), ord($p[$o+1]), ord($p[$o+2])]; }
function near(array $a, array $b): bool { return abs($a[0]-$b[0]) <= 3 && abs($a[1]-$b[1]) <= 3 && abs($a[2]-$b[2]) <= 3; }

function run_backend(string $name): string {
    global $W;
    $ctx = @vio_create($name, ["width" => $W, "height" => $W, "headless" => true, "vsync" => false]);
    if (!$ctx) return "skip (unavailable)";
    if (!vio_supports_feature($ctx, VIO_FEATURE_STENCIL) || !vio_supports_feature($ctx, VIO_FEATURE_3D_PIPELINE)
        || !vio_supports_feature($ctx, VIO_FEATURE_RENDER_TARGET_LAYERED) || !vio_supports_feature($ctx, VIO_FEATURE_RENDER_TARGET_CUBE)) {
        vio_destroy($ctx);
        return "skip (no stencil / layered targets)";
    }
    $fmt = $name === 'opengl' ? VIO_SHADER_GLSL_RAW : VIO_SHADER_GLSL;
    $vs = "#version 330 core\nlayout(location=0) in vec3 aPos;\nvoid main(){ gl_Position = vec4(aPos, 1.0); }";
    $fsColor = static fn (string $rgb): string => "#version 330 core\nlayout(location=0) out vec4 o;\nvoid main(){ o = vec4($rgb, 1.0); }";
    $mk = static function (string $rgb, array $extra) use ($ctx, $vs, $fsColor, $fmt) {
        return vio_pipeline($ctx, ['shader' => vio_shader($ctx, ['vertex' => $vs, 'fragment' => $fsColor($rgb), 'format' => $fmt]),
            'cull_mode' => VIO_CULL_NONE, 'blend' => VIO_BLEND_NONE] + $extra + ['depth_test' => false]);
    };
    $pMark  = $mk("1.0, 1.0, 1.0", ['attachment_color_mask' => [0], 'stencil' => ['func' => VIO_CMP_ALWAYS, 'ref' => 1, 'pass' => VIO_STENCIL_REPLACE]]);
    $pEqual = $mk("0.0, 1.0, 0.0", ['stencil' => ['func' => VIO_CMP_EQUAL, 'ref' => 1, 'write_mask' => 0]]);
    $pOther = $mk("1.0, 0.0, 0.0", ['stencil' => ['func' => VIO_CMP_NOTEQUAL, 'ref' => 1, 'write_mask' => 0]]);
    /* depth_only: mark without depth, then a depth-writing quad gated by the stencil. */
    $pDMark = $mk("1.0, 1.0, 1.0", ['stencil' => ['func' => VIO_CMP_ALWAYS, 'ref' => 1, 'pass' => VIO_STENCIL_REPLACE]]);
    $pDepth = $mk("1.0, 1.0, 1.0", ['depth_test' => true, 'depth_write' => true,
                                    'stencil' => ['func' => VIO_CMP_EQUAL, 'ref' => 1, 'write_mask' => 0]]);
    if (!$pMark || !$pEqual || !$pOther || !$pDMark || !$pDepth) { vio_destroy($ctx); return "FAIL\n  pipeline not created"; }

    $left = vio_mesh($ctx, ['vertices' => [-1,-1,0, 0,-1,0, 0,1,0, -1,1,0], 'indices' => [0,1,2, 0,2,3], 'layout' => [VIO_FLOAT3]]);
    $full = vio_mesh($ctx, ['vertices' => [-1,-1,0, 1,-1,0, 1,1,0, -1,1,0], 'indices' => [0,1,2, 0,2,3], 'layout' => [VIO_FLOAT3]]);

    $fail = [];
    $colour = function (string $label, VioRenderTarget $rt, int $layer) use ($ctx, $W, $pMark, $pEqual, $pOther, $left, $full, &$fail) {
        vio_begin($ctx);
        vio_bind_render_target($ctx, $rt, $layer);
        vio_viewport($ctx, 0, 0, $W, $W);
        vio_clear($ctx, 0, 0, 0, 1);
        vio_bind_pipeline($ctx, $pMark);  vio_draw($ctx, $left);
        vio_bind_pipeline($ctx, $pEqual); vio_draw($ctx, $full);
        vio_bind_pipeline($ctx, $pOther); vio_draw($ctx, $full);
        vio_unbind_render_target($ctx);
        vio_end($ctx);
        $p = vio_read_render_target($rt, $layer);
        if (!$p || strlen($p) !== $W * $W * 4) { $fail[] = "$label: readback size"; return; }
        $l = px($p, 4, $W >> 1, $W);
        $r = px($p, $W - 4, $W >> 1, $W);
        if (!near($l, [0, 255, 0])) $fail[] = "$label: left (stencil == 1) should be green, got " . json_encode($l);
        if (!near($r, [255, 0, 0])) $fail[] = "$label: right (stencil == 0) should be red, got " . json_encode($r);
    };

    $arr = vio_render_target($ctx, ['width' => $W, 'height' => $W, 'layers' => 2]);
    if ($arr instanceof VioRenderTarget) $colour("A array layer 1", $arr, 1); else $fail[] = "A: array target not created";
    $cube = vio_render_target($ctx, ['cube' => true, 'size' => $W]);
    if ($cube instanceof VioRenderTarget) $colour("B cube face 2", $cube, 2); else $fail[] = "B: cube target not created";

    $d = vio_render_target($ctx, ['width' => $W, 'height' => $W, 'depth_only' => true]);
    if (!($d instanceof VioRenderTarget)) { $fail[] = "C: depth_only target not created"; }
    else {
        vio_begin($ctx);
        vio_bind_render_target($ctx, $d);
        vio_viewport($ctx, 0, 0, $W, $W);
        vio_clear($ctx, 0, 0, 0, 1);
        vio_bind_pipeline($ctx, $pDMark); vio_draw($ctx, $left);
        vio_bind_pipeline($ctx, $pDepth); vio_draw($ctx, $full);
        vio_unbind_render_target($ctx);
        vio_end($ctx);
        $p = vio_read_render_target($d);
        if (!$p || strlen($p) !== $W * $W * 4) { $fail[] = "C: readback size"; }
        else {
            $l = px($p, 4, $W >> 1, $W);
            $r = px($p, $W - 4, $W >> 1, $W);
            if ($l[0] > 240) $fail[] = "C: left (stencil == 1) should hold the quad's depth, got " . json_encode($l);
            if ($r[0] < 250) $fail[] = "C: right (stencil == 0) should keep the cleared depth, got " . json_encode($r);
        }
    }
    unset($arr, $cube, $d);
    vio_destroy($ctx);
    return $fail ? "FAIL\n  " . implode("\n  ", $fail) : "OK";
}

foreach (['opengl', 'd3d11', 'd3d12', 'vulkan', 'metal'] as $b) {
    echo "$b: ", run_backend($b), "\n";
}
echo "DONE\n";
?>
--EXPECTF--
opengl: %s
d3d11: %s
d3d12: %s
vulkan: %s
metal: %s
DONE

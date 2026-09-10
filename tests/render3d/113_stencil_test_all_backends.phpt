--TEST--
Stencil test and write through vio_pipeline(['stencil' => [...]]) (VIO_FEATURE_STENCIL)
--EXTENSIONS--
vio
--SKIPIF--
<?php
$any = false;
foreach (vio_backends() as $b) {
    if ($b === 'null') continue;
    $c = @vio_create($b, ["width" => 8, "height" => 8, "headless" => true, "vsync" => false]);
    if (!$c) continue;
    if (vio_supports_feature($c, VIO_FEATURE_STENCIL) && vio_supports_feature($c, VIO_FEATURE_3D_PIPELINE)) $any = true;
    vio_destroy($c);
}
if (!$any) die("skip no backend with a stencil test");
?>
--FILE--
<?php
/* GAP-PHASE5-PLAN Block 1. Three draws into a 32x32 target cleared to black
 * (vio_clear resets the stencil plane to 0):
 *   1. a quad over the LEFT half with the colour write masked off and
 *      stencil ALWAYS / ref 1 / pass REPLACE -> stencil = 1 on the left, 0 right
 *   2. a fullscreen green quad with stencil EQUAL ref 1 (no write) -> left green
 *   3. a fullscreen red quad with stencil NOTEQUAL ref 1        -> right red
 * Expected: left pixel green, right pixel red. Backends that report the
 * feature must get this exactly; the depth test is off throughout. */
$W = 32;
function px(string $p, int $x, int $y, int $w): array { $o = ($y*$w+$x)*4; return [ord($p[$o]), ord($p[$o+1]), ord($p[$o+2])]; }
function near(array $a, array $b): bool { return abs($a[0]-$b[0]) <= 3 && abs($a[1]-$b[1]) <= 3 && abs($a[2]-$b[2]) <= 3; }

function run_backend(string $name): string {
    global $W;
    $ctx = @vio_create($name, ["width" => $W, "height" => $W, "headless" => true, "vsync" => false]);
    if (!$ctx) return "skip (unavailable)";
    if (!vio_supports_feature($ctx, VIO_FEATURE_STENCIL) || !vio_supports_feature($ctx, VIO_FEATURE_3D_PIPELINE)) {
        vio_destroy($ctx);
        return "skip (no stencil)";
    }
    $fmt = $name === 'opengl' ? VIO_SHADER_GLSL_RAW : VIO_SHADER_GLSL;
    $vs = "#version 330 core\nlayout(location=0) in vec3 aPos;\nlayout(location=1) in vec2 aUv;\nout vec2 vUv;\nvoid main(){ vUv = aUv; gl_Position = vec4(aPos, 1.0); }";
    $fsColor = static fn (string $rgb): string => "#version 330 core\nin vec2 vUv;\nlayout(location=0) out vec4 o;\nvoid main(){ o = vec4($rgb, 1.0); }";
    $formats = [VIO_FORMAT_RGBA8];
    $mk = static function (string $rgb, array $extra) use ($ctx, $vs, $fsColor, $fmt, $formats) {
        return vio_pipeline($ctx, ['shader' => vio_shader($ctx, ['vertex' => $vs, 'fragment' => $fsColor($rgb), 'format' => $fmt]),
            'depth_test' => false, 'cull_mode' => VIO_CULL_NONE, 'blend' => VIO_BLEND_NONE, 'attachments' => $formats] + $extra);
    };
    /* 1: write stencil only (colour masked off per attachment — the scalar mask keeps its "0 = RGBA" default). */
    $pMark  = $mk("1.0, 1.0, 1.0", ['attachment_color_mask' => [0], 'stencil' => ['func' => VIO_CMP_ALWAYS, 'ref' => 1, 'pass' => VIO_STENCIL_REPLACE]]);
    $pEqual = $mk("0.0, 1.0, 0.0", ['stencil' => ['func' => VIO_CMP_EQUAL, 'ref' => 1, 'write_mask' => 0]]);
    $pOther = $mk("1.0, 0.0, 0.0", ['stencil' => ['func' => VIO_CMP_NOTEQUAL, 'ref' => 1, 'write_mask' => 0]]);
    if (!$pMark || !$pEqual || !$pOther) { vio_destroy($ctx); return "FAIL\n  pipeline not created"; }

    $left = vio_mesh($ctx, ['vertices' => [-1,-1,0,0,0, 0,-1,0,1,0, 0,1,0,1,1, -1,1,0,0,1], 'indices' => [0,1,2, 0,2,3], 'layout' => [VIO_FLOAT3, VIO_FLOAT2]]);
    $full = vio_mesh($ctx, ['vertices' => [-1,-1,0,0,0, 1,-1,0,1,0, 1,1,0,1,1, -1,1,0,0,1], 'indices' => [0,1,2, 0,2,3], 'layout' => [VIO_FLOAT3, VIO_FLOAT2]]);
    $rt = vio_render_target($ctx, ['width' => $W, 'height' => $W, 'attachments' => $formats]);
    if (!($rt instanceof VioRenderTarget)) { vio_destroy($ctx); return "FAIL\n  render target not created"; }

    vio_begin($ctx);
    vio_bind_render_target($ctx, $rt);
    vio_viewport($ctx, 0, 0, $W, $W);
    vio_clear($ctx, 0, 0, 0, 1);
    vio_bind_pipeline($ctx, $pMark);
    vio_draw($ctx, $left);
    vio_bind_pipeline($ctx, $pEqual);
    vio_draw($ctx, $full);
    vio_bind_pipeline($ctx, $pOther);
    vio_draw($ctx, $full);
    vio_unbind_render_target($ctx);
    vio_end($ctx);

    $p = vio_read_render_target($rt);
    if (!$p || strlen($p) !== $W * $W * 4) { vio_destroy($ctx); return "FAIL\n  readback size"; }
    $fail = [];
    $l = px($p, 4, $W >> 1, $W);
    $r = px($p, $W - 4, $W >> 1, $W);
    if (!near($l, [0, 255, 0])) $fail[] = "left (stencil == 1) should be green, got " . json_encode($l);
    if (!near($r, [255, 0, 0])) $fail[] = "right (stencil == 0) should be red, got " . json_encode($r);
    /* The mark pass must not have written colour. */
    $white = px($p, 4, 2, $W);
    if (near($white, [255, 255, 255])) $fail[] = "mark pass wrote colour despite the write mask";
    unset($rt);
    vio_destroy($ctx);
    return $fail ? "FAIL\n  " . implode("\n  ", $fail) : "OK";
}

foreach (['opengl', 'd3d11', 'd3d12', 'metal', 'vulkan'] as $b) {
    echo "$b: ", run_backend($b), "\n";
}
echo "DONE\n";
?>
--EXPECTF--
opengl: %s
d3d11: %s
d3d12: %s
metal: %s
vulkan: %s
DONE

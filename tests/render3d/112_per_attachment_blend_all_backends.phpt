--TEST--
Per-attachment blend mode and colour write mask on an MRT pipeline (attachment_blend / attachment_color_mask)
--EXTENSIONS--
vio
--SKIPIF--
<?php
$any = false;
foreach (vio_backends() as $b) {
    if ($b === 'null') continue;
    $c = @vio_create($b, ["width" => 8, "height" => 8, "headless" => true, "vsync" => false]);
    if (!$c) continue;
    if (vio_supports_feature($c, VIO_FEATURE_MRT) && vio_supports_feature($c, VIO_FEATURE_3D_PIPELINE)) $any = true;
    vio_destroy($c);
}
if (!$any) die("skip no backend with multiple render targets");
?>
--FILE--
<?php
/* A deferred-style renderer draws its transparent pass into the SAME multi-attachment
 * target as the opaque pass: colour attachments must alpha-blend, the G-buffer attachment
 * must stay exactly as the opaque pass left it (or be overwritten, for surfaces the
 * reflection pass should see). One blend state for every attachment cannot express that,
 * so vio_pipeline() takes 'attachment_blend' and 'attachment_color_mask' arrays.
 *
 * Target: 3 attachments, cleared to (0,0,0,1) / (0.2,0.4,0.6,1) / (0.2,0.4,0.6,1).
 * Draw: a fullscreen quad writing o0 = (1,0,0,0.5), o1 = (1,1,1,0.5), o2 = (1,1,1,0.5) with
 *   attachment_blend      = [ALPHA, NONE, NONE]
 *   attachment_color_mask = [RGBA,  0,    R   ]
 * Expected: att0 = 50 % red over black (128,0,0), att1 untouched (51,102,153),
 *           att2 red channel overwritten only (255,102,153). */
$W = 16;
function px(string $p, int $x, int $y, int $w): array { $o = ($y*$w+$x)*4; return [ord($p[$o]), ord($p[$o+1]), ord($p[$o+2])]; }
function near(array $a, array $b): bool { return abs($a[0]-$b[0]) <= 3 && abs($a[1]-$b[1]) <= 3 && abs($a[2]-$b[2]) <= 3; }

function run_backend(string $name): string {
    global $W;
    $ctx = @vio_create($name, ["width" => $W, "height" => $W, "headless" => true, "vsync" => false]);
    if (!$ctx) return "skip (unavailable)";
    if (!vio_supports_feature($ctx, VIO_FEATURE_MRT) || !vio_supports_feature($ctx, VIO_FEATURE_3D_PIPELINE)) {
        vio_destroy($ctx);
        return "skip (no MRT)";
    }
    $fmt = $name === 'opengl' ? VIO_SHADER_GLSL_RAW : VIO_SHADER_GLSL;
    $vs = "#version 330 core\nlayout(location=0) in vec3 aPos;\nlayout(location=1) in vec2 aUv;\nout vec2 vUv;\nvoid main(){ vUv = aUv; gl_Position = vec4(aPos, 1.0); }";
    $fs = "#version 330 core\nin vec2 vUv;\nlayout(location=0) out vec4 o0;\nlayout(location=1) out vec4 o1;\nlayout(location=2) out vec4 o2;\n"
        . "void main(){ o0 = vec4(1.0, 0.0, 0.0, 0.5); o1 = vec4(1.0, 1.0, 1.0, 0.5); o2 = vec4(1.0, 1.0, 1.0, 0.5); }";
    $formats = [VIO_FORMAT_RGBA8, VIO_FORMAT_RGBA8, VIO_FORMAT_RGBA8];
    $shader = vio_shader($ctx, ['vertex' => $vs, 'fragment' => $fs, 'format' => $fmt]);
    $pipe = vio_pipeline($ctx, [
        'shader' => $shader, 'depth_test' => false, 'cull_mode' => VIO_CULL_NONE,
        'blend' => VIO_BLEND_ALPHA,
        'attachments' => $formats,
        'attachment_blend' => [VIO_BLEND_ALPHA, VIO_BLEND_NONE, VIO_BLEND_NONE],
        'attachment_color_mask' => [VIO_COLOR_RGBA, 0, VIO_COLOR_R],
    ]);
    if (!$pipe) { vio_destroy($ctx); return "FAIL\n  pipeline not created"; }
    $quad = vio_mesh($ctx, ['vertices' => [-1,-1,0,0,1, 1,-1,0,1,1, 1,1,0,1,0, -1,1,0,0,0], 'indices' => [0,1,2, 0,2,3], 'layout' => [VIO_FLOAT3, VIO_FLOAT2]]);
    $rt = vio_render_target($ctx, ['width' => $W, 'height' => $W, 'attachments' => $formats]);
    if (!($rt instanceof VioRenderTarget)) { vio_destroy($ctx); return "FAIL\n  MRT target not created"; }

    /* Fill the data attachments with a known value first: a plain pipeline writing every
     * attachment, no blending. */
    $fsFill = "#version 330 core\nin vec2 vUv;\nlayout(location=0) out vec4 o0;\nlayout(location=1) out vec4 o1;\nlayout(location=2) out vec4 o2;\n"
            . "void main(){ o0 = vec4(0.0, 0.0, 0.0, 1.0); o1 = vec4(0.2, 0.4, 0.6, 1.0); o2 = vec4(0.2, 0.4, 0.6, 1.0); }";
    $pipeFill = vio_pipeline($ctx, ['shader' => vio_shader($ctx, ['vertex' => $vs, 'fragment' => $fsFill, 'format' => $fmt]),
                                    'depth_test' => false, 'cull_mode' => VIO_CULL_NONE, 'blend' => VIO_BLEND_NONE, 'attachments' => $formats]);

    vio_begin($ctx);
    vio_bind_render_target($ctx, $rt);
    vio_viewport($ctx, 0, 0, $W, $W);
    vio_clear($ctx, 0, 0, 0, 1);
    vio_bind_pipeline($ctx, $pipeFill);
    vio_draw($ctx, $quad);
    vio_bind_pipeline($ctx, $pipe);
    vio_draw($ctx, $quad);
    vio_unbind_render_target($ctx);
    vio_end($ctx);

    $fail = [];
    $expect = [[128, 0, 0], [51, 102, 153], [255, 102, 153]];
    $what = ['alpha-blended colour', 'masked-off data attachment untouched', 'red channel overwritten only'];
    for ($i = 0; $i < 3; $i++) {
        $p = vio_read_render_target($rt, -1, $i);
        if (!$p || strlen($p) !== $W * $W * 4) { $fail[] = "att$i: readback size"; continue; }
        $got = px($p, $W >> 1, $W >> 1, $W);
        if (!near($got, $expect[$i])) $fail[] = "att$i ({$what[$i]}): got " . json_encode($got) . ", expected " . json_encode($expect[$i]);
    }
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

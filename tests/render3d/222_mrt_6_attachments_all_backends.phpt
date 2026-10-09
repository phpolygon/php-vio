--TEST--
MRT with more than four colour attachments (VIO_MAX_COLOR_ATTACHMENTS = 8): 4 x RGBA16F + RG16F + R8 written in one draw read back per attachment, the sixth samples; eight RGBA8 attachments work, a ninth is refused
--EXTENSIONS--
vio
--SKIPIF--
<?php
$any = false;
foreach (vio_backends() as $b) {
    if ($b === 'null') continue;
    $c = @vio_create($b, ["width" => 8, "height" => 8, "headless" => true, "vsync" => false]);
    if (!$c) continue;
    if (vio_supports_feature($c, VIO_FEATURE_MRT) && vio_supports_feature($c, VIO_FEATURE_3D_PIPELINE)
        && vio_supports_feature($c, VIO_FEATURE_READ_PIXELS)) $any = true;
    vio_destroy($c);
}
if (!$any) die("skip no backend with multiple render targets");
?>
--FILE--
<?php
/* A temporal G-buffer needs more than four outputs (colour planes + motion
 * vectors + reactive mask). Attachment i writes its own colour; the RGBA8
 * readback converts every format (missing channels read 0), so RG16F comes
 * back (r, g, 0) and R8 (r, 0, 0). Attachment 5 is also sampled onto the
 * swapchain through vio_render_target_texture($rt, 5). */
$W = 16;
function px(string $p, int $x, int $y, int $w): array { $o = ($y*$w+$x)*4; return [ord($p[$o]), ord($p[$o+1]), ord($p[$o+2])]; }
function near(array $a, array $b, int $t = 3): bool { return abs($a[0]-$b[0]) <= $t && abs($a[1]-$b[1]) <= $t && abs($a[2]-$b[2]) <= $t; }

echo "max: ", VIO_MAX_COLOR_ATTACHMENTS, "\n";

function run_backend(string $name): string {
    global $W;
    $ctx = @vio_create($name, ["width" => $W, "height" => $W, "headless" => true, "vsync" => false]);
    if (!$ctx) return "skip (unavailable)";
    if (!vio_supports_feature($ctx, VIO_FEATURE_MRT) || !vio_supports_feature($ctx, VIO_FEATURE_3D_PIPELINE)
        || !vio_supports_feature($ctx, VIO_FEATURE_READ_PIXELS)) {
        vio_destroy($ctx);
        return "skip (no MRT)";
    }
    $fail = [];
    $fmt = $name === 'opengl' ? VIO_SHADER_GLSL_RAW : VIO_SHADER_GLSL;
    $vs = "#version 330 core\nlayout(location=0) in vec3 aPos;\nout vec2 vUv;\nvoid main(){ vUv = aPos.xy * 0.5 + 0.5; gl_Position = vec4(aPos, 1.0); }";
    $cols = [[1.0, 0.0, 0.0], [0.0, 1.0, 0.0], [0.0, 0.0, 1.0], [1.0, 1.0, 0.0], [0.5, 0.25, 1.0], [0.75, 1.0, 1.0], [0.25, 0.5, 0.75], [1.0, 0.5, 0.0]];
    $mrtFs = function (int $n) use ($cols): string {
        $s = "#version 330 core\nin vec2 vUv;\n";
        for ($i = 0; $i < $n; $i++) $s .= "layout(location=$i) out vec4 o$i;\n";
        $s .= "void main(){\n";
        for ($i = 0; $i < $n; $i++) $s .= sprintf("  o%d = vec4(%.4f, %.4f, %.4f, 1.0);\n", $i, ...$cols[$i]);
        return $s . "}\n";
    };
    $fsTex = "#version 330 core\nin vec2 vUv;\nuniform sampler2D u_tex;\nlayout(location=0) out vec4 o;\nvoid main(){ o = texture(u_tex, vUv); }";
    $quad = vio_mesh($ctx, ['vertices' => [-1,-1,0, 1,-1,0, 1,1,0, -1,1,0], 'indices' => [0,1,2, 0,2,3], 'layout' => [VIO_FLOAT3]]);
    $q = fn(float $v): int => (int)round($v * 255);

    // 4 x RGBA16F + RG16F + R8
    $formats = [VIO_FORMAT_RGBA16F, VIO_FORMAT_RGBA16F, VIO_FORMAT_RGBA16F, VIO_FORMAT_RGBA16F, VIO_FORMAT_RG16F, VIO_FORMAT_R8];
    $p6 = vio_pipeline($ctx, ['shader' => vio_shader($ctx, ['vertex' => $vs, 'fragment' => $mrtFs(6), 'format' => $fmt]),
                              'depth_test' => false, 'cull_mode' => VIO_CULL_NONE, 'attachments' => $formats]);
    $rt = vio_render_target($ctx, ['width' => $W, 'height' => $W, 'attachments' => $formats]);
    if (!($rt instanceof VioRenderTarget) || !$p6) { vio_destroy($ctx); return "FAIL\n  6-attachment target / pipeline not created"; }
    vio_begin($ctx);
    vio_bind_render_target($ctx, $rt);
    vio_bind_pipeline($ctx, $p6);
    vio_draw($ctx, $quad);
    vio_unbind_render_target($ctx);
    vio_end($ctx);
    for ($i = 0; $i < 6; $i++) {
        $c = $cols[$i];
        $want = [$q($c[0]), $q($c[1]), $q($c[2])];
        if ($formats[$i] === VIO_FORMAT_RG16F) $want[2] = 0;
        if ($formats[$i] === VIO_FORMAT_R8) { $want[1] = 0; $want[2] = 0; }
        $p = vio_read_render_target($rt, -1, $i);
        if (!$p || strlen($p) !== $W * $W * 4) { $fail[] = "att$i: readback size"; continue; }
        foreach ([[1, 1], [$W - 2, $W - 2], [$W >> 1, $W >> 1]] as [$x, $y]) {
            $got = px($p, $x, $y, $W);
            if (!near($got, $want)) { $fail[] = "att$i ($x,$y) " . json_encode($got) . " want " . json_encode($want); break; }
        }
    }
    $pTex = vio_pipeline($ctx, ['shader' => vio_shader($ctx, ['vertex' => $vs, 'fragment' => $fsTex, 'format' => $fmt]),
                                'depth_test' => false, 'cull_mode' => VIO_CULL_NONE]);
    vio_clear($ctx, 0, 0, 0, 1);
    vio_begin($ctx);
    vio_bind_pipeline($ctx, $pTex);
    vio_set_uniform($ctx, 'u_tex', 0);
    vio_bind_texture($ctx, vio_render_target_texture($rt, 5), 0);
    vio_draw($ctx, $quad);
    vio_end($ctx);
    $got = px(vio_read_pixels($ctx), $W >> 1, $W >> 1, $W);
    if (!near($got, [191, 0, 0])) $fail[] = "att5 sampled " . json_encode($got);

    // eight RGBA8 attachments
    $f8 = array_fill(0, 8, VIO_FORMAT_RGBA8);
    $p8 = vio_pipeline($ctx, ['shader' => vio_shader($ctx, ['vertex' => $vs, 'fragment' => $mrtFs(8), 'format' => $fmt]),
                              'depth_test' => false, 'cull_mode' => VIO_CULL_NONE, 'attachments' => $f8]);
    $rt8 = vio_render_target($ctx, ['width' => $W, 'height' => $W, 'attachments' => $f8]);
    if (!($rt8 instanceof VioRenderTarget) || !$p8) {
        $fail[] = "8-attachment target / pipeline not created";
    } else {
        vio_begin($ctx);
        vio_bind_render_target($ctx, $rt8);
        vio_bind_pipeline($ctx, $p8);
        vio_draw($ctx, $quad);
        vio_unbind_render_target($ctx);
        vio_end($ctx);
        for ($i = 0; $i < 8; $i++) {
            $c = $cols[$i];
            $got = px(vio_read_render_target($rt8, -1, $i), $W >> 1, $W >> 1, $W);
            if (!near($got, [$q($c[0]), $q($c[1]), $q($c[2])])) $fail[] = "rgba8 att$i " . json_encode($got);
        }
    }

    // a ninth attachment is refused
    $r9 = @vio_render_target($ctx, ['width' => $W, 'height' => $W, 'attachments' => array_fill(0, 9, VIO_FORMAT_RGBA8)]);
    if ($r9 !== false) $fail[] = "9 attachments accepted";

    unset($rt, $rt8);
    vio_destroy($ctx);
    return $fail ? "FAIL\n  " . implode("\n  ", $fail) : "OK";
}

foreach (['opengl', 'd3d11', 'd3d12', 'metal', 'vulkan'] as $b) {
    echo "$b: ", run_backend($b), "\n";
}
echo "DONE\n";
?>
--EXPECTF--
max: 8
opengl: %r(OK|skip \(.*\))%r
d3d11: %r(OK|skip \(.*\))%r
d3d12: %r(OK|skip \(.*\))%r
metal: %r(OK|skip \(.*\))%r
vulkan: %r(OK|skip \(.*\))%r
DONE

--TEST--
MRT with mixed attachment formats (RGBA16F / R11G11B10F / RGB10A2 / R8) reads back and samples per attachment; 'mipmaps' => true textures filter down to the average (GAP-PHASE5 Block 10b)
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
/* Every attachment writes (0.5, 0.25, 1.0, 1.0). vio_read_render_target converts
 * each format to RGBA8: missing channels read 0, so R8 comes back (128, 0, 0).
 * Attachment 1 is also sampled onto the swapchain through
 * vio_render_target_texture($rt, 1). A 4x4 black / white pixel checker created
 * with 'mipmaps' => true samples grey at LOD 2 on every backend. */
$W = 16;
function px(string $p, int $x, int $y, int $w): array { $o = ($y*$w+$x)*4; return [ord($p[$o]), ord($p[$o+1]), ord($p[$o+2])]; }
function near(array $a, array $b, int $t = 4): bool { return abs($a[0]-$b[0]) <= $t && abs($a[1]-$b[1]) <= $t && abs($a[2]-$b[2]) <= $t; }

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
    $fsMrt = "#version 330 core\nin vec2 vUv;\nlayout(location=0) out vec4 o0;\nlayout(location=1) out vec4 o1;\nlayout(location=2) out vec4 o2;\nlayout(location=3) out vec4 o3;\n"
           . "void main(){ vec4 c = vec4(0.5, 0.25, 1.0, 1.0); o0 = c; o1 = c; o2 = c; o3 = c; }";
    $fsTex = "#version 330 core\nin vec2 vUv;\nuniform sampler2D u_tex;\nuniform float u_lod;\nlayout(location=0) out vec4 o;\nvoid main(){ o = textureLod(u_tex, vUv, u_lod); }";
    $formats = [VIO_FORMAT_RGBA16F, VIO_FORMAT_R11G11B10F, VIO_FORMAT_RGB10A2, VIO_FORMAT_R8];
    $quad = vio_mesh($ctx, ['vertices' => [-1,-1,0, 1,-1,0, 1,1,0, -1,1,0], 'indices' => [0,1,2, 0,2,3], 'layout' => [VIO_FLOAT3]]);
    $pMrt = vio_pipeline($ctx, ['shader' => vio_shader($ctx, ['vertex' => $vs, 'fragment' => $fsMrt, 'format' => $fmt]),
                                'depth_test' => false, 'cull_mode' => VIO_CULL_NONE, 'attachments' => $formats]);
    $pTex = vio_pipeline($ctx, ['shader' => vio_shader($ctx, ['vertex' => $vs, 'fragment' => $fsTex, 'format' => $fmt]),
                                'depth_test' => false, 'cull_mode' => VIO_CULL_NONE]);
    $rt = vio_render_target($ctx, ['width' => $W, 'height' => $W, 'attachments' => $formats]);
    if (!($rt instanceof VioRenderTarget) || !$pMrt) { vio_destroy($ctx); return "FAIL\n  MRT target / pipeline not created"; }

    vio_begin($ctx);
    vio_bind_render_target($ctx, $rt);
    vio_bind_pipeline($ctx, $pMrt);
    vio_draw($ctx, $quad);
    vio_unbind_render_target($ctx);
    vio_end($ctx);

    $want = [[128, 64, 255], [128, 64, 255], [128, 64, 255], [128, 0, 0]];
    for ($i = 0; $i < 4; $i++) {
        $p = vio_read_render_target($rt, -1, $i);
        if (!$p || strlen($p) !== $W * $W * 4) { $fail[] = "att$i: readback size"; continue; }
        $got = px($p, $W >> 1, $W >> 1, $W);
        if (!near($got, $want[$i])) $fail[] = "att$i readback " . json_encode($got) . " want " . json_encode($want[$i]);
    }

    vio_clear($ctx, 0, 0, 0, 1);
    vio_begin($ctx);
    vio_bind_pipeline($ctx, $pTex);
    vio_set_uniform($ctx, 'u_tex', 0);
    vio_set_uniform($ctx, 'u_lod', 0.0);
    vio_bind_texture($ctx, vio_render_target_texture($rt, 1), 0);
    vio_draw($ctx, $quad);
    vio_end($ctx);
    $got = px(vio_read_pixels($ctx), $W >> 1, $W >> 1, $W);
    if (!near($got, [128, 64, 255])) $fail[] = "att1 sampled " . json_encode($got);

    $checker = '';
    for ($y = 0; $y < 4; $y++) for ($x = 0; $x < 4; $x++) $checker .= (($x + $y) & 1) ? "\xFF\xFF\xFF\xFF" : "\x00\x00\x00\xFF";
    $tex = vio_texture($ctx, ['data' => $checker, 'width' => 4, 'height' => 4, 'mipmaps' => true, 'filter' => VIO_FILTER_LINEAR]);
    foreach ([[0.0, null], [2.0, [128, 128, 128]]] as [$lod, $expect]) {
        vio_clear($ctx, 0, 0, 0, 1);
        vio_begin($ctx);
        vio_bind_pipeline($ctx, $pTex);
        vio_set_uniform($ctx, 'u_tex', 0);
        vio_set_uniform($ctx, 'u_lod', $lod);
        vio_bind_texture($ctx, $tex, 0);
        vio_draw($ctx, $quad);
        vio_end($ctx);
        $p = vio_read_pixels($ctx);
        if ($expect === null) {
            // LOD 0 keeps the checker: neighbouring texels still differ clearly (linear
            // filtering between the 4 texels spread over 16 pixels softens it to ~200 vs ~55).
            $a = px($p, 2, 2, $W)[0]; $b = px($p, 6, 2, $W)[0];
            if (abs($a - $b) < 100) $fail[] = "mip lod 0 lost the checker ($a vs $b)";
        } elseif (!near($got = px($p, $W >> 1, $W >> 1, $W), $expect, 12)) {
            $fail[] = "mip lod $lod " . json_encode($got) . " want " . json_encode($expect);
        }
    }

    unset($rt, $tex);
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

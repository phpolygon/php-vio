--TEST--
vio_texture(['anisotropy' => N]) is accepted (clamped 1..16) and sampling stays correct on every 3D backend
--EXTENSIONS--
vio
--SKIPIF--
<?php
$any = false;
foreach (vio_backends() as $b) {
    if ($b === 'null' || $b === 'vulkan') continue;
    $c = @vio_create($b, ["width" => 8, "height" => 8, "headless" => true, "vsync" => false]);
    if (!$c) continue;
    if (vio_supports_feature($c, VIO_FEATURE_3D_PIPELINE) && vio_supports_feature($c, VIO_FEATURE_READ_PIXELS)) $any = true;
    vio_destroy($c);
}
if (!$any) die("skip no backend with a 3D pipeline + readback");
?>
--FILE--
<?php
/* D3D-VULKAN-GAP-PLAN.md 2.6. Anisotropic filtering is a sampler property
 * (D3D11 FILTER_ANISOTROPIC, D3D12 sampler combo, Vulkan samplerAnisotropy, GL
 * TEXTURE_MAX_ANISOTROPY). A flat, screen-aligned quad samples identically with
 * or without it, so the contract checked here is: every value (including out
 * of range ones) yields a valid texture, and the drawn colour is exact. The
 * anisotropic path itself is exercised on the GPU (a wrong sampler descriptor
 * would fail texture creation / draw validation). */
$W = 16; $H = 16;
function px(string $p, int $x, int $y, int $w): array { $o = ($y*$w+$x)*4; return [ord($p[$o]), ord($p[$o+1]), ord($p[$o+2])]; }
function near(array $a, array $b): bool { return abs($a[0]-$b[0]) <= 3 && abs($a[1]-$b[1]) <= 3 && abs($a[2]-$b[2]) <= 3; }

function run_backend(string $name): string {
    global $W, $H;
    $ctx = @vio_create($name, ["width" => $W, "height" => $H, "headless" => true, "vsync" => false]);
    if (!$ctx) return "skip (unavailable)";
    if (!vio_supports_feature($ctx, VIO_FEATURE_3D_PIPELINE) || !vio_supports_feature($ctx, VIO_FEATURE_READ_PIXELS)) {
        vio_destroy($ctx);
        return "skip (no 3D pipeline)";
    }
    $fmt = $name === 'opengl' ? VIO_SHADER_GLSL_RAW : VIO_SHADER_GLSL;
    $vs = "#version 330 core\nlayout(location=0) in vec3 aPos;\nlayout(location=1) in vec2 aUv;\nout vec2 vUv;\nvoid main(){ vUv = aUv; gl_Position = vec4(aPos, 1.0); }";
    $fs = "#version 330 core\nin vec2 vUv; uniform sampler2D u_tex;\nlayout(location=0) out vec4 o;\nvoid main(){ o = texture(u_tex, vUv); }";
    $pipe = vio_pipeline($ctx, ['shader' => vio_shader($ctx, ['vertex' => $vs, 'fragment' => $fs, 'format' => $fmt]), 'depth_test' => false, 'blend' => VIO_BLEND_NONE]);
    $quad = vio_mesh($ctx, ['vertices' => [-1,-1,0,0,1, 1,-1,0,1,1, 1,1,0,1,0, -1,1,0,0,0], 'indices' => [0,1,2, 0,2,3], 'layout' => [VIO_FLOAT3, VIO_FLOAT2]]);

    $fail = [];
    foreach ([1, 2, 4, 8, 16, 99, -5, 0] as $a) {
        /* 4x4 uniform orange texture, mipmapped so the anisotropic path has LODs to walk. */
        $tex = @vio_texture($ctx, ['data' => str_repeat("\xFF\x80\x00\xFF", 16), 'width' => 4, 'height' => 4,
                                   'filter' => VIO_FILTER_LINEAR, 'mipmaps' => true, 'anisotropy' => $a]);
        if (!$tex) { $fail[] = "anisotropy=$a: texture creation failed"; continue; }
        vio_clear($ctx, 0, 0, 0, 1);
        vio_begin($ctx);
        vio_bind_pipeline($ctx, $pipe);
        vio_set_uniform($ctx, 'u_tex', 0);
        vio_bind_texture($ctx, $tex, 0);
        vio_draw($ctx, $quad);
        vio_end($ctx);
        $c = px(vio_read_pixels($ctx), 8, 8, $W);
        if (!near($c, [255, 128, 0])) $fail[] = "anisotropy=$a: centre " . json_encode($c);
    }
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

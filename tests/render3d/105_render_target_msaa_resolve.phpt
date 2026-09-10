--TEST--
Multisampled render targets resolve: interior pixels exact, an edge pixel is a coverage blend, the resolved texture samples correctly
--EXTENSIONS--
vio
--SKIPIF--
<?php
$any = false;
foreach (vio_backends() as $b) {
    if ($b === 'null') continue;
    $c = @vio_create($b, ["width" => 8, "height" => 8, "headless" => true, "vsync" => false]);
    if (!$c) continue;
    if (vio_supports_feature($c, VIO_FEATURE_RENDER_TARGET_MSAA) && vio_supports_feature($c, VIO_FEATURE_3D_PIPELINE)
        && vio_supports_feature($c, VIO_FEATURE_READ_PIXELS)) $any = true;
    vio_destroy($c);
}
if (!$any) die("skip no backend with multisampled render targets");
?>
--FILE--
<?php
/* D3D-VULKAN-GAP-PLAN.md Phase 3. Only backends that report
 * VIO_FEATURE_RENDER_TARGET_MSAA = 1 are checked (D3D11 implements it via a
 * multisampled colour texture + ResolveSubresource; OpenGL / Metal already
 * had it; D3D12 reports 0 until the PSO carries a sample count).
 *
 * A 64x64 target (samples => 4) is cleared black and a green triangle whose
 * hypotenuse cuts the target diagonally is drawn. After unbind:
 *   - a pixel deep inside the triangle is pure green, one far outside is black
 *   - a pixel ON the diagonal is a blend (coverage-weighted): neither 0 nor 255
 *   - the resolved texture sampled onto the swapchain shows the same interior. */
$W = 32; $H = 32; $S = 64;
function px(string $p, int $x, int $y, int $w): array { $o = ($y*$w+$x)*4; return [ord($p[$o]), ord($p[$o+1]), ord($p[$o+2])]; }
function near(array $a, array $b): bool { return abs($a[0]-$b[0]) <= 3 && abs($a[1]-$b[1]) <= 3 && abs($a[2]-$b[2]) <= 3; }

function run_backend(string $name): string {
    global $W, $H, $S;
    $ctx = @vio_create($name, ["width" => $W, "height" => $H, "headless" => true, "vsync" => false]);
    if (!$ctx) return "skip (unavailable)";
    if (!vio_supports_feature($ctx, VIO_FEATURE_RENDER_TARGET_MSAA) || !vio_supports_feature($ctx, VIO_FEATURE_3D_PIPELINE)
        || !vio_supports_feature($ctx, VIO_FEATURE_READ_PIXELS)) {
        vio_destroy($ctx);
        return "skip (no MSAA render targets)";
    }
    $fail = [];
    $fmt = $name === 'opengl' ? VIO_SHADER_GLSL_RAW : VIO_SHADER_GLSL;
    $vs = "#version 330 core\nlayout(location=0) in vec3 aPos;\nlayout(location=1) in vec2 aUv;\nout vec2 vUv;\nvoid main(){ vUv = aUv; gl_Position = vec4(aPos, 1.0); }";
    $fs_fill = "#version 330 core\nin vec2 vUv;\nlayout(location=0) out vec4 o;\nvoid main(){ o = vec4(0.0, 1.0, 0.0, 1.0); }";
    $fs_tex  = "#version 330 core\nin vec2 vUv; uniform sampler2D u_tex;\nlayout(location=0) out vec4 o;\nvoid main(){ o = texture(u_tex, vUv); }";
    $p_fill = vio_pipeline($ctx, ['shader' => vio_shader($ctx, ['vertex' => $vs, 'fragment' => $fs_fill, 'format' => $fmt]), 'depth_test' => false, 'cull_mode' => VIO_CULL_NONE]);
    $p_tex  = vio_pipeline($ctx, ['shader' => vio_shader($ctx, ['vertex' => $vs, 'fragment' => $fs_tex, 'format' => $fmt]), 'depth_test' => false, 'cull_mode' => VIO_CULL_NONE]);
    /* Triangle covering the lower-left half (hypotenuse from top-left to bottom-right). */
    $tri  = vio_mesh($ctx, ['vertices' => [-1,-1,0,0,0, 1,-1,0,1,0, -1,1,0,0,1], 'layout' => [VIO_FLOAT3, VIO_FLOAT2]]);
    $quad = vio_mesh($ctx, ['vertices' => [-1,-1,0,0,1, 1,-1,0,1,1, 1,1,0,1,0, -1,1,0,0,0], 'indices' => [0,1,2, 0,2,3], 'layout' => [VIO_FLOAT3, VIO_FLOAT2]]);

    $rt = vio_render_target($ctx, ['width' => $S, 'height' => $S, 'samples' => 4]);
    if (!($rt instanceof VioRenderTarget)) { vio_destroy($ctx); return "FAIL\n  MSAA render target not created"; }

    vio_begin($ctx);
    vio_bind_render_target($ctx, $rt);
    vio_clear($ctx, 0, 0, 0, 1);
    vio_bind_pipeline($ctx, $p_fill);
    vio_draw($ctx, $tri);
    vio_unbind_render_target($ctx);
    vio_end($ctx);

    $p = vio_read_render_target($rt);
    if (!$p || strlen($p) !== $S * $S * 4) { vio_destroy($ctx); return "FAIL\n  readback size"; }
    /* Image is top-down: the triangle occupies x + (S-1-y) < S, i.e. below the diagonal. */
    if (!near(px($p, 8, $S - 8, $S), [0, 255, 0]))  $fail[] = "interior not green " . json_encode(px($p, 8, $S - 8, $S));
    if (!near(px($p, $S - 8, 8, $S), [0, 0, 0]))    $fail[] = "exterior not black " . json_encode(px($p, $S - 8, 8, $S));
    /* Walk the diagonal and require at least one partially covered pixel. */
    $blend = false;
    for ($i = 2; $i < $S - 2; $i++) {
        foreach ([[$i, $i], [$i, $i - 1], [$i - 1, $i]] as [$x, $y]) {
            $g = px($p, $x, $y, $S)[1];
            if ($g > 20 && $g < 235) { $blend = true; break 2; }
        }
    }
    if (!$blend) $fail[] = "no coverage-blended edge pixel (resolve missing?)";

    /* Sample the resolved texture onto the swapchain. */
    $tex = vio_render_target_texture($rt);
    vio_clear($ctx, 0, 0, 0, 1);
    vio_begin($ctx);
    vio_bind_pipeline($ctx, $p_tex);
    vio_set_uniform($ctx, 'u_tex', 0);
    vio_bind_texture($ctx, $tex, 0);
    vio_draw($ctx, $quad);
    vio_end($ctx);
    /* Render-target textures are row-0-at-bottom on OpenGL and row-0-at-top on
     * D3D / Metal, so the sampled triangle may appear vertically flipped. Probe
     * the left-middle (inside either way) and right-middle (outside either way). */
    $q = vio_read_pixels($ctx);
    if (!near(px($q, 4, $H >> 1, $W), [0, 255, 0]))      $fail[] = "sampled interior not green " . json_encode(px($q, 4, $H >> 1, $W));
    if (!near(px($q, $W - 4, $H >> 1, $W), [0, 0, 0]))   $fail[] = "sampled exterior not black " . json_encode(px($q, $W - 4, $H >> 1, $W));

    unset($tex, $rt);
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

--TEST--
Texture sampler state: 'filter' (NEAREST / LINEAR) and 'wrap' (REPEAT / CLAMP / MIRROR) are honoured on every 3D backend
--EXTENSIONS--
vio
--SKIPIF--
<?php
$any = false;
foreach (vio_backends() as $b) {
    if ($b === 'null') continue;
    $c = @vio_create($b, ["width" => 8, "height" => 8, "headless" => true, "vsync" => false]);
    if (!$c) continue;
    if (vio_supports_feature($c, VIO_FEATURE_3D_PIPELINE) && vio_supports_feature($c, VIO_FEATURE_READ_PIXELS)) $any = true;
    vio_destroy($c);
}
if (!$any) die("skip no backend with a 3D pipeline + readback");
?>
--FILE--
<?php
/* D3D-VULKAN-GAP-PLAN.md Phase 1. D3D12 used to bind eight STATIC LINEAR/WRAP
 * samplers, so a texture's filter / wrap were silently ignored there. This
 * test draws a 2x2 texture (red, green / blue, white) magnified 16x and reads
 * pixels back:
 *   - NEAREST: the pixel just left of the red|green seam is pure red;
 *     LINEAR: it is a red/green blend.
 *   - With UVs running 0..2 horizontally and NEAREST: REPEAT shows red again
 *     at u=1.25, CLAMP shows the edge texel (green), MIRROR shows green at
 *     u=1.25 and red at u=1.75.
 * Runs on every available backend with a 3D pipeline (opengl / d3d11 / d3d12 /
 * metal) so the contract is cross-backend, not just the D3D12 fix. */
$W = 32; $H = 32;

function px(string $p, int $x, int $y, int $w): array { $o = ($y*$w+$x)*4; return [ord($p[$o]), ord($p[$o+1]), ord($p[$o+2])]; }
function near(array $a, array $b, int $tol = 8): bool { return abs($a[0]-$b[0]) <= $tol && abs($a[1]-$b[1]) <= $tol && abs($a[2]-$b[2]) <= $tol; }

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
    /* v=0 at the bottom (GL convention) so texture row 0 lands at the TOP (same quad as test 093). */
    $quad1 = vio_mesh($ctx, ['vertices' => [-1,-1,0,0,1, 1,-1,0,1,1, 1,1,0,1,0, -1,1,0,0,0], 'indices' => [0,1,2, 0,2,3], 'layout' => [VIO_FLOAT3, VIO_FLOAT2]]);
    $quad2 = vio_mesh($ctx, ['vertices' => [-1,-1,0,0,1, 1,-1,0,2,1, 1,1,0,2,0, -1,1,0,0,0], 'indices' => [0,1,2, 0,2,3], 'layout' => [VIO_FLOAT3, VIO_FLOAT2]]);

    $data = "\xFF\x00\x00\xFF" . "\x00\xFF\x00\xFF" . "\x00\x00\xFF\xFF" . "\xFF\xFF\xFF\xFF";
    $draw = function ($tex, $mesh) use ($ctx, $pipe) {
        vio_clear($ctx, 0, 0, 0, 1);
        vio_begin($ctx);
        vio_bind_pipeline($ctx, $pipe);
        vio_set_uniform($ctx, 'u_tex', 0);
        vio_bind_texture($ctx, $tex, 0);
        vio_draw($ctx, $mesh);
        vio_end($ctx);
        return vio_read_pixels($ctx);
    };

    $fail = [];
    // --- filter ---
    $p = $draw(vio_texture($ctx, ['data' => $data, 'width' => 2, 'height' => 2, 'filter' => VIO_FILTER_NEAREST]), $quad1);
    if (!near(px($p, 4, 4, $W), [255, 0, 0]))   $fail[] = "nearest texel0 " . json_encode(px($p, 4, 4, $W));
    if (!near(px($p, 15, 8, $W), [255, 0, 0]))  $fail[] = "nearest seam " . json_encode(px($p, 15, 8, $W));
    if (!near(px($p, 27, 27, $W), [255, 255, 255])) $fail[] = "nearest texel3 " . json_encode(px($p, 27, 27, $W));
    $p = $draw(vio_texture($ctx, ['data' => $data, 'width' => 2, 'height' => 2, 'filter' => VIO_FILTER_LINEAR]), $quad1);
    $s = px($p, 15, 8, $W);
    if (!($s[0] > 90 && $s[0] < 170 && $s[1] > 90 && $s[1] < 170)) $fail[] = "linear seam not blended " . json_encode($s);
    // --- wrap (NEAREST so texel boundaries stay crisp; u runs 0..2) ---
    $p = $draw(vio_texture($ctx, ['data' => $data, 'width' => 2, 'height' => 2, 'filter' => VIO_FILTER_NEAREST, 'wrap' => VIO_WRAP_REPEAT]), $quad2);
    if (!near(px($p, 20, 4, $W), [255, 0, 0]))  $fail[] = "repeat u=1.25 " . json_encode(px($p, 20, 4, $W));
    $p = $draw(vio_texture($ctx, ['data' => $data, 'width' => 2, 'height' => 2, 'filter' => VIO_FILTER_NEAREST, 'wrap' => VIO_WRAP_CLAMP]), $quad2);
    if (!near(px($p, 20, 4, $W), [0, 255, 0]))  $fail[] = "clamp u=1.25 " . json_encode(px($p, 20, 4, $W));
    if (!near(px($p, 28, 4, $W), [0, 255, 0]))  $fail[] = "clamp u=1.75 " . json_encode(px($p, 28, 4, $W));
    $p = $draw(vio_texture($ctx, ['data' => $data, 'width' => 2, 'height' => 2, 'filter' => VIO_FILTER_NEAREST, 'wrap' => VIO_WRAP_MIRROR]), $quad2);
    if (!near(px($p, 20, 4, $W), [0, 255, 0]))  $fail[] = "mirror u=1.25 " . json_encode(px($p, 20, 4, $W));
    if (!near(px($p, 28, 4, $W), [255, 0, 0]))  $fail[] = "mirror u=1.75 " . json_encode(px($p, 28, 4, $W));

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

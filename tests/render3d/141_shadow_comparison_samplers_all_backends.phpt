--TEST--
Comparison sampling: sampler2DShadow, sampler2DArrayShadow and samplerCubeShadow return the depth test result (ref <= stored depth) on every backend; the same depth texture still reads raw through sampler2D, and the 2D batch samples normally afterwards
--EXTENSIONS--
vio
--SKIPIF--
<?php
$any = false;
foreach (vio_backends() as $b) {
    if ($b === 'null') continue;
    $c = @vio_create($b, ["width" => 8, "height" => 8, "headless" => true, "vsync" => false]);
    if (!$c) continue;
    if (vio_supports_feature($c, VIO_FEATURE_RENDER_TARGET_DEPTH) && vio_supports_feature($c, VIO_FEATURE_3D_PIPELINE)) $any = true;
    vio_destroy($c);
}
if (!$any) die("skip no backend with depth targets");
?>
--FILE--
<?php
/* OpenGL used to sample sampler*Shadow without GL_TEXTURE_COMPARE_MODE, which
 * is undefined; it now binds a comparison sampler object (LEQUAL, linear) to
 * the units a shadow sampler of the current program reads, for that draw only.
 * The other backends already pick a comparison sampler from the shader's
 * sampler type. Every depth here is uniform over the target, so the checks do
 * not depend on the row order. Stored depth = (z + 1) / 2 on GL / D3D / Vulkan
 * (vio's clip-space fixup), z itself on Metal (NDC z is 0..1 there).
 *   A. sampler2DShadow: depth 0.5, ref 0.4 -> 1, ref 0.6 -> 0
 *   B. sampler2DArrayShadow: layer depths 0.5 / 0.75, refs around each
 *   C. samplerCubeShadow (VIO_FEATURE_RENDER_TARGET_LAYERED): a depth per face
 *   D. in the same frame, sampler2D on the same texture reads the raw depth,
 *      and a 2D sprite on unit 0 is not compared */
$W = 16;
function px(string $p, int $x, int $y, int $w): array { $o = ($y*$w+$x)*4; return [ord($p[$o]), ord($p[$o+1]), ord($p[$o+2])]; }
function near(array $a, array $b, int $tol = 4): bool { return abs($a[0]-$b[0]) <= $tol && abs($a[1]-$b[1]) <= $tol && abs($a[2]-$b[2]) <= $tol; }

function run_backend(string $name): string {
    global $W;
    $ctx = @vio_create($name, ["width" => $W, "height" => $W, "headless" => true, "vsync" => false]);
    if (!$ctx) return "skip (unavailable)";
    if (!vio_supports_feature($ctx, VIO_FEATURE_RENDER_TARGET_DEPTH) || !vio_supports_feature($ctx, VIO_FEATURE_3D_PIPELINE)) {
        vio_destroy($ctx);
        return "skip (no depth targets)";
    }
    $fail = [];
    $layered = vio_supports_feature($ctx, VIO_FEATURE_RENDER_TARGET_LAYERED);
    $depthOf = fn(float $z): float => vio_backend_name($ctx) === 'metal' ? $z : ($z + 1.0) / 2.0;   /* z stays in 0..1 throughout */
    $vs = "#version 450\nlayout(location=0) in vec3 aPos;\nuniform float u_z;\nvoid main(){ gl_Position = vec4(aPos.xy, u_z, 1.0); }";
    $fsDepth = "#version 450\nvoid main(){ }";
    $full = vio_mesh($ctx, ['vertices' => [-1,-1,0, 1,-1,0, 1,1,0, -1,1,0], 'indices' => [0,1,2, 0,2,3], 'layout' => [VIO_FLOAT3]]);
    $base = ['depth_test' => false, 'cull_mode' => VIO_CULL_NONE];
    $pDepth = vio_pipeline($ctx, ['shader' => vio_shader($ctx, ['vertex' => $vs, 'fragment' => $fsDepth]),
                                  'depth_test' => true, 'depth_write' => true, 'cull_mode' => VIO_CULL_NONE]);
    /* Fill a target (one layer / face at a time) with full-screen depth. */
    $fill = function ($rt, array $zs) use ($ctx, $pDepth, $full) {
        vio_begin($ctx);
        foreach ($zs as $l => $z) {
            if ($l === -1) vio_bind_render_target($ctx, $rt); else vio_bind_render_target($ctx, $rt, $l);
            vio_clear($ctx, 0, 0, 0, 1);
            vio_bind_pipeline($ctx, $pDepth);
            vio_set_uniform($ctx, 'u_z', $z);
            vio_draw($ctx, $full);
        }
        vio_unbind_render_target($ctx);
        vio_end($ctx);
    };
    /* One full-screen draw into the swapchain; returns the centre pixel. */
    $sample = function ($pipe, callable $bind, array $uniforms) use ($ctx, $full, $W): array {
        vio_clear($ctx, 0, 0, 0, 1);
        vio_begin($ctx);
        vio_bind_pipeline($ctx, $pipe);
        $bind();
        foreach ($uniforms as $k => $v) vio_set_uniform($ctx, $k, $v);
        vio_set_uniform($ctx, 'u_z', 0.0);
        vio_draw($ctx, $full);
        vio_end($ctx);
        return px(vio_read_pixels($ctx), $W >> 1, $W >> 1, $W);
    };

    /* ---- A: sampler2DShadow ---------------------------------------------- */
    $fs2 = "#version 450\nuniform sampler2DShadow u_sh; uniform float u_ref;\nlayout(location=0) out vec4 o;\n"
         . "void main(){ float s = texture(u_sh, vec3(0.5, 0.5, u_ref)); o = vec4(s, s, s, 1.0); }";
    $p2 = vio_pipeline($ctx, ['shader' => vio_shader($ctx, ['vertex' => $vs, 'fragment' => $fs2])] + $base);
    $rt = vio_render_target($ctx, ['width' => $W, 'height' => $W, 'depth_only' => true]);
    $fill($rt, [-1 => 0.5]);
    $dA = $depthOf(0.5);   /* 0.75, Metal 0.5 */
    $tex = vio_render_target_texture($rt);
    foreach ([[$dA - 0.1, 255], [$dA + 0.1, 0]] as [$ref, $want]) {
        $got = $sample($p2, function () use ($ctx, $tex) { vio_set_uniform($ctx, 'u_sh', 0); vio_bind_texture($ctx, $tex, 0); }, ['u_ref' => (float)$ref]);
        if (!near($got, [$want, $want, $want])) $fail[] = "A: sampler2DShadow ref $ref " . json_encode($got) . " want $want";
    }

    /* ---- D: same texture raw, then a 2D sprite, in one frame ------------- */
    $fsRaw = "#version 450\nuniform sampler2D u_tex;\nlayout(location=0) out vec4 o;\nvoid main(){ float d = texture(u_tex, vec2(0.5)).r; o = vec4(d, d, d, 1.0); }";
    $pRaw = vio_pipeline($ctx, ['shader' => vio_shader($ctx, ['vertex' => $vs, 'fragment' => $fsRaw])] + $base);
    $red = vio_texture($ctx, ['data' => str_repeat("\xFF\x00\x00\xFF", 16), 'width' => 4, 'height' => 4]);
    vio_clear($ctx, 0, 0, 0, 1);
    vio_begin($ctx);
    vio_bind_pipeline($ctx, $p2);
    vio_set_uniform($ctx, 'u_sh', 0);
    vio_bind_texture($ctx, $tex, 0);
    vio_set_uniform($ctx, 'u_ref', $dA - 0.1);
    vio_set_uniform($ctx, 'u_z', 0.0);
    vio_draw($ctx, $full);
    vio_viewport($ctx, 0, 0, $W >> 1, $W);   /* raw read: left half */
    vio_bind_pipeline($ctx, $pRaw);
    vio_set_uniform($ctx, 'u_tex', 0);
    vio_bind_texture($ctx, $tex, 0);
    vio_set_uniform($ctx, 'u_z', 0.0);
    vio_draw($ctx, $full);
    vio_viewport($ctx, 0, 0, $W, $W);
    vio_sprite($ctx, $red, ['x' => 12, 'y' => 12, 'width' => 4, 'height' => 4]);
    vio_draw_2d($ctx);
    vio_end($ctx);
    $img = vio_read_pixels($ctx);
    $raw = null;
    $g = (int)round($dA * 255);
    foreach ([[3, 3], [3, 12]] as [$x, $y]) { $c = px($img, $x, $y, $W); if (near($c, [$g, $g, $g])) $raw = $c; }
    if ($raw === null) $fail[] = "D: raw sampler2D read of the depth texture " . json_encode([px($img, 3, 3, $W), px($img, 3, 12, $W)]) . " want $g";
    $sprite = false;
    foreach ([[13, 13], [13, 2]] as [$x, $y]) if (near(px($img, $x, $y, $W), [255, 0, 0])) $sprite = true;
    if (!$sprite) $fail[] = "D: 2D sprite after the shadow draw is not red " . json_encode([px($img, 13, 13, $W), px($img, 13, 2, $W)]);

    /* ---- B: sampler2DArrayShadow ---------------------------------------- */
    if ($layered) {
        $fsA = "#version 450\nuniform sampler2DArrayShadow u_sh; uniform float u_ref; uniform float u_layer;\nlayout(location=0) out vec4 o;\n"
             . "void main(){ float s = texture(u_sh, vec4(0.5, 0.5, u_layer, u_ref)); o = vec4(s, s, s, 1.0); }";
        $pA = vio_pipeline($ctx, ['shader' => vio_shader($ctx, ['vertex' => $vs, 'fragment' => $fsA])] + $base);
        $arr = vio_render_target($ctx, ['width' => $W, 'height' => $W, 'layers' => 2, 'depth_only' => true]);
        $fill($arr, [0 => 0.0, 1 => 0.5]);
        $d0 = $depthOf(0.0); $d1 = $depthOf(0.5);
        $atex = vio_render_target_texture($arr);
        foreach ([[0, $d0 - 0.05, 255], [0, $d0 + 0.05, 0], [1, $d1 - 0.05, 255], [1, $d1 + 0.05, 0]] as [$layer, $ref, $want]) {
            $got = $sample($pA, function () use ($ctx, $atex) { vio_set_uniform($ctx, 'u_sh', 0); vio_bind_texture($ctx, $atex, 0); },
                           ['u_ref' => $ref, 'u_layer' => (float)$layer]);
            if (!near($got, [$want, $want, $want])) $fail[] = "B: layer $layer ref $ref " . json_encode($got) . " want $want";
        }

        /* ---- C: samplerCubeShadow ---------------------------------------- */
        $fsC = "#version 450\nuniform samplerCubeShadow u_sh; uniform float u_ref; uniform vec3 u_dir;\nlayout(location=0) out vec4 o;\n"
             . "void main(){ float s = texture(u_sh, vec4(u_dir, u_ref)); o = vec4(s, s, s, 1.0); }";
        $pC = vio_pipeline($ctx, ['shader' => vio_shader($ctx, ['vertex' => $vs, 'fragment' => $fsC])] + $base);
        $cube = vio_render_target($ctx, ['cube' => true, 'size' => $W, 'depth_only' => true]);
        $zs = []; for ($f = 0; $f < 6; $f++) $zs[$f] = 0.1 + 0.15 * $f;   /* z 0.1 .. 0.85: inside 0..1, so Metal (which clips z < 0) keeps every face */
        $fill($cube, $zs);
        $cm = vio_render_target_cubemap($cube);
        $dirs = [[1,0,0], [-1,0,0], [0,1,0], [0,-1,0], [0,0,1], [0,0,-1]];
        for ($f = 0; $f < 6; $f++) {
            $d = $depthOf($zs[$f]);
            foreach ([[$d - 0.05, 255], [$d + 0.05, 0]] as [$ref, $want]) {
                $got = $sample($pC, function () use ($ctx, $cm) { vio_set_uniform($ctx, 'u_sh', 0); vio_bind_cubemap($ctx, $cm, 0); },
                               ['u_ref' => $ref, 'u_dir' => $dirs[$f]]);
                if (!near($got, [$want, $want, $want])) $fail[] = "C: face $f ref " . round($ref, 2) . " " . json_encode($got) . " want $want";
            }
        }
    }

    unset($tex, $atex, $cm, $rt, $arr, $cube);
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

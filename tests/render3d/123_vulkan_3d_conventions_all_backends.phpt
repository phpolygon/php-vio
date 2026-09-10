--TEST--
3D conventions on every backend (GAP-PHASE5 Block 10): depth test, back-face culling, uniforms per stage, texture V orientation, instancing, render-target orientation, depth-only targets - Vulkan matches D3D
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
if (!$any) die("skip no 3D backend");
?>
--FILE--
<?php
/* Screen-space checks read vio_read_pixels (top-down on every backend). Render
 * targets follow the documented split: OpenGL stores row 0 at the bottom, the
 * other backends at the top (NDC +Y = row 0), so a target sampled with GL-style
 * UVs comes out flipped there. Vulkan must behave exactly like D3D11/D3D12. */
$W = 32;
function px(string $p, int $x, int $y, int $w): array { $o = ($y * $w + $x) * 4; return [ord($p[$o]), ord($p[$o + 1]), ord($p[$o + 2])]; }
function near(array $c, array $want, int $tol = 8): bool { return abs($c[0]-$want[0]) <= $tol && abs($c[1]-$want[1]) <= $tol && abs($c[2]-$want[2]) <= $tol; }

function run_backend(string $name): string {
    global $W;
    $ctx = @vio_create($name, ['width' => $W, 'height' => $W, 'headless' => true, 'vsync' => false]);
    if (!$ctx) return "skip (unavailable)";
    if (!vio_supports_feature($ctx, VIO_FEATURE_3D_PIPELINE) || !vio_supports_feature($ctx, VIO_FEATURE_READ_PIXELS)) {
        vio_destroy($ctx);
        return "skip (no 3D pipeline)";
    }
    $gl = $name === 'opengl';
    $fmt = $gl ? VIO_SHADER_GLSL_RAW : VIO_SHADER_GLSL;
    $fail = [];
    $vs  = "#version 330 core\nlayout(location=0) in vec3 aPos;\nuniform vec2 u_offset;\nvoid main(){ gl_Position = vec4(aPos.xy + u_offset, aPos.z, 1.0); }";
    $fs  = "#version 330 core\nuniform vec4 u_color;\nlayout(location=0) out vec4 o;\nvoid main(){ o = u_color; }";
    $vsT = "#version 330 core\nlayout(location=0) in vec3 aPos;\nout vec2 v_uv;\nvoid main(){ v_uv = aPos.xy * 0.5 + 0.5; gl_Position = vec4(aPos, 1.0); }";
    $fsT = "#version 330 core\nin vec2 v_uv;\nuniform sampler2D u_tex;\nlayout(location=0) out vec4 o;\nvoid main(){ o = texture(u_tex, v_uv); }";
    $vsI = "#version 330 core\nlayout(location=0) in vec3 aPos;\nlayout(location=3) in mat4 aModel;\nvoid main(){ gl_Position = aModel * vec4(aPos, 1.0); }";
    $sh  = vio_shader($ctx, ['vertex' => $vs,  'fragment' => $fs,  'format' => $fmt]);
    $shT = vio_shader($ctx, ['vertex' => $vsT, 'fragment' => $fsT, 'format' => $fmt]);
    $shI = vio_shader($ctx, ['vertex' => $vsI, 'fragment' => $fs,  'format' => $fmt]);
    $pDepth = vio_pipeline($ctx, ['shader' => $sh, 'depth_test' => true, 'cull_mode' => VIO_CULL_BACK]);
    $pFlat  = vio_pipeline($ctx, ['shader' => $sh, 'depth_test' => false]);
    $pTex   = vio_pipeline($ctx, ['shader' => $shT, 'depth_test' => false]);
    $pInst  = vio_pipeline($ctx, ['shader' => $shI, 'depth_test' => false]);
    $quad = fn(float $z) => vio_mesh($ctx, ['vertices' => [-0.25,-0.25,$z, 0.25,-0.25,$z, 0.25,0.25,$z, -0.25,0.25,$z], 'indices' => [0,1,2, 0,2,3], 'layout' => [VIO_FLOAT3]]);
    $near = $quad(0.0);
    $far  = $quad(0.5);
    $cw   = vio_mesh($ctx, ['vertices' => [-0.25,-0.25,0.0, 0.25,0.25,0.0, 0.25,-0.25,0.0], 'layout' => [VIO_FLOAT3]]);
    $full = vio_mesh($ctx, ['vertices' => [-1,-1,0, 1,-1,0, 1,1,0, -1,1,0], 'indices' => [0,1,2, 0,2,3], 'layout' => [VIO_FLOAT3]]);
    $top  = vio_mesh($ctx, ['vertices' => [-1,0,0, 1,0,0, 1,1,0, -1,1,0], 'indices' => [0,1,2, 0,2,3], 'layout' => [VIO_FLOAT3]]);

    // Depth: near red first, far green afterwards stays hidden. Culling: a clockwise triangle vanishes.
    vio_clear($ctx, 0, 0, 0, 1);
    vio_begin($ctx);
    vio_bind_pipeline($ctx, $pDepth);
    vio_set_uniform($ctx, 'u_offset', [-0.5, 0.5]);
    vio_set_uniform($ctx, 'u_color', [1.0, 0.0, 0.0, 1.0]);
    vio_draw($ctx, $near);
    vio_set_uniform($ctx, 'u_color', [0.0, 1.0, 0.0, 1.0]);
    vio_draw($ctx, $far);
    vio_set_uniform($ctx, 'u_offset', [0.5, 0.5]);
    vio_set_uniform($ctx, 'u_color', [1.0, 1.0, 0.0, 1.0]);
    vio_draw($ctx, $cw);
    vio_set_uniform($ctx, 'u_offset', [0.5, -0.5]);   // uniforms land per draw: bottom-right blue
    vio_set_uniform($ctx, 'u_color', [0.0, 0.0, 1.0, 1.0]);
    vio_draw($ctx, $near);
    vio_end($ctx);
    $p = vio_read_pixels($ctx);
    if (!near($c = px($p, 8, 8, $W), [255, 0, 0])) $fail[] = "depth: top-left " . json_encode($c);
    if (!near($c = px($p, 24, 10, $W), [0, 0, 0])) $fail[] = "cull: top-right " . json_encode($c);
    if (!near($c = px($p, 24, 24, $W), [0, 0, 255])) $fail[] = "uniforms: bottom-right " . json_encode($c);

    // Texture rows: data row 0 (red | green) samples at v = 0, i.e. the bottom of the screen with these UVs.
    $tex = vio_texture($ctx, ['data' => "\xFF\x00\x00\xFF\x00\xFF\x00\xFF\x00\x00\xFF\xFF\xFF\xFF\xFF\xFF", 'width' => 2, 'height' => 2, 'filter' => VIO_FILTER_NEAREST]);
    vio_clear($ctx, 0, 0, 0, 1);
    vio_begin($ctx);
    vio_bind_pipeline($ctx, $pTex);
    vio_set_uniform($ctx, 'u_tex', 0);
    vio_bind_texture($ctx, $tex, 0);
    vio_draw($ctx, $full);
    vio_end($ctx);
    $p = vio_read_pixels($ctx);
    if (!near($c = px($p, 4, 28, $W), [255, 0, 0])) $fail[] = "texture: bottom-left " . json_encode($c);
    if (!near($c = px($p, 28, 4, $W), [255, 255, 255])) $fail[] = "texture: top-right " . json_encode($c);

    // Instancing: two packed mat4 instances left and right, nothing in between.
    $mats = pack('f*', 0.25,0,0,0, 0,0.25,0,0, 0,0,1,0, -0.5,0,0,1,  0.25,0,0,0, 0,0.25,0,0, 0,0,1,0, 0.5,0,0,1);
    vio_clear($ctx, 0, 0, 0, 1);
    vio_begin($ctx);
    vio_bind_pipeline($ctx, $pInst);
    vio_set_uniform($ctx, 'u_color', [0.0, 1.0, 0.0, 1.0]);
    vio_draw_instanced($ctx, $full, $mats, 2);
    vio_end($ctx);
    $p = vio_read_pixels($ctx);
    if (!near($c = px($p, 8, 16, $W), [0, 255, 0]) || !near(px($p, 24, 16, $W), [0, 255, 0]) || !near(px($p, 16, 16, $W), [0, 0, 0])) {
        $fail[] = "instancing: " . json_encode([px($p, 8, 16, $W), px($p, 16, 16, $W), px($p, 24, 16, $W)]);
    }

    // Render target: the top NDC half red; readback row 0 / sampled orientation per convention.
    if (vio_supports_feature($ctx, VIO_FEATURE_RENDER_TARGET)) {
        $rt = vio_render_target($ctx, ['width' => $W, 'height' => $W]);
        vio_clear($ctx, 0, 0, 0, 1);
        vio_begin($ctx);
        vio_bind_render_target($ctx, $rt);
        vio_bind_pipeline($ctx, $pFlat);
        vio_set_uniform($ctx, 'u_offset', [0.0, 0.0]);
        vio_set_uniform($ctx, 'u_color', [1.0, 0.0, 0.0, 1.0]);
        vio_draw($ctx, $top);
        vio_unbind_render_target($ctx);
        vio_end($ctx);
        $rb = vio_read_render_target($rt);
        if (!$gl) {
            if (!$rb || !near($c = px($rb, 16, 2, $W), [255, 0, 0])) $fail[] = "rt readback row 0 " . json_encode($c ?? null);
        }
        vio_clear($ctx, 0, 0, 0, 1);
        vio_begin($ctx);
        vio_bind_pipeline($ctx, $pTex);
        vio_set_uniform($ctx, 'u_tex', 0);
        vio_bind_texture($ctx, vio_render_target_texture($rt), 0);
        vio_draw($ctx, $full);
        vio_end($ctx);
        $p = vio_read_pixels($ctx);
        $redRow = $gl ? 4 : 28;   // GL-style UVs: flipped on the top-left-origin backends
        if (!near($c = px($p, 16, $redRow, $W), [255, 0, 0])) $fail[] = "rt sampled row $redRow " . json_encode($c);
    }

    // Depth-only target: a quad at depth 0.5 (NDC z 0) over a cleared 1.0 target.
    if (vio_supports_feature($ctx, VIO_FEATURE_RENDER_TARGET_DEPTH)) {
        $dt = vio_render_target($ctx, ['width' => $W, 'height' => $W, 'depth_only' => true]);
        $pShadow = vio_pipeline($ctx, ['shader' => $sh, 'depth_test' => true]);
        vio_clear($ctx, 0, 0, 0, 1);
        vio_begin($ctx);
        vio_bind_render_target($ctx, $dt);
        vio_bind_pipeline($ctx, $pShadow);
        vio_set_uniform($ctx, 'u_offset', [0.0, 0.0]);
        vio_set_uniform($ctx, 'u_color', [1.0, 1.0, 1.0, 1.0]);
        vio_draw($ctx, $near);
        vio_unbind_render_target($ctx);
        vio_end($ctx);
        $rb = vio_read_render_target($dt);
        if ($rb !== false) {
            $mid = px($rb, 16, 16, $W)[0];
            $corner = px($rb, 1, 1, $W)[0];
            if (!($mid > 100 && $mid < 155 && $corner > 245)) $fail[] = "depth target: centre $mid corner $corner";
        }
    }

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

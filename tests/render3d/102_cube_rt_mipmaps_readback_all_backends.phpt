--TEST--
Cube render targets on every backend: per-face bind, vio_read_render_target(face), vio_generate_mipmaps + textureLod, borrowed VioCubemap
--EXTENSIONS--
vio
--SKIPIF--
<?php
$any = false;
foreach (vio_backends() as $b) {
    if ($b === 'null' || $b === 'vulkan') continue;
    $c = @vio_create($b, ["width" => 8, "height" => 8, "headless" => true, "vsync" => false]);
    if (!$c) continue;
    if (vio_supports_feature($c, VIO_FEATURE_RENDER_TARGET_CUBE) && vio_supports_feature($c, VIO_FEATURE_3D_PIPELINE)
        && vio_supports_feature($c, VIO_FEATURE_READ_PIXELS)) $any = true;
    vio_destroy($c);
}
if (!$any) die("skip no backend with cube render targets");
?>
--FILE--
<?php
/* D3D-VULKAN-GAP-PLAN.md Phase 2.2 / 2.3 / 2.4. Test 090 covers the same API
 * on 'auto' only; this one walks every available backend so the D3D11 and
 * D3D12 implementations (previously VIO_FEATURE_RENDER_TARGET_CUBE = 0) are
 * held to the OpenGL / Metal contract:
 *   - six faces drawn with distinct colours, each read back with
 *     vio_read_render_target($rt, $face)
 *   - the generated mip chain: textureLod at the last level equals level 0 for
 *     a uniform face
 *   - the borrowed VioCubemap wrapper samples the RT
 *   - vio_generate_mipmaps on a 'mipmaps' => true texture and CPU cubemap */
$W = 32; $H = 32; $SIZE = 16;
function px(string $p, int $x, int $y, int $w): array { $o = ($y*$w+$x)*4; return [ord($p[$o]), ord($p[$o+1]), ord($p[$o+2])]; }
function near(array $a, array $b): bool { return abs($a[0]-$b[0]) <= 3 && abs($a[1]-$b[1]) <= 3 && abs($a[2]-$b[2]) <= 3; }

function run_backend(string $name): string {
    global $W, $H, $SIZE;
    $ctx = @vio_create($name, ["width" => $W, "height" => $H, "headless" => true, "vsync" => false]);
    if (!$ctx) return "skip (unavailable)";
    if (!vio_supports_feature($ctx, VIO_FEATURE_RENDER_TARGET_CUBE) || !vio_supports_feature($ctx, VIO_FEATURE_3D_PIPELINE)
        || !vio_supports_feature($ctx, VIO_FEATURE_READ_PIXELS)) {
        vio_destroy($ctx);
        return "skip (no cube render targets)";
    }
    $fail = [];
    $fmt = $name === 'opengl' ? VIO_SHADER_GLSL_RAW : VIO_SHADER_GLSL;
    $vs_fill = "#version 330 core\nlayout(location=0) in vec3 aPos;\nvoid main(){ gl_Position = vec4(aPos, 1.0); }";
    $fs_fill = "#version 330 core\nuniform vec4 u_color;\nlayout(location=0) out vec4 o;\nvoid main(){ o = u_color; }";
    $fs_cube = "#version 330 core\nuniform samplerCube u_env; uniform vec3 u_dir; uniform float u_lod;\n"
             . "layout(location=0) out vec4 o;\nvoid main(){ o = textureLod(u_env, u_dir, u_lod); }";
    $p_fill = vio_pipeline($ctx, ['shader' => vio_shader($ctx, ['vertex' => $vs_fill, 'fragment' => $fs_fill, 'format' => $fmt]), 'depth_test' => false]);
    $p_cube = vio_pipeline($ctx, ['shader' => vio_shader($ctx, ['vertex' => $vs_fill, 'fragment' => $fs_cube, 'format' => $fmt]), 'depth_test' => false]);
    $quad = vio_mesh($ctx, ['vertices' => [-1,-1,0, 1,-1,0, 1,1,0, -1,1,0], 'indices' => [0,1,2, 0,2,3], 'layout' => [VIO_FLOAT3]]);

    $rt = vio_render_target($ctx, ['cube' => true, 'size' => $SIZE, 'mipmaps' => true]);
    if (!($rt instanceof VioRenderTarget)) { vio_destroy($ctx); return "FAIL\n  cube render target not created"; }

    $colors = [[1,0,0], [0,1,0], [0,0,1], [1,1,0], [1,0,1], [0,1,1]];
    $dirs   = [[1,0,0], [-1,0,0], [0,1,0], [0,-1,0], [0,0,1], [0,0,-1]];

    vio_clear($ctx, 0, 0, 0, 1);
    vio_begin($ctx);
    for ($f = 0; $f < 6; $f++) {
        vio_bind_render_target($ctx, $rt, $f);
        vio_bind_pipeline($ctx, $p_fill);
        vio_set_uniform($ctx, 'u_color', [$colors[$f][0], $colors[$f][1], $colors[$f][2], 1.0]);
        vio_draw($ctx, $quad);
    }
    vio_unbind_render_target($ctx);
    if (!vio_generate_mipmaps($ctx, $rt)) $fail[] = "generate_mipmaps(rt) returned false";
    vio_end($ctx);

    // Per-face readback straight from the target.
    for ($f = 0; $f < 6; $f++) {
        $p = vio_read_render_target($rt, $f);
        $want = [$colors[$f][0] * 255, $colors[$f][1] * 255, $colors[$f][2] * 255];
        if (!$p || strlen($p) !== $SIZE * $SIZE * 4) { $fail[] = "face $f readback size"; continue; }
        if (!near(px($p, $SIZE >> 1, $SIZE >> 1, $SIZE), $want)) $fail[] = "face $f readback " . json_encode(px($p, $SIZE >> 1, $SIZE >> 1, $SIZE)) . " want " . json_encode($want);
    }

    // Sample through the borrowed cubemap at LOD 0 and the last LOD.
    $cm = vio_render_target_cubemap($rt);
    if (!($cm instanceof VioCubemap)) { $fail[] = "vio_render_target_cubemap failed"; }
    else {
        $maxLod = (int)floor(log($SIZE, 2));
        foreach ([0.0, (float)$maxLod] as $lod) {
            for ($f = 0; $f < 6; $f++) {
                vio_begin($ctx);
                vio_bind_pipeline($ctx, $p_cube);
                vio_set_uniform($ctx, 'u_env', 0);
                vio_bind_cubemap($ctx, $cm, 0);
                vio_set_uniform($ctx, 'u_dir', $dirs[$f]);
                vio_set_uniform($ctx, 'u_lod', $lod);
                vio_draw($ctx, $quad);
                vio_end($ctx);
                $px = px(vio_read_pixels($ctx), 16, 16, $W);
                $want = [$colors[$f][0] * 255, $colors[$f][1] * 255, $colors[$f][2] * 255];
                if (!near($px, $want)) $fail[] = "face $f lod $lod: " . json_encode($px) . " want " . json_encode($want);
            }
        }
    }

    // Render into mip level 1 (no depth attachment) and sample it.
    vio_begin($ctx);
    vio_bind_render_target($ctx, $rt, 2, 1);
    vio_bind_pipeline($ctx, $p_fill);
    vio_set_uniform($ctx, 'u_color', [1.0, 1.0, 1.0, 1.0]);
    vio_draw($ctx, $quad);
    vio_unbind_render_target($ctx);
    vio_end($ctx);
    if ($cm instanceof VioCubemap) {
        vio_begin($ctx);
        vio_bind_pipeline($ctx, $p_cube);
        vio_set_uniform($ctx, 'u_env', 0);
        vio_bind_cubemap($ctx, $cm, 0);
        vio_set_uniform($ctx, 'u_dir', [0, 1, 0]);
        vio_set_uniform($ctx, 'u_lod', 1.0);
        vio_draw($ctx, $quad);
        vio_end($ctx);
        if (!near(px(vio_read_pixels($ctx), 16, 16, $W), [255, 255, 255])) $fail[] = "level-1 face not white: " . json_encode(px(vio_read_pixels($ctx), 16, 16, $W));
    }

    // Mip generation on an ordinary mipmapped texture and a CPU cubemap.
    $tex = vio_texture($ctx, ['data' => str_repeat("\xFF\x00\x00\xFF", 16), 'width' => 4, 'height' => 4, 'mipmaps' => true]);
    if (!vio_generate_mipmaps($ctx, $tex)) $fail[] = "generate_mipmaps(texture) returned false";
    $faces = [];
    for ($f = 0; $f < 6; $f++) { $face = []; for ($i = 0; $i < 4; $i++) array_push($face, 0, 0, 255, 255); $faces[] = $face; }
    $cm2 = vio_cubemap($ctx, ['pixels' => $faces, 'width' => 2, 'height' => 2, 'mipmaps' => true]);
    if (!($cm2 instanceof VioCubemap) || !vio_generate_mipmaps($ctx, $cm2)) $fail[] = "generate_mipmaps(cubemap) failed";
    // The CPU cubemap samples blue at LOD 0 and LOD 1.
    if ($cm2 instanceof VioCubemap) {
        foreach ([0.0, 1.0] as $lod) {
            vio_begin($ctx);
            vio_bind_pipeline($ctx, $p_cube);
            vio_set_uniform($ctx, 'u_env', 0);
            vio_bind_cubemap($ctx, $cm2, 0);
            vio_set_uniform($ctx, 'u_dir', [0, 0, 1]);
            vio_set_uniform($ctx, 'u_lod', $lod);
            vio_draw($ctx, $quad);
            vio_end($ctx);
            if (!near(px(vio_read_pixels($ctx), 16, 16, $W), [0, 0, 255])) $fail[] = "cpu cubemap lod $lod: " . json_encode(px(vio_read_pixels($ctx), 16, 16, $W));
        }
    }

    unset($cm, $cm2, $rt);
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

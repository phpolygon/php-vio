<?php
/* GEOMETRY-STAGES-PLAN Phase 1a. The building blocks for single-pass cube and
 * cascaded shadow maps, still bound one layer at a time:
 *   A. a 3-layer RGBA8 array: each layer cleared to its own colour, a quad
 *      drawn into layer 1 only; readback per layer, then sampled as
 *      sampler2DArray (layer selected by a uniform)
 *   B. a 2-layer depth_only array: a full-screen quad at a different depth per
 *      layer; readback (grey ramp) and sampler2DArray .r
 *   C. a depth_only cube: a different depth per face; readback per face and
 *      samplerCube .r through vio_render_target_cubemap()
 *   D. the option contract (layers + cube, layers + samples, bind out of range)
 * Every check reads either a whole uniformly filled layer or the centre of a
 * centred quad, so the row order of the backend does not matter. */
$W = 16;
function px(string $p, int $x, int $y, int $w): array { $o = ($y*$w+$x)*4; return [ord($p[$o]), ord($p[$o+1]), ord($p[$o+2])]; }
function near(array $a, array $b, int $tol = 3): bool { return abs($a[0]-$b[0]) <= $tol && abs($a[1]-$b[1]) <= $tol && abs($a[2]-$b[2]) <= $tol; }

function run_backend(string $name): string {
    global $W;
    $ctx = @vio_create($name, ["width" => $W, "height" => $W, "headless" => true, "vsync" => false]);
    if (!$ctx) return "skip (unavailable)";
    if (!vio_supports_feature($ctx, VIO_FEATURE_RENDER_TARGET_LAYERED) || !vio_supports_feature($ctx, VIO_FEATURE_3D_PIPELINE)) {
        vio_destroy($ctx);
        return "skip (no layered render targets)";
    }
    $fail = [];
    $vs = "#version 450\nlayout(location=0) in vec3 aPos;\nuniform float u_z;\nvoid main(){ gl_Position = vec4(aPos.xy, u_z, 1.0); }";
    $fsColor = "#version 450\nuniform vec4 u_color;\nlayout(location=0) out vec4 o;\nvoid main(){ o = u_color; }";
    $fsDepth = "#version 450\nvoid main(){ }";
    $fsArray = "#version 450\nuniform sampler2DArray u_arr; uniform float u_layer; uniform vec2 u_uv; uniform float u_depth;\n"
             . "layout(location=0) out vec4 o;\n"
             . "void main(){ vec4 c = texture(u_arr, vec3(u_uv, u_layer)); o = u_depth > 0.5 ? vec4(c.rrr, 1.0) : c; }";
    $fsCube = "#version 450\nuniform samplerCube u_cube; uniform vec3 u_dir;\nlayout(location=0) out vec4 o;\n"
            . "void main(){ float d = texture(u_cube, u_dir).r; o = vec4(d, d, d, 1.0); }";
    $base = ['depth_test' => false, 'cull_mode' => VIO_CULL_NONE];
    $pColor = vio_pipeline($ctx, ['shader' => vio_shader($ctx, ['vertex' => $vs, 'fragment' => $fsColor])] + $base);
    $pDepth = vio_pipeline($ctx, ['shader' => vio_shader($ctx, ['vertex' => $vs, 'fragment' => $fsDepth]),
                                  'depth_test' => true, 'depth_write' => true, 'cull_mode' => VIO_CULL_NONE]);
    $pArray = vio_pipeline($ctx, ['shader' => vio_shader($ctx, ['vertex' => $vs, 'fragment' => $fsArray])] + $base);
    $pCube  = vio_pipeline($ctx, ['shader' => vio_shader($ctx, ['vertex' => $vs, 'fragment' => $fsCube])] + $base);
    $full  = vio_mesh($ctx, ['vertices' => [-1,-1,0, 1,-1,0, 1,1,0, -1,1,0], 'indices' => [0,1,2, 0,2,3], 'layout' => [VIO_FLOAT3]]);
    $small = vio_mesh($ctx, ['vertices' => [-0.5,-0.5,0, 0.5,-0.5,0, 0.5,0.5,0, -0.5,0.5,0], 'indices' => [0,1,2, 0,2,3], 'layout' => [VIO_FLOAT3]]);
    /* NDC z -> stored depth: (z+1)/2 on GL / D3D / Vulkan (vio's clip-space
     * fixup), z itself on Metal (NDC z is 0..1 there). The z values below stay
     * in 0..1 so nothing is clipped on Metal. */
    $metal = vio_backend_name($ctx) === 'metal';
    $grey = fn(float $z): int => (int)round(($metal ? $z : ($z + 1.0) / 2.0) * 255);

    /* Sample one value into the swapchain and return the centre pixel. */
    $sample = function (callable $bind) use ($ctx, $full, $W): array {
        vio_clear($ctx, 0, 0, 0, 1);
        vio_begin($ctx); $bind(); vio_draw($ctx, $full); vio_end($ctx);
        return px(vio_read_pixels($ctx), $W >> 1, $W >> 1, $W);
    };

    /* ---- A: colour array ------------------------------------------------ */
    $colors = [[255, 0, 0], [0, 255, 0], [0, 0, 255]];
    $arr = vio_render_target($ctx, ['width' => $W, 'height' => $W, 'layers' => 3]);
    if (!($arr instanceof VioRenderTarget)) { $fail[] = "A: array target not created"; }
    else {
        /* The quad goes into layer 1 while it is bound (re-binding keeps the
         * contents on every backend; test 142). */
        vio_begin($ctx);
        foreach ($colors as $l => $c) {
            vio_bind_render_target($ctx, $arr, $l);
            vio_clear($ctx, $c[0] / 255, $c[1] / 255, $c[2] / 255, 1);
            if ($l === 1) {
                vio_bind_pipeline($ctx, $pColor);
                vio_set_uniform($ctx, 'u_z', 0.0);
                vio_set_uniform($ctx, 'u_color', [1.0, 1.0, 0.0, 1.0]);
                vio_draw($ctx, $small);
            }
        }
        vio_unbind_render_target($ctx);
        vio_end($ctx);
        foreach ($colors as $l => $c) {
            $p = vio_read_render_target($arr, $l);
            if (!$p || strlen($p) !== $W * $W * 4) { $fail[] = "A: layer $l readback size"; continue; }
            $centre = $l === 1 ? [255, 255, 0] : $c;
            if (!near(px($p, 8, 8, $W), $centre)) $fail[] = "A: layer $l centre " . json_encode(px($p, 8, 8, $W)) . " want " . json_encode($centre);
            if (!near(px($p, 1, 1, $W), $c))      $fail[] = "A: layer $l corner " . json_encode(px($p, 1, 1, $W)) . " want " . json_encode($c);
        }
        $tex = vio_render_target_texture($arr);
        if (!($tex instanceof VioTexture)) { $fail[] = "A: vio_render_target_texture failed"; }
        else {
            foreach ($colors as $l => $c) {
                foreach ([[0.1, 0.1, $c], [0.5, 0.5, $l === 1 ? [255, 255, 0] : $c]] as [$u, $v, $want]) {
                    $got = $sample(function () use ($ctx, $pArray, $tex, $l, $u, $v) {
                        vio_bind_pipeline($ctx, $pArray);
                        vio_set_uniform($ctx, 'u_z', 0.0);
                        vio_set_uniform($ctx, 'u_arr', 0);
                        vio_bind_texture($ctx, $tex, 0);
                        vio_set_uniform($ctx, 'u_layer', (float)$l);
                        vio_set_uniform($ctx, 'u_uv', [$u, $v]);
                        vio_set_uniform($ctx, 'u_depth', 0.0);
                    });
                    if (!near($got, $want)) $fail[] = "A: sampled layer $l at ($u,$v) " . json_encode($got) . " want " . json_encode($want);
                }
            }
        }
    }

    /* ---- B: depth array ------------------------------------------------- */
    $depthZ = [0.25, 0.5];
    $darr = vio_render_target($ctx, ['width' => $W, 'height' => $W, 'layers' => 2, 'depth_only' => true]);
    if (!($darr instanceof VioRenderTarget)) { $fail[] = "B: depth array target not created"; }
    else {
        vio_begin($ctx);
        foreach ($depthZ as $l => $z) {
            vio_bind_render_target($ctx, $darr, $l);
            vio_clear($ctx, 0, 0, 0, 1);
            vio_bind_pipeline($ctx, $pDepth);
            vio_set_uniform($ctx, 'u_z', $z);
            vio_draw($ctx, $full);
        }
        vio_unbind_render_target($ctx);
        vio_end($ctx);
        foreach ($depthZ as $l => $z) {
            $p = vio_read_render_target($darr, $l);
            $g = $grey($z);
            if (!$p || strlen($p) !== $W * $W * 4) { $fail[] = "B: layer $l readback size"; continue; }
            if (!near(px($p, 8, 8, $W), [$g, $g, $g], 4)) $fail[] = "B: layer $l depth " . json_encode(px($p, 8, 8, $W)) . " want $g";
        }
        $dtex = vio_render_target_texture($darr);
        if (!($dtex instanceof VioTexture)) { $fail[] = "B: vio_render_target_texture failed"; }
        else {
            foreach ($depthZ as $l => $z) {
                $g = $grey($z);
                $got = $sample(function () use ($ctx, $pArray, $dtex, $l) {
                    vio_bind_pipeline($ctx, $pArray);
                    vio_set_uniform($ctx, 'u_z', 0.0);
                    vio_set_uniform($ctx, 'u_arr', 0);
                    vio_bind_texture($ctx, $dtex, 0);
                    vio_set_uniform($ctx, 'u_layer', (float)$l);
                    vio_set_uniform($ctx, 'u_uv', [0.5, 0.5]);
                    vio_set_uniform($ctx, 'u_depth', 1.0);
                });
                if (!near($got, [$g, $g, $g], 4)) $fail[] = "B: sampled depth layer $l " . json_encode($got) . " want $g";
            }
        }
    }

    /* ---- C: depth cube -------------------------------------------------- */
    $dirs = [[1,0,0], [-1,0,0], [0,1,0], [0,-1,0], [0,0,1], [0,0,-1]];
    $cube = vio_render_target($ctx, ['cube' => true, 'size' => $W, 'depth_only' => true]);
    if (!($cube instanceof VioRenderTarget)) { $fail[] = "C: depth cube target not created"; }
    else {
        vio_begin($ctx);
        for ($f = 0; $f < 6; $f++) {
            vio_bind_render_target($ctx, $cube, $f);
            vio_clear($ctx, 0, 0, 0, 1);
            vio_bind_pipeline($ctx, $pDepth);
            vio_set_uniform($ctx, 'u_z', 0.05 + 0.15 * $f);
            vio_draw($ctx, $full);
        }
        vio_unbind_render_target($ctx);
        vio_end($ctx);
        for ($f = 0; $f < 6; $f++) {
            $g = $grey(0.05 + 0.15 * $f);
            $p = vio_read_render_target($cube, $f);
            if (!$p || strlen($p) !== $W * $W * 4) { $fail[] = "C: face $f readback size"; continue; }
            if (!near(px($p, 8, 8, $W), [$g, $g, $g], 4)) $fail[] = "C: face $f depth " . json_encode(px($p, 8, 8, $W)) . " want $g";
        }
        $cm = vio_render_target_cubemap($cube);
        if (!($cm instanceof VioCubemap)) { $fail[] = "C: vio_render_target_cubemap failed"; }
        else {
            for ($f = 0; $f < 6; $f++) {
                $g = $grey(0.05 + 0.15 * $f);
                $got = $sample(function () use ($ctx, $pCube, $cm, $dirs, $f) {
                    vio_bind_pipeline($ctx, $pCube);
                    vio_set_uniform($ctx, 'u_z', 0.0);
                    vio_set_uniform($ctx, 'u_cube', 0);
                    vio_bind_cubemap($ctx, $cm, 0);
                    vio_set_uniform($ctx, 'u_dir', $dirs[$f]);
                });
                if (!near($got, [$g, $g, $g], 4)) $fail[] = "C: sampled face $f " . json_encode($got) . " want $g";
            }
        }
    }

    /* ---- D: option contract --------------------------------------------- */
    if (@vio_render_target($ctx, ['cube' => true, 'size' => 8, 'layers' => 2]) !== false) $fail[] = "D: layers + cube accepted";
    if (@vio_render_target($ctx, ['width' => 8, 'height' => 8, 'layers' => 2, 'samples' => 4]) !== false) $fail[] = "D: layers + samples accepted";
    if (@vio_render_target($ctx, ['width' => 8, 'height' => 8, 'layers' => 65]) !== false) $fail[] = "D: 65 layers accepted";
    if ($arr instanceof VioRenderTarget) {
        $warned = false;
        set_error_handler(function () use (&$warned) { $warned = true; return true; });
        vio_begin($ctx);
        vio_bind_render_target($ctx, $arr, 3);
        vio_end($ctx);
        restore_error_handler();
        if (!$warned) $fail[] = "D: binding layer 3 of a 3-layer target did not warn";
        if (vio_generate_mipmaps($ctx, $arr) !== false) $fail[] = "D: generate_mipmaps on an array target returned true";
    }

    unset($tex, $dtex, $cm, $arr, $darr, $cube);
    vio_destroy($ctx);
    return $fail ? "FAIL\n  " . implode("\n  ", $fail) : "OK";
}

foreach (['opengl', 'd3d11', 'd3d12', 'vulkan', 'metal'] as $b) {
    echo "$b: ", run_backend($b), "\n";
}
echo "DONE\n";
?>

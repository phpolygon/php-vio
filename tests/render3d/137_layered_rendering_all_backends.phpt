--TEST--
Layered rendering: vio_bind_render_target(VIO_RT_ALL_LAYERS) + gl_Layer from a geometry stage (and from the vertex stage with VIO_FEATURE_VERTEX_LAYER) render every layer / cube face in one pass
--EXTENSIONS--
vio
--SKIPIF--
<?php
$any = false;
foreach (vio_backends() as $b) {
    if ($b === 'null') continue;
    $c = @vio_create($b, ["width" => 8, "height" => 8, "headless" => true, "vsync" => false]);
    if (!$c) continue;
    if (vio_supports_feature($c, VIO_FEATURE_LAYERED_RENDER)) $any = true;
    vio_destroy($c);
}
if (!$any) die("skip no backend with layered rendering");
?>
--FILE--
<?php
/* GEOMETRY-STAGES-PLAN Phase 1b / 1c - single-pass cube and cascade rendering:
 *   A. vio_clear with every layer bound clears every layer
 *   B. one draw of a full-screen triangle through a geometry stage that emits
 *      it once per layer with gl_Layer = i and a per-layer colour
 *   C. a depth cube filled in ONE draw (the point-light shadow case): the GS
 *      writes each face at its own depth
 *   D. gl_Layer from the vertex stage (VIO_FEATURE_VERTEX_LAYER): one instance
 *      per layer, gl_Layer = gl_InstanceIndex, no geometry stage
 * Every check reads a whole uniformly filled layer, so the row order of the
 * backend does not matter. The geometry stage reads its position from a user
 * varying (the portable form, see test 109). */
$W = 16;
function px(string $p, int $x, int $y, int $w): array { $o = ($y*$w+$x)*4; return [ord($p[$o]), ord($p[$o+1]), ord($p[$o+2])]; }
function near(array $a, array $b, int $tol = 3): bool { return abs($a[0]-$b[0]) <= $tol && abs($a[1]-$b[1]) <= $tol && abs($a[2]-$b[2]) <= $tol; }

function run_backend(string $name): string {
    global $W;
    $ctx = @vio_create($name, ["width" => $W, "height" => $W, "headless" => true, "vsync" => false]);
    if (!$ctx) return "skip (unavailable)";
    if (!vio_supports_feature($ctx, VIO_FEATURE_LAYERED_RENDER)) {
        vio_destroy($ctx);
        return "skip (no layered rendering)";
    }
    $fail = [];
    $gs = vio_supports_feature($ctx, VIO_FEATURE_GEOMETRY);
    $palette = [[255, 0, 0], [0, 255, 0], [0, 0, 255], [255, 255, 0], [255, 0, 255], [0, 255, 255]];
    $tri = vio_mesh($ctx, ['vertices' => [-1,-1,0, 3,-1,0, -1,3,0], 'layout' => [VIO_FLOAT3]]);
    $base = ['depth_test' => false, 'cull_mode' => VIO_CULL_NONE];
    $glslPalette = "const vec3 PALETTE[6] = vec3[6](vec3(1,0,0), vec3(0,1,0), vec3(0,0,1), vec3(1,1,0), vec3(1,0,1), vec3(0,1,1));\n";

    /* ---- A: clear with every layer bound ------------------------------- */
    $arr = vio_render_target($ctx, ['width' => $W, 'height' => $W, 'layers' => 3]);
    vio_begin($ctx);
    for ($l = 0; $l < 3; $l++) {
        vio_bind_render_target($ctx, $arr, $l);
        vio_clear($ctx, $palette[$l][0] / 255, $palette[$l][1] / 255, $palette[$l][2] / 255, 1);
    }
    vio_bind_render_target($ctx, $arr, VIO_RT_ALL_LAYERS);
    vio_clear($ctx, 0.5, 0.5, 0.5, 1);
    vio_unbind_render_target($ctx);
    vio_end($ctx);
    for ($l = 0; $l < 3; $l++) {
        $got = px(vio_read_render_target($arr, $l), 8, 8, $W);
        if (!near($got, [128, 128, 128])) $fail[] = "A: layer $l after the all-layer clear " . json_encode($got);
    }

    /* ---- B: one draw, every layer, geometry stage ------------------------ */
    if ($gs) {
        $vs = "#version 450\nlayout(location=0) in vec3 aPos;\nlayout(location=0) out vec4 vPos;\n"
            . "void main(){ vPos = vec4(aPos, 1.0); gl_Position = vPos; }";
        $gsSrc = "#version 450\nlayout(triangles) in;\nlayout(triangle_strip, max_vertices = 18) out;\n"
               . "layout(location=0) in vec4 vPos[];\nlayout(location=0) out vec3 gColor;\n"
               . "uniform float u_layers; uniform float u_z0; uniform float u_dz;\n" . $glslPalette
               . "void main(){\n"
               . "  for (int l = 0; l < int(u_layers); l++) {\n"
               . "    for (int i = 0; i < 3; i++) {\n"
               . "      gl_Layer = l; gColor = PALETTE[l];\n"
               . "      gl_Position = vec4(vPos[i].xy, u_z0 + u_dz * float(l), 1.0); EmitVertex();\n"
               . "    }\n"
               . "    EndPrimitive();\n"
               . "  }\n}";
        $fs = "#version 450\nlayout(location=0) in vec3 gColor;\nlayout(location=0) out vec4 o;\nvoid main(){ o = vec4(gColor, 1.0); }";
        $fsDepth = "#version 450\nlayout(location=0) in vec3 gColor;\nvoid main(){ }";
        $sh = vio_shader($ctx, ['vertex' => $vs, 'geometry' => $gsSrc, 'fragment' => $fs]);
        $shDepth = vio_shader($ctx, ['vertex' => $vs, 'geometry' => $gsSrc, 'fragment' => $fsDepth]);
        if (!$sh || !$shDepth) { $fail[] = "B: layered geometry shader not created"; }
        else {
            $p = vio_pipeline($ctx, ['shader' => $sh] + $base);
            vio_begin($ctx);
            vio_bind_render_target($ctx, $arr, VIO_RT_ALL_LAYERS);
            vio_clear($ctx, 0, 0, 0, 1);
            vio_bind_pipeline($ctx, $p);
            vio_set_uniforms($ctx, ['u_layers' => 3.0, 'u_z0' => 0.0, 'u_dz' => 0.0]);
            vio_draw($ctx, $tri);
            vio_unbind_render_target($ctx);
            vio_end($ctx);
            for ($l = 0; $l < 3; $l++) {
                $got = px(vio_read_render_target($arr, $l), 8, 8, $W);
                if (!near($got, $palette[$l])) $fail[] = "B: layer $l " . json_encode($got) . " want " . json_encode($palette[$l]);
            }

            /* ---- C: depth cube in one draw ---------------------------------- */
            $cube = vio_render_target($ctx, ['cube' => true, 'size' => $W, 'depth_only' => true]);
            $pd = vio_pipeline($ctx, ['shader' => $shDepth, 'depth_test' => true, 'depth_write' => true, 'cull_mode' => VIO_CULL_NONE]);
            vio_begin($ctx);
            vio_bind_render_target($ctx, $cube, VIO_RT_ALL_LAYERS);
            vio_clear($ctx, 0, 0, 0, 1);
            vio_bind_pipeline($ctx, $pd);
            vio_set_uniforms($ctx, ['u_layers' => 6.0, 'u_z0' => -0.8, 'u_dz' => 0.3]);
            vio_draw($ctx, $tri);
            vio_unbind_render_target($ctx);
            vio_end($ctx);
            for ($f = 0; $f < 6; $f++) {
                $g = (int)round(((-0.8 + 0.3 * $f + 1.0) / 2.0) * 255);
                $got = px(vio_read_render_target($cube, $f), 8, 8, $W);
                if (!near($got, [$g, $g, $g], 4)) $fail[] = "C: face $f depth " . json_encode($got) . " want $g";
            }
        }
    }

    /* ---- D: gl_Layer from the vertex stage ------------------------------ */
    if (vio_supports_feature($ctx, VIO_FEATURE_VERTEX_LAYER)) {
        $vsLayer = "#version 450\n#extension GL_ARB_shader_viewport_layer_array : require\n"
                 . "layout(location=0) in vec3 aPos;\nlayout(location=0) out vec3 vColor;\n" . $glslPalette
                 . "void main(){ gl_Layer = gl_InstanceIndex; vColor = PALETTE[gl_InstanceIndex]; gl_Position = vec4(aPos, 1.0); }";
        $fsV = "#version 450\nlayout(location=0) in vec3 vColor;\nlayout(location=0) out vec4 o;\nvoid main(){ o = vec4(vColor, 1.0); }";
        $shV = vio_shader($ctx, ['vertex' => $vsLayer, 'fragment' => $fsV]);
        if (!$shV) { $fail[] = "D: vertex-layer shader not created"; }
        else {
            $pv = vio_pipeline($ctx, ['shader' => $shV] + $base);
            $ident = pack('f*', 1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1);
            vio_begin($ctx);
            vio_bind_render_target($ctx, $arr, VIO_RT_ALL_LAYERS);
            vio_clear($ctx, 0, 0, 0, 1);
            vio_bind_pipeline($ctx, $pv);
            vio_draw_instanced($ctx, $tri, str_repeat($ident, 3), 3);
            vio_unbind_render_target($ctx);
            vio_end($ctx);
            for ($l = 0; $l < 3; $l++) {
                $got = px(vio_read_render_target($arr, $l), 8, 8, $W);
                if (!near($got, $palette[$l])) $fail[] = "D: layer $l " . json_encode($got) . " want " . json_encode($palette[$l]);
            }
        }
    }

    /* ---- contract ------------------------------------------------------- */
    $plain = vio_render_target($ctx, ['width' => 8, 'height' => 8]);
    $warned = false;
    set_error_handler(function () use (&$warned) { $warned = true; return true; });
    vio_begin($ctx);
    vio_bind_render_target($ctx, $plain, VIO_RT_ALL_LAYERS);
    vio_end($ctx);
    restore_error_handler();
    if (!$warned) $fail[] = "VIO_RT_ALL_LAYERS on a plain target did not warn";

    unset($arr, $cube, $plain);
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

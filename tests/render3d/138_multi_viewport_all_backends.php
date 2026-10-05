<?php
/* GEOMETRY-STAGES-PLAN Phase 1d - all CSM cascades into one atlas in one pass.
 * A 32x16 target split into a left and a right 16x16 viewport (full height, so
 * the row order of the backend does not matter):
 *   A. a geometry stage emits a full-screen triangle once per viewport with
 *      gl_ViewportIndex = i and its own colour: left red, right green
 *   B. a shader that never writes gl_ViewportIndex lands in viewport 0 only
 *   C. gl_ViewportIndex = gl_InstanceIndex in the vertex stage
 *      (VIO_FEATURE_VERTEX_LAYER), no geometry stage
 *   D. vio_viewport() afterwards returns to one viewport over the whole target
 *   E. the argument contract */
$W = 32; $H = 16;
function px(string $p, int $x, int $y, int $w): array { $o = ($y*$w+$x)*4; return [ord($p[$o]), ord($p[$o+1]), ord($p[$o+2])]; }
function near(array $a, array $b): bool { return abs($a[0]-$b[0]) <= 3 && abs($a[1]-$b[1]) <= 3 && abs($a[2]-$b[2]) <= 3; }

function run_backend(string $name): string {
    global $W, $H;
    $ctx = @vio_create($name, ["width" => 16, "height" => 16, "headless" => true, "vsync" => false]);
    if (!$ctx) return "skip (unavailable)";
    if (!vio_supports_feature($ctx, VIO_FEATURE_MULTI_VIEWPORT)) {
        vio_destroy($ctx);
        return "skip (no multiple viewports)";
    }
    $fail = [];
    $rt = vio_render_target($ctx, ['width' => $W, 'height' => $H]);
    $tri = vio_mesh($ctx, ['vertices' => [-1,-1,0, 3,-1,0, -1,3,0], 'layout' => [VIO_FLOAT3]]);
    $base = ['depth_test' => false, 'cull_mode' => VIO_CULL_NONE];
    $halves = [[0, 0, 16, 16], [16, 0, 16, 16]];
    $COLORS = "const vec3 COLORS[2] = vec3[2](vec3(1,0,0), vec3(0,1,0));\n";
    $fs = "#version 450\nlayout(location=0) in vec3 vColor;\nlayout(location=0) out vec4 o;\nvoid main(){ o = vec4(vColor, 1.0); }";
    /* One frame into $rt with both viewports set; returns the left / right centre pixels. */
    $frame = function (callable $draw, bool $two = true) use ($ctx, $rt, $halves, $W): array {
        vio_begin($ctx);
        vio_bind_render_target($ctx, $rt);
        vio_clear($ctx, 0, 0, 0, 1);
        if ($two && !vio_viewports($ctx, $halves)) echo "vio_viewports returned false\n";
        $draw();
        vio_unbind_render_target($ctx);
        vio_end($ctx);
        $p = vio_read_render_target($rt);
        return [px($p, 8, 8, $W), px($p, 24, 8, $W)];
    };

    /* ---- A: geometry stage ---------------------------------------------- */
    if (vio_supports_feature($ctx, VIO_FEATURE_GEOMETRY)) {
        $vs = "#version 450\nlayout(location=0) in vec3 aPos;\nlayout(location=0) out vec4 vPos;\nvoid main(){ vPos = vec4(aPos, 1.0); gl_Position = vPos; }";
        $gs = "#version 450\nlayout(triangles) in;\nlayout(triangle_strip, max_vertices = 6) out;\n"
            . "layout(location=0) in vec4 vPos[];\nlayout(location=0) out vec3 vColor;\n" . $COLORS
            . "void main(){ for (int v = 0; v < 2; v++) { for (int i = 0; i < 3; i++) {\n"
            . "  gl_ViewportIndex = v; vColor = COLORS[v]; gl_Position = vPos[i]; EmitVertex(); } EndPrimitive(); } }";
        $sh = vio_shader($ctx, ['vertex' => $vs, 'geometry' => $gs, 'fragment' => $fs]);
        if (!$sh) { $fail[] = "A: viewport-index geometry shader not created"; }
        else {
            $p = vio_pipeline($ctx, ['shader' => $sh] + $base);
            [$l, $r] = $frame(function () use ($ctx, $p, $tri) { vio_bind_pipeline($ctx, $p); vio_draw($ctx, $tri); });
            if (!near($l, [255, 0, 0])) $fail[] = "A: left viewport " . json_encode($l) . " want red";
            if (!near($r, [0, 255, 0])) $fail[] = "A: right viewport " . json_encode($r) . " want green";
        }
    }

    /* ---- B: no gl_ViewportIndex -> viewport 0 ---------------------------- */
    $vsPlain = "#version 450\nlayout(location=0) in vec3 aPos;\nlayout(location=0) out vec3 vColor;\nvoid main(){ vColor = vec3(1, 0, 0); gl_Position = vec4(aPos, 1.0); }";
    $pPlain = vio_pipeline($ctx, ['shader' => vio_shader($ctx, ['vertex' => $vsPlain, 'fragment' => $fs])] + $base);
    [$l, $r] = $frame(function () use ($ctx, $pPlain, $tri) { vio_bind_pipeline($ctx, $pPlain); vio_draw($ctx, $tri); });
    if (!near($l, [255, 0, 0])) $fail[] = "B: viewport 0 " . json_encode($l) . " want red";
    if (!near($r, [0, 0, 0]))   $fail[] = "B: viewport 1 drawn without gl_ViewportIndex " . json_encode($r);

    /* ---- C: gl_ViewportIndex from the vertex stage ----------------------- */
    if (vio_supports_feature($ctx, VIO_FEATURE_VERTEX_LAYER)) {
        $vsIdx = "#version 450\n#extension GL_ARB_shader_viewport_layer_array : require\n"
               . "layout(location=0) in vec3 aPos;\nlayout(location=0) out vec3 vColor;\n" . $COLORS
               . "void main(){ gl_ViewportIndex = gl_InstanceIndex; vColor = COLORS[gl_InstanceIndex]; gl_Position = vec4(aPos, 1.0); }";
        $shV = vio_shader($ctx, ['vertex' => $vsIdx, 'fragment' => $fs]);
        if (!$shV) { $fail[] = "C: viewport-index vertex shader not created"; }
        else {
            $pV = vio_pipeline($ctx, ['shader' => $shV] + $base);
            $ident = pack('f*', 1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1);
            [$l, $r] = $frame(function () use ($ctx, $pV, $tri, $ident) { vio_bind_pipeline($ctx, $pV); vio_draw_instanced($ctx, $tri, $ident . $ident, 2); });
            if (!near($l, [255, 0, 0])) $fail[] = "C: left viewport " . json_encode($l) . " want red";
            if (!near($r, [0, 255, 0])) $fail[] = "C: right viewport " . json_encode($r) . " want green";
        }
    }

    /* ---- D: back to one viewport ---------------------------------------- */
    [$l, $r] = $frame(function () use ($ctx, $pPlain, $tri, $halves, $W, $H) {
        vio_viewports($ctx, $halves);
        vio_viewport($ctx, 0, 0, $W, $H);
        vio_bind_pipeline($ctx, $pPlain);
        vio_draw($ctx, $tri);
    }, false);
    if (!near($l, [255, 0, 0]) || !near($r, [255, 0, 0])) $fail[] = "D: vio_viewport did not restore a full viewport " . json_encode([$l, $r]);

    /* ---- E: contract ------------------------------------------------------ */
    vio_begin($ctx);
    vio_bind_render_target($ctx, $rt);
    if (@vio_viewports($ctx, array_fill(0, 17, [0, 0, 1, 1])) !== false) $fail[] = "E: 17 viewports accepted";
    if (@vio_viewports($ctx, [[0, 0, 16]]) !== false) $fail[] = "E: a 3-element viewport accepted";
    if (@vio_viewports($ctx, [[0, 0, 0, 16], [0, 0, 16, 16]]) !== false) $fail[] = "E: a zero-width viewport accepted";
    if (@vio_viewports($ctx, []) !== false) $fail[] = "E: an empty list accepted";
    vio_unbind_render_target($ctx);
    vio_end($ctx);

    unset($rt);
    vio_destroy($ctx);
    return $fail ? "FAIL\n  " . implode("\n  ", $fail) : "OK";
}

foreach (['opengl', 'd3d11', 'd3d12', 'vulkan', 'metal'] as $b) {
    echo "$b: ", run_backend($b), "\n";
}
echo "DONE\n";
?>

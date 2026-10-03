--TEST--
Geometry / tessellation stages on every draw path (batch, instanced, uint16, indirect, vertex storage), in HDR / MRT / MSAA / stencil targets, and through the shader cache
--EXTENSIONS--
vio
--SKIPIF--
<?php
$any = false;
foreach (vio_backends() as $b) {
    if ($b === 'null') continue;
    $c = @vio_create($b, ["width" => 8, "height" => 8, "headless" => true, "vsync" => false]);
    if (!$c) continue;
    if (vio_supports_feature($c, VIO_FEATURE_GEOMETRY) && vio_supports_feature($c, VIO_FEATURE_3D_PIPELINE)
        && vio_supports_feature($c, VIO_FEATURE_READ_PIXELS)) $any = true;
    vio_destroy($c);
}
if (!$any) die("skip no backend with a geometry stage");
?>
--FILE--
<?php
/* Tests 109 / 110 prove the stages work through vio_draw into the swapchain.
 * This one walks the paths that arrived later and each carry their own
 * pipeline / constant-block plumbing:
 *   - draw paths: vio_submit_batch, vio_draw_instanced, a uint16 index buffer,
 *     vio_draw_indirect, vio_draw_instanced_from_buffer
 *   - pipeline variants: HDR target, two-attachment MRT target, 4x MSAA target,
 *     a stencil-writing pipeline (D3D12 PSO / Vulkan render-pass variants must
 *     keep the extra stages)
 *   - the on-disk shader cache: the SAME vertex + fragment source with and
 *     without a geometry stage are different programs (OpenGL program binary
 *     key, Vulkan's vertex stage only carries the clip-space fixup when it is
 *     the last stage). Shader A draws a quad from triangles, shader B expands a
 *     point into the same quad in the GS; both must give the same image in
 *     either compile order, from a cold and from a warm cache.
 * The GS quad is centred, the tessellated disc is symmetric, so the checks do
 * not depend on the row order of the readback. */
$W = 64; $R = 0.8;
function px(string $p, int $x, int $y, int $w): array { $o = ($y*$w+$x)*4; return [ord($p[$o]), ord($p[$o+1]), ord($p[$o+2])]; }
function near(array $a, array $b): bool { return abs($a[0]-$b[0]) <= 3 && abs($a[1]-$b[1]) <= 3 && abs($a[2]-$b[2]) <= 3; }
function polar(float $r, float $deg): array { global $W; $a = deg2rad($deg); return [(int)round($W/2 + cos($a) * $r * $W/2), (int)round($W/2 - sin($a) * $r * $W/2)]; }
function green_mask(string $p): string { $m = ''; for ($i = 0, $n = strlen($p); $i < $n; $i += 4) $m .= ord($p[$i+1]) > 128 ? '1' : '0'; return $m; }

$VS = "#version 450\nlayout(location=0) in vec3 aPos;\nlayout(location=0) out vec4 vPos;\n"
    . "void main(){ vPos = vec4(aPos, 1.0); gl_Position = vPos; }";
$GS = "#version 450\nlayout(points) in;\nlayout(triangle_strip, max_vertices = 4) out;\n"
    . "layout(location=0) in vec4 vPos[];\nuniform float u_half;\n"
    . "void main(){ vec4 c = vPos[0];\n"
    . "  gl_Position = c + vec4(-u_half, -u_half, 0.0, 0.0); EmitVertex();\n"
    . "  gl_Position = c + vec4( u_half, -u_half, 0.0, 0.0); EmitVertex();\n"
    . "  gl_Position = c + vec4(-u_half,  u_half, 0.0, 0.0); EmitVertex();\n"
    . "  gl_Position = c + vec4( u_half,  u_half, 0.0, 0.0); EmitVertex();\n"
    . "  EndPrimitive(); }";
$FS  = "#version 450\nlayout(location=0) out vec4 o;\nvoid main(){ o = vec4(0.0, 1.0, 0.0, 1.0); }";
$FS2 = "#version 450\nlayout(location=0) out vec4 o0;\nlayout(location=1) out vec4 o1;\n"
     . "void main(){ o0 = vec4(0.0, 1.0, 0.0, 1.0); o1 = vec4(0.0, 1.0, 0.0, 1.0); }";
$TVS = "#version 450\nlayout(location=0) in vec3 aPos;\nvoid main(){ gl_Position = vec4(aPos, 1.0); }";
$TCS = "#version 450\nlayout(vertices = 4) out;\nuniform float u_level;\n"
     . "void main(){ gl_out[gl_InvocationID].gl_Position = gl_in[gl_InvocationID].gl_Position;\n"
     . "  if (gl_InvocationID == 0) { gl_TessLevelOuter[0] = u_level; gl_TessLevelOuter[1] = u_level;\n"
     . "    gl_TessLevelOuter[2] = u_level; gl_TessLevelOuter[3] = u_level;\n"
     . "    gl_TessLevelInner[0] = u_level; gl_TessLevelInner[1] = u_level; } }";
$TES = "#version 450\nlayout(quads, equal_spacing, ccw) in;\nuniform float u_radius;\n"
     . "void main(){ float a = gl_TessCoord.x * 6.28318530718; float r = gl_TessCoord.y * u_radius;\n"
     . "  gl_Position = vec4(cos(a) * r, sin(a) * r, 0.0, 1.0); }";

/* One frame into the swapchain (null target) or a render target; returns RGBA8. */
function frame($ctx, $rt, int $attachment, callable $draw): string {
    if (!$rt) {
        vio_clear($ctx, 0, 0, 0, 1);
        vio_begin($ctx); $draw(); vio_end($ctx);
        return vio_read_pixels($ctx);
    }
    vio_begin($ctx);
    vio_bind_render_target($ctx, $rt);
    vio_clear($ctx, 0, 0, 0, 1);
    $draw();
    vio_unbind_render_target($ctx);
    vio_end($ctx);
    return vio_read_render_target($rt, -1, $attachment);
}

/* GS quad: half-size 0.5 around the centre = pixels 16..47. */
function gs_ok(string $p, string $what, array &$fail): void {
    global $W;
    if (strlen($p) !== $W * $W * 4) { $fail[] = "$what: readback size " . strlen($p); return; }
    if (!near(px($p, 32, 32, $W), [0, 255, 0])) $fail[] = "$what: centre not green " . json_encode(px($p, 32, 32, $W));
    if (!near(px($p, 20, 44, $W), [0, 255, 0])) $fail[] = "$what: inside quad not green " . json_encode(px($p, 20, 44, $W));
    if (!near(px($p, 4, 4, $W), [0, 0, 0]))     $fail[] = "$what: outside quad not black " . json_encode(px($p, 4, 4, $W));
    if (!near(px($p, 58, 58, $W), [0, 0, 0]))   $fail[] = "$what: outside quad (far corner) not black " . json_encode(px($p, 58, 58, $W));
}

/* Tessellated disc at level 16: green at 0.9R on the diagonal, black outside R. */
function tess_ok(string $p, string $what, array &$fail): void {
    global $W, $R;
    if (strlen($p) !== $W * $W * 4) { $fail[] = "$what: readback size " . strlen($p); return; }
    foreach ([45.0, 225.0] as $deg) {
        [$x, $y] = polar(0.9 * $R, $deg);
        if (!near(px($p, $x, $y, $W), [0, 255, 0])) $fail[] = "$what: disc at {$deg}deg/0.9R not green " . json_encode(px($p, $x, $y, $W));
    }
    [$x, $y] = polar(1.15 * $R, 45.0);
    if (!near(px($p, $x, $y, $W), [0, 0, 0])) $fail[] = "$what: outside disc not black " . json_encode(px($p, $x, $y, $W));
}

function run_backend(string $name): string {
    global $W, $R, $VS, $GS, $FS, $FS2, $TVS, $TCS, $TES;
    $ctx = @vio_create($name, ["width" => $W, "height" => $W, "headless" => true, "vsync" => false]);
    if (!$ctx) return "skip (unavailable)";
    if (!vio_supports_feature($ctx, VIO_FEATURE_GEOMETRY) || !vio_supports_feature($ctx, VIO_FEATURE_3D_PIPELINE)
        || !vio_supports_feature($ctx, VIO_FEATURE_READ_PIXELS)) {
        vio_destroy($ctx);
        return "skip (no geometry stage)";
    }
    $fail = [];
    $tess = vio_supports_feature($ctx, VIO_FEATURE_TESSELLATION);
    $base = ['depth_test' => false, 'cull_mode' => VIO_CULL_NONE];

    $gsShader = vio_shader($ctx, ['vertex' => $VS, 'geometry' => $GS, 'fragment' => $FS]);
    $gsPipe = vio_pipeline($ctx, ['shader' => $gsShader, 'topology' => VIO_POINTS] + $base);
    $point   = vio_mesh($ctx, ['vertices' => [0, 0, 0], 'layout' => [VIO_FLOAT3]]);
    $point16 = vio_mesh($ctx, ['vertices' => [0, 0, 0], 'indices' => [0], 'layout' => [VIO_FLOAT3]]);
    if (vio_mesh_index_bytes($point16) !== 2) $fail[] = "point mesh should use 2-byte indices";
    $gsSet = function () use ($ctx, $gsPipe) { vio_bind_pipeline($ctx, $gsPipe); vio_set_uniform($ctx, 'u_half', 0.5); };

    if ($tess) {
        $tShader = vio_shader($ctx, ['vertex' => $TVS, 'tess_control' => $TCS, 'tess_eval' => $TES, 'fragment' => $FS]);
        $tPipe = vio_pipeline($ctx, ['shader' => $tShader, 'patch_vertices' => 4] + $base);
        $patch   = vio_mesh($ctx, ['vertices' => [-1,-1,0, 1,-1,0, 1,1,0, -1,1,0], 'layout' => [VIO_FLOAT3]]);
        $patch16 = vio_mesh($ctx, ['vertices' => [-1,-1,0, 1,-1,0, 1,1,0, -1,1,0], 'indices' => [0, 1, 2, 3], 'layout' => [VIO_FLOAT3]]);
        $tSet = function () use ($ctx, $tPipe, $R) {
            vio_bind_pipeline($ctx, $tPipe);
            vio_set_uniform($ctx, 'u_level', 16.0);
            vio_set_uniform($ctx, 'u_radius', $R);
        };
    }
    /* PATCHES without tessellation stages is refused up front. */
    if (@vio_pipeline($ctx, ['shader' => vio_shader($ctx, ['vertex' => $TVS, 'fragment' => $FS]), 'topology' => VIO_PATCHES]) !== false) {
        $fail[] = "VIO_PATCHES without tessellation stages was accepted";
    }

    /* ---- draw paths ---------------------------------------------------- */
    gs_ok(frame($ctx, null, 0, function () use ($ctx, $gsPipe, $point) {
        vio_submit_batch($ctx, [['pipeline' => $gsPipe, 'uniforms' => ['u_half' => 0.5], 'mesh' => $point]]);
    }), "gs submit_batch", $fail);
    $ident = pack('f*', 1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1);
    gs_ok(frame($ctx, null, 0, function () use ($ctx, $gsSet, $point, $ident) {
        $gsSet(); vio_draw_instanced($ctx, $point, $ident . $ident, 2);
    }), "gs draw_instanced", $fail);
    gs_ok(frame($ctx, null, 0, function () use ($ctx, $gsSet, $point16) { $gsSet(); vio_draw($ctx, $point16); }), "gs uint16 indices", $fail);
    if ($tess) {
        tess_ok(frame($ctx, null, 0, function () use ($ctx, $tPipe, $patch, $R) {
            vio_submit_batch($ctx, [['pipeline' => $tPipe, 'uniforms' => ['u_level' => 16.0, 'u_radius' => $R], 'mesh' => $patch]]);
        }), "tess submit_batch", $fail);
        tess_ok(frame($ctx, null, 0, function () use ($ctx, $tSet, $patch, $ident) {
            $tSet(); vio_draw_instanced($ctx, $patch, $ident, 1);
        }), "tess draw_instanced", $fail);
        tess_ok(frame($ctx, null, 0, function () use ($ctx, $tSet, $patch16) { $tSet(); vio_draw($ctx, $patch16); }), "tess uint16 indices", $fail);
    }
    if (vio_supports_feature($ctx, VIO_FEATURE_INDIRECT_DRAW)) {
        $args = vio_storage_buffer($ctx, ['data' => pack('V*', 1, 1, 0, 0, 0), 'indirect' => true]);
        gs_ok(frame($ctx, null, 0, function () use ($ctx, $gsSet, $point16, $args) { $gsSet(); vio_draw_indirect($ctx, $point16, $args, 1); }), "gs draw_indirect", $fail);
        if ($tess) {
            $targs = vio_storage_buffer($ctx, ['data' => pack('V*', 4, 1, 0, 0, 0), 'indirect' => true]);
            tess_ok(frame($ctx, null, 0, function () use ($ctx, $tSet, $patch16, $targs) { $tSet(); vio_draw_indirect($ctx, $patch16, $targs, 1); }), "tess draw_indirect", $fail);
        }
    }
    if (vio_supports_feature($ctx, VIO_FEATURE_VERTEX_STORAGE)) {
        gs_ok(frame($ctx, null, 0, function () use ($ctx, $gsSet, $point) { $gsSet(); vio_draw_instanced_from_buffer($ctx, $point, 1); }), "gs draw_instanced_from_buffer", $fail);
        if ($tess) {
            tess_ok(frame($ctx, null, 0, function () use ($ctx, $tSet, $patch) { $tSet(); vio_draw_instanced_from_buffer($ctx, $patch, 1); }), "tess draw_instanced_from_buffer", $fail);
        }
    }

    /* ---- pipeline variants: target formats, MRT, MSAA, stencil -------- */
    $targets = [];
    if (vio_supports_feature($ctx, VIO_FEATURE_RENDER_TARGET_HDR)) $targets['hdr'] = [['hdr' => true], []];
    if (vio_supports_feature($ctx, VIO_FEATURE_RENDER_TARGET_MSAA)) $targets['msaa x4'] = [['samples' => 4], []];
    if (vio_supports_feature($ctx, VIO_FEATURE_STENCIL)) {
        $targets['stencil'] = [[], ['stencil' => ['func' => VIO_CMP_ALWAYS, 'ref' => 1, 'pass' => VIO_STENCIL_REPLACE]]];
    }
    foreach ($targets as $label => [$rtOpts, $pipeOpts]) {
        $rt = vio_render_target($ctx, ['width' => $W, 'height' => $W] + $rtOpts);
        $p = vio_pipeline($ctx, ['shader' => $gsShader, 'topology' => VIO_POINTS] + $pipeOpts + $base);
        gs_ok(frame($ctx, $rt, 0, function () use ($ctx, $p, $point) {
            vio_bind_pipeline($ctx, $p); vio_set_uniform($ctx, 'u_half', 0.5); vio_draw($ctx, $point);
        }), "gs $label target", $fail);
        if ($tess) {
            $tp = vio_pipeline($ctx, ['shader' => $tShader, 'patch_vertices' => 4] + $pipeOpts + $base);
            tess_ok(frame($ctx, $rt, 0, function () use ($ctx, $tp, $patch, $R) {
                vio_bind_pipeline($ctx, $tp); vio_set_uniform($ctx, 'u_level', 16.0); vio_set_uniform($ctx, 'u_radius', $R);
                vio_draw($ctx, $patch);
            }), "tess $label target", $fail);
        }
    }
    if (vio_supports_feature($ctx, VIO_FEATURE_MRT)) {
        $formats = [VIO_FORMAT_RGBA8, VIO_FORMAT_RGBA8];
        $rt = vio_render_target($ctx, ['width' => $W, 'height' => $W, 'attachments' => $formats]);
        $mrtShader = vio_shader($ctx, ['vertex' => $VS, 'geometry' => $GS, 'fragment' => $FS2]);
        $p = vio_pipeline($ctx, ['shader' => $mrtShader, 'topology' => VIO_POINTS, 'attachments' => $formats] + $base);
        $draw = function () use ($ctx, $p, $point) { vio_bind_pipeline($ctx, $p); vio_set_uniform($ctx, 'u_half', 0.5); vio_draw($ctx, $point); };
        gs_ok(frame($ctx, $rt, 0, $draw), "gs MRT attachment 0", $fail);
        gs_ok(vio_read_render_target($rt, -1, 1), "gs MRT attachment 1", $fail);
    }

    vio_destroy($ctx);
    return $fail ? "FAIL\n  " . implode("\n  ", $fail) : "OK";
}

/* Shader A: the quad as two triangles, VS + FS only. Shader B: the identical
 * VS + FS source plus a GS that expands a point at the quad centre. The quad
 * sits off-centre (upper or lower half depending on the row order), so a
 * vertex stage that skips or doubles the clip-space fixup shows up as a
 * mirrored image. */
function cache_backend(string $name, string $dir): string {
    global $W, $VS, $GS, $FS;
    $opts = ["width" => $W, "height" => $W, "headless" => true, "vsync" => false, "shader_cache" => $dir];
    $ctx = @vio_create($name, $opts);
    if (!$ctx) return "skip (unavailable)";
    if (!vio_supports_feature($ctx, VIO_FEATURE_GEOMETRY) || !vio_supports_feature($ctx, VIO_FEATURE_3D_PIPELINE)) {
        vio_destroy($ctx);
        return "skip (no geometry stage)";
    }
    vio_destroy($ctx);
    $render = function ($ctx, string $which) use ($VS, $GS, $FS): string {
        $base = ['depth_test' => false, 'cull_mode' => VIO_CULL_NONE];
        if ($which === 'A') {
            $p = vio_pipeline($ctx, ['shader' => vio_shader($ctx, ['vertex' => $VS, 'fragment' => $FS])] + $base);
            $m = vio_mesh($ctx, ['vertices' => [-0.25,0.25,0, 0.25,0.25,0, 0.25,0.75,0, -0.25,0.75,0], 'indices' => [0,1,2, 0,2,3], 'layout' => [VIO_FLOAT3]]);
            return frame($ctx, null, 0, function () use ($ctx, $p, $m) { vio_bind_pipeline($ctx, $p); vio_draw($ctx, $m); });
        }
        $p = vio_pipeline($ctx, ['shader' => vio_shader($ctx, ['vertex' => $VS, 'geometry' => $GS, 'fragment' => $FS]), 'topology' => VIO_POINTS] + $base);
        $m = vio_mesh($ctx, ['vertices' => [0, 0.5, 0], 'layout' => [VIO_FLOAT3]]);
        return frame($ctx, null, 0, function () use ($ctx, $p, $m) { vio_bind_pipeline($ctx, $p); vio_set_uniform($ctx, 'u_half', 0.25); vio_draw($ctx, $m); });
    };
    $fail = [];
    foreach ([['B', 'A'], ['A', 'B']] as $round => $order) {   /* round 0: cold cache, round 1: warm */
        $ctx = vio_create($name, $opts);
        $img = [];
        foreach ($order as $which) $img[$which] = green_mask($render($ctx, $which));
        vio_destroy($ctx);
        $lit = substr_count($img['A'], '1');
        if ($lit < 200) $fail[] = "round $round: shader A lit only $lit pixels";
        if ($img['A'] !== $img['B']) {
            $fail[] = "round $round (" . implode(' then ', $order) . "): shader A and B differ ("
                    . substr_count($img['A'], '1') . " vs " . substr_count($img['B'], '1') . " green pixels)";
        }
    }
    return $fail ? "FAIL\n  " . implode("\n  ", $fail) : "OK";
}

$dir = sys_get_temp_dir() . '/vio-stage-cache-' . getmypid();
foreach (['opengl', 'd3d11', 'd3d12', 'vulkan', 'metal'] as $b) {
    echo "$b: ", run_backend($b), "\n";
    @mkdir($dir);
    echo "$b cache: ", cache_backend($b, $dir), "\n";
    foreach (glob($dir . '/*') ?: [] as $f) @unlink($f);
    @rmdir($dir);
}
echo "DONE\n";
?>
--EXPECTF--
opengl: %s
opengl cache: %s
d3d11: %s
d3d11 cache: %s
d3d12: %s
d3d12 cache: %s
vulkan: %s
vulkan cache: %s
metal: %s
metal cache: %s
DONE

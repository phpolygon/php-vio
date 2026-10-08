--TEST--
Multiview (VIO_FEATURE_MULTIVIEW): vio_shader(['view_count' => N]) + a layered target bound with VIO_RT_ALL_LAYERS renders every draw once per view into layer gl_ViewIndex, on every backend that reports the flag
--EXTENSIONS--
vio
--FILE--
<?php
/* GL_EXT_multiview: one draw, N views, view v lands in layer v.
 *   A. two views, the fragment stage colours by gl_ViewIndex
 *   B. the vertex stage moves geometry by gl_ViewIndex (view 1 only covers the
 *      left half) - per-view vertex work, not just a per-layer clear
 *   C. instanced: two instances (left / right half) per view - per-instance
 *      attributes must step per instance, not per (instance, view)
 *   D. four views
 *   E. the option contract: view_count outside 2..4 is refused
 *   F. an indirect draw renders every view too (Metal multiplies the record's
 *      instance count on the CPU side)
 * Columns are compared (x never flips between backends; rows may).
 * D3D12 needs SM 6.1 + ViewInstancingTier >= 1 (DXC). VIO_REQUIRE_MULTIVIEW
 * makes the listed backends mandatory. */
$W = 16;
$PAL = "const vec3 PAL[4] = vec3[4](vec3(1,0,0), vec3(0,1,0), vec3(0,0,1), vec3(1,1,0));\n";
$VS = "#version 450\n#extension GL_EXT_multiview : require\n"
    . "layout(location=0) in vec3 aPos;\nlayout(location=0) flat out int vview;\nuniform float u_shrink;\n"
    . "void main(){ vview = int(gl_ViewIndex);\n"
    . "  vec3 p = aPos; if (gl_ViewIndex == 1u && u_shrink > 0.5) p.x = p.x * 0.5 - 0.5;\n"
    . "  gl_Position = vec4(p, 1.0); }";
$VSI = "#version 450\n#extension GL_EXT_multiview : require\n"
    . "layout(location=0) in vec3 aPos;\nlayout(location=3) in mat4 aModel;\nlayout(location=0) flat out int vview;\n"
    . "void main(){ vview = int(gl_ViewIndex); gl_Position = aModel * vec4(aPos, 1.0); }";
$FS = "#version 450\nlayout(location=0) flat in int vview;\nlayout(location=0) out vec4 o;\n" . $PAL
    . "void main(){ o = vec4(PAL[vview], 1.0); }";
$PALRGB = [[255, 0, 0], [0, 255, 0], [0, 0, 255], [255, 255, 0]];

$opts = ["width" => $W, "height" => $W, "headless" => true, "vsync" => false, "shader_model" => 6];
$dxc = getenv('VIO_DXC_DIR') ?: '';
if ($dxc === '') foreach (glob('C:/Program Files (x86)/Windows Kits/10/bin/10.*/x64/dxcompiler.dll') ?: [] as $cand) $dxc = dirname($cand);
if ($dxc !== '') $opts['dxc_dir'] = $dxc;
$require = array_map('trim', explode(',', getenv('VIO_REQUIRE_MULTIVIEW') ?: ''));

function px(string $p, int $x, int $y, int $w): array { $o = ($y * $w + $x) * 4; return [ord($p[$o]), ord($p[$o + 1]), ord($p[$o + 2])]; }
function near(array $a, array $b): bool { return abs($a[0] - $b[0]) <= 3 && abs($a[1] - $b[1]) <= 3 && abs($a[2] - $b[2]) <= 3; }
/* Column colour at mid-height: left quarter and the right edge (B's shrunken
 * triangle reaches x = 0.5 at mid-height, so the right quarter is still inside). */
function cols($ctx, $rt, int $layer, int $w): array {
    $p = vio_read_render_target($rt, $layer);
    return [px($p, $w >> 2, $w >> 1, $w), px($p, $w - 2, $w >> 1, $w)];
}

function run_backend(string $name): string {
    global $W, $VS, $VSI, $FS, $PALRGB, $opts, $require;
    $req = in_array($name, $require, true);
    $ctx = @vio_create($name, $opts);
    if ($ctx && vio_backend_name($ctx) !== $name) { vio_destroy($ctx); $ctx = null; }
    if (!$ctx) return $req ? "FAIL\n  required but unavailable" : "skip (unavailable)";
    if (!vio_supports_feature($ctx, VIO_FEATURE_MULTIVIEW)) {
        /* Without the flag the option is refused, not ignored. */
        $sh = @vio_shader($ctx, ['vertex' => $VS, 'fragment' => $FS, 'view_count' => 2]);
        vio_destroy($ctx);
        if ($sh) return "FAIL\n  'view_count' accepted without VIO_FEATURE_MULTIVIEW";
        return $req ? "FAIL\n  required but VIO_FEATURE_MULTIVIEW is 0" : "skip (no multiview)";
    }
    $fail = [];
    $tri = vio_mesh($ctx, ['vertices' => [-1,-1,0, 3,-1,0, -1,3,0], 'layout' => [VIO_FLOAT3]]);
    $base = ['depth_test' => false, 'cull_mode' => VIO_CULL_NONE];
    $draw = function ($rt, $pipe, callable $issue) use ($ctx) {
        vio_begin($ctx);
        $layers = 4;
        for ($l = 0; $l < $layers; $l++) { if (@vio_bind_render_target($ctx, $rt, $l) === false) break; vio_clear($ctx, 0, 0, 0, 1); }
        vio_bind_render_target($ctx, $rt, VIO_RT_ALL_LAYERS);
        vio_bind_pipeline($ctx, $pipe);
        $issue();
        vio_unbind_render_target($ctx);
        vio_end($ctx);
    };

    /* A + B */
    $sh = vio_shader($ctx, ['vertex' => $VS, 'fragment' => $FS, 'view_count' => 2]);
    if (!$sh) { vio_destroy($ctx); return "FAIL\n  multiview shader not created"; }
    $pipe = vio_pipeline($ctx, ['shader' => $sh] + $base);
    $rt2 = vio_render_target($ctx, ['width' => $W, 'height' => $W, 'layers' => 2]);
    foreach ([0.0 => 'A', 1.0 => 'B'] as $shrink => $tag) {
        $draw($rt2, $pipe, function () use ($ctx, $tri, $shrink) { vio_set_uniform($ctx, 'u_shrink', (float)$shrink); vio_draw($ctx, $tri); });
        for ($v = 0; $v < 2; $v++) {
            [$l, $r] = cols($ctx, $rt2, $v, $W);
            $wantR = ($tag === 'B' && $v === 1) ? [0, 0, 0] : $PALRGB[$v];
            if (!near($l, $PALRGB[$v]) || !near($r, $wantR)) $fail[] = "$tag: layer $v columns " . json_encode([$l, $r]) . ", want " . json_encode([$PALRGB[$v], $wantR]);
        }
    }

    /* C: instanced, two instances per view (left and right half). */
    $shi = vio_shader($ctx, ['vertex' => $VSI, 'fragment' => $FS, 'view_count' => 2]);
    if (!$shi) $fail[] = "C: instanced multiview shader not created";
    else {
        $pipei = vio_pipeline($ctx, ['shader' => $shi] + $base);
        $quad = vio_mesh($ctx, ['vertices' => [-1,-1,0, 1,-1,0, 1,1,0, -1,1,0], 'indices' => [0,1,2, 0,2,3], 'layout' => [VIO_FLOAT3]]);
        $m = fn(float $tx) => [0.5,0,0,0, 0,1,0,0, 0,0,1,0, $tx,0,0,1];   /* column-major: half width, shifted */
        $draw($rt2, $pipei, function () use ($ctx, $quad, $m) { vio_draw_instanced($ctx, $quad, array_merge($m(-0.5), $m(0.5)), 2); });
        for ($v = 0; $v < 2; $v++) {
            [$l, $r] = cols($ctx, $rt2, $v, $W);
            if (!near($l, $PALRGB[$v]) || !near($r, $PALRGB[$v])) $fail[] = "C: layer $v columns " . json_encode([$l, $r]) . ", want both " . json_encode($PALRGB[$v]);
        }
    }

    /* D: four views. */
    $sh4 = vio_shader($ctx, ['vertex' => $VS, 'fragment' => $FS, 'view_count' => 4]);
    if (!$sh4) $fail[] = "D: four-view shader not created";
    else {
        $rt4 = vio_render_target($ctx, ['width' => $W, 'height' => $W, 'layers' => 4]);
        $draw($rt4, vio_pipeline($ctx, ['shader' => $sh4] + $base), function () use ($ctx, $tri) { vio_set_uniform($ctx, 'u_shrink', 0.0); vio_draw($ctx, $tri); });
        for ($v = 0; $v < 4; $v++) {
            [$l, $r] = cols($ctx, $rt4, $v, $W);
            if (!near($l, $PALRGB[$v]) || !near($r, $PALRGB[$v])) $fail[] = "D: layer $v " . json_encode([$l, $r]) . ", want " . json_encode($PALRGB[$v]);
        }
    }

    /* F: indirect. */
    if (vio_supports_feature($ctx, VIO_FEATURE_INDIRECT_DRAW)) {
        $quadf = vio_mesh($ctx, ['vertices' => [-1,-1,0, 1,-1,0, 1,1,0, -1,1,0], 'indices' => [0,1,2, 0,2,3], 'layout' => [VIO_FLOAT3]]);
        $args = vio_storage_buffer($ctx, ['data' => pack('V*', 6, 1, 0, 0, 0), 'indirect' => true]);
        $draw($rt2, $pipe, function () use ($ctx, $quadf, $args) { vio_set_uniform($ctx, 'u_shrink', 0.0); vio_draw_indirect($ctx, $quadf, $args, 1, 0); });
        for ($v = 0; $v < 2; $v++) {
            [$l, $r] = cols($ctx, $rt2, $v, $W);
            if (!near($l, $PALRGB[$v]) || !near($r, $PALRGB[$v])) $fail[] = "F: layer $v " . json_encode([$l, $r]) . ", want " . json_encode($PALRGB[$v]);
        }
    }

    /* E: the contract. */
    foreach ([1, 5] as $bad) {
        if (@vio_shader($ctx, ['vertex' => $VS, 'fragment' => $FS, 'view_count' => $bad])) $fail[] = "E: view_count $bad accepted";
    }
    vio_destroy($ctx);
    return $fail ? "FAIL\n  " . implode("\n  ", $fail) : "OK";
}

foreach (['opengl', 'd3d11', 'd3d12', 'vulkan', 'metal'] as $b) echo "$b: ", run_backend($b), "\n";
echo "DONE\n";
?>
--EXPECTF--
opengl: %r(OK|skip \(.*\))%r
d3d11: %r(OK|skip \(.*\))%r
d3d12: %r(OK|skip \(.*\))%r
vulkan: %r(OK|skip \(.*\))%r
metal: %r(OK|skip \(.*\))%r
DONE

--TEST--
Multiview with geometry and tessellation stages: VIO_FEATURE_MULTIVIEW_GEOMETRY / _TESSELLATION let vio_shader combine 'view_count' with a geometry stage or a tessellation pair, gl_ViewIndex reaches those stages, and every view lands in its layer; without the flag the combination is refused
--EXTENSIONS--
vio
--FILE--
<?php
/* OPEN-ITEMS-PLAN A27. The geometry stage / the tessellation evaluation stage
 * colour by gl_ViewIndex; view 0 must come out red, view 1 green. */
$W = 16;
$VS = "#version 450\nlayout(location=0) in vec3 aPos;\nvoid main(){ gl_Position = vec4(aPos, 1.0); }";
$GS = "#version 450\n#extension GL_EXT_multiview : require\nlayout(triangles) in;\nlayout(triangle_strip, max_vertices = 3) out;\n"
    . "layout(location=0) flat out int vview;\n"
    . "void main(){ for (int i = 0; i < 3; i++) { vview = int(gl_ViewIndex); gl_Position = gl_in[i].gl_Position; EmitVertex(); } EndPrimitive(); }";
$TCS = "#version 450\nlayout(vertices = 3) out;\n"
    . "void main(){ gl_out[gl_InvocationID].gl_Position = gl_in[gl_InvocationID].gl_Position;\n"
    . "  if (gl_InvocationID == 0) { gl_TessLevelInner[0] = 1.0; gl_TessLevelOuter[0] = 1.0; gl_TessLevelOuter[1] = 1.0; gl_TessLevelOuter[2] = 1.0; } }";
$TES = "#version 450\n#extension GL_EXT_multiview : require\nlayout(triangles, equal_spacing, ccw) in;\nlayout(location=0) flat out int vview;\n"
    . "void main(){ vview = int(gl_ViewIndex);\n"
    . "  gl_Position = gl_TessCoord.x * gl_in[0].gl_Position + gl_TessCoord.y * gl_in[1].gl_Position + gl_TessCoord.z * gl_in[2].gl_Position; }";
$FS = "#version 450\nlayout(location=0) flat in int vview;\nlayout(location=0) out vec4 o;\n"
    . "void main(){ o = vview == 0 ? vec4(1.0, 0.0, 0.0, 1.0) : vec4(0.0, 1.0, 0.0, 1.0); }";

$opts = ['width' => $W, 'height' => $W, 'headless' => true, 'vsync' => false, 'shader_model' => 6];
$dxc = getenv('VIO_DXC_DIR') ?: '';
if ($dxc === '') foreach (glob('C:/Program Files (x86)/Windows Kits/10/bin/10.*/x64/dxcompiler.dll') ?: [] as $cand) $dxc = dirname($cand);
if ($dxc !== '') $opts['dxc_dir'] = $dxc;
$require = array_map('trim', explode(',', getenv('VIO_REQUIRE_MULTIVIEW_STAGES') ?: ''));

function centre(string $p): array { $o = (8 * 16 + 8) * 4; return [ord($p[$o]), ord($p[$o + 1]), ord($p[$o + 2])]; }

function stage_case($ctx, int $flag, array $stages, string $label, int $patch): string {
    global $W;
    $sh = @vio_shader($ctx, $stages + ['view_count' => 2]);
    if (!vio_supports_feature($ctx, $flag)) return $sh ? "$label FAIL: accepted without its flag" : "$label refused";
    if (!$sh) return "$label FAIL: shader not created";
    $pipe = vio_pipeline($ctx, ['shader' => $sh, 'depth_test' => false, 'cull_mode' => VIO_CULL_NONE] + ($patch ? ['patch_vertices' => 3] : []));
    $tri = vio_mesh($ctx, ['vertices' => [-1,-1,0, 3,-1,0, -1,3,0], 'layout' => [VIO_FLOAT3]]);
    $rt = vio_render_target($ctx, ['width' => $W, 'height' => $W, 'layers' => 2]);
    vio_begin($ctx);
    for ($l = 0; $l < 2; $l++) { vio_bind_render_target($ctx, $rt, $l); vio_clear($ctx, 0, 0, 0, 1); }
    vio_bind_render_target($ctx, $rt, VIO_RT_ALL_LAYERS);
    vio_bind_pipeline($ctx, $pipe);
    vio_draw($ctx, $tri);
    vio_unbind_render_target($ctx);
    vio_end($ctx);
    $err = [];
    foreach ([0 => [255, 0, 0], 1 => [0, 255, 0]] as $v => $want) {
        $c = centre(vio_read_render_target($rt, $v));
        if (abs($c[0] - $want[0]) > 3 || abs($c[1] - $want[1]) > 3) $err[] = "layer $v " . json_encode($c);
    }
    return $err ? "$label FAIL: " . implode(', ', $err) : "$label OK";
}

function run_backend(string $name): string {
    global $VS, $GS, $TCS, $TES, $FS, $opts, $require;
    $req = in_array($name, $require, true);
    $ctx = @vio_create($name, $opts);
    if ($ctx && vio_backend_name($ctx) !== $name) { vio_destroy($ctx); $ctx = null; }
    if (!$ctx) return $req ? "FAIL (unavailable)" : "skip (unavailable)";
    if (!vio_supports_feature($ctx, VIO_FEATURE_MULTIVIEW)) { vio_destroy($ctx); return $req ? "FAIL (no multiview)" : "skip (no multiview)"; }
    $out = [];
    if (vio_supports_feature($ctx, VIO_FEATURE_GEOMETRY))
        $out[] = stage_case($ctx, VIO_FEATURE_MULTIVIEW_GEOMETRY, ['vertex' => $VS, 'geometry' => $GS, 'fragment' => $FS], 'geometry', 0);
    if (vio_supports_feature($ctx, VIO_FEATURE_TESSELLATION))
        $out[] = stage_case($ctx, VIO_FEATURE_MULTIVIEW_TESSELLATION, ['vertex' => $VS, 'tess_control' => $TCS, 'tess_eval' => $TES, 'fragment' => $FS], 'tessellation', 1);
    vio_destroy($ctx);
    $res = implode(' | ', $out);
    if ($req && !str_contains($res, 'OK')) return "FAIL (required): $res";
    return str_contains($res, 'FAIL') ? "FAIL\n  $res" : "OK";
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

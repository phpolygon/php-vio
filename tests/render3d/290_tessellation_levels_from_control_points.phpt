--TEST--
Tessellation levels computed from control point data (a distance per edge through a helper function, a model matrix chosen by a uniform) on every backend, D3D12 with Shader Model 6 and on every hardware D3D adapter
--EXTENSIONS--
vio
--SKIPIF--
<?php
$any = false;
foreach (vio_backends() as $b) {
    if ($b === 'null') continue;
    $c = @vio_create($b, ["width" => 8, "height" => 8, "headless" => true, "vsync" => false]);
    if (!$c) continue;
    if (vio_supports_feature($c, VIO_FEATURE_TESSELLATION)) $any = true;
    vio_destroy($c);
}
if (!$any) die("skip no backend with tessellation");
?>
--FILE--
<?php
/* The control stage computes each outer level from the control points: the
 * distance of the edge's midpoint (model matrix applied) to an eye point.
 * The eye sits on the midpoint of the edge between points 0 and 2, so
 * outer[1] = 6 and every other level is 1; the evaluation stage marks interior
 * vertices of the t.y = 0 edge red (as test 144 B).
 *
 * On D3D the hull shader is generated from the GLSL. Under Shader Model 6 its
 * state used to be statics, which DXC keeps as DXIL globals in a hull shader;
 * the Intel UHD 630 D3D12 driver aborted the process compiling the pipeline
 * when the levels depended on control point data. The hardware runs open
 * every D3D adapter (on a CI runner only WARP exists, so they skip). */
$W = 64;
$VS = "#version 450\nlayout(location=0) in vec3 aPos;\nlayout(location=0) out vec3 tcPos;\nlayout(location=1) out vec4 tcCol;\n"
    . "void main(){ tcPos = aPos; tcCol = vec4(aPos, 1.0); gl_Position = vec4(aPos, 1.0); }";
$TCS = <<<'GLSL'
#version 450
layout(vertices = 3) out;
uniform vec4 u_tess;          // max level, distance scale, -, on
uniform vec3 u_eye;
uniform mat4 u_model;
uniform int  u_instanced;
layout(location = 0) in vec3 tcPos[];
layout(location = 1) in vec4 tcCol[];
layout(location = 0) out vec3 tePos[];
layout(location = 1) out vec4 teCol[];

float edgeLevel(vec3 a, vec3 b, vec3 eye) {
    return max(1.0, u_tess.x - distance((a + b) * 0.5, eye) * u_tess.y);
}

void main() {
    tePos[gl_InvocationID] = tcPos[gl_InvocationID];
    teCol[gl_InvocationID] = tcCol[gl_InvocationID];
    if (gl_InvocationID == 0) {
        mat4 m = u_instanced == 1 ? mat4(tcCol[0], tcCol[1], tcCol[2], vec4(0.0, 0.0, 0.0, 1.0)) : u_model;
        vec3 p0 = (m * vec4(tcPos[0], 1.0)).xyz;
        vec3 p1 = (m * vec4(tcPos[1], 1.0)).xyz;
        vec3 p2 = (m * vec4(tcPos[2], 1.0)).xyz;
        float l0 = u_tess.w > 0.5 ? edgeLevel(p1, p2, u_eye) : 1.0;
        float l1 = u_tess.w > 0.5 ? edgeLevel(p2, p0, u_eye) : 1.0;
        float l2 = u_tess.w > 0.5 ? edgeLevel(p0, p1, u_eye) : 1.0;
        gl_TessLevelOuter[0] = l0;
        gl_TessLevelOuter[1] = l1;
        gl_TessLevelOuter[2] = l2;
        gl_TessLevelInner[0] = 1.0;
    }
    gl_out[gl_InvocationID].gl_Position = gl_in[gl_InvocationID].gl_Position;
}
GLSL;
$TES = "#version 450\nlayout(triangles, equal_spacing, ccw) in;\nlayout(location=0) in vec3 tePos[];\nlayout(location=1) in vec4 teCol[];\nlayout(location=0) out vec3 c;\n"
     . "void main(){ vec3 t = gl_TessCoord; float m = (t.y == 0.0 && t.x > 0.0 && t.z > 0.0) ? 1.0 : 0.0;\n"
     . "  c = vec3(m, t.y, teCol[0].w * 0.0);\n"
     . "  gl_Position = vec4(tePos[0] * t.x + tePos[1] * t.y + tePos[2] * t.z, 1.0); }";
$FS = "#version 450\nlayout(location=0) in vec3 c;\nlayout(location=0) out vec4 o;\nvoid main(){ o = vec4(c, 1.0); }";

function run_backend(string $name, array $opts = []): string {
    global $W, $VS, $TCS, $TES, $FS;
    $ctx = @vio_create($name, ["width" => $W, "height" => $W, "headless" => true, "vsync" => false] + $opts);
    if (!$ctx) return "skip (unavailable)";
    if (!vio_supports_feature($ctx, VIO_FEATURE_TESSELLATION)) { vio_destroy($ctx); return "skip (no tessellation)"; }
    if (isset($opts['shader_model']) && (vio_swapchain_info($ctx)['shader_model'] ?? 5) < 6) {
        vio_destroy($ctx);
        return getenv('VIO_REQUIRE_SM6') ? "FAIL\n  VIO_REQUIRE_SM6 set but DXC unavailable" : "skip (no DXC)";
    }
    $sh = vio_shader($ctx, ['vertex' => $VS, 'tess_control' => $TCS, 'tess_eval' => $TES, 'fragment' => $FS]);
    $pipe = $sh ? vio_pipeline($ctx, ['shader' => $sh, 'patch_vertices' => 3, 'depth_test' => false, 'cull_mode' => VIO_CULL_BACK]) : null;
    if (!$pipe) { vio_destroy($ctx); return "FAIL\n  shader or pipeline not created"; }
    $tri = vio_mesh($ctx, ['vertices' => [-1,-1,0, 1,-1,0, 0,1,0], 'layout' => [VIO_FLOAT3]]);
    $fail = [];
    /* on: outer[1] = 6 from the distances; off: every level 1 */
    foreach (['on' => [1.0, true], 'off' => [0.0, false]] as $k => [$on, $split]) {
        vio_clear($ctx, 0, 0, 0, 1);
        vio_begin($ctx);
        vio_bind_pipeline($ctx, $pipe);
        vio_set_uniform($ctx, 'u_tess', [6.0, 100.0, 0.0, $on]);
        vio_set_uniform($ctx, 'u_eye', [-0.5, 0.0, 0.0]);
        vio_set_uniform($ctx, 'u_model', [1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1]);
        vio_set_uniform($ctx, 'u_instanced', 0);
        vio_draw($ctx, $tri);
        vio_end($ctx);
        $p = vio_read_pixels($ctx);
        $lit = 0; $red = 0; $g = -1;
        for ($i = 0; $i < $W * $W; $i++) {
            $r = ord($p[$i * 4]);
            if ($r || ord($p[$i * 4 + 1]) || ord($p[$i * 4 + 2])) $lit++;
            if ($r > $red) { $red = $r; $g = ord($p[$i * 4 + 1]); }
        }
        if (abs($lit - 2048) > 128) $fail[] = "$k: $lit px lit, want ~2048";
        elseif ($split && ($red < 128 || $g > 40)) $fail[] = "$k: edge t.y = 0 not subdivided (red $red at v=" . round($g / 255, 2) . ")";
        elseif (!$split && $red > 0) $fail[] = "$k: an edge was subdivided with every level 1 (red $red)";
    }
    vio_destroy($ctx);
    return $fail ? "FAIL\n  " . implode("\n  ", $fail) : "OK";
}

foreach (['opengl', 'd3d11', 'd3d12', 'vulkan', 'metal'] as $b) {
    echo "$b: ", run_backend($b), "\n";
}
/* DXC as in test 119: VIO_DXC_DIR, else the Windows SDK, else the search path. */
$sm6 = ['shader_model' => 6];
$dxc = getenv('VIO_DXC_DIR') ?: '';
if ($dxc === '') foreach (glob('C:/Program Files (x86)/Windows Kits/10/bin/10.*/x64/dxcompiler.dll') ?: [] as $cand) $dxc = dirname($cand);
if ($dxc !== '') $sm6['dxc_dir'] = $dxc;
echo "d3d12 sm6: ", run_backend('d3d12', $sm6), "\n";

/* Every hardware adapter of D3D11 / D3D12 (D3D12 with Shader Model 6 too). */
foreach (['d3d11' => [[]], 'd3d12' => [[], $sm6]] as $b => $variants) {
    $adapters = in_array($b, vio_backends(), true) ? (vio_adapters($b)[$b] ?? []) : [];
    $results = [];
    foreach ($adapters as $a) {
        if (($a['device_type'] ?? '') === 'software') continue;
        foreach ($variants as $v) {
            $r = run_backend($b, ['headless_hardware' => true, 'adapter' => $a['index']] + $v);
            if (!str_starts_with($r, 'skip')) $results[] = $r === 'OK' ? 'OK' : "{$a['name']}" . (isset($v['shader_model']) ? ' sm6' : '') . ": $r";
        }
    }
    $bad = array_filter($results, fn($r) => $r !== 'OK');
    echo "$b hardware: ", !$results ? "skip (no hardware adapter)" : ($bad ? "FAIL\n  " . implode("\n  ", $bad) : "OK"), "\n";
}
echo "DONE\n";
?>
--EXPECTF--
opengl: %r(OK|skip \(.*\))%r
d3d11: %r(OK|skip \(.*\))%r
d3d12: %r(OK|skip \(.*\))%r
vulkan: %r(OK|skip \(.*\))%r
metal: %r(OK|skip \(.*\))%r
d3d12 sm6: %r(OK|skip \(.*\))%r
d3d11 hardware: %r(OK|skip \(.*\))%r
d3d12 hardware: %r(OK|skip \(.*\))%r
DONE

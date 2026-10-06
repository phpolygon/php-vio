--TEST--
Barycentric coordinates (VIO_FEATURE_BARYCENTRICS): gl_BaryCoordEXT in the fragment stage equals the interpolation weights of the triangle's vertices, in vertex order, on every backend that reports the flag
--EXTENSIONS--
vio
--FILE--
<?php
/* Each vertex carries a unit vector (1,0,0) / (0,1,0) / (0,0,1) as an ordinary
 * varying; interpolated, that IS the barycentric weight of each vertex. The
 * fragment shader compares it with gl_BaryCoordEXT and writes green where they
 * agree - independent of the target's row order, and it catches a backend that
 * permutes the vertices. Two triangles with different winding of their screen
 * corners cover the target. w = 1, so
 * perspective-correct and linear weights coincide.
 * Metal reports the flag wherever vio_backend_info() lists barycentrics;
 * D3D12 needs Shader Model 6.1 (DXC) and BarycentricsSupported.
 * VIO_REQUIRE_BARYCENTRICS=vulkan makes the listed backends mandatory. */
$W = 16;
$VS = "#version 450\nlayout(location=0) in vec3 aPos;\nlayout(location=1) in vec3 aW;\nlayout(location=0) out vec3 c;\n"
    . "void main(){ c = aW; gl_Position = vec4(aPos, 1.0); }";
$FS = "#version 450\n"
    . "#extension GL_EXT_fragment_shader_barycentric : require\n"
    . "layout(location=0) in vec3 c;\nlayout(location=0) out vec4 o;\n"
    . "void main(){ bool ok = distance(gl_BaryCoordEXT, c) < 0.02 && abs(gl_BaryCoordEXT.x + gl_BaryCoordEXT.y + gl_BaryCoordEXT.z - 1.0) < 0.01;\n"
    . "  o = ok ? vec4(0.0, 1.0, 0.0, 1.0) : vec4(1.0, 0.0, 0.0, 1.0); }";

$opts = ["width" => $W, "height" => $W, "headless" => true, "vsync" => false, "shader_model" => 6];
$dxc = getenv('VIO_DXC_DIR') ?: '';
if ($dxc === '') foreach (glob('C:/Program Files (x86)/Windows Kits/10/bin/10.*/x64/dxcompiler.dll') ?: [] as $cand) $dxc = dirname($cand);
if ($dxc !== '') $opts['dxc_dir'] = $dxc;
$require = array_map('trim', explode(',', getenv('VIO_REQUIRE_BARYCENTRICS') ?: ''));

function run_backend(string $name): string {
    global $W, $VS, $FS, $opts, $require;
    $req = in_array($name, $require, true);
    $ctx = @vio_create($name, $opts);
    if ($ctx && vio_backend_name($ctx) !== $name) { vio_destroy($ctx); $ctx = null; }
    if (!$ctx) return $req ? "FAIL\n  required but unavailable" : "skip (unavailable)";
    $flag = vio_supports_feature($ctx, VIO_FEATURE_BARYCENTRICS);
    $info = vio_backend_info($ctx);
    if ($name === 'metal' && is_array($info) && $flag !== $info['caps']['barycentrics']) {
        vio_destroy($ctx);
        return "FAIL\n  VIO_FEATURE_BARYCENTRICS " . json_encode($flag) . " but caps barycentrics " . json_encode($info['caps']['barycentrics']);
    }
    if (!$flag) {
        vio_destroy($ctx);
        return $req ? "FAIL\n  required but VIO_FEATURE_BARYCENTRICS is 0" : "skip (no barycentrics)";
    }
    $sh = vio_shader($ctx, ['vertex' => $VS, 'fragment' => $FS]);
    if (!$sh) { vio_destroy($ctx); return "FAIL\n  shader not created"; }
    $pipe = vio_pipeline($ctx, ['shader' => $sh, 'depth_test' => false, 'cull_mode' => VIO_CULL_NONE]);
    /* Vertex k of every triangle carries e_k, so the interpolated varying is
     * the barycentric vector in vertex order. No shared vertices (a shared one
     * would be vertex 0 of one triangle and vertex 1 of the other). */
    $quad = vio_mesh($ctx, ['vertices' => [
        -1,-1,0, 1,0,0,   1,-1,0, 0,1,0,   1,1,0, 0,0,1,
        -1,-1,0, 1,0,0,   1,1,0, 0,1,0,   -1,1,0, 0,0,1,
    ], 'layout' => [VIO_FLOAT3, VIO_FLOAT3]]);
    vio_clear($ctx, 0, 0, 0, 1);
    vio_begin($ctx);
    vio_bind_pipeline($ctx, $pipe);
    vio_draw($ctx, $quad);
    vio_end($ctx);
    $p = vio_read_pixels($ctx);
    vio_destroy($ctx);
    $bad = 0; $first = null;
    for ($y = 0; $y < $W; $y++) for ($x = 0; $x < $W; $x++) {
        $o = ($y * $W + $x) * 4;
        if (ord($p[$o + 1]) < 250 || ord($p[$o]) > 5) { $bad++; $first ??= "($x,$y) " . json_encode([ord($p[$o]), ord($p[$o + 1]), ord($p[$o + 2])]); }
    }
    return $bad ? "FAIL\n  $bad of " . ($W * $W) . " pixels wrong, first $first" : "OK";
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

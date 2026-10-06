--TEST--
Quad operations (VIO_FEATURE_SUBGROUP_QUAD): GL_KHR_shader_subgroup_quad in the fragment stage reads the 2x2 neighbours of each pixel on every backend that reports the flag
--EXTENSIONS--
vio
--FILE--
<?php
/* A full-screen quad; each fragment knows its integer pixel (gl_FragCoord).
 * Fragment quads are aligned 2x2 blocks at even coordinates on every API, so:
 *   subgroupQuadSwapHorizontal(x) == x ^ 1, subgroupQuadSwapVertical(y) == y ^ 1,
 *   subgroupQuadSwapDiagonal(x + 4096 y) == (x ^ 1) + 4096 (y ^ 1),
 *   subgroupQuadBroadcast(x, 0) == x & ~1.
 * The row order of the target (origin top or bottom) does not matter: with an
 * even height the flip keeps the row pairs. Green = all hold.
 * Metal reports the flag wherever vio_backend_info() lists quad_group;
 * D3D12 needs Shader Model 6 (DXC). VIO_REQUIRE_SUBGROUP_QUAD=vulkan,opengl
 * makes the listed backends mandatory (Linux CI). */
$W = 16;
$VS = "#version 450\nlayout(location=0) in vec3 aPos;\nvoid main(){ gl_Position = vec4(aPos, 1.0); }";
$FS = "#version 450\n"
    . "#extension GL_KHR_shader_subgroup_quad : require\n"
    . "layout(location=0) out vec4 o;\n"
    . "void main(){ uint x = uint(gl_FragCoord.x), y = uint(gl_FragCoord.y);\n"
    . "  bool ok = subgroupQuadSwapHorizontal(x) == (x ^ 1u)\n"
    . "         && subgroupQuadSwapVertical(y) == (y ^ 1u)\n"
    . "         && subgroupQuadSwapDiagonal(x + 4096u * y) == ((x ^ 1u) + 4096u * (y ^ 1u))\n"
    . "         && subgroupQuadBroadcast(x, 0u) == (x & ~1u);\n"
    . "  o = ok ? vec4(0.0, 1.0, 0.0, 1.0) : vec4(1.0, 0.0, 0.0, 1.0); }";

$opts = ["width" => $W, "height" => $W, "headless" => true, "vsync" => false, "shader_model" => 6];
$dxc = getenv('VIO_DXC_DIR') ?: '';
if ($dxc === '') foreach (glob('C:/Program Files (x86)/Windows Kits/10/bin/10.*/x64/dxcompiler.dll') ?: [] as $cand) $dxc = dirname($cand);
if ($dxc !== '') $opts['dxc_dir'] = $dxc;
$require = array_map('trim', explode(',', getenv('VIO_REQUIRE_SUBGROUP_QUAD') ?: ''));

function run_backend(string $name): string {
    global $W, $VS, $FS, $opts, $require;
    $req = in_array($name, $require, true);
    $ctx = @vio_create($name, $opts);
    if ($ctx && vio_backend_name($ctx) !== $name) { vio_destroy($ctx); $ctx = null; }
    if (!$ctx) return $req ? "FAIL\n  required but unavailable" : "skip (unavailable)";
    $flag = vio_supports_feature($ctx, VIO_FEATURE_SUBGROUP_QUAD);
    $info = vio_backend_info($ctx);
    if ($name === 'metal' && is_array($info) && $flag !== $info['caps']['quad_group']) {
        vio_destroy($ctx);
        return "FAIL\n  VIO_FEATURE_SUBGROUP_QUAD " . json_encode($flag) . " but caps quad_group " . json_encode($info['caps']['quad_group']);
    }
    if (!$flag) {
        vio_destroy($ctx);
        if ($name === 'd3d12' && getenv('VIO_REQUIRE_SM6')) return "FAIL\n  VIO_REQUIRE_SM6 set but no quad operations";
        return $req ? "FAIL\n  required but VIO_FEATURE_SUBGROUP_QUAD is 0" : "skip (no quad operations)";
    }
    $sh = vio_shader($ctx, ['vertex' => $VS, 'fragment' => $FS]);
    if (!$sh) { vio_destroy($ctx); return "FAIL\n  shader not created"; }
    $pipe = vio_pipeline($ctx, ['shader' => $sh, 'depth_test' => false, 'cull_mode' => VIO_CULL_NONE]);
    $quad = vio_mesh($ctx, ['vertices' => [-1,-1,0, 1,-1,0, 1,1,0, -1,1,0], 'indices' => [0,1,2, 0,2,3], 'layout' => [VIO_FLOAT3]]);
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

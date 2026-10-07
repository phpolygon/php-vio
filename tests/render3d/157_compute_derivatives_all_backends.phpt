--TEST--
Compute derivatives (VIO_FEATURE_COMPUTE_DERIVATIVES): dFdx / dFdy in a compute shader with derivative_group_quadsNV on every backend that reports the flag
--EXTENSIONS--
vio
--FILE--
<?php
/* An 8x8 group in quad layout (2x2 blocks of local x / y): for the linear
 * function f = 3 x + 5 y every thread must see dFdx(f) = 3 and dFdy(f) = 5 -
 * coarse and fine agree for a linear function, so the result is exact.
 * Metal has no derivatives in kernel functions (the driver rejects them): 0.
 * D3D12 needs SM 6.6. VIO_REQUIRE_COMPUTE_DERIVATIVES=vulkan makes the listed
 * backends mandatory. */
$CS = "#version 450\n"
    . "#extension GL_NV_compute_shader_derivatives : require\n"
    . "layout(local_size_x = 8, local_size_y = 8) in;\n"
    . "layout(derivative_group_quadsNV) in;\n"
    . "layout(std430, binding = 0) buffer B { vec2 v[]; } b;\n"
    . "void main(){ float f = float(gl_LocalInvocationID.x * 3u + gl_LocalInvocationID.y * 5u);\n"
    . "  b.v[gl_LocalInvocationIndex] = vec2(dFdx(f), dFdy(f)); }\n";
$opts = ["width" => 16, "height" => 16, "headless" => true, "vsync" => false, "shader_model" => 6];
$dxc = getenv('VIO_DXC_DIR') ?: '';
if ($dxc === '') foreach (glob('C:/Program Files (x86)/Windows Kits/10/bin/10.*/x64/dxcompiler.dll') ?: [] as $cand) $dxc = dirname($cand);
if ($dxc !== '') $opts['dxc_dir'] = $dxc;
function open_backend(string $name, int $flag, string $env, ?string &$skip): mixed {
    global $opts;
    $req = in_array($name, array_map('trim', explode(',', getenv($env) ?: '')), true);
    $ctx = @vio_create($name, $opts);
    if ($ctx && vio_backend_name($ctx) !== $name) { vio_destroy($ctx); $ctx = null; }
    if (!$ctx) { $skip = $req ? "FAIL\n  required but unavailable" : "skip (unavailable)"; return null; }
    if (!vio_supports_feature($ctx, $flag)) { vio_destroy($ctx); $skip = $req ? "FAIL\n  required but the flag is 0" : "skip (not supported)"; return null; }
    return $ctx;
}

function run_backend(string $name): string {
    global $CS;
    $ctx = open_backend($name, VIO_FEATURE_COMPUTE_DERIVATIVES, 'VIO_REQUIRE_COMPUTE_DERIVATIVES', $skip);
    if ($name === 'metal' && $ctx) { vio_destroy($ctx); return "FAIL\n  metal reports VIO_FEATURE_COMPUTE_DERIVATIVES (kernels have no derivatives)"; }
    if (!$ctx) return $skip;
    $cp = vio_compute_pipeline($ctx, ['source' => $CS]);
    if (!$cp) { vio_destroy($ctx); return "FAIL\n  compute pipeline not created"; }
    $buf = vio_storage_buffer($ctx, ['size' => 64 * 8]);
    vio_compute_bind_buffer($ctx, $cp, $buf, 0, VIO_COMPUTE_WRITE);
    vio_compute_dispatch($ctx, $cp, 1, 1, 1);
    $v = array_values(unpack('g*', vio_storage_buffer_read($ctx, $buf)));
    vio_destroy($ctx);
    for ($t = 0; $t < 64; $t++) {
        if ($v[2 * $t] !== 3.0 || $v[2 * $t + 1] !== 5.0) return "FAIL\n  thread $t: (" . $v[2 * $t] . ", " . $v[2 * $t + 1] . "), want (3, 5)";
    }
    return "OK";
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

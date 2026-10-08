--TEST--
16-bit floats (VIO_FEATURE_SHADER_FLOAT16): float16_t arithmetic in shaders really is half precision on every backend that reports the flag
--EXTENSIONS--
vio
--FILE--
<?php
/* GL_EXT_shader_explicit_arithmetic_types_float16 without 16-bit storage: the
 * kernel reads floats, converts to float16_t at runtime and computes in half
 * precision. 2048 converts exactly; 2048 + 1 = 2049 is not representable in
 * binary16 (11-bit significand) and the half addition rounds it to 2048, so
 * the sum is 2048.75, while a backend that keeps float16_t at 32 bits (HLSL
 * min16float, a precision hint only) yields 2049.75.
 * The proof must sit in half ARITHMETIC on exact inputs: drivers fold inexact
 * float -> float16_t conversions away (lavapipe returns 0.1 for half(0.1);
 * NVIDIA keeps half(2049) at 2049, although its half add / mul round).
 * D3D12 needs SM 6.2 + Native16BitShaderOpsSupported (half, -enable-16bit-types).
 * VIO_REQUIRE_SHADER_FLOAT16=vulkan,d3d12 makes the listed backends mandatory. */
$CS = "#version 450\n"
    . "#extension GL_EXT_shader_explicit_arithmetic_types_float16 : require\n"
    . "layout(local_size_x = 2) in;\n"
    . "layout(std430, binding = 0) buffer B { float v[]; } b;\n"
    . "void main(){ uint i = gl_GlobalInvocationID.x;\n"
    . "  float16_t a = float16_t(b.v[i] + 2048.0);\n"
    . "  f16vec2 p = f16vec2(a, float16_t(0.5)) + f16vec2(float16_t(1.0), float16_t(0.25));\n"
    . "  b.v[2 + i] = float(p.x) + float(p.y); }\n";
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
    $ctx = open_backend($name, VIO_FEATURE_SHADER_FLOAT16, 'VIO_REQUIRE_SHADER_FLOAT16', $skip);
    if (!$ctx) return $skip;
    $cp = vio_compute_pipeline($ctx, ['source' => $CS]);
    if (!$cp) { vio_destroy($ctx); return "FAIL\n  compute pipeline not created"; }
    $buf = vio_storage_buffer($ctx, ['data' => pack('g*', 0, 0, 0, 0)]);
    vio_compute_bind_buffer($ctx, $cp, $buf, 0, VIO_COMPUTE_WRITE);
    vio_compute_dispatch($ctx, $cp, 1, 1, 1);
    $v = array_values(unpack('g*', vio_storage_buffer_read($ctx, $buf)));
    vio_destroy($ctx);
    $fail = [];
    foreach ([2, 3] as $k) if ($v[$k] !== 2048.75) $fail[] = "(half(2048) + 1) + (0.5 + 0.25) = {$v[$k]}, want 2048.75";
    return $fail ? "FAIL\n  " . implode("\n  ", array_unique($fail)) : "OK";
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

--TEST--
16-bit float specials (Shader Model 6.9, SM69-PLAN Phase 1): isnan / isinf on float16_t built from bit patterns (NaN, +-Inf, 1.0, a denormal, 0, the largest half) classify correctly in a compute kernel on every backend with VIO_FEATURE_SHADER_FLOAT16
--EXTENSIONS--
vio
--FILE--
<?php
/* Shader Model 6.9 defines the 16-bit overloads of the IsSpecialFloat family
 * on half. A contract test, not a 6.9 probe: the OS WARP (SM 6.8), WARP 1.0.21
 * (SM 6.9), the RTX 2080, Vulkan and OpenGL classify alike already. The values
 * come from bit patterns (unpackHalf2x16 is core GLSL; uint16BitsToFloat16
 * needs an int16 extension NVIDIA's GL lacks), so no conversion hides a special. */
$CS = <<<'GLSL'
#version 450
#extension GL_EXT_shader_explicit_arithmetic_types_float16 : require
layout(local_size_x = 8) in;
layout(std430, binding = 0) buffer O { uint o[]; };
const uint bits[8] = uint[8](0x7E00u, 0x7C00u, 0xFC00u, 0x3C00u, 0x0001u, 0x0000u, 0x7BFFu, 0xFE01u);
void main() {
    uint i = gl_GlobalInvocationID.x;
    /* unpackHalf2x16 (core) gives the exact float; the conversion keeps NaN / Inf / denormals */
    float16_t h = float16_t(unpackHalf2x16(bits[i]).x);
    o[i] = (isnan(h) ? 1u : 0u) | (isinf(h) ? 2u : 0u);
}
GLSL;
$opts = ['width' => 8, 'height' => 8, 'headless' => true, 'vsync' => false, 'shader_model' => 6];
$require = array_map('trim', explode(',', getenv('VIO_REQUIRE_SHADER_FLOAT16') ?: ''));

function run_backend(string $name): string {
    global $CS, $opts, $require;
    $req = in_array($name, $require, true);
    $ctx = @vio_create($name, $opts);
    if ($ctx && vio_backend_name($ctx) !== $name) { vio_destroy($ctx); $ctx = null; }
    if (!$ctx) return $req ? "FAIL\n  required but unavailable" : "skip (unavailable)";
    if (!vio_supports_feature($ctx, VIO_FEATURE_COMPUTE) || !vio_supports_feature($ctx, VIO_FEATURE_SHADER_FLOAT16)) {
        vio_destroy($ctx);
        return $req ? "FAIL\n  required but VIO_FEATURE_SHADER_FLOAT16 is 0" : "skip (no 16-bit floats)";
    }
    $cp = vio_compute_pipeline($ctx, ['source' => $CS]);
    if (!$cp) { vio_destroy($ctx); return "FAIL\n  compute pipeline not created"; }
    $o = vio_storage_buffer($ctx, ['size' => 32]);
    vio_compute_bind_buffer($ctx, $cp, $o, 0, VIO_COMPUTE_WRITE);
    vio_compute_dispatch($ctx, $cp, 1, 1, 1);
    $got = array_values(unpack('V8', vio_storage_buffer_read($ctx, $o)));
    vio_destroy($ctx);
    /* NaN, +Inf, -Inf, 1.0, denormal, 0, max half (65504), NaN with payload */
    $want = [1, 2, 2, 0, 0, 0, 0, 1];
    return $got === $want ? "OK" : "FAIL\n  " . json_encode($got) . ", want " . json_encode($want);
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

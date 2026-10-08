--TEST--
Long vectors (VIO_FEATURE_LONG_VECTOR, Shader Model 6.9, SM69-PLAN Phase 2): a compute kernel loads, scales and stores vectors of 12 floats - GLSL GL_EXT_long_vector on Vulkan, HLSL vector<float, 12> through vio_compute_pipeline(['hlsl' => ...]) on D3D12; the 'hlsl' override replaces the translated kernel on D3D11 / D3D12 and is ignored elsewhere
--EXTENSIONS--
vio
--FILE--
<?php
/* Four threads, twelve floats each: v = a * 3 + 1 as one 12-component vector.
 * SPIRV-Cross has no HLSL form for long vectors (it writes float12 and a
 * 12-letter swizzle), so D3D12 runs the HLSL kernel given next to the GLSL one;
 * the GLSL kernel stays required for the reflection (binding 0 -> t0,
 * binding 1 -> u1). VIO_REQUIRE_LONG_VECTOR=d3d12,vulkan makes backends
 * mandatory. */
$N = 12;
$GLSL = <<<'GLSL'
#version 460
#extension GL_EXT_long_vector : require
layout(local_size_x = 4) in;
layout(std430, binding = 0) readonly buffer A { float a[]; };
layout(std430, binding = 1) writeonly buffer O { float o[]; };
void main() {
    uint base = gl_GlobalInvocationID.x * 12u;
    vector<float, 12> v;
    for (uint i = 0u; i < 12u; i++) v[i] = a[base + i];
    v = v * 3.0 + 1.0;
    for (uint i = 0u; i < 12u; i++) o[base + i] = v[i];
}
GLSL;
$HLSL = <<<'HLSL'
ByteAddressBuffer a : register(t0);
RWByteAddressBuffer o : register(u1);
[numthreads(4, 1, 1)]
void main(uint3 id : SV_DispatchThreadID) {
    uint base = id.x * 12 * 4;
    vector<float, 12> v = a.Load< vector<float, 12> >(base);
    v = v * 3.0 + 1.0;
    o.Store< vector<float, 12> >(base, v);
}
HLSL;
/* The plain override check: GLSL doubles, HLSL doubles and adds 0.5. */
$PLAIN_GLSL = "#version 450\nlayout(local_size_x = 4) in;\n"
    . "layout(std430, binding = 0) readonly buffer A { float a[]; };\n"
    . "layout(std430, binding = 1) writeonly buffer O { float o[]; };\n"
    . "void main(){ uint i = gl_GlobalInvocationID.x; o[i] = a[i] * 2.0; }\n";
$PLAIN_HLSL = "ByteAddressBuffer a : register(t0);\nRWByteAddressBuffer o : register(u1);\n"
    . "[numthreads(4, 1, 1)]\nvoid main(uint3 id : SV_DispatchThreadID) {\n"
    . "    o.Store(id.x * 4, asuint(asfloat(a.Load(id.x * 4)) * 2.0 + 0.5)); }\n";

$opts = ['width' => 8, 'height' => 8, 'headless' => true, 'vsync' => false, 'shader_model' => 6];
$require = array_map('trim', explode(',', getenv('VIO_REQUIRE_LONG_VECTOR') ?: ''));

function dispatch($ctx, $cp, array $in, int $groups): array {
    $a = vio_storage_buffer($ctx, ['data' => pack('g*', ...$in)]);
    $o = vio_storage_buffer($ctx, ['size' => count($in) * 4]);
    vio_compute_bind_buffer($ctx, $cp, $a, 0, VIO_COMPUTE_READ);
    vio_compute_bind_buffer($ctx, $cp, $o, 1, VIO_COMPUTE_WRITE);
    vio_compute_dispatch($ctx, $cp, $groups, 1, 1);
    return array_values(unpack('g*', vio_storage_buffer_read($ctx, $o)));
}

function run_backend(string $name): string {
    global $GLSL, $HLSL, $PLAIN_GLSL, $PLAIN_HLSL, $N, $opts, $require;
    $req = in_array($name, $require, true);
    $ctx = @vio_create($name, $opts);
    if ($ctx && vio_backend_name($ctx) !== $name) { vio_destroy($ctx); $ctx = null; }
    if (!$ctx) return $req ? "FAIL\n  required but unavailable" : "skip (unavailable)";
    if (!vio_supports_feature($ctx, VIO_FEATURE_COMPUTE)) { vio_destroy($ctx); return "skip (no compute)"; }
    $fail = [];

    /* the override itself, with a kernel every backend can run */
    $cp = vio_compute_pipeline($ctx, ['source' => $PLAIN_GLSL, 'hlsl' => $PLAIN_HLSL]);
    $got = $cp ? dispatch($ctx, $cp, [1.0, 2.0, 3.0, 4.0], 1) : null;
    $want = in_array($name, ['d3d11', 'd3d12'], true) ? [2.5, 4.5, 6.5, 8.5] : [2.0, 4.0, 6.0, 8.0];
    if ($got !== $want) $fail[] = "override: " . json_encode($got) . ", want " . json_encode($want);
    foreach (['', 5] as $bad) {
        try { vio_compute_pipeline($ctx, ['source' => $PLAIN_GLSL, 'hlsl' => $bad]); $fail[] = "'hlsl' => " . json_encode($bad) . " accepted"; }
        catch (ValueError $e) {}
    }

    if (!vio_supports_feature($ctx, VIO_FEATURE_LONG_VECTOR)) {
        vio_destroy($ctx);
        if ($fail) return "FAIL\n  " . implode("\n  ", $fail);
        return $req ? "FAIL\n  required but VIO_FEATURE_LONG_VECTOR is 0" : "skip (no long vectors)";
    }
    $in = [];
    for ($i = 0; $i < 4 * $N; $i++) $in[] = (float)($i % 17) - 4.0;
    $cp = vio_compute_pipeline($ctx, ['source' => $GLSL, 'hlsl' => $HLSL]);
    $got = $cp ? dispatch($ctx, $cp, $in, 1) : null;
    $want = array_map(fn($x) => $x * 3.0 + 1.0, $in);
    if ($got !== $want) $fail[] = "long vector: " . json_encode($got ? array_slice($got, 0, 14) : $got) . "..., want " . json_encode(array_slice($want, 0, 14)) . "...";
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

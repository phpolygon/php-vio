--TEST--
vio_compute_pipeline(['msl' => ...]) (OPEN-ITEMS A17): Metal runs the given MSL kernel with the GLSL kernel's bindings and local size, the other backends the GLSL; a broken MSL kernel fails on Metal; with MSL 4 tensors (caps 'tensors') an inline-tensor kernel runs
--EXTENSIONS--
vio
--FILE--
<?php
/* The GLSL kernel doubles, the MSL override triples and adds one: the
 * result names the kernel that ran. */
$GLSL = "#version 450\nlayout(local_size_x = 64) in;\n"
      . "layout(std430, binding = 0) readonly buffer In { float a[]; };\n"
      . "layout(std430, binding = 1) writeonly buffer Out { float b[]; };\n"
      . "void main() { uint i = gl_GlobalInvocationID.x; b[i] = a[i] * 2.0; }";
$MSL = "#include <metal_stdlib>\nusing namespace metal;\n"
     . "kernel void main0(device const float *src [[buffer(0)]], device float *dst [[buffer(1)]],\n"
     . "                  uint i [[thread_position_in_grid]]) { dst[i] = src[i] * 3.0f + 1.0f; }\n";
/* MSL 4: the same through tensors over the buffers (tensor_inline) */
$MSL_TENSOR = "#include <metal_stdlib>\n#include <metal_tensor>\nusing namespace metal;\n"
     . "kernel void main0(device float *src [[buffer(0)]], device float *dst [[buffer(1)]],\n"
     . "                  uint i [[thread_position_in_grid]]) {\n"
     . "    auto a = tensor<device float, dextents<int32_t, 1>, tensor_inline>(src, dextents<int32_t, 1>(64));\n"
     . "    auto b = tensor<device float, dextents<int32_t, 1>, tensor_inline>(dst, dextents<int32_t, 1>(64));\n"
     . "    b[int(i)] = a[int(i)] * 3.0f + 1.0f;\n"
     . "}\n";

function run_kernel($ctx, array $cfg): ?array {
    $cp = @vio_compute_pipeline($ctx, $cfg);
    if (!$cp) return null;
    $in = vio_storage_buffer($ctx, ['data' => pack('g*', ...range(0, 63)), 'stride' => 4]);
    $out = vio_storage_buffer($ctx, ['size' => 64 * 4, 'stride' => 4]);
    vio_compute_bind_buffer($ctx, $cp, $in, 0, VIO_COMPUTE_READ);
    vio_compute_bind_buffer($ctx, $cp, $out, 1, VIO_COMPUTE_WRITE);
    vio_compute_dispatch($ctx, $cp, 1, 1, 1);
    return array_values(unpack('g*', vio_storage_buffer_read($ctx, $out)));
}

function run_backend(string $name): string {
    global $GLSL, $MSL, $MSL_TENSOR;
    $ctx = @vio_create($name, ['width' => 8, 'height' => 8, 'headless' => true]);
    if ($ctx && vio_backend_name($ctx) !== $name) { vio_destroy($ctx); $ctx = null; }
    if (!$ctx) return "skip (unavailable)";
    if (!vio_supports_feature($ctx, VIO_FEATURE_COMPUTE)) { vio_destroy($ctx); return "skip (no compute)"; }
    $fail = [];
    $metal = $name === 'metal';
    $r = run_kernel($ctx, ['source' => $GLSL, 'msl' => $MSL]);
    $want = array_map(fn($x) => $metal ? $x * 3.0 + 1.0 : $x * 2.0, range(0, 63));
    if ($r === null) $fail[] = "pipeline with 'msl' failed";
    elseif ($r != $want) $fail[] = "results " . json_encode(array_slice($r, 0, 4)) . " (expected the " . ($metal ? 'MSL' : 'GLSL') . " kernel)";
    if ($metal) {
        if (run_kernel($ctx, ['source' => $GLSL, 'msl' => "kernel void main0( this is not MSL"]) !== null) $fail[] = "broken MSL accepted";
        $caps = vio_backend_info($ctx)['caps'] ?? [];
        if (!empty($caps['tensors'])) {
            $t = run_kernel($ctx, ['source' => $GLSL, 'msl' => $MSL_TENSOR]);
            if ($t === null || $t != $want) $fail[] = "tensor kernel " . json_encode($t === null ? null : array_slice($t, 0, 4));
        }
    }
    try { vio_compute_pipeline($ctx, ['source' => $GLSL, 'msl' => '']); $fail[] = "empty 'msl' accepted"; } catch (ValueError $e) {}
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

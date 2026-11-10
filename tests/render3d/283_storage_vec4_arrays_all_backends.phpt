--TEST--
Storage buffers with a stride > 4: a compute kernel reading and writing vec4 arrays sees every element on every backend, hardware D3D11 / D3D12 included
--DESCRIPTION--
SPIRV-Cross turns every GLSL std430 buffer into an HLSL (RW)ByteAddressBuffer. D3D11
and D3D12 built a structured view for a buffer with a stride > 4, so a raw Load4 at
byte 16 addressed element 16 of a 16-byte view: out of range, read as 0 on hardware
(WARP does not check) - a vec4 array came back as its first element only. Every GLSL
storage view is raw now.
--EXTENSIONS--
vio
--SKIPIF--
<?php
if (!extension_loaded('vio')) die('skip vio not loaded');
?>
--FILE--
<?php
$cs = <<<'GLSL'
#version 450
layout(local_size_x = 1) in;
layout(std430, binding = 0) readonly buffer In { vec4 a[]; };
layout(std430, binding = 1) writeonly buffer Out { vec4 v[]; };
void main() { uint i = gl_GlobalInvocationID.x; v[i] = a[i] * 2.0 + vec4(0.0, 1.0, 2.0, 3.0); }
GLSL;
const N = 6;
$vals = [];
for ($i = 0; $i < N * 4; $i++) $vals[] = (float) ($i + 1);
$want = [];
for ($i = 0; $i < N * 4; $i++) $want[] = $vals[$i] * 2.0 + ($i % 4);

$runs = [];
$seen = [];
foreach (['auto', 'metal', 'opengl', 'd3d11', 'd3d12', 'vulkan'] as $candidate) {
    $probe = @vio_create($candidate, ['width' => 4, 'height' => 4, 'headless' => true]);
    if (!$probe) continue;
    $resolved = vio_backend_name($probe);
    vio_destroy($probe);
    if (in_array($resolved, $seen, true)) continue;
    $seen[] = $resolved;
    $runs[] = [$candidate, $resolved, []];
}
foreach (['d3d11', 'd3d12'] as $be) {
    $hw = @vio_create($be, ['width' => 4, 'height' => 4, 'headless' => true, 'headless_hardware' => true]);
    if (!$hw) continue;
    $info = vio_gpu_info();
    vio_destroy($hw);
    if (!str_contains((string) ($info['name'] ?? ''), 'Basic Render')) $runs[] = [$be, $be . '_hw', ['headless_hardware' => true]];
}
if (!$runs) echo "none: skipped\n";
foreach ($runs as [$be, $label, $extra]) {
    $ctx = @vio_create($be, ['width' => 8, 'height' => 8, 'headless' => true, 'vsync' => false] + $extra);
    if (!$ctx || !vio_supports_feature($ctx, VIO_FEATURE_COMPUTE)) { echo "$label: skipped\n"; if ($ctx) vio_destroy($ctx); continue; }
    $cp = vio_compute_pipeline($ctx, ['source' => $cs]);
    $in = vio_storage_buffer($ctx, ['data' => pack('f*', ...$vals), 'stride' => 16]);
    $out = vio_storage_buffer($ctx, ['size' => N * 16, 'stride' => 16]);
    if (!$cp || !$in || !$out) { echo "$label: setup failed\n"; vio_destroy($ctx); continue; }
    vio_compute_bind_buffer($ctx, $cp, $in, 0, VIO_COMPUTE_READ);
    vio_compute_bind_buffer($ctx, $cp, $out, 1, VIO_COMPUTE_WRITE);
    vio_compute_dispatch($ctx, $cp, N, 1, 1);
    $bytes = vio_storage_buffer_read($ctx, $out);
    $got = is_string($bytes) && strlen($bytes) >= N * 16 ? array_values(unpack('f' . (N * 4), $bytes)) : null;
    echo $label, ': ', $got === $want ? 'OK' : 'FAIL ' . json_encode($got), "\n";
    vio_destroy($ctx);
}
?>
--EXPECTREGEX--
(\w+: (OK|skipped))(\n(\w+: (OK|skipped)))*

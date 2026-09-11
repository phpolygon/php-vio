--TEST--
Async compute: every dispatch keeps the params and buffers it was recorded with, also when the pipeline is reused
--DESCRIPTION--
One kernel writes its Params value into the buffer bound at slot 0. Inside one
frame it is dispatched twice with ['async' => true]: buffer A with value 1, then,
after a rebind to buffer B, value 2; a third vio_compute_set_uniforms() without a
dispatch follows. A must read 1 and B 2. D3D12 and Metal used to point every
recorded dispatch at the pipeline's single params buffer, which the CPU rewrote
before the GPU ran the frame, so A read 2 (or 3). The second part reuses the
pipeline across frames without waiting, which the same single buffer raced
against the other in-flight frame.
--EXTENSIONS--
vio
--SKIPIF--
<?php
if (!extension_loaded('vio')) die('skip vio not loaded');
$__ok = false;
foreach (['auto', 'metal', 'opengl'] as $__b) {
    $__c = @vio_create($__b, ['width' => 8, 'height' => 8, 'headless' => true]);
    if ($__c) { vio_destroy($__c); $__ok = true; break; }
}
if (!$__ok) die('skip no headless GPU context available');
?>
--FILE--
<?php
$backends = [];
$seen = [];
foreach (['auto', 'metal', 'opengl', 'd3d11', 'd3d12', 'vulkan'] as $candidate) {
    $probe = @vio_create($candidate, ['width' => 4, 'height' => 4, 'headless' => true]);
    if (!$probe) continue;
    $resolved = vio_backend_name($probe);
    vio_destroy($probe);
    if (in_array($resolved, $seen, true)) continue;
    $seen[] = $resolved;
    $backends[] = $candidate;
}
if (!$backends) { echo "none: skipped\n"; }

$cs = <<<'GLSL'
#version 450
layout(local_size_x = 1) in;
layout(std430, binding = 0) writeonly buffer Out { float v[]; };
layout(std140, binding = 2) uniform Params { float value; float pad0; float pad1; float pad2; };
void main() { v[gl_GlobalInvocationID.x] = value; }
GLSL;

const N = 4;

/** @return list<float>|null */
function floats($ctx, $buf): ?array
{
    $bytes = vio_storage_buffer_read($ctx, $buf);
    if (!is_string($bytes) || strlen($bytes) < N * 4) return null;
    return array_values(unpack('f' . N, $bytes));
}

foreach ($backends as $be) {
    $ctx = @vio_create($be, ['width' => 16, 'height' => 16, 'headless' => true, 'vsync' => false]);
    if (!$ctx) { echo "$be: skipped\n"; continue; }
    $name = vio_backend_name($ctx);
    if (!vio_supports_feature($ctx, VIO_FEATURE_COMPUTE)) {
        echo "$name: skipped\n";
        vio_destroy($ctx);
        continue;
    }
    $cp = vio_compute_pipeline($ctx, ['source' => $cs]);
    $bufs = [];
    for ($i = 0; $i < 8; $i++) {
        $bufs[] = vio_storage_buffer($ctx, ['size' => N * 4, 'stride' => 4]);
    }
    if (!$cp || in_array(false, $bufs, true)) { echo "$name: setup failed\n"; vio_destroy($ctx); continue; }
    $ok = true;

    // --- One frame, one pipeline, two recorded dispatches with different params.
    vio_begin($ctx);
    vio_clear($ctx, 0, 0, 0, 1);
    vio_compute_bind_buffer($ctx, $cp, $bufs[0], 0, VIO_COMPUTE_WRITE);
    vio_compute_set_uniforms($ctx, $cp, pack('f4', 1.0, 0, 0, 0));
    vio_compute_dispatch($ctx, $cp, N, 1, 1, ['async' => true]);
    vio_compute_bind_buffer($ctx, $cp, $bufs[1], 0, VIO_COMPUTE_WRITE);
    vio_compute_set_uniforms($ctx, $cp, pack('f4', 2.0, 0, 0, 0));
    vio_compute_dispatch($ctx, $cp, N, 1, 1, ['async' => true]);
    vio_compute_set_uniforms($ctx, $cp, pack('f4', 3.0, 0, 0, 0));   // staged, never dispatched
    vio_end($ctx);

    foreach ([0 => 1.0, 1 => 2.0] as $i => $want) {
        $got = floats($ctx, $bufs[$i]);
        if ($got !== array_fill(0, N, $want)) {
            $ok = false;
            echo "$name: same-frame buffer $i = ", json_encode($got), " (want $want)\n";
        }
    }

    // --- Consecutive frames reuse the pipeline without waiting in between.
    for ($f = 0; $f < 6; $f++) {
        vio_begin($ctx);
        vio_clear($ctx, 0, 0, 0, 1);
        vio_compute_bind_buffer($ctx, $cp, $bufs[2 + $f], 0, VIO_COMPUTE_WRITE);
        vio_compute_set_uniforms($ctx, $cp, pack('f4', 10.0 + $f, 0, 0, 0));
        vio_compute_dispatch($ctx, $cp, N, 1, 1, ['async' => true]);
        vio_end($ctx);
    }
    for ($f = 0; $f < 6; $f++) {
        $got = floats($ctx, $bufs[2 + $f]);
        if ($got !== array_fill(0, N, 10.0 + $f)) {
            $ok = false;
            echo "$name: frame $f buffer = ", json_encode($got), "\n";
        }
    }

    echo $name, ': ', $ok ? 'OK' : 'FAIL', "\n";
    vio_destroy($ctx);
}
?>
--EXPECTREGEX--
(\w+: (OK|skipped))(\n(\w+: (OK|skipped)))*

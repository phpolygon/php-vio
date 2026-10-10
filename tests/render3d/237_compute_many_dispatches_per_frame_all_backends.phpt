--TEST--
Compute: 64+ dispatches per frame (async, sync, mixed, across frames) each read and write their own buffers
--DESCRIPTION--
One kernel copies its input buffer (binding 0, read) plus its Params value into
its output buffer (binding 1, write). Every dispatch binds its own pair and its
own value, so a readback shows exactly which buffers each dispatch really used.
Scenarios: 64 async dispatches in one frame; 64 dispatches alternating async and
sync in one frame; 4 frames x 24 async dispatches back to back without a wait;
64 sync dispatches outside a frame. D3D12 wrote every dispatch's descriptor
table into the next of only 16 blocks of a ring that ran without a fence: from
the 17th dispatch per frame (or once the other in-flight frame still held its
blocks) a recorded dispatch read and wrote another dispatch's buffers. With a
hardware D3D12 adapter the same runs headless on the GPU too ('d3d12_hw').
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
// Hardware D3D12 (headless WARP is the default): only where a GPU adapter exists.
$hw = @vio_create('d3d12', ['width' => 4, 'height' => 4, 'headless' => true, 'headless_hardware' => true]);
if ($hw) {
    $info = vio_gpu_info();
    vio_destroy($hw);
    if (!str_contains((string)($info['name'] ?? ''), 'Basic Render')) {
        $runs[] = ['d3d12', 'd3d12_hw', ['headless_hardware' => true]];
    }
}
if (!$runs) { echo "none: skipped\n"; }

$cs = <<<'GLSL'
#version 450
layout(local_size_x = 1) in;
layout(std430, binding = 0) readonly buffer In { float a[]; };
layout(std430, binding = 1) writeonly buffer Out { float v[]; };
layout(std140, binding = 2) uniform Params { float value; float pad0; float pad1; float pad2; };
void main() { uint i = gl_GlobalInvocationID.x; v[i] = a[i] + value; }
GLSL;

const N = 4;
const COUNT = 96;

function expected(int $k): array
{
    $out = [];
    for ($x = 0; $x < N; $x++) $out[] = (float)($k * 100 + $x + $k * 10000);
    return $out;
}

foreach ($runs as [$be, $label, $extra]) {
    $ctx = @vio_create($be, ['width' => 16, 'height' => 16, 'headless' => true, 'vsync' => false] + $extra);
    if (!$ctx) { echo "$label: skipped\n"; continue; }
    if (!vio_supports_feature($ctx, VIO_FEATURE_COMPUTE)) {
        echo "$label: skipped\n";
        vio_destroy($ctx);
        continue;
    }
    $cp = vio_compute_pipeline($ctx, ['source' => $cs]);
    $in = [];
    $out = [];
    for ($k = 0; $k < COUNT; $k++) {
        $vals = [];
        for ($x = 0; $x < N; $x++) $vals[] = (float)($k * 100 + $x);
        $in[] = vio_storage_buffer($ctx, ['data' => pack('f*', ...$vals), 'stride' => 4]);
        $out[] = vio_storage_buffer($ctx, ['size' => N * 4, 'stride' => 4]);
    }
    if (!$cp || in_array(false, $in, true) || in_array(false, $out, true)) {
        echo "$label: setup failed\n";
        vio_destroy($ctx);
        continue;
    }
    $ok = true;

    $dispatch = function (int $k, bool $async) use ($ctx, $cp, $in, &$out): void {
        vio_compute_bind_buffer($ctx, $cp, $in[$k], 0, VIO_COMPUTE_READ);
        vio_compute_bind_buffer($ctx, $cp, $out[$k], 1, VIO_COMPUTE_WRITE);
        vio_compute_set_uniforms($ctx, $cp, pack('f4', (float)($k * 10000), 0, 0, 0));
        vio_compute_dispatch($ctx, $cp, N, 1, 1, $async ? ['async' => true] : []);
    };
    $check = function (string $scenario, array $ks) use ($ctx, &$out, $label, &$ok): void {
        $bad = 0;
        foreach ($ks as $k) {
            $bytes = vio_storage_buffer_read($ctx, $out[$k]);
            $got = is_string($bytes) && strlen($bytes) >= N * 4 ? array_values(unpack('f' . N, $bytes)) : null;
            if ($got !== expected($k)) {
                if ($bad++ < 3) echo "$label: $scenario dispatch $k = ", json_encode($got), "\n";
            }
        }
        if ($bad) { $ok = false; echo "$label: $scenario $bad of ", count($ks), " dispatches wrong\n"; }
    };
    // Fresh (zeroed) outputs per scenario: a dispatch that writes nothing fails too.
    $fresh = function () use ($ctx, &$out): void {
        for ($k = 0; $k < COUNT; $k++) $out[$k] = vio_storage_buffer($ctx, ['size' => N * 4, 'stride' => 4]);
    };

    // 1) 64 async dispatches in one frame.
    vio_begin($ctx);
    vio_clear($ctx, 0, 0, 0, 1);
    for ($k = 0; $k < 64; $k++) $dispatch($k, true);
    vio_end($ctx);
    $check('async-frame', range(0, 63));

    $fresh();
    // 2) 64 dispatches alternating async / sync in one frame.
    vio_begin($ctx);
    vio_clear($ctx, 0, 0, 0, 1);
    for ($k = 32; $k < 96; $k++) $dispatch($k, ($k & 1) === 0);
    vio_end($ctx);
    $check('mixed-frame', range(32, 95));

    $fresh();
    // 3) 4 frames x 24 async dispatches back to back, no wait in between.
    for ($f = 0; $f < 4; $f++) {
        vio_begin($ctx);
        vio_clear($ctx, 0, 0, 0, 1);
        for ($j = 0; $j < 24; $j++) $dispatch($f * 24 + $j, true);
        vio_end($ctx);
    }
    $check('frames', range(0, 95));

    $fresh();
    // 4) 64 sync dispatches outside a frame.
    for ($k = 0; $k < 64; $k++) $dispatch($k, false);
    $check('outside-frame', range(0, 63));

    echo $label, ': ', $ok ? 'OK' : 'FAIL', "\n";
    vio_destroy($ctx);
}
?>
--EXPECTREGEX--
(\w+: (OK|skipped))(\n(\w+: (OK|skipped)))*

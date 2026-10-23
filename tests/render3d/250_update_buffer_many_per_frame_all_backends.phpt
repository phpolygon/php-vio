--TEST--
vio_update_buffer: many updates per frame (overlapping, partial, across frames, ring growth) land in call order
--DESCRIPTION--
Storage buffers are written with vio_update_buffer and copied by a compute kernel
(binding 0 -> binding 1); the readback of the copy must equal a CPU model that
applies the same writes in call order. Scenarios: four overlapping / partial
updates per buffer for 24 buffers in one frame, then an async dispatch each;
4 frames back to back with a full update + dispatch per buffer and frame (no
wait in between); update + sync dispatch inside a frame; updates outside a
frame; 8 x 4 updates of 256 KiB in one frame (more than the initial D3D12
upload ring); 300 one-float updates plus 50 rewrites of the same float in one
frame. D3D12 used to create a committed UPLOAD buffer and submit a list per
call (~0.5 ms each on an RTX 2080); the writes are now staged in a persistently
mapped ring and copied by one batch list per frame, so this pins that the
batch keeps the order and the visibility of the old per-call submission. With a
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
layout(local_size_x = 64) in;
layout(std430, binding = 0) readonly buffer In { float a[]; };
layout(std430, binding = 1) writeonly buffer Out { float v[]; };
void main() { uint i = gl_GlobalInvocationID.x; v[i] = a[i]; }
GLSL;

const N = 64;          // floats per small buffer
const BIG = 65536;     // floats per big buffer (256 KiB)

/** Write $vals at float offset $off into the model and the buffer. */
function put($buf, array &$model, int $off, array $vals): void
{
    vio_update_buffer($buf, pack('f*', ...$vals), $off * 4);
    foreach ($vals as $i => $v) $model[$off + $i] = (float)$v;
}
function seq(float $base, int $n): array
{
    $o = [];
    for ($i = 0; $i < $n; $i++) $o[] = $base + $i;
    return $o;
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
    if (!$cp) { echo "$label: setup failed\n"; vio_destroy($ctx); continue; }
    $ok = true;

    $copy = function ($src, $dst, int $floats, bool $async) use ($ctx, $cp): void {
        vio_compute_bind_buffer($ctx, $cp, $src, 0, VIO_COMPUTE_READ);
        vio_compute_bind_buffer($ctx, $cp, $dst, 1, VIO_COMPUTE_WRITE);
        vio_compute_dispatch($ctx, $cp, intdiv($floats, 64), 1, 1, $async ? ['async' => true] : []);
    };
    $check = function (string $what, $dst, array $model) use ($ctx, $label, &$ok): void {
        $n = count($model);
        $bytes = vio_storage_buffer_read($ctx, $dst);
        $got = is_string($bytes) && strlen($bytes) >= $n * 4 ? array_values(unpack('f' . $n, $bytes)) : null;
        if ($got === $model) return;
        $ok = false;
        if ($got === null) { echo "$label: $what: no readback\n"; return; }
        foreach ($model as $i => $v) {
            if ($got[$i] !== $v) { echo "$label: $what: [$i] = {$got[$i]}, expected $v\n"; return; }
        }
    };
    $newbuf = fn (int $floats) => vio_storage_buffer($ctx, ['size' => $floats * 4, 'stride' => 4]);

    // 1) 24 buffers, four overlapping / partial updates each, then a dispatch - one frame.
    $src = $dst = $model = [];
    for ($k = 0; $k < 24; $k++) { $src[$k] = $newbuf(N); $dst[$k] = $newbuf(N); $model[$k] = array_fill(0, N, 0.0); }
    vio_begin($ctx);
    vio_clear($ctx, 0, 0, 0, 1);
    for ($k = 0; $k < 24; $k++) {
        put($src[$k], $model[$k], 0, seq($k * 1000, N));          // whole buffer
        put($src[$k], $model[$k], 16, seq($k * 1000 + 500, 16));  // inside the first
        put($src[$k], $model[$k], 8, seq($k * 1000 + 700, 16));   // overlaps both
        put($src[$k], $model[$k], N - 4, [-1, -2, -3, -$k]);      // tail, disjoint
        $copy($src[$k], $dst[$k], N, true);
    }
    vio_end($ctx);
    for ($k = 0; $k < 24; $k++) $check("frame buffer $k", $dst[$k], $model[$k]);

    // 2) 4 frames back to back, one full update + dispatch per buffer and frame.
    // Own source buffers per frame: Vulkan / Metal write host-visible memory
    // directly, so rewriting a buffer an in-flight frame still reads would race.
    $outs = [];
    for ($f = 0; $f < 4; $f++) {
        vio_begin($ctx);
        vio_clear($ctx, 0, 0, 0, 1);
        for ($k = 0; $k < 8; $k++) {
            $m = array_fill(0, N, 0.0);
            $fs = $newbuf(N);
            put($fs, $m, 0, seq($f * 100000 + $k * 1000, N));
            $outs[$f][$k] = [$newbuf(N), $m, $fs];
            $copy($fs, $outs[$f][$k][0], N, true);
        }
        vio_end($ctx);
    }
    for ($f = 0; $f < 4; $f++) for ($k = 0; $k < 8; $k++) $check("frames $f/$k", $outs[$f][$k][0], $outs[$f][$k][1]);

    // 3) Update + sync dispatch inside a frame (read right away).
    vio_begin($ctx);
    vio_clear($ctx, 0, 0, 0, 1);
    $m = array_fill(0, N, 0.0);
    $s = $newbuf(N); $d = $newbuf(N);
    put($s, $m, 0, seq(4242, N));
    put($s, $m, 32, seq(-77, 8));
    $copy($s, $d, N, false);
    $check('sync in frame', $d, $m);
    vio_end($ctx);

    // 4) Updates outside a frame, sync dispatch.
    $m = array_fill(0, N, 0.0);
    $s = $newbuf(N); $d = $newbuf(N);
    put($s, $m, 0, seq(31337, N));
    put($s, $m, 1, [0.5, 0.25]);
    $copy($s, $d, N, false);
    $check('outside frame', $d, $m);

    // 5) 8 buffers x 4 updates of 256 KiB in one frame (8 MiB staged).
    $bs = $bd = $bm = [];
    for ($k = 0; $k < 8; $k++) { $bs[$k] = $newbuf(BIG); $bd[$k] = $newbuf(BIG); $bm[$k] = array_fill(0, BIG, 0.0); }
    vio_begin($ctx);
    vio_clear($ctx, 0, 0, 0, 1);
    for ($r = 0; $r < 4; $r++) {
        for ($k = 0; $k < 8; $k++) put($bs[$k], $bm[$k], 0, seq($r * 1000000 + $k * 100000, BIG));
    }
    for ($k = 0; $k < 8; $k++) $copy($bs[$k], $bd[$k], BIG, true);
    vio_end($ctx);
    for ($k = 0; $k < 8; $k++) $check("big $k", $bd[$k], $bm[$k]);

    // 6) 300 one-float updates + 50 rewrites of float 7 in one frame.
    $m = array_fill(0, 512, 0.0);
    $s = $newbuf(512); $d = $newbuf(512);
    vio_begin($ctx);
    vio_clear($ctx, 0, 0, 0, 1);
    for ($i = 0; $i < 300; $i++) put($s, $m, $i, [$i * 3 + 1]);
    for ($i = 0; $i < 50; $i++) put($s, $m, 7, [1000 + $i]);
    $copy($s, $d, 512, true);
    vio_end($ctx);
    $check('one-float updates', $d, $m);

    echo $label, ': ', $ok ? 'OK' : 'FAIL', "\n";
    vio_destroy($ctx);
}
?>
--EXPECTREGEX--
(\w+: (OK|skipped))(\n(\w+: (OK|skipped)))*

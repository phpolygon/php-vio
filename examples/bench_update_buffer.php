<?php
/**
 * Cost of vio_update_buffer on GPU-local (storage) buffers, the way GPU
 * particle emitters use it: every frame each emitter writes its spawn block
 * (vio_update_buffer) and runs one async compute dispatch that reads it.
 *
 *   php -d extension=vio examples/bench_update_buffer.php [backends] [emitters] [bytes] [frames] [updates]
 *
 *   backends  comma- or '+'-separated, default d3d12,vulkan ('d3d12_hw' = D3D12 on the GPU
 *             adapter instead of headless WARP, same as VIO_D3D_HEADLESS_HARDWARE=1)
 *   emitters  storage buffers updated per frame (default 8)
 *   bytes     size of each update (default 4096)
 *   frames    measured frames (default 300, plus 20 warm-up)
 *   updates   vio_update_buffer calls per emitter and frame (default 1)
 *
 * Reported per frame (median over the measured frames): "update" = wall time of
 * all vio_update_buffer calls of the frame, "frame" = vio_begin .. vio_end.
 * Every backend runs in its own process.
 */

$backends = preg_split('/[,+]/', $argv[1] ?? 'd3d12,vulkan');
$E        = (int)($argv[2] ?? 8);
$BYTES    = (int)($argv[3] ?? 4096);
$FRAMES   = (int)($argv[4] ?? 300);
$UPD      = (int)($argv[5] ?? 1);
$WARMUP   = 20;

if (count($backends) > 1) {
    foreach ($backends as $b) {
        $cmd = escapeshellarg(PHP_BINARY)
             . ' -n -d extension_dir=' . escapeshellarg(ini_get('extension_dir'))
             . ' -d extension=' . (PHP_OS_FAMILY === 'Windows' ? 'php_vio.dll' : 'vio')
             . ' ' . escapeshellarg(__FILE__) . ' ' . escapeshellarg($b) . " $E $BYTES $FRAMES $UPD";
        passthru($cmd);
    }
    exit(0);
}

$label = $backends[0];
$be = $label === 'd3d12_hw' ? 'd3d12' : $label;
$opts = ['width' => 64, 'height' => 64, 'headless' => true, 'vsync' => false];
if ($label === 'd3d12_hw') $opts['headless_hardware'] = true;
$ctx = @vio_create($be, $opts);
if (!$ctx) { printf("%-9s skipped (no context)\n", $label); exit(0); }
$gpu = vio_gpu_info()['name'] ?? '?';

$cs = <<<'GLSL'
#version 450
layout(local_size_x = 64) in;
layout(std430, binding = 0) readonly buffer In { float a[]; };
layout(std430, binding = 1) writeonly buffer Out { float v[]; };
void main() { uint i = gl_GlobalInvocationID.x; v[i] = a[i] * 2.0; }
GLSL;
$cp = vio_compute_pipeline($ctx, ['source' => $cs]);
$floats = intdiv($BYTES, 4);
$in = $out = [];
for ($e = 0; $e < $E; $e++) {
    $in[]  = vio_storage_buffer($ctx, ['size' => $BYTES, 'stride' => 4]);
    $out[] = vio_storage_buffer($ctx, ['size' => $BYTES, 'stride' => 4]);
}
$payload = [];
for ($v = 0; $v < 4; $v++) $payload[] = pack('f*', ...array_fill(0, $floats, (float)$v));
$groups = max(1, intdiv($floats + 63, 64));

$upd = $frame = [];
for ($f = 0; $f < $WARMUP + $FRAMES; $f++) {
    $t0 = hrtime(true);
    vio_begin($ctx);
    vio_clear($ctx, 0, 0, 0, 1);
    $u = 0;
    for ($e = 0; $e < $E; $e++) {
        $t = hrtime(true);
        for ($r = 0; $r < $UPD; $r++) vio_update_buffer($in[$e], $payload[($f + $r) & 3]);
        $u += hrtime(true) - $t;
        if ($cp) {
            vio_compute_bind_buffer($ctx, $cp, $in[$e], 0, VIO_COMPUTE_READ);
            vio_compute_bind_buffer($ctx, $cp, $out[$e], 1, VIO_COMPUTE_WRITE);
            vio_compute_dispatch($ctx, $cp, $groups, 1, 1, ['async' => true]);
        }
    }
    vio_end($ctx);
    if ($f >= $WARMUP) { $upd[] = $u / 1e6; $frame[] = (hrtime(true) - $t0) / 1e6; }
}
vio_destroy($ctx);

function median(array $a): float { sort($a); $n = count($a); return $n ? ($n & 1 ? $a[$n >> 1] : ($a[$n / 2 - 1] + $a[$n / 2]) / 2) : 0.0; }
printf("%-9s %2d emitters x %d x %6d B: update %.3f ms/frame (%.1f us/call), frame %.3f ms  [%s]\n",
    $label, $E, $UPD, $BYTES, median($upd), median($upd) * 1000 / max(1, $E * $UPD), median($frame), $gpu);

--TEST--
vio_gpu_frame_time(): GPU timestamps of the last completed frame on every backend that reports VIO_FEATURE_GPU_TIMESTAMP
--EXTENSIONS--
vio
--SKIPIF--
<?php
$any = false;
foreach (vio_backends() as $b) {
    if ($b === 'null') continue;
    $c = @vio_create($b, ["width" => 8, "height" => 8, "headless" => true, "vsync" => false]);
    if (!$c) continue;
    if (vio_supports_feature($c, VIO_FEATURE_GPU_TIMESTAMP)) $any = true;
    vio_destroy($c);
}
if (!$any) die("skip no backend with GPU timestamps");
?>
--FILE--
<?php
/* GAP-PHASE5-PLAN Block 3. Each backend renders a handful of frames (clear +
 * a 2D rectangle, so no 3D pipeline is required — Vulkan is 2D-only), waits
 * for the GPU, and must then report a plausible GPU time for a finished frame:
 * 0 <= ms < 5000. Before the first frame the value is -1. Backends without the
 * feature return -1 always. */
function run_backend(string $name): string {
    $ctx = @vio_create($name, ["width" => 32, "height" => 32, "headless" => true, "vsync" => false]);
    if (!$ctx) return "skip (unavailable)";
    if (!vio_supports_feature($ctx, VIO_FEATURE_GPU_TIMESTAMP)) {
        $v = vio_gpu_frame_time($ctx);
        vio_destroy($ctx);
        return $v < 0.0 ? "OK (unsupported, -1)" : "FAIL\n  unsupported backend returned $v";
    }
    $fail = [];
    $before = vio_gpu_frame_time($ctx);
    if ($before >= 0.0) $fail[] = "value before any frame should be negative, got $before";
    for ($i = 0; $i < 6; $i++) {
        vio_clear($ctx, 0.1, 0.2, 0.3, 1.0);
        vio_begin($ctx);
        vio_rect($ctx, 4, 4, 20, 20, ['fill' => 0xFF00FF00]);
        vio_draw_2d($ctx);
        vio_end($ctx);
        vio_read_pixels($ctx);   /* readback waits for the GPU */
    }
    /* One more frame so the ring harvests a finished slot. */
    vio_begin($ctx);
    vio_end($ctx);
    vio_gpu_flush($ctx);
    $ms = vio_gpu_frame_time($ctx);
    if (!is_float($ms)) $fail[] = "not a float";
    if ($ms < 0.0) $fail[] = "no GPU time after 7 frames ($ms)";
    if ($ms >= 5000.0) $fail[] = "implausible GPU time $ms ms";
    vio_destroy($ctx);
    return $fail ? "FAIL\n  " . implode("\n  ", $fail) : "OK";
}

foreach (['opengl', 'd3d11', 'd3d12', 'vulkan', 'metal'] as $b) {
    echo "$b: ", run_backend($b), "\n";
}
echo "DONE\n";
?>
--EXPECTF--
opengl: %s
d3d11: %s
d3d12: %s
vulkan: %s
metal: %s
DONE

--TEST--
Waitable swapchain: vio_create(['frame_latency' => 1]) caps the CPU run-ahead on D3D11 / D3D12 (VIO_FEATURE_FRAME_LATENCY)
--EXTENSIONS--
vio
--SKIPIF--
<?php
$any = false;
foreach (vio_backends() as $b) {
    if ($b === 'null') continue;
    $c = @vio_create($b, ["width" => 8, "height" => 8, "headless" => true, "vsync" => false]);
    if ($c) { $any = true; vio_destroy($c); }
}
if (!$any) die("skip no GPU backend");
?>
--FILE--
<?php
/* GAP-PHASE5-PLAN Block 5. With frame_latency => 1 a backend that reports
 * VIO_FEATURE_FRAME_LATENCY creates its swapchain with the frame-latency
 * waitable object and vio_swapchain_info() says so; frames keep rendering
 * (vio_begin waits on the object). Backends without the feature accept the
 * option, report latency 0 and are otherwise unaffected. */
function run_backend(string $name): string {
    $ctx = @vio_create($name, ["width" => 32, "height" => 32, "headless" => true, "vsync" => false, "frame_latency" => 1]);
    if (!$ctx) return "skip (unavailable)";
    $fail = [];
    $has = vio_supports_feature($ctx, VIO_FEATURE_FRAME_LATENCY);
    $info = vio_swapchain_info($ctx);
    foreach (['buffer_count', 'frame_latency', 'waitable', 'hdr_output', 'format'] as $k) {
        if (!array_key_exists($k, $info)) $fail[] = "info lacks $k";
    }
    if ($has) {
        if (($info['frame_latency'] ?? 0) !== 1) $fail[] = "frame_latency should be 1, got " . json_encode($info['frame_latency'] ?? null);
        if (($info['waitable'] ?? false) !== true) $fail[] = "waitable object not in use";
        if (($info['buffer_count'] ?? 0) < 2) $fail[] = "buffer_count " . json_encode($info['buffer_count'] ?? null);
    } else {
        if (($info['frame_latency'] ?? 0) !== 0) $fail[] = "unsupported backend must report latency 0";
        if (($info['waitable'] ?? false) !== false) $fail[] = "unsupported backend must not be waitable";
    }
    /* A few frames through the waitable path. */
    for ($i = 0; $i < 5; $i++) {
        vio_clear($ctx, 0.2, 0.3, 0.4, 1.0);
        vio_begin($ctx);
        vio_rect($ctx, 2, 2, 10, 10, ['fill' => 0xFF00FF00]);
        vio_draw_2d($ctx);
        vio_end($ctx);
    }
    $p = vio_read_pixels($ctx);
    if (!$p || strlen($p) < 32 * 32 * 4) $fail[] = "readback after waitable frames";
    vio_destroy($ctx);
    return $fail ? "FAIL\n  " . implode("\n  ", $fail) : ($has ? "OK" : "OK (no frame-latency control)");
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

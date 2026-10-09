--TEST--
Native upscaler DLSS (VIO_UPSCALER_DLSS, TEMPORAL-S4) without its runtime: every backend reports supported = false with a reason (D3D12 / Vulkan name nvngx_dlss.dll or "built without DLSS") and no warning, vio_upscaler_render_size() is false without a warning, info() has its keys, create() refuses with a warning
--EXTENSIONS--
vio
--INI--
vio.dlss_path={PWD}
vio.ffx_path={PWD}
--FILE--
<?php
/* vio.dlss_path points at the test directory, which holds no nvngx_dlss.dll:
 * an explicit path is the only place searched, so DLSS is missing on D3D12 /
 * Vulkan as well - also in a build with --with-dlss on an RTX GPU. Other
 * backends never have a native upscaler. Nothing may warn except create(). */
echo function_exists('vio_upscaler_render_size') ? "render_size ok\n" : "FAIL no vio_upscaler_render_size\n";

foreach (['null', 'opengl', 'd3d11', 'd3d12', 'vulkan', 'metal'] as $b) {
    $ctx = @vio_create($b, ['width' => 16, 'height' => 16, 'headless' => true, 'headless_hardware' => true, 'vsync' => false]);
    if ($ctx && vio_backend_name($ctx) !== $b) { vio_destroy($ctx); $ctx = null; }
    if (!$ctx) { echo "$b: skip (unavailable)\n"; continue; }
    $fail = [];

    error_clear_last();
    $s = vio_upscaler_supported($ctx, VIO_UPSCALER_DLSS);
    $info = vio_upscaler_info($ctx, VIO_UPSCALER_DLSS);
    $rs = vio_upscaler_render_size($ctx, VIO_UPSCALER_DLSS, VIO_UPSCALE_QUALITY, 1920, 1080);
    $rsAa = vio_upscaler_render_size($ctx, VIO_UPSCALER_DLSS, VIO_UPSCALE_NATIVE_AA, 1920, 1080);
    if (($e = error_get_last()) !== null) $fail[] = "warning: " . $e['message'];

    if ($s !== false) $fail[] = "DLSS supported without nvngx_dlss.dll";
    if ($rs !== false || $rsAa !== false) $fail[] = "render_size " . json_encode([$rs, $rsAa]);
    foreach (['provider', 'backend', 'supported', 'reason', 'version', 'driver', 'library', 'device', 'live', 'host_bytes'] as $k) {
        if (!array_key_exists($k, $info)) $fail[] = "info lacks '$k'";
    }
    if (($info['provider'] ?? null) !== 'dlss') $fail[] = "provider " . json_encode($info['provider'] ?? null);
    if (($info['supported'] ?? null) !== false) $fail[] = "info supported";
    if (!is_string($info['reason'] ?? null) || $info['reason'] === '') $fail[] = "empty reason";
    if (in_array($b, ['d3d12', 'vulkan'], true) && PHP_OS_FAMILY !== 'Darwin'
        && strpos($info['reason'], 'nvngx_dlss') === false && strpos($info['reason'], 'built without DLSS') === false
        && strpos($info['reason'], 'not available') === false) {
        $fail[] = "reason: " . $info['reason'];
    }

    error_clear_last();
    $u = @vio_upscaler_create($ctx, ['provider' => VIO_UPSCALER_DLSS, 'display_width' => 16, 'display_height' => 16]);
    if ($u !== false) $fail[] = "create succeeded";
    $e = error_get_last();
    if (!$e || strpos($e['message'], 'vio_upscaler_create') === false) $fail[] = "create without a warning";

    // Argument contract of vio_upscaler_render_size.
    foreach ([[99, VIO_UPSCALE_QUALITY, 64, 64], [VIO_UPSCALER_DLSS, 9, 64, 64], [VIO_UPSCALER_DLSS, VIO_UPSCALE_QUALITY, 0, 64]] as $a) {
        try { vio_upscaler_render_size($ctx, ...$a); $fail[] = "render_size " . json_encode($a) . " accepted"; }
        catch (ValueError) {}
    }

    echo "$b: ", $fail ? "FAIL " . implode('; ', $fail) : "OK", "\n";
    vio_destroy($ctx);
}
echo "DONE\n";
?>
--EXPECTF--
render_size ok
null: %r(OK|skip \(unavailable\))%r
opengl: %r(OK|skip \(unavailable\))%r
d3d11: %r(OK|skip \(unavailable\))%r
d3d12: %r(OK|skip \(unavailable\))%r
vulkan: %r(OK|skip \(unavailable\))%r
metal: %r(OK|skip \(unavailable\))%r
DONE

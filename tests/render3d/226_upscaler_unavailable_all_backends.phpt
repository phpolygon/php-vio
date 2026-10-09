--TEST--
Native upscalers (vio_upscaler_*, TEMPORAL-S3): without the provider library every backend reports supported = false with a reason and no warning, info() has its keys, create() refuses with a warning, arguments are validated
--EXTENSIONS--
vio
--INI--
vio.ffx_path={PWD}
vio.dlss_path={PWD}
--FILE--
<?php
/* vio.ffx_path points at the test directory, which holds no
 * amd_fidelityfx_*.dll: an explicit path is the only place searched, so the
 * provider is missing on D3D12 / Vulkan as well. Other backends never have a
 * native upscaler. Nothing here may print a warning except create(). */
$consts = [VIO_UPSCALER_FSR3, VIO_UPSCALER_DLSS, VIO_UPSCALER_XESS,
           VIO_UPSCALE_NATIVE_AA, VIO_UPSCALE_QUALITY, VIO_UPSCALE_BALANCED,
           VIO_UPSCALE_PERFORMANCE, VIO_UPSCALE_ULTRA_PERFORMANCE];
echo implode(',', $consts), "\n";
echo is_int(VIO_FEATURE_UPSCALER_NATIVE) ? "feature constant ok\n" : "FAIL feature constant\n";

$expectErr = function (callable $f, string $what) use (&$fail) {
    try { $f(); $fail[] = "$what: no ValueError"; }
    catch (ValueError $e) { /* expected */ }
};

foreach (['null', 'opengl', 'd3d11', 'd3d12', 'vulkan', 'metal'] as $b) {
    $ctx = @vio_create($b, ['width' => 16, 'height' => 16, 'headless' => true, 'vsync' => false]);
    if ($ctx && vio_backend_name($ctx) !== $b) { vio_destroy($ctx); $ctx = null; }
    if (!$ctx) { echo "$b: skip (unavailable)\n"; continue; }
    $fail = [];

    error_clear_last();
    $s = vio_upscaler_supported($ctx);
    $info = vio_upscaler_info($ctx);
    $sd = vio_upscaler_supported($ctx, VIO_UPSCALER_DLSS);
    $sx = vio_upscaler_supported($ctx, VIO_UPSCALER_XESS);
    $flag = vio_supports_feature($ctx, VIO_FEATURE_UPSCALER_NATIVE);
    if (($e = error_get_last()) !== null) $fail[] = "warning: " . $e['message'];

    if ($s !== false) $fail[] = "FSR3 supported without the library";
    if ($sd !== false || $sx !== false) $fail[] = "DLSS/XeSS supported";
    if ($flag !== false) $fail[] = "VIO_FEATURE_UPSCALER_NATIVE set";
    foreach (['provider', 'backend', 'supported', 'reason', 'version', 'library', 'live'] as $k) {
        if (!array_key_exists($k, $info)) $fail[] = "info lacks '$k'";
    }
    if (($info['provider'] ?? null) !== 'fsr3') $fail[] = "provider " . json_encode($info['provider'] ?? null);
    if (($info['backend'] ?? null) !== $b) $fail[] = "backend " . json_encode($info['backend'] ?? null);
    if (($info['supported'] ?? null) !== false) $fail[] = "info supported";
    if (!is_string($info['reason'] ?? null) || $info['reason'] === '') $fail[] = "empty reason";
    if (($info['live'] ?? null) !== 0) $fail[] = "live " . json_encode($info['live'] ?? null);
    $dll = ['d3d12' => 'amd_fidelityfx_dx12.dll', 'vulkan' => 'amd_fidelityfx_vk.dll'][$b] ?? null;
    if ($dll !== null && PHP_OS_FAMILY === 'Windows' && strpos($info['reason'], $dll) === false
        && strpos($info['reason'], 'built without') === false) {
        $fail[] = "reason does not name $dll: " . $info['reason'];
    }
    $id = vio_upscaler_info($ctx, VIO_UPSCALER_DLSS);
    if (($id['provider'] ?? null) !== 'dlss' || ($id['supported'] ?? null) !== false || ($id['reason'] ?? '') === '') $fail[] = "DLSS info " . json_encode($id);

    error_clear_last();
    $u = @vio_upscaler_create($ctx, ['display_width' => 16, 'display_height' => 16, 'quality' => VIO_UPSCALE_PERFORMANCE]);
    if ($u !== false) $fail[] = "create succeeded";
    $e = error_get_last();
    if (!$e || strpos($e['message'], 'vio_upscaler_create') === false) $fail[] = "create without a warning";

    $expectErr(fn() => vio_upscaler_supported($ctx, 99), 'provider 99');
    $expectErr(fn() => vio_upscaler_info($ctx, 0), 'info provider 0');
    $expectErr(fn() => vio_upscaler_create($ctx, ['display_height' => 16]), 'create without display_width');
    $expectErr(fn() => vio_upscaler_create($ctx, ['display_width' => 16, 'display_height' => 16, 'quality' => 7]), 'quality 7');
    $expectErr(fn() => vio_upscaler_create($ctx, ['display_width' => 16, 'display_height' => 16, 'provider' => 42]), 'create provider 42');

    echo "$b: ", $fail ? "FAIL " . implode('; ', $fail) : "OK", "\n";
    vio_destroy($ctx);
}
echo "DONE\n";
?>
--EXPECTF--
1,2,3,0,1,2,3,4
feature constant ok
null: %r(OK|skip \(unavailable\))%r
opengl: %r(OK|skip \(unavailable\))%r
d3d11: %r(OK|skip \(unavailable\))%r
d3d12: %r(OK|skip \(unavailable\))%r
vulkan: %r(OK|skip \(unavailable\))%r
metal: %r(OK|skip \(unavailable\))%r
DONE

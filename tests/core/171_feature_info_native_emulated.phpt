--TEST--
vio_feature_info: every VIO_FEATURE_* reports supported / emulated / method consistently with vio_supports_feature on every backend; Metal's geometry stage and multiview are emulated, the other backends run them natively
--EXTENSIONS--
vio
--FILE--
<?php
/* OPEN-ITEMS-PLAN A9. 'emulated' means vio (or the platform) reaches the feature
 * by other means than the API's own stage or call - e.g. Metal runs a GLSL
 * geometry shader as compute kernels. 'method' names how; null when native. */
$features = array_filter(get_defined_constants(true)['vio'] ?? [], fn($k) => str_starts_with($k, 'VIO_FEATURE_'), ARRAY_FILTER_USE_KEY);
echo count($features) > 40 ? "constants OK\n" : "FAIL only " . count($features) . " VIO_FEATURE_* constants\n";

/* Features whose support is native wherever this backend reports it. */
$native = [
    'opengl' => ['VIO_FEATURE_GEOMETRY', 'VIO_FEATURE_TESSELLATION'],   /* multiview: OVR or instancing */
    'd3d11'  => ['VIO_FEATURE_GEOMETRY', 'VIO_FEATURE_TESSELLATION'],
    'd3d12'  => ['VIO_FEATURE_GEOMETRY', 'VIO_FEATURE_TESSELLATION', 'VIO_FEATURE_MULTIVIEW', 'VIO_FEATURE_RAY_QUERY'],
    'vulkan' => ['VIO_FEATURE_GEOMETRY', 'VIO_FEATURE_TESSELLATION', 'VIO_FEATURE_MULTIVIEW', 'VIO_FEATURE_RAY_QUERY'],
    'metal'  => ['VIO_FEATURE_TESSELLATION', 'VIO_FEATURE_COMPUTE'],
    'null'   => [],
];
/* Features that are emulated wherever this backend reports them. */
$emulated = [
    'metal' => ['VIO_FEATURE_GEOMETRY', 'VIO_FEATURE_GEOMETRY_INSTANCING', 'VIO_FEATURE_MULTIVIEW'],
    'd3d11' => ['VIO_FEATURE_MULTIVIEW'],   /* by instancing (OPEN-ITEMS-PLAN A10) */
];

foreach (['null', 'opengl', 'd3d11', 'd3d12', 'vulkan', 'metal'] as $b) {
    $ctx = @vio_create($b, ['width' => 16, 'height' => 16, 'headless' => true, 'vsync' => false]);
    if ($ctx && vio_backend_name($ctx) !== $b) { vio_destroy($ctx); $ctx = null; }
    if (!$ctx) { echo "$b: skip (unavailable)\n"; continue; }
    $err = [];
    $nEmu = 0;
    foreach ($features as $name => $f) {
        $i = vio_feature_info($ctx, $f);
        if (!is_array($i) || array_keys($i) !== ['supported', 'emulated', 'method']) { $err[] = "$name shape " . json_encode($i); continue; }
        if ($i['supported'] !== vio_supports_feature($ctx, $f)) $err[] = "$name supported differs";
        if ($i['emulated'] && !$i['supported']) $err[] = "$name emulated but unsupported";
        if ($i['emulated'] !== (is_string($i['method']) && $i['method'] !== '')) $err[] = "$name method " . json_encode($i['method']);
        if ($i['supported'] && in_array($name, $native[$b] ?? [], true) && $i['emulated']) $err[] = "$name should be native";
        if ($i['supported'] && in_array($name, $emulated[$b] ?? [], true) && !$i['emulated']) $err[] = "$name should be emulated";
        $nEmu += $i['emulated'] ? 1 : 0;
    }
    $u = vio_feature_info($ctx, 99999);
    if ($u !== ['supported' => false, 'emulated' => false, 'method' => null]) $err[] = "unknown feature " . json_encode($u);
    echo "$b: ", $err ? "FAIL\n  " . implode("\n  ", $err) : "OK", "\n";
    vio_destroy($ctx);
}
echo "DONE\n";
?>
--EXPECTF--
constants OK
null: %r(OK|skip \(.*\))%r
opengl: %r(OK|skip \(.*\))%r
d3d11: %r(OK|skip \(.*\))%r
d3d12: %r(OK|skip \(.*\))%r
vulkan: %r(OK|skip \(.*\))%r
metal: %r(OK|skip \(.*\))%r
DONE

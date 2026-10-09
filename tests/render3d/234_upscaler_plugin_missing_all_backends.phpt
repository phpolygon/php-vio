--TEST--
Native upscaler plugin (TEMPORAL-S4): without vio_dlss.dll / libvio_dlss.so DLSS is unsupported on every backend without a warning - the reason names the plugin file and where it was looked for (VIO_DLSS_PLUGIN, then vio.dlss_plugin_path which wins), info()['plugin'] is empty, render_size() is false, create() refuses with a warning
--EXTENSIONS--
vio
--ENV--
VIO_DLSS_PLUGIN={PWD}/no-such-dir
--FILE--
<?php
/* The DLSS provider is not part of php-vio: it comes from a plugin library
 * (include/vio_upscale_plugin.h). An explicit place - the environment variable,
 * or the ini setting, which wins - is the only one searched. Other backends
 * never have a native upscaler. Nothing may warn except create(). */
$file = PHP_OS_FAMILY === 'Windows' ? 'vio_dlss.dll' : 'libvio_dlss.so';
echo ini_get('vio.dlss_plugin_path') === '' ? "ini ok\n" : "FAIL ini " . var_export(ini_get('vio.dlss_plugin_path'), true) . "\n";

foreach (['env' => null, 'ini' => __DIR__] as $how => $iniPath) {
    if ($iniPath !== null) ini_set('vio.dlss_plugin_path', $iniPath);
    foreach (['null', 'opengl', 'd3d11', 'd3d12', 'vulkan', 'metal'] as $b) {
        $ctx = @vio_create($b, ['width' => 16, 'height' => 16, 'headless' => true, 'vsync' => false]);
        if ($ctx && vio_backend_name($ctx) !== $b) { vio_destroy($ctx); $ctx = null; }
        if (!$ctx) { echo "$how $b: skip (unavailable)\n"; continue; }
        $fail = [];

        error_clear_last();
        $s = vio_upscaler_supported($ctx, VIO_UPSCALER_DLSS);
        $info = vio_upscaler_info($ctx, VIO_UPSCALER_DLSS);
        $rs = vio_upscaler_render_size($ctx, VIO_UPSCALER_DLSS, VIO_UPSCALE_QUALITY, 1920, 1080);
        if (($e = error_get_last()) !== null) $fail[] = "warning: " . $e['message'];

        if ($s !== false) $fail[] = "DLSS supported without its plugin";
        if ($rs !== false) $fail[] = "render_size " . json_encode($rs);
        if (!array_key_exists('plugin', $info)) $fail[] = "info lacks 'plugin'";
        elseif ($info['plugin'] !== '') $fail[] = "plugin " . json_encode($info['plugin']);
        if (($info['supported'] ?? null) !== false || ($info['reason'] ?? '') === '') $fail[] = "info " . json_encode($info);
        if (in_array($b, ['d3d12', 'vulkan'], true) && PHP_OS_FAMILY !== 'Darwin') {
            $where = $how === 'env' ? 'VIO_DLSS_PLUGIN' : 'vio.dlss_plugin_path';
            if (strpos($info['reason'], $file) === false || strpos($info['reason'], $where) === false) $fail[] = "reason: " . $info['reason'];
        }
        // FSR 3.1 is in-tree: never a plugin.
        if (vio_upscaler_info($ctx, VIO_UPSCALER_FSR3)['plugin'] !== '') $fail[] = "fsr3 plugin";

        error_clear_last();
        $u = @vio_upscaler_create($ctx, ['provider' => VIO_UPSCALER_DLSS, 'display_width' => 16, 'display_height' => 16]);
        if ($u !== false) $fail[] = "create succeeded";
        $e = error_get_last();
        if (!$e || strpos($e['message'], 'vio_upscaler_create') === false) $fail[] = "create without a warning";

        echo "$how $b: ", $fail ? "FAIL " . implode('; ', $fail) : "OK", "\n";
        vio_destroy($ctx);
    }
}
echo "DONE\n";
?>
--EXPECTF--
ini ok
env null: %r(OK|skip \(unavailable\))%r
env opengl: %r(OK|skip \(unavailable\))%r
env d3d11: %r(OK|skip \(unavailable\))%r
env d3d12: %r(OK|skip \(unavailable\))%r
env vulkan: %r(OK|skip \(unavailable\))%r
env metal: %r(OK|skip \(unavailable\))%r
ini null: %r(OK|skip \(unavailable\))%r
ini opengl: %r(OK|skip \(unavailable\))%r
ini d3d11: %r(OK|skip \(unavailable\))%r
ini d3d12: %r(OK|skip \(unavailable\))%r
ini vulkan: %r(OK|skip \(unavailable\))%r
ini metal: %r(OK|skip \(unavailable\))%r
DONE

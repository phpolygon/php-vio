--TEST--
Native upscaler plugin ABI (include/vio_upscale_plugin.h, TEMPORAL-S4): a library without vio_upscale_plugin_get, a plugin that refuses php-vio's ABI, one built for another ABI, one with a too small provider table and one for another provider are refused - supported = false with the reason, no warning - and a refused plugin is not kept: the matching one loads afterwards in the same process (D3D12 / Vulkan, needs the test plugin built next to php_vio)
--EXTENSIONS--
vio
--SKIPIF--
<?php
if (!extension_loaded('vio')) die('skip vio not loaded');
$plugin = ini_get('extension_dir') . DIRECTORY_SEPARATOR . (PHP_OS_FAMILY === 'Windows' ? 'vio_test_upscaler.dll' : 'vio_test_upscaler.so');
if (!is_file($plugin)) die("skip test plugin not built ($plugin)");
$any = false;
foreach (['d3d12', 'vulkan'] as $b) {
    $c = @vio_create($b, ['width' => 16, 'height' => 16, 'headless' => true, 'vsync' => false]);
    if ($c && vio_backend_name($c) === $b) $any = true;
    if ($c) vio_destroy($c);
}
if (!$any) die('skip neither D3D12 nor Vulkan');
?>
--FILE--
<?php
$dir = ini_get('extension_dir');
$plugin = $dir . DIRECTORY_SEPARATOR . (PHP_OS_FAMILY === 'Windows' ? 'vio_test_upscaler.dll' : 'vio_test_upscaler.so');
$noExport = $dir . DIRECTORY_SEPARATOR . (PHP_OS_FAMILY === 'Windows' ? 'php_vio.dll' : 'vio.so');   // a library without the entry point

$cases = [
    'no entry point' => [$noExport, [], 'vio_upscale_plugin_get'],
    'refuses ABI'    => [$plugin, ['VIO_TEST_UPSCALER_REFUSE' => '1'], 'plugin ABI 1'],
    'other ABI'      => [$plugin, ['VIO_TEST_UPSCALER_ABI' => '2'], 'plugin ABI 2'],
    'small table'    => [$plugin, ['VIO_TEST_UPSCALER_SIZE' => '16'], 'provider table'],
    'other provider' => [$plugin, ['VIO_TEST_UPSCALER_ID' => '1'], "'fsr3'"],
    'matching'       => [$plugin, [], null],
];
$envKeys = ['VIO_TEST_UPSCALER_REFUSE', 'VIO_TEST_UPSCALER_ABI', 'VIO_TEST_UPSCALER_SIZE', 'VIO_TEST_UPSCALER_ID'];

foreach (['d3d12', 'vulkan'] as $b) {
    $ctx = @vio_create($b, ['width' => 16, 'height' => 16, 'headless' => true, 'vsync' => false]);
    if ($ctx && vio_backend_name($ctx) !== $b) { vio_destroy($ctx); $ctx = null; }
    if (!$ctx) { echo "$b: skip (unavailable)\n"; continue; }
    $fail = [];
    foreach ($cases as $name => [$path, $env, $expect]) {
        // The loader is the backend's neighbour, not part of it: the refusals
        // run on the first backend; an accepted plugin stays for the process.
        if ($expect !== null && !empty($loaded)) continue;
        foreach ($envKeys as $k) putenv($k);
        foreach ($env as $k => $v) putenv("$k=$v");
        ini_set('vio.dlss_plugin_path', $path);
        error_clear_last();
        $s = vio_upscaler_supported($ctx, VIO_UPSCALER_DLSS);
        $info = vio_upscaler_info($ctx, VIO_UPSCALER_DLSS);
        if (($e = error_get_last()) !== null) $fail[] = "$name: warning " . $e['message'];
        if ($expect === null) {
            if ($s !== true) $fail[] = "$name: not supported (" . $info['reason'] . ")";
            if (strcasecmp(str_replace('/', '\\', $info['plugin']), str_replace('/', '\\', $path)) !== 0) $fail[] = "$name: plugin " . $info['plugin'];
            $loaded = true;
        } else {
            if ($s !== false) $fail[] = "$name: supported";
            if ($info['plugin'] !== '') $fail[] = "$name: plugin kept " . $info['plugin'];
            if (strpos($info['reason'], $expect) === false) $fail[] = "$name: reason '" . $info['reason'] . "' lacks $expect";
        }
    }
    foreach ($envKeys as $k) putenv($k);
    echo "$b: ", $fail ? "FAIL " . implode('; ', $fail) : "OK", "\n";
    vio_destroy($ctx);
}
echo "DONE\n";
?>
--EXPECTF--
d3d12: %r(OK|skip \(unavailable\))%r
vulkan: %r(OK|skip \(unavailable\))%r
DONE

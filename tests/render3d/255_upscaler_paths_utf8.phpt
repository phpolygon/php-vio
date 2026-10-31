--TEST--
Upscaler library paths are UTF-8 (Windows): a plugin and a runtime under a folder with an umlaut and characters the ANSI code page cannot spell load through vio.dlss_plugin_path, VIO_DLSS_PLUGIN and next to the PHP executable, vio.dlss_plugin_path / vio.ffx_path take such a path, 'plugin' and 'library' come back as valid UTF-8; 'library' also names the runtime when the provider says no (D3D12 / Vulkan, needs the test plugin built next to php_vio)
--EXTENSIONS--
vio
--SKIPIF--
<?php
if (!extension_loaded('vio')) die('skip vio not loaded');
if (PHP_OS_FAMILY !== 'Windows') die('skip Windows paths');
$plugin = ini_get('extension_dir') . DIRECTORY_SEPARATOR . (PHP_OS_FAMILY === 'Windows' ? 'vio_test_upscaler.dll' : 'vio_test_upscaler.so');
if (!is_file($plugin)) die("skip test plugin not built ($plugin)");
?>
--FILE--
<?php
$dir = ini_get('extension_dir');
$win = PHP_OS_FAMILY === 'Windows';
$plugin = $dir . DIRECTORY_SEPARATOR . ($win ? 'vio_test_upscaler.dll' : 'vio_test_upscaler.so');
$file = $win ? 'vio_dlss.dll' : 'libvio_dlss.so';
$ext = $win ? 'php_vio.dll' : 'vio.so';
$core = dirname(PHP_BINARY) . DIRECTORY_SEPARATOR . (PHP_ZTS ? 'php8ts.dll' : 'php8.dll');

/* The child: one way to the plugin, prints what came back. */
if (($argv[1] ?? '') === 'child') {
    $res = [];
    foreach (['d3d12', 'vulkan'] as $b) {
        $ctx = @vio_create($b, ['width' => 16, 'height' => 16, 'headless' => true, 'vsync' => false]);
        if ($ctx && vio_backend_name($ctx) !== $b) { vio_destroy($ctx); $ctx = null; }
        if (!$ctx) continue;
        $i = vio_upscaler_info($ctx, VIO_UPSCALER_DLSS);
        $res[$b] = [$i['supported'], $i['plugin'], $i['library'], $i['reason'], vio_upscaler_info($ctx, VIO_UPSCALER_FSR3)['library']];
        vio_destroy($ctx);
    }
    echo json_encode($res, JSON_UNESCAPED_UNICODE | JSON_INVALID_UTF8_SUBSTITUTE), "\n";
    exit(0);
}

$root = sys_get_temp_dir() . DIRECTORY_SEPARATOR . 'vio Müller 日本 ' . getmypid();
$side = $root . DIRECTORY_SEPARATOR . 'Spüle';
mkdir($side, 0777, true);
copy($plugin, "$side/$file");
copy($plugin, "$root/$file");
$nextToExe = $win && is_file($core);
if ($nextToExe) {
    copy(PHP_BINARY, "$root/php.exe");
    copy($core, $root . DIRECTORY_SEPARATOR . basename($core));
}

$run = function (array $d, array $env, ?string $exe = null) use ($dir, $ext): array {
    // Through an ini file: a command line goes through the ANSI code page on Windows.
    $ini = tempnam(sys_get_temp_dir(), 'vio');
    $lines = ['extension_dir="' . $dir . '"', "extension=$ext"];
    foreach ($d as $k => $v) $lines[] = "$k=\"$v\"";
    file_put_contents($ini, implode("\n", $lines) . "\n");
    // An array: no shell in between, so the executable's own path survives as well.
    $argv = [$exe ?? PHP_BINARY, '-c', $ini, __FILE__, 'child'];
    $p = proc_open($argv, [1 => ['pipe', 'w'], 2 => ['pipe', 'w']], $pipes, null, $env + getenv());
    $out = stream_get_contents($pipes[1]);
    $err = stream_get_contents($pipes[2]);
    proc_close($p);
    unlink($ini);
    $r = json_decode(trim($out), true);
    return is_array($r) ? $r : ['error' => [false, '', '', "no output: $out $err"]];
};

$fail = [];
$check = function (string $case, array $res, string $want) use (&$fail) {
    if (!$res) { $fail[] = "$case: no backend"; return; }
    foreach ($res as $b => [$ok, $pl, $lib, $why]) {
        if ($ok !== true) { $fail[] = "$case/$b: unsupported ($why)"; continue; }
        if (!preg_match('//u', $pl) || !preg_match('//u', $lib)) $fail[] = "$case/$b: not UTF-8";
        if (strcasecmp(str_replace('/', '\\', $pl), str_replace('/', '\\', $want)) !== 0) $fail[] = "$case/$b: plugin '$pl' != '$want'";
    }
};

$file_in_side = $side . DIRECTORY_SEPARATOR . $file;
$check('ini file', $run(['vio.dlss_plugin_path' => $file_in_side], []), $file_in_side);
$check('ini dir', $run(['vio.dlss_plugin_path' => $side], []), $file_in_side);
$check('env', $run([], ['VIO_DLSS_PLUGIN' => $file_in_side]), $file_in_side);
if ($nextToExe) $check('next to php.exe', $run([], [], $root . DIRECTORY_SEPARATOR . 'php.exe'), $root . DIRECTORY_SEPARATOR . $file);

// FSR 3.1 through vio.ffx_path: the host's loader, its library reported also where WARP says no.
$ffx = [];
foreach (['amd_fidelityfx_dx12.dll', 'amd_fidelityfx_vk.dll'] as $f) {
    $src = dirname(PHP_BINARY) . DIRECTORY_SEPARATOR . $f;
    if (is_file($src) && copy($src, "$root/$f")) $ffx[] = $f;
}
if (count($ffx) === 2) {
    foreach ($run(['vio.ffx_path' => $root], []) as $b => $r) {
        if (strpos($r[4], 'Müller 日本') === false) $fail[] = "ffx/$b: library '{$r[4]}'";
    }
}
foreach ($ffx as $f) @unlink("$root/$f");

// Longer in UTF-8 than query.library holds (but below MAX_PATH): loads, 'plugin' whole, 'library' cut at a character.
$long = $root . DIRECTORY_SEPARATOR . str_repeat('日', 170);
if ($nextToExe && mkdir($long)) {
    $inLong = $long . DIRECTORY_SEPARATOR . $file;
    copy($plugin, $inLong);
    $res = $run(['vio.dlss_plugin_path' => $inLong], []);
    $check('long', $res, $inLong);
    foreach ($res as $b => $r) {
        if (!preg_match('//u', $r[2]) || !str_starts_with($inLong, str_replace('/', '\\', $r[2])) || strlen($r[2]) > 511) $fail[] = "long/$b: library '{$r[2]}'";
    }
    @unlink($inLong);
    @rmdir($long);
}

// find_file with a buffer too small: 0 and a reason, never a cut path.
$res = $run(['vio.dlss_plugin_path' => $file_in_side], ['VIO_TEST_UPSCALER_FIND_SMALL' => '1']);
foreach ($res as $b => [$ok, $pl, $lib, $why]) {
    if ($ok !== false || strpos($why, 'does not fit') === false) $fail[] = "small/$b: '$why'";
}

// A plugin that says no still names what it loaded.
$res = $run(['vio.dlss_plugin_path' => $file_in_side], ['VIO_TEST_UPSCALER_UNSUPPORTED' => '1']);
foreach ($res as $b => [$ok, $pl, $lib, $why]) {
    if ($ok !== false || strpos($why, 'not on this GPU') === false) $fail[] = "no/$b: $ok $why";
    if (strcasecmp(str_replace('/', '\\', $lib), str_replace('/', '\\', $file_in_side)) !== 0) $fail[] = "no/$b: library '$lib'";
}

// vio.ffx_path / vio.dlss_path go the same way: nothing found, the name in the reason is intact.
$res = $run(['vio.dlss_plugin_path' => $side . DIRECTORY_SEPARATOR . 'fehlt'], []);
foreach ($res as $b => [$ok, $pl, $lib, $why]) {
    if ($ok !== false || strpos($why, 'Müller 日本') === false) $fail[] = "missing/$b: '$why'";
}

foreach (["$side/$file", "$root/$file", "$root/php.exe", $root . DIRECTORY_SEPARATOR . basename($core)] as $f) @unlink($f);
@rmdir($side);
@rmdir($root);
echo $fail ? "FAIL\n" . implode("\n", $fail) . "\n" : "OK\n";
echo "DONE\n";
?>
--EXPECT--
OK
DONE

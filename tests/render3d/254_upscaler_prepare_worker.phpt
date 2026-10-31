--TEST--
vio_upscaler_prepare: a provider's first-use work runs on a worker thread while frames go on. prepare returns at once, 'state' goes preparing -> ready without blocking, supported / render_size / create answer "not yet" instead of waiting and never reach the provider meanwhile, 'timings' reports the steps, $probe = false leaves an idle provider alone, a failed or refused preparation says why, a plugin built before the slots keeps working (prepare false, 'idle', the honest flag), destroying the context and ending the process with a preparation running wait for it (D3D12 / Vulkan, needs the test plugin built next to php_vio)
--EXTENSIONS--
vio
--SKIPIF--
<?php
if (!extension_loaded('vio')) die('skip vio not loaded');
$plugin = ini_get('extension_dir') . DIRECTORY_SEPARATOR . (PHP_OS_FAMILY === 'Windows' ? 'vio_test_upscaler.dll' : 'vio_test_upscaler.so');
if (!is_file($plugin)) die("skip test plugin not built ($plugin)");
?>
--FILE--
<?php
const WORK_MS = 400;
$plugin = ini_get('extension_dir') . DIRECTORY_SEPARATOR . (PHP_OS_FAMILY === 'Windows' ? 'vio_test_upscaler.dll' : 'vio_test_upscaler.so');
ini_set('vio.dlss_plugin_path', $plugin);
ini_set('vio.ffx_path', __DIR__);   // no FSR: VIO_FEATURE_UPSCALER_NATIVE is the plugin's alone
$open = function (string $b) {
    $ctx = @vio_create($b, ['width' => 16, 'height' => 16, 'headless' => true, 'vsync' => false]);
    if ($ctx && vio_backend_name($ctx) !== $b) { vio_destroy($ctx); return null; }
    return $ctx ?: null;
};

/* Scenarios that need a fresh process (the provider table stays for the process). */
if (($argv[1] ?? '') !== '') {
    $ctx = $open($argv[2]);
    switch ($argv[1]) {
        case 'old':   // a plugin built before timings / prepare_begin / prepare_run
            $flag = vio_supports_feature($ctx, VIO_FEATURE_UPSCALER_PREPARE);
            $p = vio_upscaler_prepare($ctx, VIO_UPSCALER_DLSS);
            $i = vio_upscaler_info($ctx, VIO_UPSCALER_DLSS, false);
            $j = vio_upscaler_info($ctx, VIO_UPSCALER_DLSS);
            $up = vio_upscaler_create($ctx, ['provider' => VIO_UPSCALER_DLSS, 'display_width' => 16, 'display_height' => 16]);
            echo json_encode([$flag, $p, $i['state'], $i['supported'], strpos($i['reason'], 'cannot prepare') !== false, $j['state'], $j['supported'], (bool)$up]), "\n";
            unset($up);
            vio_destroy($ctx);
            break;
        case 'exit':  // the process ends with the worker running, no vio_destroy
            vio_upscaler_prepare($ctx, VIO_UPSCALER_DLSS);
            echo vio_upscaler_info($ctx, VIO_UPSCALER_DLSS)['state'], "\n";
            exit(7);
        case 'fatal': // ... through a fatal error
            vio_upscaler_prepare($ctx, VIO_UPSCALER_DLSS);
            echo vio_upscaler_info($ctx, VIO_UPSCALER_DLSS)['state'], "\n";
            trigger_error('boom', E_USER_ERROR);
    }
    exit(0);
}

$child = function (string $case, string $b, array $env) use ($plugin) {
    $cmd = escapeshellarg(PHP_BINARY) . ' -n -d extension_dir=' . escapeshellarg(ini_get('extension_dir'))
         . ' -d extension=vio -d error_reporting=0 -d display_errors=0 ' . escapeshellarg(__FILE__) . " $case $b";
    $p = proc_open($cmd, [1 => ['pipe', 'w'], 2 => ['pipe', 'w']], $pipes, null, $env + getenv());
    $out = stream_get_contents($pipes[1]);
    stream_get_contents($pipes[2]);
    return [trim($out), proc_close($p)];
};

$wait = function ($ctx, int $provider, float $limitMs = 5000.0): array {
    $t = hrtime(true);
    do {
        $i = vio_upscaler_info($ctx, $provider);
        if ($i['state'] !== 'preparing') return $i;
        vio_begin($ctx); vio_clear($ctx, 0, 0, 0, 1); vio_end($ctx);
    } while ((hrtime(true) - $t) / 1e6 < $limitMs);
    return $i;
};

foreach (['d3d12', 'vulkan'] as $b) {
    $ctx = $open($b);
    if (!$ctx) { echo "$b: skip (unavailable)\n"; continue; }
    $fail = [];
    $dlss = VIO_UPSCALER_DLSS;

    // Nothing started: $probe = false does not touch the provider.
    $i = vio_upscaler_info($ctx, $dlss, false);
    if ($i['state'] !== 'idle' || $i['supported'] !== false || $i['version'] !== '') $fail[] = "idle " . json_encode($i);
    if (vio_supports_feature($ctx, VIO_FEATURE_UPSCALER_PREPARE) !== true) $fail[] = "flag off";

    // VIO_FEATURE_UPSCALER_NATIVE answers without the provider's first-use work (no FSR here).
    $before = $i['timings']['test_supported'] ?? 0;
    if (vio_supports_feature($ctx, VIO_FEATURE_UPSCALER_NATIVE) !== true) $fail[] = "native flag off";
    $i = vio_upscaler_info($ctx, $dlss, false);
    if ($i['state'] !== 'idle' || ($i['timings']['test_supported'] ?? 0) != $before) $fail[] = "native flag ran supported() " . json_encode($i);

    // A preparation that fails: the reason, from the worker.
    putenv('VIO_TEST_UPSCALER_PREPARE_FAIL=1');
    if (vio_upscaler_prepare($ctx, $dlss) !== true) $fail[] = "failing prepare not started";
    $i = $wait($ctx, $dlss);
    putenv('VIO_TEST_UPSCALER_PREPARE_FAIL');
    if ($i['state'] !== 'failed' || $i['supported'] !== false || strpos($i['reason'], 'preparation failed') === false) $fail[] = "failed " . json_encode($i);
    if (!isset($i['timings']['prepare_run'])) $fail[] = "no prepare_run timing " . json_encode($i['timings']);
    if (vio_supports_feature($ctx, VIO_FEATURE_UPSCALER_NATIVE) !== false) $fail[] = "native flag on after the worker said no";
    if (@vio_upscaler_create($ctx, ['provider' => $dlss, 'display_width' => 16, 'display_height' => 16]) !== false) $fail[] = "create after failure";

    // A refused one: at once, on this thread, no worker, flag follows.
    vio_destroy($ctx);
    $ctx = $open($b);
    putenv('VIO_TEST_UPSCALER_PREPARE_REFUSE=1');
    if (vio_supports_feature($ctx, VIO_FEATURE_UPSCALER_PREPARE) !== false) $fail[] = "flag on a refusing provider";
    $r = vio_upscaler_prepare($ctx, $dlss);
    $i = vio_upscaler_info($ctx, $dlss, false);
    if ($r !== false || $i['state'] !== 'failed' || strpos($i['reason'], 'refuses to prepare') === false) $fail[] = "refused " . json_encode($i);
    putenv('VIO_TEST_UPSCALER_PREPARE_REFUSE');
    vio_destroy($ctx);
    $ctx = $open($b);

    // The real thing: frames go on while the provider works for WORK_MS.
    putenv('VIO_TEST_UPSCALER_PREPARE_MS=' . WORK_MS);
    $t0 = hrtime(true);
    $r = vio_upscaler_prepare($ctx, $dlss);
    $back = (hrtime(true) - $t0) / 1e6;
    if ($r !== true) $fail[] = "prepare false";
    if ($back > WORK_MS / 2) $fail[] = "prepare blocked $back ms";
    if (vio_upscaler_prepare($ctx, $dlss) !== true) $fail[] = "second prepare while running";

    // Everything else says "not yet", without waiting and without a warning - create warns.
    error_clear_last();
    $i = vio_upscaler_info($ctx, $dlss);
    if ($i['state'] !== 'preparing' || $i['supported'] !== false || strpos($i['reason'], 'preparing') === false) $fail[] = "preparing " . json_encode($i);
    if (vio_upscaler_supported($ctx, $dlss) !== false) $fail[] = "supported while preparing";
    if (vio_upscaler_render_size($ctx, $dlss, VIO_UPSCALE_QUALITY, 16, 16) !== false) $fail[] = "render_size while preparing";
    if (vio_supports_feature($ctx, VIO_FEATURE_UPSCALER_PREPARE) !== true) $fail[] = "flag while preparing";
    if (($e = error_get_last()) !== null) $fail[] = "warned: " . $e['message'];
    $up = @vio_upscaler_create($ctx, ['provider' => $dlss, 'display_width' => 16, 'display_height' => 16]);
    $e = error_get_last();
    if ($up !== false || !$e || strpos($e['message'], 'preparing') === false) $fail[] = "create while preparing " . json_encode($e);
    if ((hrtime(true) - $t0) / 1e6 > WORK_MS * 0.9) $fail[] = "the calls above waited for the worker";

    $frames = 0;
    do {
        vio_begin($ctx); vio_clear($ctx, 0, 0, 0, 1); vio_end($ctx);
        $frames++;
        $i = vio_upscaler_info($ctx, $dlss);
    } while ($i['state'] === 'preparing' && (hrtime(true) - $t0) / 1e6 < 5000);
    $done = (hrtime(true) - $t0) / 1e6;
    if ($i['state'] !== 'ready' || $i['supported'] !== true) $fail[] = "not ready " . json_encode($i);
    if ($frames < 3) $fail[] = "only $frames frames while preparing";
    if ($done < WORK_MS * 0.9) $fail[] = "ready after $done ms";
    $tm = $i['timings'];
    foreach (['prepare_begin', 'prepare_run'] as $k) if (!isset($tm[$k])) $fail[] = "no timing $k " . json_encode($tm);
    if (($tm['test_worker'] ?? 0) != 1) $fail[] = "prepare_run ran on the PHP thread";
    if (($tm['test_overlap'] ?? -1) != 0) $fail[] = "calls reached the provider during prepare_run";
    if (($tm['prepare_run'] ?? 0) < WORK_MS * 0.9) $fail[] = "prepare_run " . ($tm['prepare_run'] ?? 0);

    // Ready: prepare is a no-op, create and a frame work.
    if (vio_upscaler_prepare($ctx, $dlss) !== true) $fail[] = "prepare when ready";
    $up = vio_upscaler_create($ctx, ['provider' => $dlss, 'display_width' => 16, 'display_height' => 16]);
    if (!$up) $fail[] = "create after ready";
    if (!isset(vio_upscaler_info($ctx, $dlss)['timings']['create'])) $fail[] = "no create timing";
    unset($up);

    // Destroying the context waits for a running worker (a new device, a new preparation).
    vio_destroy($ctx);
    $ctx = $open($b);
    if (vio_upscaler_info($ctx, $dlss, false)['state'] !== 'idle') $fail[] = "new device not idle";
    vio_upscaler_prepare($ctx, $dlss);
    $t = hrtime(true);
    vio_destroy($ctx);
    $w = (hrtime(true) - $t) / 1e6;
    if ($w < WORK_MS / 2) $fail[] = "destroy did not wait ($w ms)";

    // A second context on another backend prepares next to this one.
    $ctx = $open($b);
    $other = $open($b === 'd3d12' ? 'vulkan' : 'd3d12');
    if ($other) {
        vio_upscaler_prepare($ctx, $dlss);
        vio_upscaler_prepare($other, $dlss);
        $a = $wait($ctx, $dlss); $c = $wait($other, $dlss);
        if ($a['state'] !== 'ready' || $c['state'] !== 'ready') $fail[] = "two devices " . $a['state'] . '/' . $c['state'];
        vio_destroy($other);
    }
    vio_destroy($ctx);
    putenv('VIO_TEST_UPSCALER_PREPARE_MS');

    // Fresh processes.
    $e = ['VIO_TEST_UPSCALER_OLD' => '1'];
    [$out, $rc] = $child('old', $b, $e);
    if ($out !== '[false,false,"idle",false,true,"ready",true,true]' || $rc !== 0) $fail[] = "old plugin rc=$rc $out";
    $e = ['VIO_TEST_UPSCALER_PREPARE_MS' => (string)WORK_MS];
    [$out, $rc] = $child('exit', $b, $e);
    if ($out !== 'preparing' || $rc !== 7) $fail[] = "exit while preparing rc=$rc '$out'";
    [$out, $rc] = $child('fatal', $b, $e);
    if ($out !== 'preparing' || $rc !== 255) $fail[] = "fatal while preparing rc=$rc '$out'";

    echo "$b: ", $fail ? "FAIL " . implode('; ', $fail) : "OK", "\n";
}

// Backends without native upscalers: false, no worker, no warning.
$ctx = @vio_create('null', ['width' => 16, 'height' => 16, 'headless' => true]);
if ($ctx) {
    error_clear_last();
    $r = vio_upscaler_prepare($ctx, VIO_UPSCALER_DLSS);
    $i = vio_upscaler_info($ctx, VIO_UPSCALER_DLSS, false);
    echo "null: ", ($r === false && $i['state'] === 'idle' && $i['timings'] === [] && error_get_last() === null) ? "OK" : "FAIL", "\n";
    vio_destroy($ctx);
} else {
    echo "null: OK\n";
}
try { vio_upscaler_prepare($ctx ?? vio_create('auto', ['width' => 16, 'height' => 16, 'headless' => true]), 99); } catch (ValueError) { echo "ValueError\n"; } catch (Error) { echo "ValueError\n"; }
echo "DONE\n";
?>
--EXPECTF--
d3d12: %r(OK|skip \(unavailable\))%r
vulkan: %r(OK|skip \(unavailable\))%r
null: OK
ValueError
DONE

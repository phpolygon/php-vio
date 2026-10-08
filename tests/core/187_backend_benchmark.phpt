--TEST--
Calibration run: vio_benchmark_backends() renders the same scene on the top candidates, caches the result per (backend, adapter, driver, vio version), a cache hit skips the run; vio_create('auto', ['benchmark' => true]) picks the fastest and reports selected_by 'benchmark'
--EXTENSIONS--
vio
--FILE--
<?php
/* OPEN-ITEMS-PLAN A8 (BACKEND-SELECTION-PLAN Phase 2). */
$dir = sys_get_temp_dir() . DIRECTORY_SEPARATOR . 'vio_bench_' . getmypid();
@mkdir($dir);
$file = $dir . DIRECTORY_SEPARATOR . 'vio-benchmark.json';
$keys = ['backend', 'adapter', 'driver', 'ms', 'gpu_ms', 'cached'];

$r = vio_benchmark_backends(['frames' => 6, 'cache' => $dir]);
$ok = array_values(array_filter($r, fn($e) => $e['ms'] > 0));
$err = [];
if (!$ok) echo "skip: no backend ran the scene\n";
foreach ($r as $e) {
    if (array_keys($e) !== $keys) { $err[] = "keys " . json_encode(array_keys($e)); break; }
    if ($e['cached']) $err[] = "{$e['backend']} cached on the first run";
}
if (count($r) > 3) $err[] = "more than 3 candidates";
for ($i = 1; $i < count($ok); $i++) if ($ok[$i]['ms'] < $ok[$i - 1]['ms']) $err[] = "not sorted by ms";
$fails = array_keys(array_filter($r, fn($e) => $e['ms'] <= 0));
if ($fails && $ok && min($fails) < count($ok)) $err[] = "a failed run before a measured one";
echo "first:  ", $err ? "FAIL " . implode('; ', $err) : "OK", "\n";

if ($ok) {
    $cache = json_decode(file_get_contents($file), true);
    $missing = array_filter($ok, function ($e) use ($cache) {
        foreach ($cache['entries'] as $k => $_) if (str_starts_with($k, $e['backend'] . '|') && str_contains($k, '|' . $e['driver'] . '|')) return false;
        return true;
    });
    echo "cache:  ", ($cache['version'] ?? 0) === 1 && !$missing ? "OK" : "FAIL " . json_encode($cache), "\n";

    $r2 = vio_benchmark_backends(['frames' => 6, 'cache' => $dir]);
    $hit = array_values(array_filter($r2, fn($e) => $e['ms'] > 0));
    $same = count($hit) === count($ok) && !array_filter($hit, fn($e) => !$e['cached']);
    echo "hit:    ", $same ? "OK" : "FAIL " . json_encode($r2), "\n";

    /* Make the slowest measured candidate the fastest in the cache: auto must follow the cache. */
    $slow = end($ok);
    foreach ($cache['entries'] as $k => &$v) if (str_starts_with($k, $slow['backend'] . '|')) $v['ms'] = 0.001;
    unset($v);
    file_put_contents($file, json_encode($cache));
    $c = vio_create('auto', ['width' => 16, 'height' => 16, 'headless' => true, 'benchmark' => true,
                             'benchmark_cache' => $dir, 'benchmark_frames' => 6]);
    $i = $c ? vio_backend_info($c) : null;
    echo "auto:   ", $i && $i['selected_by'] === 'benchmark' && $i['backend'] === $slow['backend'] ? "OK" : "FAIL " . json_encode([$i['selected_by'] ?? null, $i['backend'] ?? null, $slow['backend']]), "\n";
    $bm = $i ? array_column($i['candidates'], 'benchmark_ms', 'backend') : [];
    echo "ms:     ", isset($bm[$slow['backend']]) && abs($bm[$slow['backend']] - 0.001) < 1e-9 ? "OK" : "FAIL " . json_encode($bm), "\n";
    if ($c) vio_destroy($c);
} else {
    echo "cache:  OK\nhit:    OK\nauto:   OK\nms:     OK\n";
}
@unlink($file);
@rmdir($dir);
echo "DONE\n";
?>
--EXPECTF--
%r(skip: no backend ran the scene\n)?%rfirst:  OK
cache:  OK
hit:    OK
auto:   OK
ms:     OK
DONE

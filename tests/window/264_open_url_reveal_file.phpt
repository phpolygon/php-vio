--TEST--
vio_open_url / vio_reveal_file (DESKTOP-APP-PLAN Phase 1): a URL with a scheme opens in the default handler, a file is shown in the file manager; file: URLs, URLs without a scheme and control characters are refused, a missing file is false, the null platform has neither; on macOS and Linux VIO_OPEN_COMMAND replaces the opener (here: a script that logs its arguments; Windows only checks the contract)
--EXTENSIONS--
vio
--FILE--
<?php
$dir = sys_get_temp_dir() . '/vio264-' . getmypid();
@mkdir($dir);
$log = "$dir/log.txt";
$opener = "$dir/opener.sh";
file_put_contents($opener, "#!/bin/sh\nprintf '%s\\n' \"\$*\" >> " . escapeshellarg($log) . "\n");
chmod($opener, 0755);
$target = "$dir/some file.txt";
file_put_contents($target, 'x');

$child = <<<'PHP'
<?php
[$target] = array_slice($argv, 1);
$err = function (callable $f) { try { $f(); return 'none'; } catch (ValueError $e) { return 'ValueError'; } };
$info = vio_platform_info();
$out = [];
$out[] = 'refused ' . implode(' ', [
    $err(fn() => vio_open_url('file:///etc/passwd')), $err(fn() => vio_open_url('FILE:///x')),
    $err(fn() => vio_open_url('example.com')), $err(fn() => vio_open_url("https://a\nb")),
    $err(fn() => vio_open_url('')), $err(fn() => vio_reveal_file('')), $err(fn() => vio_reveal_file("a\0b")),
]);
$out[] = 'missing ' . json_encode(vio_reveal_file($target . '.nope'));
/* without VIO_OPEN_COMMAND (Windows) a real browser would open: only the null platform calls then */
$real = getenv('VIO_OPEN_COMMAND') !== false || vio_platform() === 'null';
$out[] = 'open ' . ($real ? json_encode(vio_open_url('https://example.com/a b?x=1&y=2') === $info['open_url']) : 'skip');
$out[] = 'reveal ' . ($real ? json_encode(vio_reveal_file($target) === $info['reveal_file']) : 'skip');
$out[] = 'caps ' . json_encode([$info['open_url'], $info['reveal_file']]);
echo vio_platform(), ' ', implode(' | ', $out), "\n";
PHP;
$file = "$dir/child.php";
file_put_contents($file, $child);

$results = [];
$logs = [];
foreach (['win32', 'cocoa', 'x11', 'glfw', 'wayland', 'null'] as $platform) {
    @unlink($log);
    $cmd = escapeshellarg(PHP_BINARY) . ' -n -d extension_dir=' . escapeshellarg(ini_get('extension_dir'))
         . ' -d extension=vio ' . escapeshellarg($file) . ' ' . escapeshellarg($target);
    $env = getenv();
    $env['VIO_PLATFORM'] = $platform;
    if (PHP_OS_FAMILY !== 'Windows') $env['VIO_OPEN_COMMAND'] = $opener;
    $p = proc_open($cmd, [1 => ['pipe', 'w'], 2 => ['pipe', 'w']], $pipes, null, $env);
    $line = trim(stream_get_contents($pipes[1]));
    $errout = trim(stream_get_contents($pipes[2]));
    proc_close($p);
    if (!str_starts_with($line, $platform . ' ')) {
        if ($platform === 'null') $results[$platform] = "no result: $line $errout";
        continue;
    }
    $results[$platform] = substr($line, strlen($platform) + 1);
    /* the opener runs detached: give it a moment */
    for ($i = 0; $i < 30 && PHP_OS_FAMILY !== 'Windows' && $platform !== 'null' && count(@file($log) ?: []) < 2; $i++) usleep(100000);
    $logs[$platform] = is_file($log) ? file($log, FILE_IGNORE_NEW_LINES) : [];
}

$bad = [];
$reveal = PHP_OS_FAMILY === 'Darwin' ? "-R $target" : dirname($target);
foreach ($results as $platform => $line) {
    $has = $platform !== 'null';
    $called = $platform === 'null' || PHP_OS_FAMILY !== 'Windows' ? 'true' : 'skip';
    $want = "refused ValueError ValueError ValueError ValueError ValueError ValueError ValueError | missing false | open $called | reveal $called | caps "
          . json_encode([$has, $has]);
    if ($line !== $want) $bad[] = "$platform: $line";
    /* both openers run detached: any order */
    $wantLog = !$has || PHP_OS_FAMILY === 'Windows' ? [] : ['https://example.com/a b?x=1&y=2', $reveal];
    $got = $logs[$platform] ?? [];
    sort($got); sort($wantLog);
    if ($got !== $wantLog) $bad[] = "$platform log: " . json_encode($logs[$platform] ?? null);
}
array_map('unlink', glob("$dir/*"));
rmdir($dir);
/* the null platform and the active one must answer: a crashed child has no line */
echo isset($results['null'], $results[vio_platform()]) && !$bad ? "OK\n" : "FAIL " . json_encode([$results, $bad], JSON_PRETTY_PRINT) . "\n";
?>
--EXPECT--
OK

--TEST--
Native dialogs (DESKTOP-APP-PLAN Phase 2): vio_file_dialog (open / save / folder, filters, default path) and vio_message_box (buttons, kind); argument contract everywhere; on Win32 and Cocoa the real dialog opens and VIO_TEST_DIALOG dismisses it (cancel -> null / -1, save accepts the default name, a message box button returns its index)
--EXTENSIONS--
vio
--FILE--
<?php
$dir = sys_get_temp_dir() . DIRECTORY_SEPARATOR . 'vio266-' . getmypid();
@mkdir($dir);
$child = <<<'PHP'
<?php
[$dir] = array_slice($argv, 1);
/* a platform that is not built in falls back to the default one: that one is run under its own name */
if (vio_platform() !== getenv('VIO_PLATFORM')) { echo "skip\n"; exit; }
$ctx = @vio_create('auto', ['width' => 64, 'height' => 48, 'headless' => true]) ?: vio_create('null', ['width' => 64, 'height' => 48]);
$info = vio_platform_info();
$err = function (callable $f) { try { $f(); return 'none'; } catch (ValueError $e) { return 'ValueError'; } };
$out = [];
$out[] = 'caps ' . json_encode([$info['file_dialog'], $info['message_box']]);
$out[] = 'refused ' . implode(' ', [
    $err(fn() => vio_file_dialog($ctx, ['mode' => 'nope'])),
    $err(fn() => vio_file_dialog($ctx, ['filters' => [['name' => 'x']]])),
    $err(fn() => vio_file_dialog($ctx, ['filters' => [['name' => 'x', 'extensions' => ['p;ng']]]])),
    $err(fn() => vio_file_dialog($ctx, ['mode' => 'save', 'multiple' => true])),
    $err(fn() => vio_message_box($ctx, 'T', 'M', ['buttons' => []])),
    $err(fn() => vio_message_box($ctx, 'T', 'M', ['buttons' => ['a', 'b', 'c', 'd']])),
    $err(fn() => vio_message_box($ctx, 'T', 'M', ['kind' => 'loud'])),
]);
$run = function (string $action, callable $f) {
    static $step = 0;
    fwrite(STDERR, '[step ' . ++$step . " $action]\n");
    putenv("VIO_TEST_DIALOG=300:$action");
    $s = hrtime(true);
    $r = $f();
    putenv('VIO_TEST_DIALOG');
    return [$r, (hrtime(true) - $s) / 1e9 < 10];
};
$filters = [['name' => 'Images', 'extensions' => ['png', 'jpg']], ['name' => 'Text', 'extensions' => ['txt']]];
$save = $dir . DIRECTORY_SEPARATOR . 'saved file.txt';
$r = [
    'open cancel'   => $run('cancel', fn() => vio_file_dialog($ctx, ['title' => 'Open', 'filters' => $filters, 'multiple' => true, 'default_path' => $dir])),
    'folder cancel' => $run('cancel', fn() => vio_file_dialog($ctx, ['mode' => 'folder', 'default_path' => $dir])),
    'save accept'   => $run('accept', fn() => vio_file_dialog($ctx, ['mode' => 'save', 'filters' => $filters, 'default_path' => $save])),
    'box cancel'    => $run('cancel', fn() => vio_message_box($ctx, 'Title', "Line 1\nLine 2 äö", ['buttons' => ['Save', "Don't save", 'Cancel'], 'kind' => 'question'])),
    'box button 1'  => $run('1', fn() => vio_message_box($ctx, 'Title', 'Message', ['buttons' => ['Save', "Don't save", 'Cancel'], 'kind' => 'warning'])),
    'box button 0'  => $run('0', fn() => vio_message_box($ctx, 'Title', 'Message', ['kind' => 'error'])),
];
$out[] = 'run ' . json_encode($r, JSON_UNESCAPED_UNICODE | JSON_UNESCAPED_SLASHES);
echo vio_platform(), ' ', implode(' | ', $out), "\n";
vio_destroy($ctx);
PHP;
$file = "$dir/child.php";
file_put_contents($file, $child);
$results = [];
foreach (['win32', 'cocoa', 'x11', 'glfw', 'wayland', 'null'] as $platform) {
    if (PHP_OS_FAMILY === 'Linux' && in_array($platform, ['x11', 'glfw'], true) && !getenv('DISPLAY')) continue;
    if ($platform === 'wayland' && !getenv('WAYLAND_DISPLAY')) continue;
    /* an array: no shell in between, so terminating it ends PHP itself (cmd.exe on Windows would keep it) */
    $cmd = [PHP_BINARY, '-n', '-d', 'extension_dir=' . ini_get('extension_dir'), '-d', 'extension=vio', $file, $dir];
    $env = getenv();
    $env['VIO_PLATFORM'] = $platform;
    $env['VIO_TEST_DIALOG_TRACE'] = '1';
    /* files, not pipes: Windows pipes cannot be non-blocking, a read would wait for a hanging child */
    $o = "$dir/out-$platform.txt";
    $e = "$dir/err-$platform.txt";
    $p = proc_open($cmd, [1 => ['file', $o, 'w'], 2 => ['file', $e, 'w']], $pipes, null, $env);
    /* a dialog the hook cannot dismiss must not hang the suite: 15 s, then kill */
    $end = microtime(true) + 15;
    while (proc_get_status($p)['running'] && microtime(true) < $end) usleep(50000);
    $killed = proc_get_status($p)['running'];
    if ($killed) proc_terminate($p, 9);
    proc_close($p);
    $line = trim((string)@file_get_contents($o));
    $errout = trim((string)@file_get_contents($e)) . ($killed ? ' (killed after 15 s: the dialog stayed open)' : '');
    @unlink($o);
    @unlink($e);
    $errs[$platform] = $errout !== '' ? " [stderr: $errout]" : '';
    if (str_starts_with($line, $platform . ' ')) $results[$platform] = substr($line, strlen($platform) + 1);
    elseif ($platform === 'null' || $platform === vio_platform()) $results[$platform] = "no result: $line $errout";
}
@unlink($file);
@rmdir($dir);

$save = $dir . DIRECTORY_SEPARATOR . 'saved file.txt';
$bad = [];
foreach ($results as $platform => $line) {
    $has = in_array($platform, ['win32', 'cocoa'], true);
    $want = 'caps ' . json_encode([$has, $has]) . ' | refused ValueError ValueError ValueError ValueError ValueError ValueError ValueError | run ';
    $r = str_starts_with($line, $want) ? json_decode(substr($line, strlen($want)), true) : null;
    $wantRun = $has
        ? ['open cancel' => [null, true], 'folder cancel' => [null, true], 'save accept' => [[$save], true],
           'box cancel' => [2, true], 'box button 1' => [1, true], 'box button 0' => [0, true]]
        : ['open cancel' => [false, true], 'folder cancel' => [false, true], 'save accept' => [false, true],
           'box cancel' => [false, true], 'box button 1' => [false, true], 'box button 0' => [false, true]];
    /* macOS may resolve /var -> /private/var in the panel's URL */
    if ($r && $platform === 'cocoa' && isset($r['save accept'][0][0])) $r['save accept'][0][0] = preg_replace('#^/private(/var/)#', '$1', $r['save accept'][0][0]);
    if ($r !== $wantRun) $bad[] = "$platform: $line" . ($errs[$platform] ?? '');
}
echo isset($results['null'], $results[vio_platform()]) && !$bad ? "OK\n" : "FAIL " . json_encode([$results, $bad], JSON_PRETTY_PRINT | JSON_UNESCAPED_UNICODE | JSON_UNESCAPED_SLASHES) . "\n";
?>
--EXPECT--
OK

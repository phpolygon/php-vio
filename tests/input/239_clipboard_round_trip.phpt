--TEST--
Input: vio_clipboard_set / vio_clipboard_get - UTF-8 round-trip, no context needed, the system clipboard on every platform that has one, process-local on the null platform
--EXTENSIONS--
vio
--FILE--
<?php
// The clipboard is process-wide: no VioContext is needed. On desktop it is
// the system clipboard (platform slots); without one the extension keeps a
// process-local clipboard so set/get still round-trip.
$previous = vio_clipboard_get();
var_dump(is_string($previous));

var_dump(vio_clipboard_set("hello clipboard"));
var_dump(vio_clipboard_get());

// Multi-line, multibyte and CJK text survives byte-exact.
$text = "function f() {\n    return 'ä€😀 漢字';\n}\n";
var_dump(vio_clipboard_set($text));
var_dump(vio_clipboard_get() === $text);

// Empty string clears the text.
var_dump(vio_clipboard_set(""));
var_dump(vio_clipboard_get());

// A context does not change the behaviour.
$ctx = @vio_create("opengl", ["width" => 32, "height" => 32, "headless" => true]);
if ($ctx) {
    vio_clipboard_set("with context");
    echo vio_clipboard_get(), "\n";
    vio_destroy($ctx);
} else {
    echo "with context\n";
}

// Every platform built in, each in its own process (VIO_PLATFORM picks it):
// a round-trip inside the process, then a second process of the same
// platform reads what the first one wrote - true for a system clipboard,
// never for the process-local one of the null platform. The writer keeps
// polling events until the reader is done: on X11 the owner has to answer
// the reader's SelectionRequest itself.
$child = <<<'PHP'
<?php
[$mode, $arg, $flag] = [$argv[1], $argv[2], $argv[3]];
$t = "vio ⌘ " . $arg;
if ($mode === 'set') {
    $ctx = @vio_create('auto', ['width' => 16, 'height' => 16, 'headless' => true]);
    echo vio_platform(), ' ', (vio_clipboard_set($t) && vio_clipboard_get() === $t) ? 'roundtrip' : 'FAIL', "\n";
    fflush(STDOUT);
    $until = microtime(true) + 10;
    while (!file_exists($flag) && microtime(true) < $until) {
        if ($ctx) vio_poll_events($ctx);
        usleep(5000);
    }
} else {
    echo vio_platform(), ' ', vio_clipboard_get() === $t ? 'shared' : 'local', "\n";
}
PHP;
$file = tempnam(sys_get_temp_dir(), 'vio239');
file_put_contents($file, $child);
$start = function (string $platform, string $mode, string $arg, string $flag) use ($file): array {
    $cmd = escapeshellarg(PHP_BINARY) . ' -n -d extension_dir=' . escapeshellarg(ini_get('extension_dir'))
         . ' -d extension=vio ' . escapeshellarg($file) . ' ' . $mode . ' ' . escapeshellarg($arg) . ' ' . escapeshellarg($flag);
    $env = getenv();
    $env['VIO_PLATFORM'] = $platform;
    $p = proc_open($cmd, [1 => ['pipe', 'w'], 2 => ['pipe', 'w']], $pipes, null, $env);
    return [$p, $pipes];
};
$finish = function (array $proc): string {
    [$p, $pipes] = $proc;
    $out = trim(stream_get_contents($pipes[1]));
    stream_get_contents($pipes[2]);
    proc_close($p);
    return $out;
};
$bad = [];
$tested = 0;
foreach (['win32', 'cocoa', 'x11', 'glfw', 'wayland', 'null'] as $platform) {
    // X11 and GLFW on Linux need an X display (a Wayland-only session has none)
    if (PHP_OS_FAMILY === 'Linux' && in_array($platform, ['x11', 'glfw'], true) && !getenv('DISPLAY')) continue;
    $tag = $platform . '-' . mt_rand();
    $flag = sys_get_temp_dir() . '/vio239-' . $tag . '.done';
    $writer = $start($platform, 'set', $tag, $flag);
    $set = trim((string) fgets($writer[1][1]));
    $get = str_starts_with($set, $platform . ' ') ? $finish($start($platform, 'get', $tag, $flag)) : '';
    touch($flag);
    $finish($writer);
    unlink($flag);
    if (!str_starts_with($set, $platform . ' ')) continue;   /* not built in / no display */
    $tested++;
    // null and Wayland keep a process-local clipboard
    $want = in_array($platform, ['null', 'wayland'], true) ? "$platform local" : "$platform shared";
    if ($set !== "$platform roundtrip" || $get !== $want) $bad[] = "$platform: $set / $get";
}
unlink($file);
echo $tested > 0 && !$bad ? "platforms OK\n" : "platforms FAIL " . json_encode($bad) . "\n";

// Restore whatever the user had on the clipboard.
vio_clipboard_set($previous);
echo "OK\n";
?>
--EXPECT--
bool(true)
bool(true)
string(15) "hello clipboard"
bool(true)
bool(true)
bool(true)
string(0) ""
with context
platforms OK
OK

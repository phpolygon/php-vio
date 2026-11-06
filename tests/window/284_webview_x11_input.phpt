--TEST--
System webview on X11 (WEBVIEW-PLAN W2), visible window, each Linux engine in its own process (WPE WebKit, WebKitGTK; an engine that is not installed, no FFI or no libXtst: n/a, VIO_REQUIRE_WEBVIEW makes one engine mandatory): the page shows inside vio's window at its bounds and vio's frame around it (read back from the X server), a real click (XTest) reaches the page, typed keys land in its input field, a click into vio's window takes the keyboard back (the next key reaches vio_on_key)
--EXTENSIONS--
vio
--SKIPIF--
<?php
if (!extension_loaded('vio') || vio_platform() !== 'x11' || !getenv('DISPLAY')) die('skip X11 only');
?>
--FILE--
<?php
$dir = sys_get_temp_dir() . DIRECTORY_SEPARATOR . 'vio284-' . getmypid();
@mkdir($dir);
$child = <<<'PHP'
<?php
if (!vio_platform_info()['webview']) { echo "none\n"; exit; }
if (!class_exists('FFI')) { echo "no ffi\n"; exit; }
try {
    $x = FFI::cdef('typedef unsigned long Window; void *XOpenDisplay(const char *); unsigned long XDefaultRootWindow(void *); int XFlush(void *);
        unsigned char XKeysymToKeycode(void *, unsigned long);
        int XTranslateCoordinates(void *, Window, Window, int, int, int *, int *, Window *);
        int XWarpPointer(void *, Window, Window, int, int, unsigned, unsigned, int, int);
        typedef struct { int width, height, xoffset, format; char *data; int byte_order, bitmap_unit, bitmap_bit_order, bitmap_pad, depth, bytes_per_line, bits_per_pixel; } XImage;
        XImage *XGetImage(void *, Window, int, int, unsigned, unsigned, unsigned long, int); unsigned long XGetPixel(XImage *, int, int);', 'libX11.so.6');
    $t = FFI::cdef('int XTestFakeButtonEvent(void *, unsigned, int, unsigned long); int XTestFakeKeyEvent(void *, unsigned, int, unsigned long);', 'libXtst.so.6');
} catch (FFI\Exception $e) { echo "no xtest\n"; exit; }
$ctx = vio_create('auto', ['width' => 200, 'height' => 150, 'title' => 'vio284']);
$page = '<!doctype html><body style="margin:0;background:rgb(0,0,255)"><input id="i" style="position:absolute;left:0;top:0;width:80px;height:30px"><script>'
      . 'const i = document.getElementById("i");'
      . 'i.addEventListener("mousedown", () => window.vio.postMessage("down"));'
      . 'i.addEventListener("input", (e) => window.vio.postMessage("value:" + e.target.value));'
      . 'window.addEventListener("load", () => window.vio.postMessage("loaded"));'
      . '</script></body>';
$msgs = [];
$keys = [];
vio_on_key($ctx, function ($k, $a) use (&$keys) { if ($a === VIO_PRESS) $keys[] = $k; });
$wv = vio_webview($ctx, ['url' => 'app://x/', 'bounds' => [100, 50, 90, 60],
                         'schemes' => ['app' => fn() => ['headers' => ['Content-Type' => 'text/html'], 'body' => $page]]]);
vio_webview_on_message($wv, function ($m) use (&$msgs) { $msgs[] = json_decode($m); });
$wait = function (callable $done, float $s) use ($ctx) {
    $end = microtime(true) + $s;
    while (!$done() && microtime(true) < $end) {
        vio_wait_events($ctx, 0.05);
        vio_begin($ctx);
        vio_clear($ctx, 1.0, 0.0, 0.0);
        vio_end($ctx);
    }
};
$got = function (string $m) use (&$msgs) { return in_array($m, $msgs, true); };   /* by reference: an arrow function would copy $msgs */
$wait(fn() => $got('loaded'), 60);

$d = $x->XOpenDisplay(null);
$root = $x->XDefaultRootWindow($d);
$ox = $x->new('int'); $oy = $x->new('int'); $c = $x->new('Window');
$x->XTranslateCoordinates($d, (int)vio_native_window_handle($ctx), $root, 0, 0, FFI::addr($ox), FFI::addr($oy), FFI::addr($c));
$shot = function () use ($x, $d, $root, $ox, $oy) {
    $img = $x->XGetImage($d, $root, $ox->cdata, $oy->cdata, 200, 150, 0xffffffff, 2);
    $px = fn($a, $b) => sprintf('%06x', $x->XGetPixel($img, $a, $b) & 0xffffff);
    return [$px(20, 20), $px(180, 100), $px(190, 140)];
};
$wait(fn() => $shot()[1] === '0000ff', 10);   /* the page's first frame */
$shown = $shot();

$click = function ($wx, $wy) use ($x, $t, $d, $root, $ox, $oy) {
    $x->XWarpPointer($d, 0, $root, 0, 0, 0, 0, $ox->cdata + $wx, $oy->cdata + $wy);
    $t->XTestFakeButtonEvent($d, 1, 1, 0);
    $t->XTestFakeButtonEvent($d, 1, 0, 0);
    $x->XFlush($d);
};
$type = function ($s) use ($x, $t, $d) {
    foreach (str_split($s) as $ch) {
        $kc = $x->XKeysymToKeycode($d, ord($ch));
        $t->XTestFakeKeyEvent($d, $kc, 1, 0);
        $t->XTestFakeKeyEvent($d, $kc, 0, 0);
    }
    $x->XFlush($d);
};
$click(110, 60);
$wait(fn() => $got('down'), 10);
$wait(fn() => false, 0.3);   /* the field takes the focus */
$type('a');
$wait(fn() => $got('value:a'), 10);
$type('b');
$wait(fn() => $got('value:ab'), 10);
$click(20, 20);
$wait(fn() => false, 1.0);   /* the keyboard is back with vio */
$type('c');
$wait(function () use (&$keys) { return $keys; }, 5);
echo json_encode(['shown' => $shown, 'page' => array_values(array_diff($msgs, ['loaded'])), 'vio keys' => $keys === [VIO_KEY_C]]), "\n";
PHP;
file_put_contents("$dir/child.php", $child);
$out = [];
foreach (['wpe', 'gtk'] as $engine) {
    $env = getenv();
    $env['VIO_WEBVIEW_ENGINE'] = $engine;
    $cmd = [PHP_BINARY, '-n', '-d', 'extension_dir=' . ini_get('extension_dir'), '-d', 'extension=vio'];
    $ffiSo = PHP_EXTENSION_DIR . '/ffi.so';   /* as test 214: the shared FFI of this PHP, if it has one */
    if (extension_loaded('ffi')) array_push($cmd, '-d', 'ffi.enable=1');
    elseif (is_file($ffiSo)) array_push($cmd, '-d', 'extension=' . $ffiSo, '-d', 'ffi.enable=1');
    $cmd[] = "$dir/child.php";
    $p = proc_open($cmd, [1 => ['pipe', 'w'], 2 => ['pipe', 'w']], $pipes, null, $env);
    $o = trim(stream_get_contents($pipes[1]));
    $e = trim(stream_get_contents($pipes[2]));
    proc_close($p);
    $out[$engine] = $o . ($e !== '' && !in_array($o, ['none', 'no ffi', 'no xtest'], true) ? " [stderr: $e]" : '');
}
@unlink("$dir/child.php");
@rmdir($dir);
$want = '{"shown":["ff0000","0000ff","ff0000"],"page":["down","value:a","value:ab"],"vio keys":true}';
$bad = array_filter($out, fn($o) => $o !== $want && !in_array($o, ['none', 'no ffi', 'no xtest'], true));
$ran = array_filter($out, fn($o) => $o === $want);
/* VIO_REQUIRE_WEBVIEW: an engine must run here, unless the test cannot drive X at all (no FFI / libXtst) */
$cannot = !array_diff($out, ['no ffi', 'no xtest']);
echo !$bad && ($ran || $cannot || getenv('VIO_REQUIRE_WEBVIEW') === false) ? "OK\n" : 'FAIL ' . json_encode($out, JSON_PRETTY_PRINT | JSON_UNESCAPED_SLASHES) . "\n";
?>
--EXPECT--
OK

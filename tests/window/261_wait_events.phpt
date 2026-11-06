--TEST--
Window: vio_wait_events blocks until an event, its timeout or vio_wake (sticky until a wait takes it), costs no CPU while idle and delivers events like vio_poll_events, on every platform built in
--EXTENSIONS--
vio
--FILE--
<?php
/* Every platform runs the same contract in its own process (VIO_PLATFORM).
 * A wait may end early on a stray OS event, so the lower bound counts if one
 * of three attempts holds it; the idle check measures CPU time, which a busy
 * loop could not hide. Win32 and X11 also wake on a real OS event (FFI). */
$child = <<<'PHP'
<?php
$ctx = @vio_create('auto', ['width' => 32, 'height' => 32, 'headless' => true]) ?: vio_create('null', ['width' => 32, 'height' => 32]);
for ($i = 0; $i < 5; $i++) vio_poll_events($ctx);
$wait = function (float $t) use ($ctx) { $s = hrtime(true); vio_wait_events($ctx, $t); return (hrtime(true) - $s) / 1e9; };
$atLeast = function (float $t, float $min) use ($wait) { for ($i = 0; $i < 3; $i++) if ($wait($t) >= $min) return true; return false; };
$out = [];

$out[] = 'zero ' . json_encode($wait(0.0) < 0.05);
$e = $wait(0.2);
$out[] = 'timeout ' . json_encode($e < 1.0 && $atLeast(0.2, 0.15));

vio_wake();
$out[] = 'wake ' . json_encode($wait(5.0) < 0.5);
vio_wake();
$out[] = 'wake forever ' . json_encode($wait(-1.0) < 0.5);
$out[] = 'wake taken ' . json_encode($atLeast(0.2, 0.15));
vio_wake();
vio_poll_events($ctx);   // a poll does not take the wake
$out[] = 'wake kept ' . json_encode($wait(5.0) < 0.5);

$r0 = getrusage();
$s = hrtime(true);
while ((hrtime(true) - $s) / 1e9 < 0.5) vio_wait_events($ctx, 0.5);
$r1 = getrusage();
$cpu = ($r1['ru_utime.tv_sec'] - $r0['ru_utime.tv_sec']) + ($r1['ru_utime.tv_usec'] - $r0['ru_utime.tv_usec']) / 1e6
     + ($r1['ru_stime.tv_sec'] - $r0['ru_stime.tv_sec']) + ($r1['ru_stime.tv_usec'] - $r0['ru_stime.tv_usec']) / 1e6;
$out[] = 'idle ' . json_encode($cpu < 0.05);

/* a real OS event wakes the wait and reaches the input API */
$real = 'n/a';
if (extension_loaded('ffi') && in_array(vio_platform(), ['win32', 'x11'], true)) {
    $keys = [];
    vio_on_key($ctx, function ($k, $a) use (&$keys) { $keys[] = "$k/$a"; });
    $win = vio_native_window_handle($ctx);
    if (vio_platform() === 'win32') {
        $u = FFI::cdef('int PostMessageW(intptr_t hwnd, unsigned int msg, uintptr_t wparam, intptr_t lparam);', 'user32.dll');
        $u->PostMessageW($win, 0x0100 /* WM_KEYDOWN */, 0x41, 0x001E0001);   // A, scancode 0x1E
        $e = $wait(5.0);
    } else {
        $x = FFI::cdef(<<<'C'
typedef unsigned long XID;
typedef struct _XDisplay Display;
typedef struct { int type; unsigned long serial; int send_event; Display *display; XID window; XID root; XID subwindow;
                 unsigned long time; int x, y, x_root, y_root; unsigned int state; unsigned int code; int same_screen; } XInputEv;
typedef union { int type; XInputEv ev; long pad[24]; } XEvent;
Display *XOpenDisplay(const char *name);
int XCloseDisplay(Display *d);
int XSendEvent(Display *d, XID w, int propagate, long mask, XEvent *e);
int XFlush(Display *d);
unsigned char XKeysymToKeycode(Display *d, unsigned long keysym);
XID XDefaultRootWindow(Display *d);
C, 'libX11.so.6');
        $dpy = $x->XOpenDisplay(null);
        $ev = $x->new('XEvent');
        $ev->ev->type = 2; $ev->ev->send_event = 1; $ev->ev->display = $dpy; $ev->ev->window = $win;
        $ev->ev->root = $x->XDefaultRootWindow($dpy); $ev->ev->code = $x->XKeysymToKeycode($dpy, 0x61); $ev->ev->same_screen = 1;
        $x->XSendEvent($dpy, $win, 0, 1, FFI::addr($ev));
        $x->XFlush($dpy);
        $e = $wait(5.0);
        $x->XCloseDisplay($dpy);
    }
    $real = json_encode($e < 1.0 && in_array(VIO_KEY_A . '/' . VIO_PRESS, $keys, true));
}
$out[] = "os event $real";
echo vio_platform(), ' ', implode(' | ', $out), "\n";
vio_destroy($ctx);
PHP;
$file = tempnam(sys_get_temp_dir(), 'vio261');
file_put_contents($file, $child);
// the child runs with -n: load FFI by path when it is a shared extension
$ffi = '';
foreach ([PHP_EXTENSION_DIR . '/ffi.so', dirname(PHP_BINARY) . '/ext/php_ffi.dll'] as $f) if (is_file($f)) $ffi = '-d extension=' . escapeshellarg($f);
$results = [];
foreach (['win32', 'cocoa', 'x11', 'glfw', 'wayland', 'null'] as $platform) {
    if (PHP_OS_FAMILY === 'Linux' && in_array($platform, ['x11', 'glfw'], true) && !getenv('DISPLAY')) continue;
    $cmd = escapeshellarg(PHP_BINARY) . ' -n -d extension_dir=' . escapeshellarg(ini_get('extension_dir'))
         . ' -d extension=vio ' . $ffi . ' -d ffi.enable=1 ' . escapeshellarg($file);
    $env = getenv();
    $env['VIO_PLATFORM'] = $platform;
    $p = proc_open($cmd, [1 => ['pipe', 'w'], 2 => ['pipe', 'w']], $pipes, null, $env);
    $line = trim(stream_get_contents($pipes[1]));
    $err = trim(stream_get_contents($pipes[2]));
    proc_close($p);
    if (str_starts_with($line, $platform . ' ')) $results[$platform] = substr($line, strlen($platform) + 1);
    elseif ($platform === 'null') $results[$platform] = "no result: $line $err";
}
unlink($file);

$bad = [];
foreach ($results as $platform => $line) {
    $want = 'zero true | timeout true | wake true | wake forever true | wake taken true | wake kept true | idle true | os event ';
    if (!str_starts_with($line, $want) || !in_array(substr($line, strlen($want)), ['true', 'n/a'], true)) $bad[] = "$platform: $line";
}
echo isset($results['null']) && !$bad ? "OK\n" : "FAIL " . json_encode([$results, $bad], JSON_PRETTY_PRINT) . "\n";
?>
--EXPECT--
OK

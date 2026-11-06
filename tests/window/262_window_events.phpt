--TEST--
Window: vio_on_close (veto), vio_on_focus, vio_on_minimize, vio_on_scale and vio_on_refresh fire once per change, through injection on every platform and through real window messages on Win32 and X11
--EXTENSIONS--
vio
--FILE--
<?php
/* vio_inject_window_event takes the same funnel as the platforms' OS events.
 * Win32 (PostMessageW) and X11 (XSendEvent) also send the
 * real messages; the other platforms cannot be driven from a test process. */
$child = <<<'PHP'
<?php
$ctx = @vio_create('auto', ['width' => 32, 'height' => 32, 'headless' => true]) ?: vio_create('null', ['width' => 32, 'height' => 32]);
for ($i = 0; $i < 5; $i++) vio_poll_events($ctx);
$log = [];
$veto = true;
vio_on_close($ctx, function () use (&$log, &$veto) { $log[] = 'close'; return $veto ? false : null; });
vio_on_focus($ctx, function (bool $f) use (&$log) { $log[] = 'focus ' . json_encode($f); });
vio_on_minimize($ctx, function (bool $m) use (&$log) { $log[] = 'minimize ' . json_encode($m); });
vio_on_scale($ctx, function (float $x, float $y) use (&$log) { $log[] = "scale $x $y"; });
vio_on_refresh($ctx, function () use (&$log) { $log[] = 'refresh'; });
$take = function () use (&$log) { $s = implode(', ', $log); $log = []; return $s; };
$out = [];

$a = vio_inject_window_event($ctx, VIO_WINDOW_EVENT_CLOSE);
$out[] = 'veto ' . json_encode([$a, vio_should_close($ctx)]) . ' ' . $take();
vio_inject_window_event($ctx, VIO_WINDOW_EVENT_FOCUS, 0);
vio_inject_window_event($ctx, VIO_WINDOW_EVENT_FOCUS, 1);
vio_inject_window_event($ctx, VIO_WINDOW_EVENT_FOCUS, 1);
vio_inject_window_event($ctx, VIO_WINDOW_EVENT_MINIMIZE, 1);
vio_inject_window_event($ctx, VIO_WINDOW_EVENT_MINIMIZE, 1);
vio_inject_window_event($ctx, VIO_WINDOW_EVENT_MINIMIZE, 0);
vio_inject_window_event($ctx, VIO_WINDOW_EVENT_SCALE, 3, 3);
vio_inject_window_event($ctx, VIO_WINDOW_EVENT_SCALE, 3, 3);
vio_inject_window_event($ctx, VIO_WINDOW_EVENT_REFRESH);
$out[] = 'events ' . $take();

/* real messages: close (vetoed), focus out/in, minimize/restore, repaint */
$real = 'n/a';
if (extension_loaded('ffi') && in_array(vio_platform(), ['win32', 'x11'], true)) {
    $win = vio_native_window_handle($ctx);
    if (vio_platform() === 'win32') {
        $u = FFI::cdef('int PostMessageW(intptr_t hwnd, unsigned int msg, uintptr_t wparam, intptr_t lparam);', 'user32.dll');
        $u->PostMessageW($win, 0x0010, 0, 0);          // WM_CLOSE
        $u->PostMessageW($win, 0x0008, 0, 0);          // WM_KILLFOCUS
        $u->PostMessageW($win, 0x0007, 0, 0);          // WM_SETFOCUS
        $u->PostMessageW($win, 0x0005, 1, 0);          // WM_SIZE, SIZE_MINIMIZED
        $u->PostMessageW($win, 0x0005, 0, 0x00200020); // WM_SIZE, SIZE_RESTORED 32x32
        $u->PostMessageW($win, 0x000F, 0, 0);          // WM_PAINT (a hidden window gets none from InvalidateRect)
        vio_poll_events($ctx);
    } else {
        $x = FFI::cdef(<<<'C'
typedef unsigned long XID;
typedef unsigned long Atom;
typedef struct _XDisplay Display;
typedef struct { int type; unsigned long serial; int send_event; Display *display; XID window; Atom message_type; int format; long l[5]; } XClientEv;
typedef struct { int type; unsigned long serial; int send_event; Display *display; XID window; int mode; int detail; } XFocusEv;
typedef struct { int type; unsigned long serial; int send_event; Display *display; XID window; int x, y, width, height, count; } XExposeEv;
typedef union { int type; XClientEv client; XFocusEv focus; XExposeEv expose; long pad[24]; } XEvent;
Display *XOpenDisplay(const char *name);
int XCloseDisplay(Display *d);
int XSendEvent(Display *d, XID w, int propagate, long mask, XEvent *e);
int XFlush(Display *d);
Atom XInternAtom(Display *d, const char *name, int only_if_exists);
C, 'libX11.so.6');
        $dpy = $x->XOpenDisplay(null);
        $send = function (callable $fill) use ($x, $dpy, $win) {
            $e = $x->new('XEvent');
            $fill($e);
            $x->XSendEvent($dpy, $win, 0, 0, FFI::addr($e));
        };
        $send(function ($e) use ($x, $dpy, $win) {
            $e->client->type = 33; $e->client->send_event = 1; $e->client->display = $dpy; $e->client->window = $win;
            $e->client->message_type = $x->XInternAtom($dpy, 'WM_PROTOCOLS', 0); $e->client->format = 32;
            $e->client->l[0] = $x->XInternAtom($dpy, 'WM_DELETE_WINDOW', 0);
        });
        foreach ([10, 9] as $type) {   // FocusOut, FocusIn (NotifyNormal, NotifyNonlinear)
            $send(function ($e) use ($dpy, $win, $type) { $e->focus->type = $type; $e->focus->send_event = 1; $e->focus->display = $dpy; $e->focus->window = $win; $e->focus->detail = 3; });
        }
        $send(function ($e) use ($dpy, $win) { $e->expose->type = 12; $e->expose->send_event = 1; $e->expose->display = $dpy; $e->expose->window = $win; $e->expose->width = 32; $e->expose->height = 32; });
        $x->XFlush($dpy);
        usleep(100000);
        vio_poll_events($ctx);
        $x->XCloseDisplay($dpy);
    }
    $real = json_encode(vio_should_close($ctx)) . ' ' . $take();
}
$out[] = "os $real";

$veto = false;
$a = vio_inject_window_event($ctx, VIO_WINDOW_EVENT_CLOSE);
$out[] = 'accept ' . json_encode([$a, vio_should_close($ctx)]) . ' ' . $take();
echo vio_platform(), ' ', implode(' | ', $out), "\n";
vio_destroy($ctx);

/* without a close callback a close request closes */
$ctx = @vio_create('auto', ['width' => 32, 'height' => 32, 'headless' => true]) ?: vio_create('null', ['width' => 32, 'height' => 32]);
echo vio_platform(), ' plain ', json_encode([vio_inject_window_event($ctx, VIO_WINDOW_EVENT_CLOSE), vio_should_close($ctx)]), "\n";
vio_destroy($ctx);
PHP;
$file = tempnam(sys_get_temp_dir(), 'vio262');
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
    $lines = array_filter(explode("\n", trim(stream_get_contents($pipes[1]))));
    $err = trim(stream_get_contents($pipes[2]));
    proc_close($p);
    $mine = array_values(array_filter($lines, fn($l) => str_starts_with($l, $platform . ' ')));
    if ($mine) $results[$platform] = array_map(fn($l) => substr($l, strlen($platform) + 1), $mine);
    elseif ($platform === 'null') $results[$platform] = ['no result: ' . implode(' / ', $lines) . " $err"];
}
unlink($file);

$want = 'veto [false,false] close | events focus false, focus true, minimize true, minimize false, scale 3 3, refresh | os ';
$wantOs = 'false close, focus false, focus true, minimize true, minimize false, refresh';
$bad = [];
foreach ($results as $platform => $lines) {
    $line = $lines[0];
    $ok = count($lines) === 2 && $lines[1] === 'plain [true,true]' && str_starts_with($line, $want)
        && str_ends_with($line, ' | accept [true,true] close');
    $os = $ok ? substr($line, strlen($want), -strlen(' | accept [true,true] close')) : null;
    /* X11 has no minimize message to send (iconify is a WM request) */
    $wantHere = $platform === 'x11' ? 'false close, focus false, focus true, refresh' : $wantOs;
    if (!$ok || !in_array($os, ['n/a', $wantHere], true)) $bad[] = "$platform: " . implode(' / ', $lines);
}
echo isset($results['null']) && !$bad ? "OK\n" : "FAIL " . json_encode([$results, $bad], JSON_PRETTY_PRINT) . "\n";
?>
--EXPECT--
OK

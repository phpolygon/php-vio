--TEST--
File drop (DESKTOP-APP-PLAN Phase 2): vio_on_drop gets the dropped paths (UTF-8, spaces) and the drop point in window coordinates; vio_inject_drop takes the same path; real drops on Win32 (WM_DROPFILES) and X11 (XDND from a second display connection that serves the selection)
--EXTENSIONS--
vio
--FILE--
<?php
$child = <<<'PHP'
<?php
$ctx = @vio_create('auto', ['width' => 64, 'height' => 48, 'headless' => true]) ?: vio_create('null', ['width' => 64, 'height' => 48]);
for ($i = 0; $i < 5; $i++) vio_poll_events($ctx);
$p = vio_platform();
$drops = [];
vio_on_drop($ctx, function (array $paths, float $x, float $y) use (&$drops) { $drops[] = [$paths, $x, $y]; });
$err = function (callable $f) { try { $f(); return 'none'; } catch (ValueError $e) { return 'ValueError'; } };
$out = [];
$out[] = 'info ' . json_encode(vio_platform_info()['file_drop']);

$paths = ['/tmp/a b.txt', '/tmp/Grüße/€ 1.png'];
vio_inject_drop($ctx, $paths, 12.5, 7.0);
$out[] = 'inject ' . json_encode($drops === [[$paths, 12.5, 7.0]]) . ' '
       . $err(fn() => vio_inject_drop($ctx, [])) . ' ' . $err(fn() => vio_inject_drop($ctx, [42])) . ' ' . $err(fn() => vio_inject_drop($ctx, ["a\0b"]));
$drops = [];

$os = 'n/a';
$win = vio_native_window_handle($ctx);
if (extension_loaded('ffi') && $win && $p === 'win32') {
    /* DROPFILES { DWORD pFiles; POINT pt; BOOL fNC; BOOL fWide; } + UTF-16 paths, double NUL */
    $k = FFI::cdef('intptr_t GlobalAlloc(unsigned int flags, size_t bytes); void *GlobalLock(intptr_t h); int GlobalUnlock(intptr_t h);', 'kernel32.dll');
    $u = FFI::cdef('int PostMessageW(intptr_t hwnd, unsigned int msg, uintptr_t wparam, intptr_t lparam);', 'user32.dll');
    $files = ['C:\\Temp\\a b.txt', 'C:\\Temp\\Grüße\\€ 1.png'];
    $wide = '';
    foreach ($files as $f) $wide .= mb_convert_encoding($f, 'UTF-16LE', 'UTF-8') . "\0\0";
    $wide .= "\0\0";
    $blob = pack('VllVV', 20, 21, 13, 0, 1) . $wide;
    $h = $k->GlobalAlloc(0x0042 /* GHND */, strlen($blob));
    FFI::memcpy($k->GlobalLock($h), $blob, strlen($blob));
    $k->GlobalUnlock($h);
    $u->PostMessageW($win, 0x0233 /* WM_DROPFILES */, $h, 0);
    vio_poll_events($ctx);
    $os = json_encode($drops === [[$files, 21.0, 13.0]]) . ($drops && $drops !== [[$files, 21.0, 13.0]] ? json_encode($drops) : '');
} elseif (extension_loaded('ffi') && $win && $p === 'x11') {
    $x = FFI::cdef(<<<'C'
typedef unsigned long XID;
typedef unsigned long Atom;
typedef struct _XDisplay Display;
typedef struct { int type; unsigned long serial; int send_event; Display *display; XID window; Atom message_type; int format; long l[5]; } XClientEv;
typedef struct { int type; unsigned long serial; int send_event; Display *display; XID owner; XID requestor; Atom selection; Atom target; Atom property; unsigned long time; } XSelReqEv;
typedef struct { int type; unsigned long serial; int send_event; Display *display; XID requestor; Atom selection; Atom target; Atom property; unsigned long time; } XSelEv;
typedef union { int type; XClientEv client; XSelReqEv req; XSelEv sel; long pad[24]; } XEvent;
Display *XOpenDisplay(const char *name);
int XCloseDisplay(Display *d);
XID XDefaultRootWindow(Display *d);
XID XCreateSimpleWindow(Display *d, XID parent, int x, int y, unsigned int w, unsigned int h, unsigned int bw, unsigned long border, unsigned long bg);
Atom XInternAtom(Display *d, const char *name, int only_if_exists);
int XSetSelectionOwner(Display *d, Atom sel, XID owner, unsigned long time);
int XSendEvent(Display *d, XID w, int propagate, long mask, XEvent *e);
int XChangeProperty(Display *d, XID w, Atom prop, Atom type, int format, int mode, const char *data, int n);
int XPending(Display *d);
int XNextEvent(Display *d, XEvent *e);
int XFlush(Display *d);
int XTranslateCoordinates(Display *d, XID src, XID dst, int sx, int sy, int *dx, int *dy, XID *child);
C, 'libX11.so.6');
    $d = $x->XOpenDisplay(null);
    $root = $x->XDefaultRootWindow($d);
    $src = $x->XCreateSimpleWindow($d, $root, 0, 0, 1, 1, 0, 0, 0);
    $A = fn(string $n) => $x->XInternAtom($d, $n, 0);
    $x->XSetSelectionOwner($d, $A('XdndSelection'), $src, 0);
    $rx = $x->new('int'); $ry = $x->new('int'); $ch = $x->new('XID');
    $x->XTranslateCoordinates($d, $win, $root, 21, 13, FFI::addr($rx), FFI::addr($ry), FFI::addr($ch));
    $send = function (string $type, array $l) use ($x, $d, $win, $A) {
        $e = $x->new('XEvent');
        $e->client->type = 33; $e->client->send_event = 1; $e->client->display = $d; $e->client->window = $win;
        $e->client->message_type = $A($type); $e->client->format = 32;
        foreach ($l as $i => $v) $e->client->l[$i] = $v;
        $x->XSendEvent($d, $win, 0, 0, FFI::addr($e));
    };
    $send('XdndEnter', [$src, 5 << 24, $A('text/uri-list'), 0, 0]);
    $send('XdndPosition', [$src, 0, ($rx->cdata << 16) | $ry->cdata, 0, $A('XdndActionCopy')]);
    $send('XdndDrop', [$src, 0, 0, 0, 0]);
    $x->XFlush($d);
    $list = "file:///tmp/a%20b.txt\r\n# comment\r\nfile://localhost/tmp/Gr%C3%BC%C3%9Fe/%E2%82%AC%201.png\r\n";
    $finished = false;
    for ($i = 0; $i < 50 && !$drops; $i++) {
        vio_poll_events($ctx);
        while ($x->XPending($d)) {
            $e = $x->new('XEvent');
            $x->XNextEvent($d, FFI::addr($e));
            if ($e->type === 30 /* SelectionRequest */) {
                $r = $e->req;
                $x->XChangeProperty($d, $r->requestor, $r->property, $r->target, 8, 0, $list, strlen($list));
                $n = $x->new('XEvent');
                $n->sel->type = 31; $n->sel->send_event = 1; $n->sel->display = $d; $n->sel->requestor = $r->requestor;
                $n->sel->selection = $r->selection; $n->sel->target = $r->target; $n->sel->property = $r->property; $n->sel->time = $r->time;
                $x->XSendEvent($d, $r->requestor, 0, 0, FFI::addr($n));
                $x->XFlush($d);
            } elseif ($e->type === 33 && $e->client->message_type === $A('XdndFinished')) {
                $finished = true;
            }
        }
        usleep(20000);
    }
    for ($i = 0; $i < 10 && !$finished; $i++) {   /* XdndFinished follows the drop */
        usleep(20000);
        while ($x->XPending($d)) { $e = $x->new('XEvent'); $x->XNextEvent($d, FFI::addr($e)); if ($e->type === 33 && $e->client->message_type === $A('XdndFinished')) $finished = true; }
    }
    $want = [[['/tmp/a b.txt', '/tmp/Grüße/€ 1.png'], 21.0, 13.0]];
    $os = json_encode($drops === $want && $finished) . ($drops !== $want ? json_encode($drops) : '');
    $x->XCloseDisplay($d);
}
$out[] = "os $os";
echo $p, ' ', implode(' | ', $out), "\n";
vio_destroy($ctx);
PHP;
$file = tempnam(sys_get_temp_dir(), 'vio265');
file_put_contents($file, $child);
$ffi = '';
foreach ([PHP_EXTENSION_DIR . '/ffi.so', dirname(PHP_BINARY) . '/ext/php_ffi.dll'] as $f) if (is_file($f)) $ffi = '-d extension=' . escapeshellarg($f);
$mb = '';
foreach ([PHP_EXTENSION_DIR . '/mbstring.so', dirname(PHP_BINARY) . '/ext/php_mbstring.dll'] as $f) if (is_file($f)) $mb = '-d extension=' . escapeshellarg($f);
$results = [];
foreach (['win32', 'cocoa', 'x11', 'glfw', 'wayland', 'null'] as $platform) {
    if (PHP_OS_FAMILY === 'Linux' && in_array($platform, ['x11', 'glfw'], true) && !getenv('DISPLAY')) continue;
    if ($platform === 'wayland' && !getenv('WAYLAND_DISPLAY')) continue;
    $cmd = escapeshellarg(PHP_BINARY) . ' -n -d extension_dir=' . escapeshellarg(ini_get('extension_dir'))
         . ' -d extension=vio ' . $ffi . ' ' . $mb . ' -d ffi.enable=1 ' . escapeshellarg($file);
    $env = getenv();
    $env['VIO_PLATFORM'] = $platform;
    $proc = proc_open($cmd, [1 => ['pipe', 'w'], 2 => ['pipe', 'w']], $pipes, null, $env);
    $line = trim(stream_get_contents($pipes[1]));
    $errout = trim(stream_get_contents($pipes[2]));
    proc_close($proc);
    if (str_starts_with($line, $platform . ' ')) $results[$platform] = substr($line, strlen($platform) + 1);
    elseif ($platform === 'null') $results[$platform] = "no result: $line $errout";
}
unlink($file);
$bad = [];
foreach ($results as $platform => $line) {
    $want = 'info ' . json_encode($platform !== 'null') . ' | inject true ValueError ValueError ValueError | os ';
    $os = substr($line, strlen($want));
    if (!str_starts_with($line, $want) || !in_array($os, in_array($platform, ['win32', 'x11'], true) ? ['true', 'n/a'] : ['n/a'], true)) $bad[] = "$platform: $line";
}
/* the null platform and the active one must answer: a crashed child has no line */
echo isset($results['null'], $results[vio_platform()]) && !$bad ? "OK\n" : "FAIL " . json_encode([$results, $bad], JSON_PRETTY_PRINT | JSON_UNESCAPED_UNICODE) . "\n";
?>
--EXPECT--
OK

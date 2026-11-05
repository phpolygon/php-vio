--TEST--
Window parent and modal windows (DESKTOP-APP-PLAN Phase 4): vio_window(['parent' => $ctx|$window, 'modal' => true]) keeps a window with its parent; a modal one blocks the parent's keys, buttons, scroll, text and close requests (releases still pass) until it closes, nested modals block each level; closing a parent closes its children first; argument contract; Win32 (owner, disabled owner), X11 (WM_TRANSIENT_FOR, _NET_WM_STATE_MODAL) and Cocoa (parentWindow) asked through FFI
--EXTENSIONS--
vio
--FILE--
<?php
/* Every platform runs in its own process (VIO_PLATFORM); a GPU backend with
 * VIO_FEATURE_MULTI_WINDOW is needed for vio_window. */
$child = <<<'PHP'
<?php
$p = vio_platform();
$ctx = @vio_create('auto', ['width' => 64, 'height' => 48, 'headless' => true]);
if (!$ctx || !vio_supports_feature($ctx, VIO_FEATURE_MULTI_WINDOW)) { echo "$p n/a\n"; exit; }
$out = [];
$err = function (callable $f) { try { $f(); return 'none'; } catch (ValueError $e) { return 'ValueError'; } catch (TypeError $e) { return 'TypeError'; } };
$opt = fn(array $o) => ['width' => 24, 'height' => 16, 'headless' => true] + $o;
$info = vio_platform_info();

/* contract */
$other = vio_create('null', ['width' => 8, 'height' => 8]);
$out[] = 'contract ' . implode(' ', [
    $err(fn() => vio_window($ctx, $opt(['modal' => true]))),
    $err(fn() => vio_window($ctx, $opt(['parent' => 'x']))),
    $err(fn() => vio_window($ctx, $opt(['parent' => $other]))),
]);

/* modal over the main window: its input is blocked, the dialog's is not */
vio_inject_key($ctx, VIO_KEY_B, VIO_PRESS);   // held before the dialog opens
$d = vio_window($ctx, $opt(['parent' => $ctx, 'modal' => true, 'title' => 'Dialog']));
vio_inject_key($ctx, VIO_KEY_A, VIO_PRESS);
vio_inject_mouse_button($ctx, VIO_MOUSE_LEFT, VIO_PRESS);
vio_inject_char($ctx, 'x');
vio_inject_key($ctx, VIO_KEY_B, VIO_RELEASE);
$asked = 0;
vio_on_close($ctx, function () use (&$asked) { $asked++; return null; });
$closed = vio_inject_window_event($ctx, VIO_WINDOW_EVENT_CLOSE);
vio_inject_key($d, VIO_KEY_A, VIO_PRESS);
$out[] = 'blocked ' . json_encode([vio_key_pressed($ctx, VIO_KEY_A), vio_mouse_button($ctx, VIO_MOUSE_LEFT), vio_chars_typed($ctx),
                                   vio_key_pressed($ctx, VIO_KEY_B), $closed, $asked, vio_should_close($ctx), vio_key_pressed($d, VIO_KEY_A)]);

/* nested: a modal over the dialog blocks the dialog too; closing it unblocks the dialog, the main window stays blocked by the dialog */
$n = vio_window($ctx, $opt(['parent' => $d, 'modal' => true]));
vio_inject_key($d, VIO_KEY_C, VIO_PRESS);
$nested = vio_key_pressed($d, VIO_KEY_C);
vio_destroy_window($n);
vio_inject_key($d, VIO_KEY_C, VIO_PRESS);
vio_inject_key($ctx, VIO_KEY_C, VIO_PRESS);
$out[] = 'nested ' . json_encode([$nested, vio_key_pressed($d, VIO_KEY_C), vio_key_pressed($ctx, VIO_KEY_C)]);
vio_destroy_window($d);
vio_inject_key($ctx, VIO_KEY_A, VIO_PRESS);
$out[] = 'unblocked ' . json_encode(vio_key_pressed($ctx, VIO_KEY_A));

/* closing a parent closes its children first */
$a = vio_window($ctx, $opt([]));
$b = vio_window($ctx, $opt(['parent' => $a]));
$c = vio_window($ctx, $opt(['parent' => $b, 'modal' => true]));
vio_destroy_window($a);
$out[] = 'cascade ' . implode(' ', [$err(fn() => vio_window_size($b)), $err(fn() => vio_window_size($c)), $err(fn() => vio_window($ctx, $opt(['parent' => $b])))]);

/* the window system's own view (visible windows: Cocoa relates only those) */
$os = 'n/a';
if (extension_loaded('ffi') && $info['window_parent'] && in_array($p, ['win32', 'x11', 'cocoa'], true)) {
    $vp = vio_window($ctx, ['width' => 80, 'height' => 60, 'headless' => $p === 'x11', 'title' => 'Owner']);
    $vc = vio_window($ctx, ['width' => 60, 'height' => 40, 'headless' => $p === 'x11', 'parent' => $vp, 'modal' => true, 'title' => 'Owned']);
    $hp = vio_native_window_handle($vp);
    $hc = vio_native_window_handle($vc);
    if ($p === 'win32') {
        $u = FFI::cdef('intptr_t GetWindow(intptr_t h, unsigned int cmd); int IsWindowEnabled(intptr_t h);', 'user32.dll');
        $r = [$u->GetWindow($hc, 4) === $hp, $u->IsWindowEnabled($hp)];
        vio_destroy_window($vc);
        $r[] = $u->IsWindowEnabled($hp);
        $os = json_encode($r);
    } elseif ($p === 'x11') {
        $x = FFI::cdef(<<<'C'
typedef unsigned long Window;
typedef unsigned long Atom;
void *XOpenDisplay(const char *name);
int XGetTransientForHint(void *dpy, Window w, Window *prop);
Atom XInternAtom(void *dpy, const char *name, int only_if_exists);
int XGetWindowProperty(void *dpy, Window w, Atom prop, long off, long len, int del, Atom req, Atom *type, int *fmt, unsigned long *n, unsigned long *after, unsigned char **data);
int XCloseDisplay(void *dpy);
C, 'libX11.so.6');
        $dpy = $x->XOpenDisplay(null);
        $t = $x->new('Window');
        $x->XGetTransientForHint($dpy, $hc, FFI::addr($t));
        $type = $x->new('Atom'); $fmt = $x->new('int'); $cnt = $x->new('unsigned long'); $after = $x->new('unsigned long'); $data = $x->new('unsigned char *');
        $x->XGetWindowProperty($dpy, $hc, $x->XInternAtom($dpy, '_NET_WM_STATE', 0), 0, 16, 0, 4 /* XA_ATOM */, FFI::addr($type), FFI::addr($fmt), FFI::addr($cnt), FFI::addr($after), FFI::addr($data));
        $modal = $x->XInternAtom($dpy, '_NET_WM_STATE_MODAL', 0);
        $atoms = [];
        if ($cnt->cdata > 0) { $arr = $x->cast('Atom *', $data); for ($i = 0; $i < $cnt->cdata; $i++) $atoms[] = $arr[$i]; }
        $os = json_encode([$t->cdata === $hp, in_array($modal, $atoms, true)]);
        $x->XCloseDisplay($dpy);
    } elseif ($p === 'cocoa') {
        $o = FFI::cdef('void *sel_registerName(const char *s); uintptr_t objc_msgSend(void *obj, void *sel);', '/usr/lib/libobjc.A.dylib');
        $r = [$o->objc_msgSend($o->cast('void *', $hc), $o->sel_registerName('parentWindow')) === $hp];
        vio_destroy_window($vc);
        $os = json_encode($r);
    }
    vio_destroy_window($vp);
}
$out[] = "os $os";
$out[] = 'info ' . json_encode($info['window_parent']);
echo $p, ' ', implode(' | ', $out), "\n";
vio_destroy($ctx);
PHP;
$file = tempnam(sys_get_temp_dir(), 'vio272');
file_put_contents($file, $child);
$ffi = '';
foreach ([PHP_EXTENSION_DIR . '/ffi.so', dirname(PHP_BINARY) . '/ext/php_ffi.dll'] as $f) if (is_file($f)) $ffi = '-d extension=' . escapeshellarg($f);
$results = [];
foreach (['win32', 'cocoa', 'x11', 'glfw', 'wayland'] as $platform) {
    if (PHP_OS_FAMILY === 'Linux' && in_array($platform, ['x11', 'glfw'], true) && !getenv('DISPLAY')) continue;
    if ($platform === 'wayland' && !getenv('WAYLAND_DISPLAY')) continue;
    $cmd = [PHP_BINARY, '-n', '-d', 'extension_dir=' . ini_get('extension_dir'), '-d', 'extension=vio'];
    if ($ffi) { $cmd[] = '-d'; $cmd[] = substr($ffi, 3) ; }
    $cmd = array_merge($cmd, ['-d', 'ffi.enable=1', $file]);
    $cmd = array_map(fn($a) => str_starts_with($a, 'extension=\'') ? 'extension=' . trim(substr($a, 10), "'") : $a, $cmd);
    $env = getenv();
    $env['VIO_PLATFORM'] = $platform;
    $proc = proc_open($cmd, [1 => ['pipe', 'w'], 2 => ['pipe', 'w']], $pipes, null, $env);
    $line = trim(stream_get_contents($pipes[1]));
    $errout = trim(stream_get_contents($pipes[2]));
    proc_close($proc);
    if (str_starts_with($line, $platform . ' ')) $results[$platform] = substr($line, strlen($platform) + 1);
    elseif ($platform === vio_platform()) $results[$platform] = "no result: $line $errout";
}
unlink($file);

$wantOs = ['win32' => '[true,0,1]', 'x11' => '[true,true]', 'cocoa' => '[true]'];
$bad = [];
foreach ($results as $platform => $line) {
    if ($line === 'n/a') continue;   // no GPU backend with windows here
    $parts = explode(' | ', $line);
    $native = $platform !== 'glfw';
    $ok = $parts[0] === 'contract ValueError TypeError ValueError'
        && $parts[1] === 'blocked [false,false,"",false,false,0,false,true]'
        && $parts[2] === 'nested [false,true,false]'
        && $parts[3] === 'unblocked true'
        && $parts[4] === 'cascade ValueError ValueError ValueError'
        && $parts[5] === 'os ' . (isset($wantOs[$platform]) && $ffi !== '' || (isset($wantOs[$platform]) && extension_loaded('ffi')) ? $wantOs[$platform] : 'n/a')
        && $parts[6] === 'info ' . json_encode($native);
    if (!$ok) $bad[] = "$platform: $line";
}
echo isset($results[vio_platform()]) && !$bad ? "OK\n" : 'FAIL ' . json_encode([$results, $bad], JSON_PRETTY_PRINT) . "\n";
?>
--EXPECT--
OK

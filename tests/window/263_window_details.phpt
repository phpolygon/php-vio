--TEST--
Window details (DESKTOP-APP-PLAN Phase 1): cursor shapes and images, window icon, size limits (vio_set_window_size clamps), aspect ratio, floating, opacity; vio_platform_info() says what each platform does, the window really carries it on Win32, X11 and Cocoa
--EXTENSIONS--
vio
--FILE--
<?php
/* Every platform runs in its own process (VIO_PLATFORM). The calls return what
 * vio_platform_info() promises; on Win32, X11 and Cocoa the test then asks the
 * window system itself through FFI. */
$child = <<<'PHP'
<?php
$ctx = @vio_create('auto', ['width' => 64, 'height' => 48, 'headless' => true]) ?: vio_create('null', ['width' => 64, 'height' => 48]);
$info = vio_platform_info();
$p = vio_platform();
$keys = ['name', 'wait_events', 'cursor_shapes', 'custom_cursor', 'window_icon', 'size_limits', 'aspect_ratio', 'floating', 'opacity', 'open_url', 'reveal_file', 'file_drop', 'file_dialog', 'message_box', 'menu_bar', 'popup_menu', 'notifications', 'progress', 'tray'];
$out = [];
$out[] = 'info ' . json_encode(array_keys($info) === $keys && $info['name'] === $p);
$caps = implode('', array_map(fn($k) => $info[$k] ? '1' : '0', array_slice($keys, 1)));
$out[] = "caps $caps";

$err = function (callable $f) { try { $f(); return 'none'; } catch (ValueError $e) { return 'ValueError'; } };

$shapes = [VIO_CURSOR_SHAPE_ARROW, VIO_CURSOR_SHAPE_IBEAM, VIO_CURSOR_SHAPE_CROSSHAIR, VIO_CURSOR_SHAPE_HAND,
           VIO_CURSOR_SHAPE_RESIZE_EW, VIO_CURSOR_SHAPE_RESIZE_NS, VIO_CURSOR_SHAPE_RESIZE_NWSE,
           VIO_CURSOR_SHAPE_RESIZE_NESW, VIO_CURSOR_SHAPE_RESIZE_ALL, VIO_CURSOR_SHAPE_NOT_ALLOWED];
$r = array_unique(array_map(fn($s) => vio_set_cursor($ctx, $s), $shapes));
$out[] = 'shapes ' . json_encode($r === [$info['cursor_shapes']]) . ' ' . $err(fn() => vio_set_cursor($ctx, 99));

$img = str_repeat("\xff\x00\x00\xff", 16 * 16);
$out[] = 'cursor image ' . json_encode(vio_set_cursor_image($ctx, $img, 16, 16, 8, 8) === $info['custom_cursor'])
       . ' ' . $err(fn() => vio_set_cursor_image($ctx, 'abc', 16, 16)) . ' ' . $err(fn() => vio_set_cursor_image($ctx, $img, 16, 16, 16, 0));
vio_set_cursor($ctx, VIO_CURSOR_SHAPE_IBEAM);

$icons = [['data' => str_repeat("\x00\x80\xff\xff", 16 * 16), 'width' => 16, 'height' => 16],
          ['data' => str_repeat("\x00\x80\xff\xff", 32 * 32), 'width' => 32, 'height' => 32]];
$out[] = 'icon ' . json_encode(vio_set_window_icon($ctx, $icons) === $info['window_icon'])
       . ' ' . $err(fn() => vio_set_window_icon($ctx, [['data' => 'x', 'width' => 16, 'height' => 16]]))
       . ' ' . $err(fn() => vio_set_window_icon($ctx, ['nope']));

/* limits: the platform gets them where it can, vio_set_window_size clamps everywhere */
$l = vio_set_window_size_limits($ctx, 40, 30, 100, 80);
vio_set_window_size($ctx, 200, 200); $big = vio_window_size($ctx);
vio_set_window_size($ctx, 20, 10); $small = vio_window_size($ctx);
vio_set_window_size($ctx, 60, 50); $mid = vio_window_size($ctx);
$out[] = 'limits ' . json_encode([$l === $info['size_limits'], $big, $small, $mid])
       . ' ' . $err(fn() => vio_set_window_size_limits($ctx, 50, 50, 40, 40));
$out[] = 'aspect ' . json_encode(vio_set_window_aspect_ratio($ctx, 16, 9) === $info['aspect_ratio'])
       . ' ' . $err(fn() => vio_set_window_aspect_ratio($ctx, 16, 0));
$out[] = 'floating ' . json_encode(vio_set_window_floating($ctx, true) === $info['floating']);
$out[] = 'opacity ' . json_encode(vio_set_window_opacity($ctx, 0.5) === $info['opacity']) . ' ' . $err(fn() => vio_set_window_opacity($ctx, 1.5));

/* the window system's own view */
$os = 'n/a';
$win = vio_native_window_handle($ctx);
if (extension_loaded('ffi') && $win) {
    if ($p === 'win32') {
        $u = FFI::cdef(<<<'C'
typedef struct { long x, y; } POINT;
typedef struct { POINT reserved, maxSize, maxPosition, minTrack, maxTrack; } MINMAXINFO;
intptr_t SendMessageW(intptr_t hwnd, unsigned int msg, uintptr_t wparam, intptr_t lparam);
intptr_t GetCursor(void);
intptr_t LoadCursorW(intptr_t inst, intptr_t name);
intptr_t GetWindowLongPtrW(intptr_t hwnd, int index);
int GetLayeredWindowAttributes(intptr_t hwnd, unsigned int *key, unsigned char *alpha, unsigned int *flags);
C, 'user32.dll');
        $u->SendMessageW($win, 0x0020 /* WM_SETCURSOR */, $win, 1 /* HTCLIENT */ | (0x0200 << 16));
        $ibeam = $u->GetCursor() === $u->LoadCursorW(0, 32513);
        $mmi = $u->new('MINMAXINFO');
        $u->SendMessageW($win, 0x0024 /* WM_GETMINMAXINFO */, 0, $u->cast('intptr_t', FFI::addr($mmi))->cdata);
        $alpha = $u->new('unsigned char');
        $u->GetLayeredWindowAttributes($win, null, FFI::addr($alpha), null);
        $os = json_encode([
            'ibeam' => $ibeam,
            'icon' => $u->SendMessageW($win, 0x007F /* WM_GETICON */, 1, 0) !== 0,
            'min' => [$mmi->minTrack->x, $mmi->minTrack->y], 'max' => [$mmi->maxTrack->x, $mmi->maxTrack->y],
            'topmost' => ($u->GetWindowLongPtrW($win, -20) & 0x8) !== 0,
            'alpha' => $alpha->cdata,
        ]);
    } elseif ($p === 'x11') {
        $x = FFI::cdef(<<<'C'
typedef unsigned long XID;
typedef unsigned long Atom;
typedef struct _XDisplay Display;
typedef struct { long flags; int x, y, width, height, min_width, min_height, max_width, max_height, width_inc, height_inc;
                 struct { int x, y; } min_aspect, max_aspect; int base_width, base_height, win_gravity; } XSizeHints;
Display *XOpenDisplay(const char *name);
int XCloseDisplay(Display *d);
Atom XInternAtom(Display *d, const char *name, int only_if_exists);
int XGetWMNormalHints(Display *d, XID w, XSizeHints *hints, long *supplied);
int XGetWindowProperty(Display *d, XID w, Atom prop, long offset, long length, int del, Atom req_type,
                       Atom *actual_type, int *actual_format, unsigned long *nitems, unsigned long *bytes_after, unsigned char **data);
int XFree(void *data);
C, 'libX11.so.6');
        $dpy = $x->XOpenDisplay(null);
        $prop = function (string $name) use ($x, $dpy, $win) {
            $t = $x->new('Atom'); $f = $x->new('int'); $n = $x->new('unsigned long'); $a = $x->new('unsigned long');
            $d = $x->new('unsigned char *');
            $x->XGetWindowProperty($dpy, $win, $x->XInternAtom($dpy, $name, 0), 0, 1 << 20, 0, 0, FFI::addr($t), FFI::addr($f), FFI::addr($n), FFI::addr($a), FFI::addr($d));
            $v = [];
            if (!FFI::isNull($d)) { $l = $x->cast('unsigned long *', $d); for ($i = 0; $i < min($n->cdata, 3); $i++) $v[] = $l[$i]; $v[] = $n->cdata; $x->XFree($d); }
            return $v;
        };
        $h = $x->new('XSizeHints'); $sup = $x->new('long');
        $x->XGetWMNormalHints($dpy, $win, FFI::addr($h), FFI::addr($sup));
        $icon = $prop('_NET_WM_ICON');
        $opacity = $prop('_NET_WM_WINDOW_OPACITY');
        $os = json_encode([
            'min' => ($h->flags & 16) ? [$h->min_width, $h->min_height] : null,
            'max' => ($h->flags & 32) ? [$h->max_width, $h->max_height] : null,
            'aspect' => ($h->flags & 128) ? [$h->min_aspect->x, $h->min_aspect->y] : null,
            'icon' => $icon ? [$icon[0], $icon[1], end($icon)] : null,   // first image 16x16, 2 + 256 + 2 + 1024 longs
            'opacity' => $opacity ? round($opacity[0] / 0xFFFFFFFF, 2) : null,
        ]);
        $x->XCloseDisplay($dpy);
    } elseif ($p === 'cocoa') {
        $o = FFI::cdef(<<<'C'
typedef struct { double width, height; } NSSize;
void *sel_registerName(const char *name);
long objc_msgSend(void *obj, void *sel);
C, '/usr/lib/libobjc.A.dylib');
        $od = FFI::cdef('double objc_msgSend(void *obj, void *sel);', '/usr/lib/libobjc.A.dylib');
        $os2 = FFI::cdef('typedef struct { double width, height; } NSSize; NSSize objc_msgSend(void *obj, void *sel);', '/usr/lib/libobjc.A.dylib');
        $w = $o->cast('void *', $win);
        $min = $os2->objc_msgSend($w, $o->sel_registerName('contentMinSize'));
        $max = $os2->objc_msgSend($w, $o->sel_registerName('contentMaxSize'));
        $asp = $os2->objc_msgSend($w, $o->sel_registerName('contentAspectRatio'));
        $os = json_encode([
            'min' => [$min->width, $min->height], 'max' => [$max->width, $max->height], 'aspect' => [$asp->width, $asp->height],
            'level' => $o->objc_msgSend($w, $o->sel_registerName('level')),
            'alpha' => $od->objc_msgSend($w, $o->sel_registerName('alphaValue')),
        ]);
    }
}
$out[] = "os $os";

/* off again: limits gone, floating off, opacity 1 */
vio_set_window_size_limits($ctx, -1, -1, -1, -1);
vio_set_window_size($ctx, 200, 150);
$out[] = 'reset ' . json_encode([vio_window_size($ctx), vio_set_window_floating($ctx, false) === $info['floating'],
                                 vio_set_window_opacity($ctx, 1.0) === $info['opacity'],
                                 vio_set_window_icon($ctx, []) === $info['window_icon'],
                                 vio_set_cursor($ctx, VIO_CURSOR_SHAPE_ARROW) === $info['cursor_shapes']]);
echo $p, ' ', implode(' | ', $out), "\n";
vio_destroy($ctx);
PHP;
$file = tempnam(sys_get_temp_dir(), 'vio263');
file_put_contents($file, $child);
// the child runs with -n: load FFI by path when it is a shared extension
$ffi = '';
foreach ([PHP_EXTENSION_DIR . '/ffi.so', dirname(PHP_BINARY) . '/ext/php_ffi.dll'] as $f) if (is_file($f)) $ffi = '-d extension=' . escapeshellarg($f);
$results = [];
foreach (['win32', 'cocoa', 'x11', 'glfw', 'wayland', 'null'] as $platform) {
    if (PHP_OS_FAMILY === 'Linux' && in_array($platform, ['x11', 'glfw'], true) && !getenv('DISPLAY')) continue;
    if ($platform === 'wayland' && !getenv('WAYLAND_DISPLAY')) continue;   // no compositor, no window
    $cmd = escapeshellarg(PHP_BINARY) . ' -n -d extension_dir=' . escapeshellarg(ini_get('extension_dir'))
         . ' -d extension=vio ' . $ffi . ' -d ffi.enable=1 ' . escapeshellarg($file);
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

/* caps: wait_events cursor_shapes custom_cursor window_icon size_limits aspect_ratio floating opacity open_url reveal_file file_drop file_dialog message_box menu_bar popup_menu notifications progress tray;
 * file_dialog, notifications and progress depend on the session on Linux (D-Bus services), notifications on macOS on an app bundle */
$wantCaps = [
    'null'    => '/^100000000000000000$/',
    'win32'   => '/^111111111111111111$/',
    'cocoa'   => '/^111111111111111[01]11$/',
    'x11'     => '/^11111111111[01]000[01][01][01]$/',
    'wayland' => '/^11101000111[01]000[01][01][01]$/',
    'glfw'    => '/^11[01]{8}1[01]000[01][01][01]$/',   // icon depends on what GLFW runs on (none on macOS and Wayland)
];
$wantOs = [
    'win32' => '{"ibeam":true,"icon":true,"min":[40,30],"max":[100,80],"topmost":true,"alpha":128}',
    'x11'   => '{"min":[40,30],"max":[100,80],"aspect":[16,9],"icon":[16,16,1284],"opacity":0.5}',
    'cocoa' => '{"min":[40,30],"max":[100,80],"aspect":[16,9],"level":3,"alpha":0.5}',
];
$bad = [];
foreach ($results as $platform => $line) {
    $parts = explode(' | ', $line);
    $caps = substr($parts[1] ?? '', 5);
    $ok = ($parts[0] ?? '') === 'info true'
        && preg_match($wantCaps[$platform], $caps)
        && array_slice($parts, 2, 8) === ['shapes true ValueError', 'cursor image true ValueError ValueError',
                                          'icon true ValueError ValueError', 'limits [true,[100,80],[40,30],[60,50]] ValueError',
                                          'aspect true ValueError', 'floating true', 'opacity true ValueError',
                                          'os ' . (isset($wantOs[$platform]) && extension_loaded('ffi') || (isset($wantOs[$platform]) && $ffi) ? $wantOs[$platform] : 'n/a')]
        && ($parts[10] ?? '') === 'reset [[200,150],true,true,true,true]';
    if (!$ok) $bad[] = "$platform: $line";
}
/* the null platform and the active one must answer: a crashed child has no line */
echo isset($results['null'], $results[vio_platform()]) && !$bad ? "OK\n" : "FAIL " . json_encode([$results, $bad], JSON_PRETTY_PRINT) . "\n";
?>
--EXPECT--
OK

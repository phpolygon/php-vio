--TEST--
X11 platform (NATIVE-PLATFORM-PLAN Phase 3, OPEN-ITEMS A3): OpenGL (GLX) and Vulkan open on vio's own window, X events reach the input API with GLFW's semantics, monitors and video modes come from XRandR
--EXTENSIONS--
vio
--SKIPIF--
<?php
if (PHP_OS_FAMILY !== 'Linux') die('skip X11 platform');
if (vio_platform() !== 'x11') die('skip active platform is ' . vio_platform());
if (!getenv('DISPLAY')) die('skip no DISPLAY');
?>
--FILE--
<?php
/* The input half sends real X events from a second display connection
 * (XSendEvent through FFI, in a child process that loads FFI from PHP's own
 * extension directory - the runner points extension_dir at vio's build) and
 * checks what the platform makes of them: keys by XKB key name with
 * auto-repeat, text through the input method, mouse buttons, both wheels
 * (buttons 4-7). Without FFI that half reports a skip. */
$child = <<<'PHP'
<?php
echo "platform ", vio_platform(), "\n";

foreach (['opengl', 'vulkan'] as $b) {
    $ctx = @vio_create($b, ['width' => 48, 'height' => 32, 'headless' => true, 'vsync' => false]);
    if (!$ctx || vio_backend_name($ctx) !== $b) { echo "$b: skip\n"; continue; }
    vio_begin($ctx);
    vio_clear($ctx, 1.0, 0.0, 0.0, 1.0);
    vio_end($ctx);
    $px = vio_read_pixels($ctx);
    $ok = vio_framebuffer_size($ctx) === [48, 32] && vio_window_size($ctx) === [48, 32]
        && strlen($px) === 48 * 32 * 4 && ord($px[0]) > 240 && ord($px[1]) < 15;
    if ($b === 'opengl') {
        $gl = vio_gl_info($ctx);
        /* GLX: the version ladder lands on a core context from 3.3 up */
        $ok = $ok && $gl && version_compare(preg_replace('/^(\d+\.\d+).*/', '$1', $gl['version']), '3.3', '>=') && $gl['profile'] === 'core';
    }
    echo "$b: ", $ok ? 'OK' : 'FAIL ' . json_encode([vio_framebuffer_size($ctx), vio_window_size($ctx), bin2hex(substr($px, 0, 4)), $b === 'opengl' ? vio_gl_info($ctx) : null]), "\n";
    vio_destroy($ctx);
}

$ctx = vio_create('auto', ['width' => 64, 'height' => 64, 'headless' => true]);
$win = vio_native_window_handle($ctx);
echo "window: ", json_encode($win > 0), "\n";

/* monitors: primary first, sane geometry; video modes ascending, current mode listed */
$mons = vio_monitors($ctx);
$m0 = $mons[0] ?? null;
$modes = vio_video_modes($ctx, 0);
$area = array_map(fn($m) => $m['width'] * $m['height'], $modes);
$sorted = $area; sort($sorted);
$hasCurrent = (bool)array_filter($modes, fn($m) => $m0 && $m['width'] === $m0['width'] && $m['height'] === $m0['height']);
echo "monitors: ", json_encode($m0 && $m0['primary'] && $m0['width'] > 0 && $m0['height'] > 0 && $m0['name'] !== ''
     && $m0['scale_x'] >= 1.0 && $m0['work_width'] > 0 && $m0['work_width'] <= $m0['width']),
     " modes: ", json_encode($modes && $area === $sorted && $hasCurrent), "\n";

if (!extension_loaded('ffi')) {
    echo "input: skip (no FFI)\n";
    vio_destroy($ctx);
    exit;
}
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
$send = function (int $type, int $mask, int $code, int $px = 0, int $py = 0) use ($x, $dpy, $win) {
    $e = $x->new('XEvent');
    $e->ev->type = $type;
    $e->ev->send_event = 1;
    $e->ev->display = $dpy;
    $e->ev->window = $win;
    $e->ev->root = $x->XDefaultRootWindow($dpy);
    $e->ev->x = $px; $e->ev->y = $py;
    $e->ev->code = $code;
    $e->ev->same_screen = 1;
    $x->XSendEvent($dpy, $win, 0, $mask, FFI::addr($e));
};
const KeyPress = 2, KeyRelease = 3, ButtonPress = 4, ButtonRelease = 5, MotionNotify = 6;
const KeyPressMask = 1, KeyReleaseMask = 2, ButtonPressMask = 4, ButtonReleaseMask = 8, PointerMotionMask = 64;

$log = [];
vio_on_key($ctx, function ($key, $action, $mods) use (&$log) { $log[] = "$key/$action"; });
$poll = function () use ($ctx, $x, $dpy, &$log) { $x->XFlush($dpy); usleep(100000); $log = []; vio_poll_events($ctx); return implode(' ', $log); };

/* key A by its keycode: press, auto-repeat (a second press while held), release; the presses type 'a' */
$kc = $x->XKeysymToKeycode($dpy, 0x61);
vio_begin($ctx); vio_end($ctx);
$send(KeyPress, KeyPressMask, $kc);
$send(KeyPress, KeyPressMask, $kc);
echo "keys: ", $poll(), " A=", json_encode(vio_key_pressed($ctx, VIO_KEY_A)), " text=", bin2hex(vio_chars_typed($ctx)), "\n";
$send(KeyRelease, KeyReleaseMask, $kc);
echo "keys up: ", $poll(), "\n";

/* mouse: position, left and right buttons, wheel up (4) and right (6) */
vio_begin($ctx); vio_end($ctx);
$send(MotionNotify, PointerMotionMask, 0, 21, 13);
$send(ButtonPress, ButtonPressMask, 1, 21, 13);
$send(ButtonPress, ButtonPressMask, 3, 21, 13);
$send(ButtonPress, ButtonPressMask, 4, 21, 13);
$send(ButtonPress, ButtonPressMask, 6, 21, 13);
$poll();
echo "mouse: pos=", json_encode(vio_mouse_position($ctx)), " L=", json_encode(vio_mouse_button($ctx, VIO_MOUSE_LEFT)),
     " R=", json_encode(vio_mouse_button($ctx, VIO_MOUSE_RIGHT)), " scroll=", json_encode(vio_mouse_scroll($ctx)), "\n";
$send(ButtonRelease, ButtonReleaseMask, 1, 21, 13);
$poll();
echo "mouse up: L=", json_encode(vio_mouse_button($ctx, VIO_MOUSE_LEFT)), "\n";
$x->XCloseDisplay($dpy);
vio_destroy($ctx);
PHP;

$file = tempnam(sys_get_temp_dir(), 'vio214');
file_put_contents($file, $child);
$ffiSo = PHP_EXTENSION_DIR . '/ffi.so';
$ffi = extension_loaded('ffi') ? '' : (is_file($ffiSo) ? '-d extension=' . escapeshellarg($ffiSo) : '');
$cmd = escapeshellarg(PHP_BINARY) . ' -n -d extension_dir=' . escapeshellarg(ini_get('extension_dir'))
     . ' -d extension=vio ' . $ffi . ' -d ffi.enable=1 ' . escapeshellarg($file) . ' 2>&1';
passthru($cmd);
unlink($file);
?>
--EXPECTF--
platform x11
opengl: %r(OK|skip)%r
vulkan: %r(OK|skip)%r
window: true
monitors: true modes: true
%r(input: skip \(no FFI\)|keys: 65/1 65/2 A=true text=6161
keys up: 65/0
mouse: pos=\[21,13\] L=true R=true scroll=\[1,1\]
mouse up: L=false)%r

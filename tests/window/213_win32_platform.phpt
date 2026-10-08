--TEST--
Win32 platform (WIN32-PLATFORM-PLAN Phase 1, OPEN-ITEMS A2): every backend opens on vio's own window, OS messages reach the input API with GLFW's semantics, monitors and video modes come from the display APIs
--EXTENSIONS--
vio
--SKIPIF--
<?php
if (PHP_OS_FAMILY !== 'Windows') die('skip Win32 platform');
if (vio_platform() !== 'win32') die('skip active platform is ' . vio_platform());
if (!extension_loaded('ffi') && !is_file(dirname(PHP_BINARY) . '/ext/php_ffi.dll')) die('skip FFI not available');
?>
--FILE--
<?php
/* The input half sends real window messages (PostMessageW through FFI, in a
 * child process that loads FFI by absolute path - see test 134) and checks
 * what the WndProc makes of them: keys by scancode with the extended bit
 * (right Control), text as UTF-16 surrogate pairs, mouse buttons including
 * the X buttons, both wheels (GLFW reports a right tilt as negative x). */
$child = <<<'PHP'
<?php
echo "platform ", vio_platform(), "\n";

/* every backend on the native window: exact headless size, a clear reads back */
foreach (['opengl', 'd3d11', 'd3d12', 'vulkan'] as $b) {
    $ctx = @vio_create($b, ['width' => 48, 'height' => 32, 'headless' => true, 'vsync' => false]);
    if (!$ctx || vio_backend_name($ctx) !== $b) { echo "$b: skip\n"; continue; }
    vio_begin($ctx);
    vio_clear($ctx, 1.0, 0.0, 0.0, 1.0);
    vio_end($ctx);
    $px = vio_read_pixels($ctx);
    $ok = vio_framebuffer_size($ctx) === [48, 32] && vio_window_size($ctx) === [48, 32]
        && strlen($px) === 48 * 32 * 4 && ord($px[0]) > 240 && ord($px[1]) < 15;
    $extra = '';
    if ($b === 'opengl') {
        $gl = vio_gl_info($ctx);
        /* WGL: the version ladder lands on a core context from 3.3 up */
        $ok = $ok && $gl && version_compare(preg_replace('/^(\d+\.\d+).*/', '$1', $gl['version']), '3.3', '>=') && $gl['profile'] === 'core';
    }
    echo "$b: ", $ok ? 'OK' : 'FAIL ' . json_encode([vio_framebuffer_size($ctx), vio_window_size($ctx), bin2hex(substr($px, 0, 4)), $b === 'opengl' ? vio_gl_info($ctx) : null]), "\n";
    vio_destroy($ctx);
}

$ctx = vio_create('auto', ['width' => 64, 'height' => 64, 'headless' => true]);
$hwnd = vio_native_window_handle($ctx);
$user32 = FFI::cdef('int PostMessageW(intptr_t hwnd, unsigned int msg, uintptr_t wparam, intptr_t lparam);', 'user32.dll');
$post = function (int $msg, int $w, int $l) use ($user32, $hwnd) { $user32->PostMessageW($hwnd, $msg, $w, $l); };
const WM_KEYDOWN = 0x0100, WM_KEYUP = 0x0101, WM_CHAR = 0x0102, WM_MOUSEMOVE = 0x0200,
      WM_LBUTTONDOWN = 0x0201, WM_LBUTTONUP = 0x0202, WM_RBUTTONDOWN = 0x0204, WM_XBUTTONDOWN = 0x020B,
      WM_MOUSEWHEEL = 0x020A, WM_MOUSEHWHEEL = 0x020E;

$log = [];
vio_on_key($ctx, function ($key, $action, $mods) use (&$log) { $log[] = "$key/$action"; });
$poll = function () use ($ctx, &$log) { $log = []; vio_poll_events($ctx); return implode(' ', $log); };

/* keys: A by scancode, then right Control (scancode 0x1D + extended bit), auto-repeat */
$post(WM_KEYDOWN, 0x41, 1 | (0x1E << 16));
$post(WM_KEYDOWN, 0x41, 1 | (0x1E << 16) | (1 << 30));
$post(WM_KEYDOWN, 0x11, 1 | (0x1D << 16) | (1 << 24));
echo "keys: ", $poll(), " A=", json_encode(vio_key_pressed($ctx, VIO_KEY_A)), " RCTRL=", json_encode(vio_key_pressed($ctx, VIO_KEY_RIGHT_CONTROL)), "\n";
$post(WM_KEYUP, 0x41, 1 | (0x1E << 16) | (3 << 30));
$post(WM_KEYUP, 0x11, 1 | (0x1D << 16) | (1 << 24) | (3 << 30));
echo "keys up: ", $poll(), "\n";

/* text: 'h', then U+1F600 as a surrogate pair, a control character is dropped */
vio_begin($ctx); vio_end($ctx);
$post(WM_CHAR, 0x68, 1);
$post(WM_CHAR, 0xD83D, 1);
$post(WM_CHAR, 0xDE00, 1);
$post(WM_CHAR, 0x08, 1);
$poll();
echo "text: ", bin2hex(vio_chars_typed($ctx)), "\n";

/* mouse: position, left / right / X1 buttons, both wheels */
vio_begin($ctx); vio_end($ctx);
$post(WM_MOUSEMOVE, 0, 21 | (13 << 16));
$post(WM_LBUTTONDOWN, 1, 21 | (13 << 16));
$post(WM_RBUTTONDOWN, 2, 21 | (13 << 16));
$post(WM_XBUTTONDOWN, 1 << 16, 21 | (13 << 16));
$post(WM_MOUSEWHEEL, 240 << 16, 0);
$post(WM_MOUSEHWHEEL, 120 << 16, 0);
$poll();
echo "mouse: pos=", json_encode(vio_mouse_position($ctx)), " L=", json_encode(vio_mouse_button($ctx, VIO_MOUSE_LEFT)),
     " R=", json_encode(vio_mouse_button($ctx, VIO_MOUSE_RIGHT)), " X1=", json_encode(vio_mouse_button($ctx, 3)),
     " scroll=", json_encode(vio_mouse_scroll($ctx)), "\n";
$post(WM_LBUTTONUP, 0, 21 | (13 << 16));
$poll();
echo "mouse up: L=", json_encode(vio_mouse_button($ctx, VIO_MOUSE_LEFT)), "\n";

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
echo "gamepads: ", json_encode(is_array(vio_gamepads())), "\n";
vio_destroy($ctx);
PHP;

$file = tempnam(sys_get_temp_dir(), 'vio213');
file_put_contents($file, $child);
$ffi = extension_loaded('ffi') ? '' : '-d extension=' . escapeshellarg(dirname(PHP_BINARY) . '/ext/php_ffi.dll');
$cmd = escapeshellarg(PHP_BINARY) . ' -n -d extension_dir=' . escapeshellarg(ini_get('extension_dir'))
     . ' -d extension=vio ' . $ffi . ' -d ffi.enable=1 ' . escapeshellarg($file) . ' 2>&1';
passthru($cmd);
unlink($file);
?>
--EXPECTF--
platform win32
opengl: %r(OK|skip)%r
d3d11: %r(OK|skip)%r
d3d12: %r(OK|skip)%r
vulkan: %r(OK|skip)%r
keys: 65/1 65/2 345/1 A=true RCTRL=true
keys up: 65/0 345/0
text: 68f09f9880
mouse: pos=[21,13] L=true R=true X1=true scroll=[-1,2]
mouse up: L=false
monitors: true modes: true
gamepads: true

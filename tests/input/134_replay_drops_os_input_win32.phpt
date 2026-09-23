--TEST--
Replay owns the input: real OS window messages are dropped while replaying, delivered before and after (Win32)
--EXTENSIONS--
vio
--SKIPIF--
<?php
if (PHP_OS_FAMILY !== 'Windows') die('skip Win32 window messages');
if (!extension_loaded('ffi') && !is_file(dirname(PHP_BINARY) . '/ext/php_ffi.dll')) die('skip FFI not available');
?>
--FILE--
<?php
/* The events come from the OS, not from vio_inject_*: PostMessageW puts real
 * WM_KEYDOWN / WM_MOUSEMOVE / WM_MOUSEWHEEL messages into the queue of the
 * (hidden) GLFW window, and glfwPollEvents dispatches them to the same GLFW
 * callbacks a keyboard would reach. FFI is needed for PostMessageW; the test
 * runner loads extensions from vio's build directory only, so the check runs in
 * a child process that loads FFI by absolute path. */
$child = <<<'PHP'
<?php
$ctx = vio_create('auto', ['width' => 64, 'height' => 64, 'headless' => true]);
if (!$ctx) { echo "skip: no context\n"; exit; }
$hwnd = vio_native_window_handle($ctx);
if (!$hwnd) { echo "skip: no window handle\n"; exit; }

$user32 = FFI::cdef('int PostMessageW(intptr_t hwnd, unsigned int msg, uintptr_t wparam, intptr_t lparam);', 'user32.dll');
const WM_KEYDOWN = 0x0100, WM_KEYUP = 0x0101, WM_MOUSEMOVE = 0x0200, WM_MOUSEWHEEL = 0x020A;

// Keys by scancode (GLFW maps lParam's scancode, not the virtual-key code)
$keys = ['A' => [0x41, 0x1E], 'C' => [0x43, 0x2E], 'D' => [0x44, 0x20]];
$post = function (int $msg, int $w, int $l) use ($user32, $hwnd) { $user32->PostMessageW($hwnd, $msg, $w, $l); };
$down = fn(string $k) => $post(WM_KEYDOWN, $keys[$k][0], 1 | ($keys[$k][1] << 16));
$up   = fn(string $k) => $post(WM_KEYUP, $keys[$k][0], 1 | ($keys[$k][1] << 16) | (3 << 30));
$move = fn(int $x, int $y) => $post(WM_MOUSEMOVE, 0, $x | ($y << 16));
$wheel = fn() => $post(WM_MOUSEWHEEL, 120 << 16, 0);

$log = [];
vio_on_key($ctx, function ($key, $action) use (&$log) { $log[] = "$key/$action"; });
$poll = function () use ($ctx, &$log) { $log = []; vio_poll_events($ctx); return implode(' ', $log); };

// Before: OS input reaches the game
$down('A'); $move(10, 12); $wheel();
echo "live:     ", $poll(), ' pos=', json_encode(vio_mouse_position($ctx)), ' scroll=', json_encode(vio_mouse_scroll($ctx)), "\n";
$up('A');
echo "live up:  ", $poll(), "\n";
vio_begin($ctx); vio_end($ctx);

// During a replay: only the replayed key arrives; OS key, cursor and wheel are dropped
vio_input_replay($ctx, [
    ['tick' => 1, 'type' => 'key', 'key' => VIO_KEY_B, 'action' => VIO_PRESS],
    ['tick' => 3, 'type' => 'end'],
]);
$down('C'); $move(30, 31); $wheel();
echo "replay:   ", $poll(), ' pos=', json_encode(vio_mouse_position($ctx)), ' scroll=', json_encode(vio_mouse_scroll($ctx)),
     ' C=', json_encode(vio_key_pressed($ctx, VIO_KEY_C)), "\n";
$up('C');
echo "replay up:", $poll(), "\n";

// After stopping: the OS owns the input again
vio_input_replay_stop($ctx);
$down('D');
echo "after:    ", $poll(), "\n";
$up('D');
$poll();
vio_destroy($ctx);
PHP;

$file = tempnam(sys_get_temp_dir(), 'vio134');
file_put_contents($file, $child);
$ffi = extension_loaded('ffi') ? '' : '-d extension=' . escapeshellarg(dirname(PHP_BINARY) . '/ext/php_ffi.dll');
$cmd = escapeshellarg(PHP_BINARY) . ' -n -d extension_dir=' . escapeshellarg(ini_get('extension_dir'))
     . ' -d extension=vio ' . $ffi . ' -d ffi.enable=1 ' . escapeshellarg($file) . ' 2>&1';
passthru($cmd);
unlink($file);
?>
--EXPECT--
live:     65/1 pos=[10,12] scroll=[0,1]
live up:  65/0
replay:   66/1 pos=[10,12] scroll=[0,0] C=false
replay up:
after:    68/1

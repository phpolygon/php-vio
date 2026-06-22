--TEST--
Window state: vio_set_title, fullscreen/borderless/windowed, cursor_mode, native_window_handle
--EXTENSIONS--
vio
--SKIPIF--
<?php
require __DIR__ . '/skipif_gl.inc';
?>
--FILE--
<?php
$ctx = vio_create("opengl", ["width" => 256, "height" => 256, "headless" => true]);
if (!$ctx) { echo "SKIP\n"; exit; }

// Title — must accept any UTF-8 string and not crash.
vio_set_title($ctx, "Test Title");
vio_set_title($ctx, "");
vio_set_title($ctx, "Ümläut Tëst — 漢字");
echo "set_title OK\n";

// Cursor mode — headless still tracks the value, just no visible cursor.
vio_set_cursor_mode($ctx, VIO_CURSOR_DISABLED);
vio_set_cursor_mode($ctx, VIO_CURSOR_NORMAL);
echo "cursor_mode OK\n";

// Fullscreen / borderless / windowed — state transitions must compose.
// On a headless context these are tracked internally; no actual window
// is moved. The important contract is: any sequence is allowed, no
// transition crashes, idempotent calls are absorbed.
vio_set_fullscreen($ctx);
vio_set_fullscreen($ctx); // idempotent
echo "fullscreen 2× OK\n";

vio_set_borderless($ctx);
vio_set_borderless($ctx); // idempotent
echo "borderless 2× OK\n";

vio_set_windowed($ctx);
vio_set_windowed($ctx); // idempotent
echo "windowed 2× OK\n";

// Round-trip — every pair of opposing transitions.
vio_set_fullscreen($ctx);
vio_set_windowed($ctx);
vio_set_borderless($ctx);
vio_set_windowed($ctx);
vio_set_fullscreen($ctx);
vio_set_borderless($ctx);
echo "transitions OK\n";

// Native window handle — backend-dependent integer (HWND on Windows,
// NSWindow* on macOS, X11 Window on Linux). Must be non-zero for a
// real GLFW window, zero (or absent) for the null backend.
$handle = vio_native_window_handle($ctx);
var_dump(is_int($handle));
var_dump($handle > 0); // headless GLFW context still has a handle

// Null backend has no window — handle must be 0 / not blow up.
$null_ctx = vio_create("null");
$null_handle = vio_native_window_handle($null_ctx);
var_dump(is_int($null_handle));
var_dump($null_handle === 0);

vio_destroy($null_ctx);
vio_destroy($ctx);
echo "OK\n";
?>
--EXPECT--
set_title OK
cursor_mode OK
fullscreen 2× OK
borderless 2× OK
windowed 2× OK
transitions OK
bool(true)
bool(true)
bool(true)
bool(true)
OK

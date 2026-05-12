--TEST--
D3D12: render-state calls outside a frame do not record on a closed command list
--SKIPIF--
<?php
if (PHP_OS_FAMILY !== 'Windows') die('skip Windows only');
if (!in_array('d3d12', vio_backends(), true)) die('skip D3D12 backend not compiled in');
?>
--EXTENSIONS--
vio
--FILE--
<?php
// Regression test for the D3D12 backend's command-list state machine.
// The graphics command list is created closed and only re-Reset() inside
// begin_frame(). Backend functions that record into the list (viewport,
// scissor, clear, draw) must be safe to call when the list is closed —
// the engine layer above triggers this naturally because some callers
// set state BEFORE vio_begin to satisfy D3D11 semantics.
//
// Before the fix this produced D3D12 validation error id=547 "This API
// cannot be called on a closed command list" once per pre-begin call.
// The debug layer also occasionally tripped TDR. After the fix the
// affected calls early-return when in_frame is false.
$ctx = @vio_create('d3d12', ['width' => 64, 'height' => 64, 'headless' => true, 'debug' => 1]);
if (!$ctx instanceof VioContext) {
    echo "SKIP: WARP unavailable\n";
    exit;
}

// Pre-begin state setup — must not warn, must not crash.
vio_viewport($ctx, 0, 0, 64, 64);
echo "viewport before-frame OK\n";

// Run two frames so the command list goes through reset → record → close → reset
// at least once. The original bug was the closed-list error on the SECOND frame's
// pre-begin viewport call.
vio_begin($ctx);
vio_viewport($ctx, 0, 0, 64, 64);
vio_clear($ctx, 0.1, 0.2, 0.3, 1.0);
vio_end($ctx);
echo "frame 1 OK\n";

vio_viewport($ctx, 0, 0, 64, 64);
echo "viewport between-frames OK\n";

vio_begin($ctx);
vio_clear($ctx, 0.3, 0.2, 0.1, 1.0);
vio_end($ctx);
echo "frame 2 OK\n";

vio_destroy($ctx);
echo "OK\n";
?>
--EXPECT--
viewport before-frame OK
frame 1 OK
viewport between-frames OK
frame 2 OK
OK

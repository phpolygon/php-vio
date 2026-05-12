--TEST--
Render-state API calls outside vio_begin/vio_end are safe no-ops
--EXTENSIONS--
vio
--FILE--
<?php
// Some engine code paths set viewport / clear / bind state BEFORE the
// first vio_begin of a frame (e.g. D3D11 needs viewport before begin to
// settle the render-target binding; D3D12 doesn't — but the engine code
// can't easily skip the call per-backend). The D3D12 backend used to
// record into the still-closed command list at those moments and emit
// "API cannot be called on a closed command list" validation errors.
// All backends must treat state-setter calls outside a frame as
// safe no-ops; never warn, never crash.
$ctx = vio_create("null");
var_dump($ctx instanceof VioContext);

// ── Before any frame ────────────────────────────────────────────────
vio_viewport($ctx, 0, 0, 64, 64);
echo "viewport before-frame OK\n";

vio_clear($ctx, 0.5, 0.5, 0.5, 1.0);
echo "clear before-frame OK\n";

// ── Inside a frame — normal path, sanity check ──────────────────────
vio_begin($ctx);
vio_viewport($ctx, 0, 0, 64, 64);
vio_clear($ctx, 0.0, 0.0, 0.0, 1.0);
vio_end($ctx);
echo "frame 1 OK\n";

// ── Between frames ──────────────────────────────────────────────────
vio_viewport($ctx, 0, 0, 32, 32);
echo "viewport between-frames OK\n";

vio_clear($ctx, 1.0, 0.0, 0.0, 1.0);
echo "clear between-frames OK\n";

// ── Second frame — must still work after the inter-frame calls ──────
vio_begin($ctx);
vio_clear($ctx, 0.0, 1.0, 0.0, 1.0);
vio_end($ctx);
echo "frame 2 OK\n";

vio_destroy($ctx);
echo "OK\n";
?>
--EXPECT--
bool(true)
viewport before-frame OK
clear before-frame OK
frame 1 OK
viewport between-frames OK
clear between-frames OK
frame 2 OK
OK

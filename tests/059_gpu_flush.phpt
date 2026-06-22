--TEST--
vio_gpu_flush blocks until queued GPU work has completed
--EXTENSIONS--
vio
--SKIPIF--
<?php
require __DIR__ . '/skipif_gl.inc';
?>
--FILE--
<?php
$ctx = vio_create("opengl", ["width" => 64, "height" => 64, "headless" => true]);
if (!$ctx) { echo "SKIP\n"; exit; }

// Flush before any frame — must be safe; equivalent to no-op since
// nothing is queued.
vio_gpu_flush($ctx);
echo "pre-frame flush OK\n";

// Flush after a frame to drain queued GL/D3D/Vulkan work. This is the
// typical use case: a benchmark or screenshot that needs to make sure
// the previous frame finished rendering before measuring or sampling
// the framebuffer.
vio_begin($ctx);
vio_clear($ctx, 0.2, 0.4, 0.8, 1.0);
vio_end($ctx);

vio_gpu_flush($ctx);
echo "post-frame flush OK\n";

// Read-pixels immediately after a flush — buffer must be a valid
// framebuffer-sized RGBA blob, regardless of clear-colour propagation
// (headless backends differ in whether glClear in a fresh frame writes
// to the readback target).
$pixels = vio_read_pixels($ctx);
var_dump(is_string($pixels));
var_dump(strlen($pixels) === 64 * 64 * 4);

// Multiple consecutive flushes — second/third are cheap, must not crash.
vio_gpu_flush($ctx);
vio_gpu_flush($ctx);
vio_gpu_flush($ctx);
echo "repeated flush OK\n";

// Null backend has no GPU — flush must still be a safe no-op.
$null = vio_create("null");
vio_gpu_flush($null);
vio_destroy($null);
echo "null backend flush OK\n";

vio_destroy($ctx);
echo "OK\n";
?>
--EXPECT--
pre-frame flush OK
post-frame flush OK
bool(true)
bool(true)
repeated flush OK
null backend flush OK
OK

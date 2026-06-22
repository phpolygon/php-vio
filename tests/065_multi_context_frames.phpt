--TEST--
Multi-context: independent frame lifecycles, no shared state crosstalk
--EXTENSIONS--
vio
--SKIPIF--
<?php
require __DIR__ . '/skipif_gl.inc';
?>
--FILE--
<?php
// Two independent contexts must be able to interleave their frame
// boundaries. The engine's `in_frame` flag is per-ctx (not global),
// so beginning a frame on A must not prevent beginning a frame on B,
// and ending B must not end A.
$ctxA = vio_create("opengl", ["width" => 32, "height" => 32, "headless" => true]);
$ctxB = vio_create("opengl", ["width" => 48, "height" => 48, "headless" => true]);
if (!$ctxA || !$ctxB) { echo "SKIP\n"; exit; }

var_dump($ctxA instanceof VioContext);
var_dump($ctxB instanceof VioContext);

// Different sizes → independent state.
$sizeA = vio_framebuffer_size($ctxA);
$sizeB = vio_framebuffer_size($ctxB);
var_dump($sizeA[0] !== $sizeB[0] || $sizeA[1] !== $sizeB[1]);

// ── Interleaved frames: begin A, begin B, end B, end A. ─────────────
vio_begin($ctxA);
vio_clear($ctxA, 1.0, 0.0, 0.0, 1.0);

vio_begin($ctxB);
vio_clear($ctxB, 0.0, 1.0, 0.0, 1.0);
vio_end($ctxB);

// A is still in-frame, B is not. Continue drawing on A.
vio_rect($ctxA, 0.0, 0.0, 10.0, 10.0, ['fill' => 0xFFFFFFFF]);
vio_draw_2d($ctxA);
vio_end($ctxA);
echo "interleaved frames OK\n";

// ── Sequential: B then A then B again. ──────────────────────────────
vio_begin($ctxB);
vio_clear($ctxB, 0.5, 0.5, 0.5, 1.0);
vio_end($ctxB);

vio_begin($ctxA);
vio_clear($ctxA, 0.2, 0.2, 0.2, 1.0);
vio_end($ctxA);

vio_begin($ctxB);
vio_end($ctxB);
echo "sequential alternation OK\n";

// ── Reading pixels from one ctx must not be affected by the other's
// state. Each readback comes from its own framebuffer. ──────────────
$pxA = vio_read_pixels($ctxA);
$pxB = vio_read_pixels($ctxB);
var_dump(strlen($pxA) === 32 * 32 * 4);
var_dump(strlen($pxB) === 48 * 48 * 4);

// ── Destroying one ctx while the other is in-frame must not crash.
// (Engine code can hold both ctxs and the editor side may dispose the
// inspector ctx while the game ctx is mid-render.) ──────────────────
vio_begin($ctxA);
vio_destroy($ctxB);  // B done; A still alive and in-frame
vio_clear($ctxA, 0.0, 0.0, 1.0, 1.0);
vio_end($ctxA);
echo "destroy-B during-A OK\n";

vio_destroy($ctxA);
echo "OK\n";
?>
--EXPECT--
bool(true)
bool(true)
bool(true)
interleaved frames OK
sequential alternation OK
bool(true)
bool(true)
destroy-B during-A OK
OK

--TEST--
vio_render_target with MSAA samples > 1: probe + fallback on unsupported backends
--EXTENSIONS--
vio
--FILE--
<?php
$ctx = vio_create("opengl", ["width" => 64, "height" => 64, "headless" => true]);
if (!$ctx) { echo "SKIP\n"; exit; }

// ── No samples key (single-sample baseline) ─────────────────────────
$rt0 = vio_render_target($ctx, ['width' => 128, 'height' => 128]);
var_dump($rt0 instanceof VioRenderTarget);

// ── samples = 1 (explicit single-sample) ────────────────────────────
$rt1 = vio_render_target($ctx, ['width' => 128, 'height' => 128, 'samples' => 1]);
var_dump($rt1 instanceof VioRenderTarget);

// ── samples = 2 / 4 / 8 — backends may or may not support each tier.
// Contract: if the requested count is rejected, the call must either
// silently fall back to single-sample (returning a valid RT) or return
// false. Never crash, never produce an unbindable handle.
foreach ([2, 4, 8] as $samples) {
    $rt = @vio_render_target($ctx, [
        'width'   => 128,
        'height'  => 128,
        'samples' => $samples,
    ]);
    // Either a valid RT (with possibly clamped samples) or false.
    var_dump($rt instanceof VioRenderTarget || $rt === false);
}

// ── Insanely high samples — must be rejected (or clamped). ──────────
$rt_crazy = @vio_render_target($ctx, [
    'width'   => 128,
    'height'  => 128,
    'samples' => 256,
]);
var_dump($rt_crazy instanceof VioRenderTarget || $rt_crazy === false);

// ── Depth-only RT cannot meaningfully MSAA-resolve but the create
// call should still accept the request. ──────────────────────────────
$rt_depth = @vio_render_target($ctx, [
    'width'      => 128,
    'height'     => 128,
    'depth_only' => true,
    'samples'    => 4,
]);
var_dump($rt_depth instanceof VioRenderTarget || $rt_depth === false);

// ── Bind + render to the MSAA RT and back. Must not crash even if
// the implementation silently fell back to single-sample. ───────────
vio_begin($ctx);
$any_msaa = false;
foreach ([2, 4] as $samples) {
    $rt = @vio_render_target($ctx, [
        'width'   => 64,
        'height'  => 64,
        'samples' => $samples,
    ]);
    if ($rt instanceof VioRenderTarget) {
        vio_bind_render_target($ctx, $rt);
        vio_clear($ctx, 0.0, 1.0, 0.0, 1.0);
        vio_unbind_render_target($ctx);
        $any_msaa = true;
    }
}
vio_end($ctx);
var_dump($any_msaa); // at least one MSAA tier should have been accepted

vio_destroy($ctx);
echo "OK\n";
?>
--EXPECT--
bool(true)
bool(true)
bool(true)
bool(true)
bool(true)
bool(true)
bool(true)
bool(true)
OK

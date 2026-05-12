--TEST--
vio_bind_render_target: alternating binds, idempotence, resource-state tracking
--EXTENSIONS--
vio
--FILE--
<?php
$ctx = vio_create("opengl", ["width" => 64, "height" => 64, "headless" => true]);
if (!$ctx) { echo "SKIP\n"; exit; }

$rt_a = vio_render_target($ctx, ['width' => 32, 'height' => 32]);
$rt_b = vio_render_target($ctx, ['width' => 64, 'height' => 64]);
$rt_depth = vio_render_target($ctx, [
    'width'      => 256,
    'height'     => 256,
    'depth_only' => true,
]);
var_dump($rt_a instanceof VioRenderTarget);
var_dump($rt_b instanceof VioRenderTarget);
var_dump($rt_depth instanceof VioRenderTarget);

vio_begin($ctx);

// ── Bind A → render → unbind. Standard cycle. ────────────────────────
vio_bind_render_target($ctx, $rt_a);
vio_clear($ctx, 1.0, 0.0, 0.0, 1.0);
vio_unbind_render_target($ctx);
echo "bind A → unbind OK\n";

// ── Bind B without unbinding A first. Some backends require unbind
// between binds (D3D11 resource hazard), others swap implicitly. The
// contract is: this must not leave the backend in a broken state. ────
vio_bind_render_target($ctx, $rt_a);
vio_bind_render_target($ctx, $rt_b);
vio_clear($ctx, 0.0, 1.0, 0.0, 1.0);
vio_unbind_render_target($ctx);
echo "bind A → bind B (no unbind) OK\n";

// ── Idempotent bind: same RT bound twice in a row. ──────────────────
vio_bind_render_target($ctx, $rt_a);
vio_bind_render_target($ctx, $rt_a);
vio_clear($ctx, 0.0, 0.0, 1.0, 1.0);
vio_unbind_render_target($ctx);
echo "bind A 2× OK\n";

// ── Depth-only RT bind (shadow-map use case). ───────────────────────
vio_bind_render_target($ctx, $rt_depth);
vio_clear($ctx, 0.0, 0.0, 0.0, 1.0); // depth-only ignores colour
vio_unbind_render_target($ctx);
echo "depth RT bind OK\n";

// ── Critical state-tracking scenario: render to RT, sample its texture,
// then re-bind the SAME RT as render target. D3D11/D3D12 must transition
// the resource state SRV → RTV correctly. Pre-fix, the wrapper SRV from
// the second texture() call was invalid and the next bind crashed. ───
vio_bind_render_target($ctx, $rt_b);
vio_clear($ctx, 1.0, 1.0, 0.0, 1.0);
vio_unbind_render_target($ctx);

$tex1 = vio_render_target_texture($rt_b);
var_dump($tex1 instanceof VioTexture);

vio_bind_texture($ctx, $tex1, 0);

// Re-bind rt_b as render target (resource transitions SRV → RT).
vio_bind_render_target($ctx, $rt_b);
vio_clear($ctx, 0.0, 1.0, 1.0, 1.0);

// Query texture again — must return a stable cached wrapper.
$tex2 = vio_render_target_texture($rt_b);
var_dump($tex2 instanceof VioTexture);

vio_unbind_render_target($ctx);
echo "RT-as-RT-then-SRV-then-RT OK\n";

// ── Unbind without preceding bind: safe no-op. ──────────────────────
vio_unbind_render_target($ctx);
vio_unbind_render_target($ctx);
echo "unbind without bind OK\n";

vio_end($ctx);

vio_destroy($ctx);
echo "OK\n";
?>
--EXPECT--
bool(true)
bool(true)
bool(true)
bind A → unbind OK
bind A → bind B (no unbind) OK
bind A 2× OK
depth RT bind OK
bool(true)
bool(true)
RT-as-RT-then-SRV-then-RT OK
unbind without bind OK
OK

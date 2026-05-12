--TEST--
2D transform and scissor stacks: push/pop pairing, nesting, draw safety
--EXTENSIONS--
vio
--FILE--
<?php
$ctx = vio_create("opengl", ["width" => 128, "height" => 128, "headless" => true]);
if (!$ctx) { echo "SKIP\n"; exit; }

vio_begin($ctx);

// ── Transform stack: push, draw, pop ───────────────────────────────
// 2D affine: (a, b, c, d, e, f) maps to
//   x' = a*x + c*y + e
//   y' = b*x + d*y + f
// Identity is (1, 0, 0, 1, 0, 0).
vio_push_transform($ctx, 1.0, 0.0, 0.0, 1.0, 10.0, 20.0);  // translate
vio_rect($ctx, 0.0, 0.0, 8.0, 8.0, ['fill' => 0xFFFF0000]);
vio_pop_transform($ctx);
echo "push/pop transform 1x OK\n";

// ── Nested pushes (3 deep) ─────────────────────────────────────────
vio_push_transform($ctx, 1.0, 0.0, 0.0, 1.0, 10.0, 0.0);
vio_push_transform($ctx, 2.0, 0.0, 0.0, 2.0, 0.0, 0.0);   // scale 2x
vio_push_transform($ctx, 1.0, 0.0, 0.0, 1.0, 5.0, 5.0);
vio_rect($ctx, 0.0, 0.0, 4.0, 4.0, ['fill' => 0xFF00FF00]);
vio_pop_transform($ctx);
vio_pop_transform($ctx);
vio_pop_transform($ctx);
echo "nested transform 3x OK\n";

// ── Scissor stack ──────────────────────────────────────────────────
vio_push_scissor($ctx, 0.0, 0.0, 64.0, 64.0);
vio_rect($ctx, 10.0, 10.0, 80.0, 80.0, ['fill' => 0xFF0000FF]); // partially clipped
vio_pop_scissor($ctx);
echo "push/pop scissor 1x OK\n";

// ── Nested scissor (intersection) ──────────────────────────────────
vio_push_scissor($ctx, 0.0, 0.0, 100.0, 100.0);
vio_push_scissor($ctx, 20.0, 20.0, 30.0, 30.0);
vio_rect($ctx, 0.0, 0.0, 128.0, 128.0, ['fill' => 0xFF888888]);
vio_pop_scissor($ctx);
vio_pop_scissor($ctx);
echo "nested scissor 2x OK\n";

// ── Interleaved transform + scissor ────────────────────────────────
vio_push_transform($ctx, 1.5, 0.0, 0.0, 1.5, 0.0, 0.0);
vio_push_scissor($ctx, 10.0, 10.0, 50.0, 50.0);
vio_rect($ctx, 0.0, 0.0, 32.0, 32.0, ['fill' => 0xFFFFFFFF]);
vio_pop_scissor($ctx);
vio_pop_transform($ctx);
echo "interleaved OK\n";

// ── Stress: 32 deep push, then 32 pops ─────────────────────────────
// Catches off-by-one or fixed-size stack overflow.
for ($i = 0; $i < 32; $i++) {
    vio_push_transform($ctx, 1.0, 0.0, 0.0, 1.0, 1.0, 0.0);
}
for ($i = 0; $i < 32; $i++) {
    vio_pop_transform($ctx);
}
echo "deep transform stack OK\n";

for ($i = 0; $i < 32; $i++) {
    vio_push_scissor($ctx, 0.0, 0.0, 128.0 - $i, 128.0 - $i);
}
for ($i = 0; $i < 32; $i++) {
    vio_pop_scissor($ctx);
}
echo "deep scissor stack OK\n";

// ── Imbalanced pop (extra pop with no matching push) ───────────────
// The defensive contract: extra pops are caught and emit a warning,
// never underflow the stack into invalid memory. This is the classic
// "user forgot a push" guard.
@vio_pop_transform($ctx);  // already at base — must emit warning, not crash
@vio_pop_scissor($ctx);    // ditto
echo "extra pop tolerated\n";

// Verify the warnings actually fire (without @-suppression).
set_error_handler(function ($severity, $msg) {
    if (str_contains($msg, 'underflow')) {
        echo "underflow warning fired: $msg\n";
    }
    return true;
});
vio_pop_transform($ctx);
vio_pop_scissor($ctx);
restore_error_handler();

vio_draw_2d($ctx);
vio_end($ctx);

vio_destroy($ctx);
echo "OK\n";
?>
--EXPECTF--
push/pop transform 1x OK
nested transform 3x OK
push/pop scissor 1x OK
nested scissor 2x OK
interleaved OK
deep transform stack OK
deep scissor stack OK
extra pop tolerated
underflow warning fired: %sTransform stack underflow
underflow warning fired: %sScissor stack underflow
OK

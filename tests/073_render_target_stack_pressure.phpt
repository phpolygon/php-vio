--TEST--
vio_push_render_target stack overflow + use-after-destroy hardening (Issue #4)
--SKIPIF--
<?php
if (!extension_loaded('vio')) die('skip vio not loaded');
?>
--FILE--
<?php
/* The push/pop stack is capped at 8 levels (vio_context_object::rt_stack[8]).
 * Pushing past that should warn but neither crash nor lose the bind. Pop on
 * empty should warn and restore default. Use after explicit destroy should
 * surface "not valid" and not blow up. */

$ctx = vio_create("opengl", ["width" => 32, "height" => 32, "headless" => true]);
if (!$ctx) {
    echo "skip: no headless OpenGL\n";
    exit;
}

vio_begin($ctx);

/* Build 10 RTs so we can push past the depth-8 limit. */
$rts = [];
for ($i = 0; $i < 10; $i++) {
    $rts[$i] = vio_create_render_target($ctx, 16, 16);
}
var_dump(count(array_filter($rts, fn($r) => $r instanceof VioRenderTarget)) === 10);

/* Push 8 — should all succeed without warning. */
for ($i = 0; $i < 8; $i++) {
    vio_push_render_target($ctx, $rts[$i]);
}
echo "8 pushes OK\n";

/* Push 9th — overflow, expect warning (replaces top per implementation). */
@vio_push_render_target($ctx, $rts[8]);
echo "9th push OK\n";

/* Push 10th — same overflow path. */
@vio_push_render_target($ctx, $rts[9]);
echo "10th push OK\n";

/* Drain the stack — pops should not warn until empty. */
for ($i = 0; $i < 8; $i++) {
    vio_pop_render_target($ctx);
}
echo "drained OK\n";

/* Now empty — pop emits warning. */
@vio_pop_render_target($ctx);
@vio_pop_render_target($ctx);
echo "empty pops OK\n";

/* Destroy + use-after-destroy — three separate operations should all
 * survive the invalid object gracefully. */
vio_destroy_render_target($rts[0]);
@vio_set_render_target($ctx, $rts[0]);
@vio_push_render_target($ctx, $rts[0]);
@vio_pop_render_target($ctx);  /* removes our errantly pushed dead RT */
echo "use-after-destroy OK\n";

/* Double-destroy — should not crash. */
@vio_destroy_render_target($rts[0]);
echo "double-destroy OK\n";

vio_end($ctx);
vio_destroy($ctx);
echo "DONE\n";
?>
--EXPECT--
bool(true)
8 pushes OK
9th push OK
10th push OK
drained OK
empty pops OK
use-after-destroy OK
double-destroy OK
DONE

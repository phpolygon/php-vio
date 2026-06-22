--TEST--
vio_create_render_target / vio_set_render_target / push/pop (Issue #4)
--SKIPIF--
<?php
require __DIR__ . '/skipif_gl.inc';
if (!extension_loaded('vio')) die('skip vio not loaded');
?>
--FILE--
<?php
/* Issue #4: new render-target API surface.
 *
 *   vio_create_render_target($ctx, $w, $h, $opts): VioRenderTarget|false
 *   vio_set_render_target($ctx, $rt | null): void
 *   vio_destroy_render_target($rt): void
 *   vio_push_render_target($ctx, $rt): void
 *   vio_pop_render_target($ctx): void
 *
 * Smoke test on a headless OpenGL context — exercises the create + bind
 * + push/pop + explicit-destroy paths. */

$ctx = vio_create("opengl", ["width" => 32, "height" => 32, "headless" => true]);
if (!$ctx) {
    echo "skip: no headless OpenGL\n";
    exit;
}

vio_begin($ctx);

/* create_render_target with explicit dimensions */
$rt = vio_create_render_target($ctx, 64, 48);
var_dump($rt instanceof VioRenderTarget);

/* HDR option maps to internal 16F format */
$hdr_rt = vio_create_render_target($ctx, 64, 48, ["format" => "rgba16f"]);
var_dump($hdr_rt instanceof VioRenderTarget);

/* depth=false → depth-only target */
$shadow = vio_create_render_target($ctx, 64, 64, ["depth" => false]);
var_dump($shadow instanceof VioRenderTarget);

/* set_render_target switches and null restores default */
vio_set_render_target($ctx, $rt);
vio_clear($ctx, 0.1, 0.2, 0.3, 1.0);
vio_set_render_target($ctx, null);
echo "set/unset OK\n";

/* push/pop stack */
vio_push_render_target($ctx, $rt);
vio_push_render_target($ctx, $shadow);
vio_pop_render_target($ctx);   /* back to $rt */
vio_pop_render_target($ctx);   /* back to default */
echo "push/pop OK\n";

/* Pop on empty stack should warn but not crash */
@vio_pop_render_target($ctx);
echo "empty pop OK\n";

/* Explicit destroy releases GPU resources. The PHP object survives but
 * subsequent binds emit a warning ("not valid"). */
vio_destroy_render_target($rt);
@vio_set_render_target($ctx, $rt);
echo "destroy + use-after-destroy OK\n";

vio_end($ctx);
vio_destroy($ctx);
echo "DONE\n";
?>
--EXPECT--
bool(true)
bool(true)
bool(true)
set/unset OK
push/pop OK
empty pop OK
destroy + use-after-destroy OK
DONE

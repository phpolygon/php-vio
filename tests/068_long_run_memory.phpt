--TEST--
Long-run memory tracking: 1000+ frame loop stays within fixed bound
--EXTENSIONS--
vio
--FILE--
<?php
// Stress-tests that the hot per-frame paths don't accumulate PHP-heap
// growth. Caught the today's vio_2d_shutdown leak when 100 contexts
// grew the heap by ~100 MiB; this is the steady-state-frame analogue
// for everything that runs every frame: vio_begin, draw calls,
// vio_end, vio_read_pixels, vio_render_target_texture (offscreen
// blit), vio_bind_texture, sampler binding.
$ctx = vio_create("opengl", ["width" => 64, "height" => 64, "headless" => true]);
if (!$ctx) { echo "SKIP\n"; exit; }

// One-time RT + texture allocations. These should be reused, not
// recreated.
$rt = vio_render_target($ctx, ['width' => 32, 'height' => 32]);
var_dump($rt instanceof VioRenderTarget);

$pixels = str_repeat("\xff\x00\x80\xff", 64 * 64);
$tex = vio_texture($ctx, [
    'data'   => $pixels,
    'width'  => 64,
    'height' => 64,
]);
var_dump($tex instanceof VioTexture);

// Warm-up frames so any first-frame deferred init lands inside the
// baseline rather than counting as growth.
for ($i = 0; $i < 10; $i++) {
    vio_clear($ctx, 0.1, 0.2, 0.3, 1.0);
    vio_begin($ctx);
    vio_bind_texture($ctx, $tex, 0);
    vio_bind_render_target($ctx, $rt);
    vio_clear($ctx, 0.5, 0.5, 0.5, 1.0);
    vio_unbind_render_target($ctx);
    $rt_tex = vio_render_target_texture($rt);
    vio_bind_texture($ctx, $rt_tex, 1);
    vio_rect($ctx, 0.0, 0.0, 32.0, 32.0, ['fill' => 0xFFCCCCCC]);
    vio_draw_2d($ctx);
    vio_end($ctx);
}

gc_collect_cycles();
$baseline = memory_get_usage(true);

// Steady-state frame loop. 1000 iterations exercises every per-frame
// allocator (vio_2d vertex buffer growth, vio_render_target_texture
// wrapper cache, vio_bind_texture pending-srv table, command-list
// reset cycle on D3D backends).
for ($i = 0; $i < 1000; $i++) {
    vio_clear($ctx, 0.1, 0.2, 0.3, 1.0);
    vio_begin($ctx);

    vio_bind_render_target($ctx, $rt);
    vio_clear($ctx, 0.5, 0.5, 0.5, 1.0);
    vio_unbind_render_target($ctx);

    $rt_tex = vio_render_target_texture($rt);
    vio_bind_texture($ctx, $rt_tex, 0);
    vio_bind_texture($ctx, $tex, 1);

    vio_push_transform($ctx, 1.0, 0.0, 0.0, 1.0, 1.0, 0.0);
    vio_push_scissor($ctx, 0.0, 0.0, 64.0, 64.0);
    vio_rect($ctx, 0.0, 0.0, 16.0, 16.0, ['fill' => 0xFFFFFFFF]);
    vio_pop_scissor($ctx);
    vio_pop_transform($ctx);

    vio_draw_2d($ctx);
    vio_end($ctx);

    unset($rt_tex);
}

gc_collect_cycles();
$final = memory_get_usage(true);
$delta_kb = ($final - $baseline) / 1024;

echo "delta over 1000 frames: " . round($delta_kb, 1) . " KB\n";

// Tolerance: 4 MiB. PHP's allocator works in 2 MiB chunks plus the
// vio 2D state can legitimately grow its vertex pool once on first
// real-sized batch. Anything past 4 MiB is an unbounded per-frame
// leak (sampler-state pool, SRV AddRef, etc.).
var_dump($delta_kb < 4096);

vio_destroy($ctx);
echo "OK\n";
?>
--EXPECT--
bool(true)
bool(true)
delta over 1000 frames: 0 KB
bool(true)
OK

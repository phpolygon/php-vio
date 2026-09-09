--TEST--
Vulkan backend: 'vsync' => false / true select a present mode and both run frames (IMMEDIATE / MAILBOX / FIFO)
--EXTENSIONS--
vio
--SKIPIF--
<?php
$c = @vio_create("vulkan", ["width" => 8, "height" => 8, "headless" => true, "vsync" => false]);
if (!$c) die("skip Vulkan context unavailable");
vio_destroy($c);
?>
--FILE--
<?php
/* D3D-VULKAN-GAP-PLAN.md 2.7. create_swapchain used to pick MAILBOX over FIFO
 * regardless of 'vsync', so vsync=false was still refresh-capped and vsync=true
 * was not FIFO. The mode is chosen at swapchain creation; the observable
 * contract here is that both settings produce a working context that renders
 * and presents several frames (a present-mode / swapchain-flag mismatch
 * surfaces as a create failure or a validation error on the first present). */
foreach ([false, true] as $vsync) {
    $ctx = vio_create("vulkan", ["width" => 32, "height" => 32, "headless" => true, "vsync" => $vsync]);
    var_dump($ctx instanceof VioContext);
    for ($i = 0; $i < 4; $i++) {
        vio_begin($ctx);
        vio_clear($ctx, 0.2, 0.4, 0.6, 1.0);
        vio_rect($ctx, 4, 4, 8, 8, ['color' => 0xFFFF0000]);
        vio_draw_2d($ctx);
        vio_end($ctx);
        vio_poll_events($ctx);
    }
    vio_destroy($ctx);
    echo "vsync=", $vsync ? "true" : "false", ": OK\n";
}
?>
--EXPECT--
bool(true)
vsync=false: OK
bool(true)
vsync=true: OK

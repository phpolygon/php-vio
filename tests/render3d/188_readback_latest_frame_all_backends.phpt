--TEST--
vio_read_pixels returns the latest presented frame on every backend: after each frame, after several unread frames in flight, after an offscreen-only frame and mid-frame (Vulkan waits the frame's fence instead of draining the device)
--EXTENSIONS--
vio
--FILE--
<?php
/* OPEN-ITEMS-PLAN A36. */
$W = 16;
function center(string $p, int $w): array { $o = (intdiv($w, 2) * $w + intdiv($w, 2)) * 4; return [ord($p[$o]), ord($p[$o + 1]), ord($p[$o + 2])]; }
$colors = [[255, 0, 0], [0, 255, 0], [0, 0, 255], [255, 255, 0], [0, 255, 255], [255, 0, 255]];

foreach (['opengl', 'd3d11', 'd3d12', 'vulkan', 'metal'] as $b) {
    $ctx = @vio_create($b, ['width' => $W, 'height' => $W, 'headless' => true, 'vsync' => false]);
    if ($ctx && vio_backend_name($ctx) !== $b) { vio_destroy($ctx); $ctx = null; }
    if (!$ctx) { echo "$b: skip (unavailable)\n"; continue; }
    $fail = [];
    $frame = function (array $c) use ($ctx) {
        vio_clear($ctx, $c[0] / 255, $c[1] / 255, $c[2] / 255, 1.0);
        vio_begin($ctx);
        vio_end($ctx);
    };
    // Every frame read back.
    foreach ($colors as $i => $c) {
        $frame($c);
        if (($got = center(vio_read_pixels($ctx), $W)) !== $c) $fail[] = "frame $i " . json_encode($got);
    }
    // Several frames in flight, read once: the last one.
    for ($i = 0; $i < 5; $i++) $frame($colors[$i % 6]);
    if (($got = center(vio_read_pixels($ctx), $W)) !== $colors[4]) $fail[] = "in flight " . json_encode($got);
    // Read twice without a frame in between: unchanged.
    if (($got = center(vio_read_pixels($ctx), $W)) !== $colors[4]) $fail[] = "reread " . json_encode($got);
    // An offscreen-only frame does not replace the swapchain content.
    $rt = vio_render_target($ctx, ['width' => 8, 'height' => 8]);
    vio_bind_render_target($ctx, $rt);
    vio_begin($ctx);
    vio_clear($ctx, 1, 1, 1, 1);
    vio_unbind_render_target($ctx);
    vio_end($ctx);
    $frame($colors[1]);
    if (($got = center(vio_read_pixels($ctx), $W)) !== $colors[1]) $fail[] = "after offscreen " . json_encode($got);
    // Mid-frame: the frame so far.
    vio_clear($ctx, 0, 0, 1, 1);
    vio_begin($ctx);
    $mid = @vio_read_pixels($ctx);
    vio_end($ctx);
    if ($mid !== false && $mid !== null && ($got = center($mid, $W)) !== [0, 0, 255]) $fail[] = "mid-frame " . json_encode($got);
    if (($got = center(vio_read_pixels($ctx), $W)) !== [0, 0, 255]) $fail[] = "after mid-frame " . json_encode($got);
    echo "$b: ", $fail ? "FAIL " . implode('; ', $fail) : "OK", "\n";
    vio_destroy($ctx);
}
echo "DONE\n";
?>
--EXPECTF--
opengl: %r(OK|skip \(.*\))%r
d3d11: %r(OK|skip \(.*\))%r
d3d12: %r(OK|skip \(.*\))%r
vulkan: %r(OK|skip \(.*\))%r
metal: %r(OK|skip \(.*\))%r
DONE

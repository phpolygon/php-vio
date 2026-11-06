--TEST--
A render target created inside a frame while another one is bound leaves the bound target bound: the next clear and draw still go into it (OpenGL bound the default framebuffer after creating the new target's FBO, so the clear landed on the screen)
--EXTENSIONS--
vio
--FILE--
<?php
function run_backend(string $name): string
{
    $ctx = @vio_create($name, ['width' => 32, 'height' => 32, 'headless' => true, 'vsync' => false]);
    if ($ctx && vio_backend_name($ctx) !== $name) { vio_destroy($ctx); $ctx = null; }
    if (!$ctx) return 'skip';
    $a = vio_render_target($ctx, ['width' => 32, 'height' => 32]);
    $fail = [];
    foreach ([['width' => 1, 'height' => 1, 'depth_only' => true], ['width' => 8, 'height' => 8]] as $config) {
        vio_begin($ctx);
        vio_bind_render_target($ctx, $a);
        vio_clear($ctx, 1, 0, 0, 1);                       // A red
        $b = vio_render_target($ctx, $config);             // created mid-frame
        vio_clear($ctx, 0, 1, 0, 1);                       // must clear A to green
        vio_unbind_render_target($ctx);
        vio_end($ctx);
        $p = (string) vio_read_render_target($a);
        $o = (16 * 32 + 16) * 4;
        $px = strlen($p) > $o + 2 ? [ord($p[$o]), ord($p[$o + 1]), ord($p[$o + 2])] : [-1, -1, -1];
        if (!($px[1] > 200 && $px[0] < 50)) $fail[] = json_encode($config) . ' -> ' . json_encode($px);
        unset($b);
    }
    vio_destroy($ctx);
    return $fail ? 'FAIL ' . implode('; ', $fail) : 'ok';
}
foreach (['opengl', 'd3d11', 'd3d12', 'vulkan', 'metal'] as $b) echo "$b: ", run_backend($b), "\n";
echo "DONE\n";
?>
--EXPECTF--
opengl: %r(ok|skip)%r
d3d11: %r(ok|skip)%r
d3d12: %r(ok|skip)%r
vulkan: %r(ok|skip)%r
metal: %r(ok|skip)%r
DONE

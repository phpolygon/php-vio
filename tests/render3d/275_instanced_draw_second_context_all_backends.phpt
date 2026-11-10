--TEST--
vio_draw_instanced draws in every context, also after an earlier context drew with more instances and was destroyed: the instance buffer belongs to the device, not to the process (D3D11 kept it in a function-static; a second context drew nothing, a second WARP device was removed)
--EXTENSIONS--
vio
--FILE--
<?php
$VS = "#version 450\nlayout(location = 0) in vec3 aPos;\nlayout(location = 3) in mat4 aModel;\nvoid main() { gl_Position = aModel * vec4(aPos, 1.0); }\n";
$FS = "#version 450\nlayout(location = 0) out vec4 o;\nvoid main() { o = vec4(0.0, 1.0, 0.0, 1.0); }\n";

function one_context(string $name, int $instances): string
{
    global $VS, $FS;
    $ctx = @vio_create($name, ['width' => 32, 'height' => 32, 'headless' => true, 'vsync' => false]);
    if ($ctx && vio_backend_name($ctx) !== $name) { vio_destroy($ctx); $ctx = null; }
    if (!$ctx) return 'skip';
    if (!vio_supports_feature($ctx, VIO_FEATURE_3D_PIPELINE)) { vio_destroy($ctx); return 'skip'; }
    $warn = [];
    set_error_handler(function (int $no, string $msg) use (&$warn) { $warn[] = $msg; return true; });
    $mesh = vio_mesh($ctx, ['vertices' => [-1, -1, 0, 3, -1, 0, -1, 3, 0], 'layout' => [VIO_FLOAT3]]);
    $pipe = vio_pipeline($ctx, ['shader' => vio_shader($ctx, ['vertex' => $VS, 'fragment' => $FS]), 'depth_test' => false, 'cull_mode' => VIO_CULL_NONE]);
    $rt = vio_render_target($ctx, ['width' => 32, 'height' => 32]);
    $m = [];
    for ($i = 0; $i < $instances; $i++) array_push($m, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1);
    vio_begin($ctx);
    vio_bind_render_target($ctx, $rt);
    vio_clear($ctx, 0, 0, 0, 1);
    vio_viewport($ctx, 0, 0, 32, 32);
    vio_bind_pipeline($ctx, $pipe);
    vio_draw_instanced($ctx, $mesh, $m, $instances);
    vio_unbind_render_target($ctx);
    vio_end($ctx);
    $px = vio_read_render_target($rt);
    restore_error_handler();
    unset($pipe, $mesh, $rt);
    vio_destroy($ctx);
    $g = is_string($px) && strlen($px) >= 32 * 32 * 4 ? ord($px[(16 * 32 + 16) * 4 + 1]) : -1;
    if ($warn) return 'FAIL warning: ' . $warn[0];
    return $g > 200 ? 'ok' : "FAIL green $g";
}

foreach (['opengl', 'd3d11', 'd3d12', 'vulkan', 'metal'] as $b) {
    // More instances first, then fewer: a buffer surviving from the first device
    // would be "big enough" and reused by the second.
    $r = [one_context($b, 256), one_context($b, 1), one_context($b, 512)];
    $r = array_unique($r);
    echo "$b: ", $r === ['skip'] ? 'skip' : implode(', ', $r), "\n";
}
echo "DONE\n";
?>
--EXPECTF--
opengl: %r(ok|skip)%r
d3d11: %r(ok|skip)%r
d3d12: %r(ok|skip)%r
vulkan: %r(ok|skip)%r
metal: %r(ok|skip)%r
DONE

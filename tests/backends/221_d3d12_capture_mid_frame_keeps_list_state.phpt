--TEST--
D3D12: a mid-frame capture (vio_save_screenshot / vio_read_pixels inside vio_begin/vio_end) re-arms the frame list - a target switch and draws with the still-bound pipeline work afterwards (the reset list had no root signature and the driver crashed)
--EXTENSIONS--
vio
--SKIPIF--
<?php
if (!in_array('d3d12', vio_backends(), true)) die('skip d3d12 not compiled in');
$c = @vio_create('d3d12', ['width' => 8, 'height' => 8, 'headless' => true]);
if (!$c) die('skip d3d12 unavailable');
vio_destroy($c);
?>
--INI--
vio.debug=1
--FILE--
<?php
/* The capture closes, executes and resets the frame command list. The pipeline
 * bound before it stays bound (no vio_bind_pipeline afterwards): switching to a
 * render target and back re-arms it on the list (root arguments included), and
 * the draws after the capture must land. */
$W = 64;
$ctx = vio_create('d3d12', ['width' => $W, 'height' => $W, 'headless' => true, 'vsync' => false]);
$VS = "#version 450\nlayout(location=0) in vec3 aPos;\nuniform vec4 u_off;\nvoid main(){ gl_Position = vec4(aPos.xy * 0.5 + u_off.xy, 0.0, 1.0); }";
$FS = "#version 450\nlayout(location=0) out vec4 o;\nuniform vec4 u_col;\nvoid main(){ o = u_col; }";
$quad = vio_mesh($ctx, ['vertices' => [-1,-1,0, 1,-1,0, 1,1,0, -1,-1,0, 1,1,0, -1,1,0], 'layout' => [VIO_FLOAT3]]);
$sh = vio_shader($ctx, ['vertex' => $VS, 'fragment' => $FS]);
$pipe = vio_pipeline($ctx, ['shader' => $sh, 'depth_test' => false, 'cull_mode' => VIO_CULL_NONE]);
$rt = vio_render_target($ctx, ['width' => 32, 'height' => 32]);
$shot = sys_get_temp_dir() . '/vio_221_' . getmypid() . '.png';
$ok = true;
foreach (['screenshot', 'read_pixels'] as $how) {
    for ($f = 0; $f < 3; $f++) {
        vio_begin($ctx);
        vio_clear($ctx, 0, 0, 0, 1);
        vio_bind_pipeline($ctx, $pipe);
        vio_set_uniform($ctx, 'u_off', [-0.5, 0, 0, 0]);
        vio_set_uniform($ctx, 'u_col', [1, 0, 0, 1]);
        vio_draw($ctx, $quad);
        if ($how === 'screenshot') vio_save_screenshot($ctx, $shot); else vio_read_pixels($ctx);
        vio_bind_render_target($ctx, $rt);
        vio_clear($ctx, 0, 0, 1, 1);
        vio_draw($ctx, $quad);
        vio_unbind_render_target($ctx);
        vio_set_uniform($ctx, 'u_off', [0.5, 0, 0, 0]);
        vio_set_uniform($ctx, 'u_col', [0, 1, 0, 1]);
        vio_draw($ctx, $quad);
        vio_end($ctx);
        $p = vio_read_pixels($ctx);
        $px = function ($x, $y) use ($p, $W) { $o = ($y * $W + $x) * 4; return [ord($p[$o]), ord($p[$o + 1]), ord($p[$o + 2])]; };
        $l = $px($W >> 2, $W >> 1);
        $r = $px(3 * $W >> 2, $W >> 1);
        if ($l !== [255, 0, 0] || $r !== [0, 255, 0]) {
            $ok = false;
            echo "$how frame $f: left ", json_encode($l), " right ", json_encode($r), "\n";
        }
    }
}
@unlink($shot);
vio_destroy($ctx);
echo $ok ? "OK\n" : "FAIL\n";
?>
--EXPECTF--
%AOK

--TEST--
D3D12: a pipeline freed right after vio_end is released once its frame has retired (the debug layer ended the process on a PSO deleted while still in use)
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
/* With vio.debug=1 the D3D12 debug layer is on where it is installed (it is a
 * no-op elsewhere). Each frame builds a pipeline, draws with it and frees it
 * right after vio_end, while the GPU may still run the frame. */
$W = 16;
$ctx = vio_create('d3d12', ['width' => $W, 'height' => $W, 'headless' => true, 'vsync' => false]);
$VS = "#version 450\nlayout(location=0) in vec3 aPos;\nvoid main(){ gl_Position = vec4(aPos, 1.0); }";
$FS = "#version 450\nlayout(location=0) out vec4 o;\nuniform vec4 u_col;\nvoid main(){ o = u_col; }";
$tri = vio_mesh($ctx, ['vertices' => [-1,-1,0, 3,-1,0, -1,3,0], 'layout' => [VIO_FLOAT3]]);
$sh = vio_shader($ctx, ['vertex' => $VS, 'fragment' => $FS]);
$cols = [[0.0, 1.0, 0.0], [0.0, 0.0, 1.0], [1.0, 0.0, 0.0]];
$ok = true;
foreach ($cols as $f => $c) {
    $pipe = vio_pipeline($ctx, ['shader' => $sh, 'depth_test' => false, 'cull_mode' => VIO_CULL_NONE]);
    vio_begin($ctx);
    vio_clear($ctx, 0, 0, 0, 1);
    vio_bind_pipeline($ctx, $pipe);
    vio_set_uniform($ctx, 'u_col', [$c[0], $c[1], $c[2], 1.0]);
    vio_draw($ctx, $tri);
    vio_end($ctx);
    unset($pipe);   /* freed while the frame may still be in flight */
    $p = vio_read_pixels($ctx);
    $o = (($W >> 1) * $W + ($W >> 1)) * 4;
    $got = [ord($p[$o]), ord($p[$o + 1]), ord($p[$o + 2])];
    $want = array_map(fn($v) => (int)round($v * 255), $c);
    if ($got !== $want) { $ok = false; echo "frame $f: ", json_encode($got), ", want ", json_encode($want), "\n"; }
}
vio_destroy($ctx);
echo $ok ? "OK\n" : "FAIL\n";
?>
--EXPECTF--
%AOK

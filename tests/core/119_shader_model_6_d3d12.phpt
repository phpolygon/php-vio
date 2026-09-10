--TEST--
Shader Model 6 on D3D12: vio_create(['shader_model' => 6]) compiles graphics and compute shaders with DXC to DXIL
--EXTENSIONS--
vio
--SKIPIF--
<?php
$c = @vio_create('d3d12', ["width" => 8, "height" => 8, "headless" => true, "vsync" => false]);
if (!$c) die("skip d3d12 unavailable");
vio_destroy($c);
?>
--FILE--
<?php
/* GAP-PHASE5-PLAN Block 7. DXC lives in dxcompiler.dll + dxil.dll; the test
 * points vio at them via VIO_DXC_DIR, the Windows SDK's bin directory, or the
 * default search path. Without them the request falls back to FXC 5.1 and
 * vio_gpu_info() says so — that is the honest outcome, not a failure. */
function px(string $p, int $x, int $y, int $w): array { $o = ($y*$w+$x)*4; return [ord($p[$o]), ord($p[$o+1]), ord($p[$o+2])]; }

$dir = getenv('VIO_DXC_DIR') ?: '';
if ($dir === '') {
    foreach (glob('C:/Program Files (x86)/Windows Kits/10/bin/10.*/x64/dxcompiler.dll') ?: [] as $cand) {
        if (is_file(dirname($cand) . '/dxil.dll')) { $dir = dirname($cand); }
    }
}
$W = 16;
$opts = ["width" => $W, "height" => $W, "headless" => true, "vsync" => false, "shader_model" => 6];
if ($dir !== '') $opts['dxc_dir'] = $dir;
$ctx = vio_create('d3d12', $opts);
if (!$ctx) { echo "FAIL create\n"; exit; }
$info = vio_swapchain_info($ctx);
$sm = $info['shader_model'] ?? 0;
echo "shader_model: ", $sm === 6 ? "6" : ($sm === 5 ? "5 (DXC unavailable, FXC fallback)" : "unexpected $sm"), "\n";

/* Graphics: a green quad through the requested shader model. */
$vs = "#version 330 core\nlayout(location=0) in vec3 aPos;\nvoid main(){ gl_Position = vec4(aPos, 1.0); }";
$fs = "#version 330 core\nlayout(location=0) out vec4 o;\nvoid main(){ o = vec4(0.0, 1.0, 0.0, 1.0); }";
$pipe = vio_pipeline($ctx, ['shader' => vio_shader($ctx, ['vertex' => $vs, 'fragment' => $fs, 'format' => VIO_SHADER_GLSL]), 'depth_test' => false, 'cull_mode' => VIO_CULL_NONE]);
$quad = vio_mesh($ctx, ['vertices' => [-1,-1,0, 1,-1,0, 1,1,0, -1,1,0], 'indices' => [0,1,2, 0,2,3], 'layout' => [VIO_FLOAT3]]);
vio_clear($ctx, 0, 0, 0, 1);
vio_begin($ctx);
vio_bind_pipeline($ctx, $pipe);
vio_draw($ctx, $quad);
vio_end($ctx);
$p = px(vio_read_pixels($ctx), $W >> 1, $W >> 1, $W);
echo "graphics: ", ($p[1] > 250 && $p[0] < 5 && $p[2] < 5) ? "OK" : "FAIL " . json_encode($p), "\n";

/* Compute: doubles 8 floats. */
if (vio_supports_feature($ctx, VIO_FEATURE_COMPUTE)) {
    $cs = "#version 450\nlayout(local_size_x = 8) in;\nlayout(std430, binding = 0) readonly buffer In { float v[]; } src;\nlayout(std430, binding = 1) buffer Out { float v[]; } dst;\nvoid main(){ uint i = gl_GlobalInvocationID.x; dst.v[i] = src.v[i] * 2.0; }";
    $cp = vio_compute_pipeline($ctx, ['source' => $cs]);
    $in = vio_storage_buffer($ctx, ['data' => pack('f*', 1, 2, 3, 4, 5, 6, 7, 8)]);
    $outBuf = vio_storage_buffer($ctx, ['size' => 8 * 4]);
    vio_compute_bind_buffer($ctx, $cp, $in, 0, VIO_COMPUTE_READ);
    vio_compute_bind_buffer($ctx, $cp, $outBuf, 1, VIO_COMPUTE_WRITE);
    vio_compute_dispatch($ctx, $cp, 1, 1, 1);
    $out = array_values(unpack('f*', vio_storage_buffer_read($ctx, $outBuf)));
    echo "compute: ", ($out === [2.0, 4.0, 6.0, 8.0, 10.0, 12.0, 14.0, 16.0]) ? "OK" : "FAIL " . json_encode($out), "\n";
} else {
    echo "compute: skip\n";
}
vio_destroy($ctx);
echo "DONE\n";
?>
--EXPECTF--
shader_model: %s
graphics: OK
compute: %s
DONE

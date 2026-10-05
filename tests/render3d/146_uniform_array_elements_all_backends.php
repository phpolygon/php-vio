<?php
/* Found in Code Rescue (D3D12-MIPGEN-STALL-PLAN, side note): the cbuffer
 * backends reflected `uniform mat4 u_inv[6]` as one entry, so "u_inv[2]" set
 * nothing and only element 0 was reachable through "u_inv". The triangle is
 * drawn with u_m[2] (u_m[0] / u_m[1] stay zero and would collapse it), in the
 * colour u_col[1] (u_col[0] stays zero). */
$W = 16;
$VS = "#version 450\nlayout(location=0) in vec3 aPos;\nuniform mat4 u_m[3];\n"
    . "void main(){ gl_Position = u_m[2] * vec4(aPos, 1.0); }";
$FS = "#version 450\nlayout(location=0) out vec4 o;\nuniform vec4 u_col[2];\n"
    . "void main(){ o = u_col[1] + u_col[0]; }";
$identity = [1.0, 0.0, 0.0, 0.0,  0.0, 1.0, 0.0, 0.0,  0.0, 0.0, 1.0, 0.0,  0.0, 0.0, 0.0, 1.0];
$zero = array_fill(0, 16, 0.0);

function run_backend(string $name): string {
    global $W, $VS, $FS, $identity, $zero;
    $ctx = @vio_create($name, ["width" => $W, "height" => $W, "headless" => true, "vsync" => false]);
    if (!$ctx) return "skip (unavailable)";
    if (!vio_supports_feature($ctx, VIO_FEATURE_3D_PIPELINE)) { vio_destroy($ctx); return "skip (no 3D pipeline)"; }
    $pipe = vio_pipeline($ctx, ['shader' => vio_shader($ctx, ['vertex' => $VS, 'fragment' => $FS]),
                                'depth_test' => false, 'cull_mode' => VIO_CULL_NONE]);
    $tri = vio_mesh($ctx, ['vertices' => [-1,-1,0, 3,-1,0, -1,3,0], 'layout' => [VIO_FLOAT3]]);
    vio_clear($ctx, 0, 0, 0, 1);
    vio_begin($ctx);
    vio_bind_pipeline($ctx, $pipe);
    vio_set_uniform($ctx, 'u_m[0]', $zero);
    vio_set_uniform($ctx, 'u_m[1]', $zero);
    vio_set_uniform($ctx, 'u_m[2]', $identity);
    vio_set_uniform($ctx, 'u_col[0]', [0.0, 0.0, 0.0, 0.0]);
    vio_set_uniform($ctx, 'u_col[1]', [0.0, 1.0, 0.0, 1.0]);
    vio_draw($ctx, $tri);
    vio_end($ctx);
    $p = vio_read_pixels($ctx);
    $o = (($W >> 1) * $W + ($W >> 1)) * 4;
    $px = [ord($p[$o]), ord($p[$o + 1]), ord($p[$o + 2])];
    vio_destroy($ctx);
    return $px === [0, 255, 0] ? "OK" : "FAIL " . json_encode($px) . ", want [0,255,0]";
}

foreach (['opengl', 'd3d11', 'd3d12', 'vulkan', 'metal'] as $b) {
    echo "$b: ", run_backend($b), "\n";
}
echo "DONE\n";
?>

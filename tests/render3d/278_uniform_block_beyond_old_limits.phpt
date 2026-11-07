--TEST--
Uniform blocks are sized by their reflection, not by vio (fix/uniform-limits): a fragment stage with more than 256 uniform entries (an array of 80 structs) and more than 4096 bytes, and a vertex stage with more than 4096 bytes, still receive their last uniforms on every backend; a block over the backend's constant-buffer size fails on D3D instead of reading garbage
--EXTENSIONS--
vio
--FILE--
<?php
/* Before: vio reflected at most 256 uniform entries and kept a 4096-byte shadow per
 * stage. A uniform past either limit was dropped without a word, and the constant
 * buffer a draw copied ended at the last reflected entry, so the shader read stale
 * memory for the rest of its block (it flickered from draw to draw on D3D12). */
$VS = "#version 450\nlayout(location=0) in vec3 a_pos;\n"
    . "uniform vec4 u_vpad[400];\nuniform vec4 u_voff;\n"
    . "void main(){ gl_Position = vec4(a_pos.xy + u_voff.xy + u_vpad[0].xy * 0.0, 0.0, 1.0); }";
$FS = "#version 450\n"
    . "struct L { vec3 p; float r; vec3 c; float i; };\n"
    . "uniform L u_l[80];\n"
    . "uniform vec4 u_pad[300];\n"
    . "uniform vec4 u_last;\n"
    . "layout(location=0) out vec4 o;\n"
    . "void main(){ o = vec4(u_last.rgb + u_l[79].c + u_pad[299].rgb + u_l[0].p * 0.0, 1.0); }";
$BIG = "#version 450\nuniform vec4 u_big[4100];\nlayout(location=0) out vec4 o;\nvoid main(){ o = u_big[4099]; }";

function run_backend(string $name, array $opts = []): string {
    global $VS, $FS, $BIG;
    $ctx = @vio_create($name, ['width' => 4, 'height' => 4, 'headless' => true, 'vsync' => false] + $opts);
    if ($ctx && vio_backend_name($ctx) !== $name) { vio_destroy($ctx); $ctx = null; }
    if (!$ctx) return "skip (unavailable)";
    if (!vio_supports_feature($ctx, VIO_FEATURE_3D_PIPELINE)) { vio_destroy($ctx); return "skip (no 3D pipeline)"; }
    $fail = [];
    $sh = vio_shader($ctx, ['vertex' => $VS, 'fragment' => $FS]);
    if (!$sh) { vio_destroy($ctx); return "FAIL: shader not created"; }
    $p = vio_pipeline($ctx, ['shader' => $sh, 'depth_test' => false, 'cull_mode' => VIO_CULL_NONE]);
    // A triangle that covers the target only once u_voff moves it into place.
    $tri = vio_mesh($ctx, ['vertices' => [9,9,0, 13,9,0, 9,13,0], 'layout' => [VIO_FLOAT3]]);
    vio_begin($ctx);
    vio_clear($ctx, 0, 0, 0, 1);
    vio_bind_pipeline($ctx, $p);
    vio_set_uniform($ctx, 'u_voff', [-10.0, -10.0, 0.0, 0.0]);
    vio_set_uniform($ctx, 'u_last', [0.2, 0.5, 0.3, 1.0]);
    vio_set_uniform($ctx, 'u_l[79].c', [0.4, 0.0, 0.0]);
    vio_set_uniform($ctx, 'u_pad[299]', [0.0, 0.0, 0.5, 0.0]);
    vio_draw($ctx, $tri);
    vio_end($ctx);
    $px = vio_read_pixels($ctx);
    $o = (1 * 4 + 1) * 4;
    $got = [ord($px[$o]), ord($px[$o + 1]), ord($px[$o + 2])];
    foreach ([153, 128, 204] as $k => $v) if (abs($got[$k] - $v) > 3) { $fail[] = "pixel " . json_encode($got) . ", want [153,128,204]"; break; }

    // 65,600 bytes: more than a D3D constant buffer holds.
    if (in_array($name, ['d3d11', 'd3d12'], true)) {
        $warn = '';
        set_error_handler(function ($no, $msg) use (&$warn) { $warn .= $msg . "\n"; return true; });
        $big = vio_shader($ctx, ['vertex' => $VS, 'fragment' => $BIG]);
        restore_error_handler();
        if ($big !== false) $fail[] = "a block over the constant-buffer size was accepted";
        elseif ($warn === '') $fail[] = "no warning for the oversized block";
    }
    vio_destroy($ctx);
    return $fail ? 'FAIL: ' . implode('; ', $fail) : 'OK';
}

foreach (['opengl', 'd3d11', 'd3d12', 'vulkan', 'metal'] as $b) echo "$b: ", run_backend($b), "\n";
echo "d3d12 sm6: ", run_backend('d3d12', ['shader_model' => 6]), "\n";
?>
--EXPECTF--
opengl: %r(OK|skip \(.*\))%r
d3d11: %r(OK|skip \(.*\))%r
d3d12: %r(OK|skip \(.*\))%r
vulkan: %r(OK|skip \(.*\))%r
metal: %r(OK|skip \(.*\))%r
d3d12 sm6: %r(OK|skip \(.*\))%r

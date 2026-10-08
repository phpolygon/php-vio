--TEST--
A bound pipeline stays bound across vio_end / vio_begin on every backend, and vio_draw before any vio_bind_pipeline does not fail (D3D12 used to draw without a PSO and remove the device)
--EXTENSIONS--
vio
--FILE--
<?php
$VS = "#version 450\nlayout(location=0) in vec2 aPos;\nvoid main(){ gl_Position = vec4(aPos, 0.0, 1.0); }";
$FS = "#version 450\nlayout(location=0) out vec4 o;\nvoid main(){ o = vec4(0.0, 1.0, 0.0, 1.0); }";
$QUAD = ['vertices' => [-1,-1, 1,-1, 1,1, -1,1], 'indices' => [0,1,2, 0,2,3], 'layout' => [VIO_FLOAT2]];

function centre(VioContext $ctx): string { return bin2hex(substr(vio_read_pixels($ctx), (8 * 16 + 8) * 4, 3)); }

function run_backend(string $name): string {
    global $VS, $FS, $QUAD;
    $ctx = @vio_create($name, ['width' => 16, 'height' => 16, 'headless' => true, 'vsync' => false]);
    if ($ctx && vio_backend_name($ctx) !== $name) { vio_destroy($ctx); $ctx = null; }
    if (!$ctx) return "skip (unavailable)";
    if (!vio_supports_feature($ctx, VIO_FEATURE_3D_PIPELINE)) { vio_destroy($ctx); return "skip (no 3D pipeline)"; }
    $err = [];

    /* Nothing bound yet: D3D11 / D3D12 / Vulkan draw nothing, GL falls back to
     * its built-in shader; either way the frame completes and reads back. */
    $quad = vio_mesh($ctx, $QUAD);
    vio_clear($ctx, 0, 0, 1, 1);
    vio_begin($ctx); vio_draw($ctx, $quad); vio_end($ctx);
    if (strlen(vio_read_pixels($ctx)) !== 16 * 16 * 4) $err[] = "unbound draw broke the readback";

    $pipe = vio_pipeline($ctx, ['shader' => vio_shader($ctx, ['vertex' => $VS, 'fragment' => $FS]), 'depth_test' => false, 'cull_mode' => VIO_CULL_NONE]);
    vio_clear($ctx, 0, 0, 0, 1);
    vio_begin($ctx); vio_bind_pipeline($ctx, $pipe); vio_draw($ctx, $quad); vio_end($ctx);
    if (($c = centre($ctx)) !== '00ff00') $err[] = "bound frame: centre $c";
    for ($f = 0; $f < 3; $f++) {   // later frames reuse the binding
        vio_clear($ctx, 0, 0, 0, 1);
        vio_begin($ctx); vio_draw($ctx, $quad); vio_end($ctx);
        if (($c = centre($ctx)) !== '00ff00') { $err[] = "frame " . ($f + 2) . " without re-bind: centre $c"; break; }
    }
    vio_destroy($ctx);
    return $err ? "FAIL\n  " . implode("\n  ", $err) : "OK";
}

foreach (['opengl', 'd3d11', 'd3d12', 'vulkan', 'metal'] as $b) echo "$b: ", run_backend($b), "\n";
echo "DONE\n";
?>
--EXPECTF--
opengl: %r(OK|skip \(.*\))%r
d3d11: %r(OK|skip \(.*\))%r
d3d12: %r(OK|skip \(.*\))%r
vulkan: %r(OK|skip \(.*\))%r
metal: %r(OK|skip \(.*\))%r
DONE

--TEST--
HDR10 swapchain output on D3D11 / D3D12: forced RGB10A2 backbuffer, PQ-encoded 2D batch, 3D PSO format variants (VIO_FEATURE_HDR_OUTPUT)
--EXTENSIONS--
vio
--SKIPIF--
<?php
$any = false;
foreach (['d3d11', 'd3d12'] as $b) {
    $c = @vio_create($b, ["width" => 8, "height" => 8, "headless" => true, "vsync" => false]);
    if ($c) { if (vio_supports_feature($c, VIO_FEATURE_HDR_OUTPUT)) $any = true; vio_destroy($c); }
}
if (!$any) die("skip no D3D backend with HDR output");
?>
--FILE--
<?php
/* GAP-PHASE5-PLAN Block 6. hdr_output => 2 forces the 10-bit path even on an
 * SDR desktop so the code runs everywhere: the swapchain is RGB10A2, the 2D
 * batch PQ-encodes its colours (white at 200 nits paper white encodes to
 * ~0.58, i.e. ~148/255 after the read-back expands 10 bits to 8), and a 3D draw
 * through an RGBA8-declared pipeline hits the RGB10A2 PSO variant (D3D12).
 * hdr_output => 1 without an HDR display must fall back to RGBA8 and report
 * hdr_output = false. */
function px(string $p, int $x, int $y, int $w): array { $o = ($y*$w+$x)*4; return [ord($p[$o]), ord($p[$o+1]), ord($p[$o+2])]; }

function run_backend(string $name): string {
    $W = 32;
    $ctx = @vio_create($name, ["width" => $W, "height" => $W, "headless" => true, "vsync" => false, "hdr_output" => 2]);
    if (!$ctx) return "skip (unavailable)";
    if (!vio_supports_feature($ctx, VIO_FEATURE_HDR_OUTPUT)) { vio_destroy($ctx); return "skip (no HDR output)"; }
    $fail = [];
    $info = vio_swapchain_info($ctx);
    if (($info['format'] ?? -1) !== VIO_FORMAT_RGB10A2) $fail[] = "forced HDR swapchain should be RGB10A2, got " . json_encode($info['format'] ?? null);
    if (($info['hdr_output'] ?? false) !== true) $fail[] = "forced HDR should report hdr_output";

    /* 2D: white rectangle over black; PQ(white @ 200 nits) ~ 0.58 -> ~148. */
    vio_clear($ctx, 0, 0, 0, 1);
    vio_begin($ctx);
    vio_rect($ctx, 4, 4, 12, 12, ['fill' => 0xFFFFFFFF]);
    vio_draw_2d($ctx);
    vio_end($ctx);
    $p = vio_read_pixels($ctx);
    $c = px($p, 8, 8, $W);
    if ($c[0] < 120 || $c[0] > 175 || abs($c[0] - $c[1]) > 6 || abs($c[0] - $c[2]) > 6) $fail[] = "white rect not PQ-encoded (~148 expected), got " . json_encode($c);
    $bg = px($p, 28, 28, $W);
    if ($bg[0] + $bg[1] + $bg[2] > 12) $fail[] = "background not black " . json_encode($bg);

    /* 3D: an RGBA8-declared pipeline drawing onto the RGB10A2 backbuffer (PSO variant on D3D12). */
    $vs = "#version 330 core\nlayout(location=0) in vec3 aPos;\nvoid main(){ gl_Position = vec4(aPos, 1.0); }";
    $fs = "#version 330 core\nlayout(location=0) out vec4 o;\nvoid main(){ o = vec4(0.0, 1.0, 0.0, 1.0); }";
    $pipe = vio_pipeline($ctx, ['shader' => vio_shader($ctx, ['vertex' => $vs, 'fragment' => $fs, 'format' => VIO_SHADER_GLSL]), 'depth_test' => false, 'cull_mode' => VIO_CULL_NONE]);
    $quad = vio_mesh($ctx, ['vertices' => [-1,-1,0, 1,-1,0, 1,1,0, -1,1,0], 'indices' => [0,1,2, 0,2,3], 'layout' => [VIO_FLOAT3]]);
    vio_clear($ctx, 0, 0, 0, 1);
    vio_begin($ctx);
    vio_bind_pipeline($ctx, $pipe);
    vio_draw($ctx, $quad);
    vio_end($ctx);
    $q = px(vio_read_pixels($ctx), $W >> 1, $W >> 1, $W);
    /* The 3D shader writes raw (0,1,0) into the 10-bit buffer: green 255, others 0. */
    if ($q[1] < 250 || $q[0] > 5 || $q[2] > 5) $fail[] = "3D draw onto the HDR backbuffer failed (PSO variant?), got " . json_encode($q);
    vio_destroy($ctx);

    /* Not forced: honest fallback unless the desktop really is HDR. */
    $ctx = @vio_create($name, ["width" => $W, "height" => $W, "headless" => true, "vsync" => false, "hdr_output" => 1]);
    if ($ctx) {
        $info = vio_swapchain_info($ctx);
        if (($info['hdr_output'] ?? false) === true && ($info['format'] ?? 0) !== VIO_FORMAT_RGB10A2) $fail[] = "hdr_output without a 10-bit format";
        if (($info['hdr_output'] ?? false) === false && ($info['format'] ?? 0) !== VIO_FORMAT_RGBA8) $fail[] = "SDR fallback must be RGBA8";
        vio_destroy($ctx);
    }
    return $fail ? "FAIL\n  " . implode("\n  ", $fail) : "OK";
}

foreach (['d3d11', 'd3d12'] as $b) {
    echo "$b: ", run_backend($b), "\n";
}
echo "DONE\n";
?>
--EXPECTF--
d3d11: %s
d3d12: %s
DONE

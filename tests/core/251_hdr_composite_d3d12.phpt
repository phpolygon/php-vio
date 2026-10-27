--TEST--
HDR composite on D3D12: screen draws blend in an SDR layer, vio_end PQ-encodes it once (vio_create 'hdr_composite')
--EXTENSIONS--
vio
--SKIPIF--
<?php
$c = @vio_create('d3d12', ["width" => 8, "height" => 8, "headless" => true, "vsync" => false]);
if (!$c) die("skip d3d12 unavailable");
$ok = vio_supports_feature($c, VIO_FEATURE_HDR_OUTPUT);
vio_destroy($c);
if (!$ok) die("skip no HDR output");
?>
--FILE--
<?php
/* Without the composite the 2D batch PQ-encodes each fragment and blending runs
 * on PQ values: 50 % white over black lands at 0.5 * PQ(200 nits) ~ 0.29 (~74),
 * far darker than SDR (antialiased text thins out). With the composite the same
 * rect blends to 0.5 in the layer and is encoded as PQ(0.5^2.2 * 200 nits) ~ 0.43
 * (~109). Opaque white encodes to ~148 either way. hdr_output => 2 forces the
 * 10-bit path on an SDR desktop. */
function px(string $p, int $x, int $y, int $w): array { $o = ($y*$w+$x)*4; return [ord($p[$o]), ord($p[$o+1]), ord($p[$o+2])]; }

$W = 32;
$fail = [];

function frame2d($ctx, bool $readMid = false): array {
    vio_clear($ctx, 0, 0, 0, 1);
    vio_begin($ctx);
    vio_rect($ctx, 2, 2, 12, 12, ['fill' => 0xFFFFFFFF]);
    vio_rect($ctx, 18, 2, 12, 12, ['fill' => 0x80FFFFFF]);
    vio_draw_2d($ctx);
    $mid = $readMid ? vio_read_pixels($ctx) : null;
    vio_end($ctx);
    return [vio_read_pixels($ctx), $mid];
}

/* Reference: the old path. */
$ctx = vio_create('d3d12', ["width" => $W, "height" => $W, "headless" => true, "vsync" => false, "hdr_output" => 2]);
$info = vio_swapchain_info($ctx);
if (($info['hdr_composite'] ?? null) !== false) $fail[] = "without the option hdr_composite must be false, got " . json_encode($info['hdr_composite'] ?? null);
[$p] = frame2d($ctx);
$oldHalf = px($p, 24, 8, $W);
if ($oldHalf[0] < 60 || $oldHalf[0] > 90) $fail[] = "old path: half white expected ~74, got " . json_encode($oldHalf);
vio_destroy($ctx);

/* Composite. */
$ctx = vio_create('d3d12', ["width" => $W, "height" => $W, "headless" => true, "vsync" => false, "hdr_output" => 2, "hdr_composite" => true]);
$info = vio_swapchain_info($ctx);
if (($info['hdr_composite'] ?? null) !== true) $fail[] = "hdr_composite should be active, got " . json_encode($info);
if (($info['hdr_output'] ?? null) !== true) $fail[] = "hdr_output should stay true";

[$p, $mid] = frame2d($ctx, true);
$white = px($p, 8, 8, $W);
if ($white[0] < 135 || $white[0] > 160 || abs($white[0] - $white[1]) > 6 || abs($white[0] - $white[2]) > 6) $fail[] = "white not PQ-encoded (~148), got " . json_encode($white);
$half = px($p, 24, 8, $W);
if ($half[0] < 98 || $half[0] > 120) $fail[] = "half white should blend in SDR and encode to ~109, got " . json_encode($half);
$bg = px($p, 8, 26, $W);
if ($bg[0] + $bg[1] + $bg[2] > 12) $fail[] = "background not black " . json_encode($bg);
/* Mid-frame the drawing is still the SDR layer. */
$midWhite = px($mid, 8, 8, $W);
$midHalf = px($mid, 24, 8, $W);
if ($midWhite[0] < 250) $fail[] = "mid-frame read should see the SDR layer (white 255), got " . json_encode($midWhite);
if ($midHalf[0] < 120 || $midHalf[0] > 136) $fail[] = "mid-frame half white should be ~128, got " . json_encode($midHalf);

/* A frame that ends on an offscreen target, then a normal one: states stay valid. */
$rt = vio_render_target($ctx, ['width' => 16, 'height' => 16]);
vio_begin($ctx);
vio_bind_render_target($ctx, $rt);
vio_rect($ctx, 0, 0, 16, 16, ['fill' => 0xFFFF0000]);
vio_draw_2d($ctx);
vio_end($ctx);
vio_unbind_render_target($ctx);
[$p] = frame2d($ctx);
$again = px($p, 8, 8, $W);
if (abs($again[0] - $white[0]) > 2) $fail[] = "frame after an offscreen frame differs: " . json_encode($again);

/* 3D onto the screen goes through the layer as well: raw green is encoded. */
$vs = "#version 330 core\nlayout(location=0) in vec3 aPos;\nvoid main(){ gl_Position = vec4(aPos, 1.0); }";
$fs = "#version 330 core\nlayout(location=0) out vec4 o;\nvoid main(){ o = vec4(0.0, 1.0, 0.0, 1.0); }";
$pipe = vio_pipeline($ctx, ['shader' => vio_shader($ctx, ['vertex' => $vs, 'fragment' => $fs, 'format' => VIO_SHADER_GLSL]), 'depth_test' => false, 'cull_mode' => VIO_CULL_NONE]);
$quad = vio_mesh($ctx, ['vertices' => [-1,-1,0, 1,-1,0, 1,1,0, -1,1,0], 'indices' => [0,1,2, 0,2,3], 'layout' => [VIO_FLOAT3]]);
vio_clear($ctx, 0, 0, 0, 1);
vio_begin($ctx);
vio_bind_pipeline($ctx, $pipe);
vio_draw($ctx, $quad);
vio_end($ctx);
$g = px(vio_read_pixels($ctx), $W >> 1, $W >> 1, $W);
/* PQ of BT.709 green in BT.2020 at 200 nits: ~(119, 146, 90). */
if ($g[1] < 135 || $g[1] > 160 || $g[0] < 105 || $g[0] > 132 || $g[2] < 75 || $g[2] > 105) $fail[] = "3D green not encoded through the layer, got " . json_encode($g);
vio_destroy($ctx);

/* Without HDR10 the option changes nothing. */
$ctx = vio_create('d3d12', ["width" => $W, "height" => $W, "headless" => true, "vsync" => false, "hdr_composite" => true]);
$info = vio_swapchain_info($ctx);
if (($info['hdr_composite'] ?? null) !== false) $fail[] = "SDR swapchain must not report hdr_composite";
[$p] = frame2d($ctx);
$sdr = px($p, 24, 8, $W);
if ($sdr[0] < 120 || $sdr[0] > 136) $fail[] = "SDR half white ~128 expected, got " . json_encode($sdr);
vio_destroy($ctx);

echo $fail ? "FAIL\n  " . implode("\n  ", $fail) . "\n" : "OK\n";
?>
--EXPECT--
OK

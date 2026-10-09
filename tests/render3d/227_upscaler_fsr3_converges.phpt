--TEST--
Native upscaler FSR 3.1 (VIO_UPSCALER_FSR3, TEMPORAL-S3): a synthetic pattern rendered at half resolution with a jitter sequence and a resting camera converges towards the display-resolution reference - far closer than bilinear upscaling and than the first frame; the bound target stays bound (D3D12 / Vulkan, needs the FidelityFX runtime)
--EXTENSIONS--
vio
--SKIPIF--
<?php
if (!extension_loaded('vio')) die('skip vio not loaded');
$any = false; $why = [];
foreach (['d3d12', 'vulkan'] as $b) {
    $c = @vio_create($b, ['width' => 16, 'height' => 16, 'headless' => true, 'headless_hardware' => true, 'vsync' => false]);
    if (!$c || vio_backend_name($c) !== $b) { $why[] = "$b unavailable"; continue; }
    if (vio_upscaler_supported($c)) $any = true; else $why[] = "$b: " . vio_upscaler_info($c)['reason'];
    vio_destroy($c);
}
if (!$any) die('skip FSR 3.1 not available (' . implode('; ', $why) . ')');
?>
--FILE--
<?php
/* Display 64x64, render 32x32 (VIO_UPSCALE_PERFORMANCE). The pattern is a
 * function of the display position; each frame renders it at the jittered
 * sample positions (a pixel samples the scene at its centre - jitter). With a
 * resting camera the jittered samples accumulate: after a full jitter cycle the
 * output must be much closer to the pattern sampled at display resolution than
 * a bilinear upscale of one render, and closer than the first frame. */
const DW = 64, DH = 64;
$verbose = getenv('VIO_TEST_VERBOSE');
$h2f = function (int $h): float {
    $e = ($h >> 10) & 31; $m = $h & 1023;
    $v = $e === 0 ? $m / 1024 * 2 ** -14 : ($e === 31 ? INF : (1 + $m / 1024) * 2 ** ($e - 15));
    return ($h >> 15) & 1 ? -$v : $v;
};
$pattern = function (float $x, float $y): array {   // display-pixel coordinates
    return [0.5 + 0.5 * sin(2 * M_PI * $x / 9.3) * cos(2 * M_PI * $y / 11.1),
            0.5 + 0.5 * sin(2 * M_PI * ($x + $y) / 10.7)];
};
$rgbaHalf = function (string $raw, int $w, int $h) use ($h2f): array {
    $v = array_values(unpack('v*', $raw));
    $px = [];
    for ($i = 0; $i < $w * $h; $i++) $px[] = [$h2f($v[$i * 4]), $h2f($v[$i * 4 + 1])];
    return $px;   // [r, g] per pixel, top row first
};
$rmse = function (array $px, int $w, int $h) use ($pattern): float {
    $s = 0.0; $n = 0;
    for ($y = 2; $y < $h - 2; $y++) for ($x = 2; $x < $w - 2; $x++) {   // borders clamp differently
        $ref = $pattern($x + 0.5, $y + 0.5);
        $p = $px[$y * $w + $x];
        $s += ($p[0] - $ref[0]) ** 2 + ($p[1] - $ref[1]) ** 2; $n += 2;
    }
    return sqrt($s / $n);
};
$bilinear = function (array $src, int $sw, int $sh) {   // render -> display, pixel centres aligned
    $out = [];
    for ($y = 0; $y < DH; $y++) for ($x = 0; $x < DW; $x++) {
        $fx = ($x + 0.5) * $sw / DW - 0.5; $fy = ($y + 0.5) * $sh / DH - 0.5;
        $x0 = (int)floor($fx); $y0 = (int)floor($fy); $tx = $fx - $x0; $ty = $fy - $y0;
        $at = function ($xx, $yy) use ($src, $sw, $sh) { return $src[max(0, min($sh - 1, $yy)) * $sw + max(0, min($sw - 1, $xx))]; };
        $a = $at($x0, $y0); $b = $at($x0 + 1, $y0); $c = $at($x0, $y0 + 1); $d = $at($x0 + 1, $y0 + 1);
        $out[] = [($a[0] * (1 - $tx) + $b[0] * $tx) * (1 - $ty) + ($c[0] * (1 - $tx) + $d[0] * $tx) * $ty,
                  ($a[1] * (1 - $tx) + $b[1] * $tx) * (1 - $ty) + ($c[1] * (1 - $tx) + $d[1] * $tx) * $ty];
    }
    return $out;
};

$vs = "#version 330 core\nlayout(location=0) in vec3 aPos;\nvoid main(){ gl_Position = vec4(aPos, 1.0); }";
$fs = "#version 330 core\nuniform vec4 u_p;\nlayout(location=0) out vec4 o_color;\nlayout(location=1) out vec4 o_motion;\n"
    . "void main(){ vec2 dp = (gl_FragCoord.xy - u_p.xy) * u_p.z;\n"
    . "  float r = 0.5 + 0.5 * sin(6.28318530718 * dp.x / 9.3) * cos(6.28318530718 * dp.y / 11.1);\n"
    . "  float g = 0.5 + 0.5 * sin(6.28318530718 * (dp.x + dp.y) / 10.7);\n"
    . "  o_color = vec4(r, g, 0.5, 1.0); o_motion = vec4(0.0); }";

foreach (['d3d12', 'vulkan'] as $b) {
    /* On D3D12 the GPU, not WARP (it faults in FSR's compute passes). */
    $ctx = @vio_create($b, ['width' => DW, 'height' => DH, 'headless' => true, 'headless_hardware' => true, 'vsync' => false]);
    if ($ctx && vio_backend_name($ctx) !== $b) { vio_destroy($ctx); $ctx = null; }
    if (!$ctx) { echo "$b: skip (unavailable)\n"; continue; }
    if (!vio_upscaler_supported($ctx)) { echo "$b: skip (no FSR 3.1)\n"; vio_destroy($ctx); continue; }
    $fail = [];
    $up = vio_upscaler_create($ctx, ['display_width' => DW, 'display_height' => DH,
                                     'quality' => VIO_UPSCALE_PERFORMANCE, 'debug' => true]);
    $info = vio_upscaler_info($ctx, $up);
    [$RW, $RH] = [$info['render_width'], $info['render_height']];
    if ([$RW, $RH] !== [32, 32]) $fail[] = "render size {$RW}x{$RH}";
    if ($info['jitter_phases'] !== 32) $fail[] = "jitter phases {$info['jitter_phases']}";
    if (!preg_match('/^\d+\.\d+/', $info['version']) || $info['library'] === '') $fail[] = "version/library " . json_encode([$info['version'], $info['library']]);
    if (vio_supports_feature($ctx, VIO_FEATURE_UPSCALER_NATIVE) !== true) $fail[] = "feature flag";

    $rt = vio_render_target($ctx, ['width' => $RW, 'height' => $RH, 'attachments' => [VIO_FORMAT_RGBA16F, VIO_FORMAT_RG16F]]);
    $out = vio_render_target($ctx, ['width' => DW, 'height' => DH, 'attachments' => [VIO_FORMAT_RGBA16F], 'storage' => true]);
    $pipe = vio_pipeline($ctx, ['shader' => vio_shader($ctx, ['vertex' => $vs, 'fragment' => $fs]), 'depth_test' => true,
                                'cull_mode' => VIO_CULL_NONE, 'blend' => VIO_BLEND_NONE,
                                'attachments' => [VIO_FORMAT_RGBA16F, VIO_FORMAT_RG16F]]);
    $pipe2 = vio_pipeline($ctx, ['shader' => vio_shader($ctx, ['vertex' => $vs, 'fragment' => $fs]), 'depth_test' => false,
                                 'cull_mode' => VIO_CULL_NONE, 'blend' => VIO_BLEND_NONE,
                                 'attachments' => [VIO_FORMAT_RGBA16F, VIO_FORMAT_RG16F]]);
    $quad = vio_mesh($ctx, ['vertices' => [-1, -1, 0, 1, -1, 0, 1, 1, 0, -1, 1, 0], 'indices' => [0, 1, 2, 0, 2, 3], 'layout' => [VIO_FLOAT3]]);
    if (!$up || !$rt || !$out || !$pipe) { echo "$b: FAIL setup\n"; vio_destroy($ctx); continue; }

    $render = function (array $jitter) use ($ctx, $rt, $pipe, $quad) {
        vio_bind_render_target($ctx, $rt);
        vio_clear($ctx, 0, 0, 0, 1);
        vio_bind_pipeline($ctx, $pipe);
        vio_set_uniform($ctx, 'u_p', [$jitter[0], $jitter[1], DW / 32, 0.0]);
        vio_draw($ctx, $quad);
    };
    // Bilinear baseline: one render without jitter.
    vio_begin($ctx);
    $render([0.0, 0.0]);
    vio_unbind_render_target($ctx);
    vio_end($ctx);
    $errBilinear = $rmse($bilinear($rgbaHalf(vio_read_render_target($rt, -1, 0, ['raw' => true]), $RW, $RH), $RW, $RH), DW, DH);

    $N = $info['jitter_phases'] * 4;
    $errFirst = $errCycle = null;
    for ($i = 0; $i < $N; $i++) {
        $j = vio_upscale_jitter($i, $info['jitter_phases']);
        vio_begin($ctx);
        $render($j);
        // Dispatch while the render target is still bound: it must stay usable.
        $ok = vio_upscaler_dispatch($ctx, $up, ['color' => [$rt, 0], 'depth' => [$rt, VIO_RT_DEPTH], 'motion' => [$rt, 1],
                                               'output' => $out, 'jitter' => $j, 'reset' => $i === 0,
                                               'frame_time_ms' => 16.7, 'near' => 0.1, 'far' => 100.0]);
        if (!$ok) { $fail[] = "dispatch $i"; vio_end($ctx); break; }
        if ($i === $N - 1) {   // draw into the bound target again after the dispatch
            vio_bind_pipeline($ctx, $pipe2);
            vio_set_uniform($ctx, 'u_p', [0.0, 0.0, DW / 32, 0.0]);
            vio_draw($ctx, $quad);
        }
        vio_unbind_render_target($ctx);
        vio_end($ctx);
        if ($i === $info['jitter_phases'] - 1) $errCycle = $rmse($rgbaHalf(vio_read_render_target($out, -1, 0, ['raw' => true]), DW, DH), DW, DH);
        if ($i === 0) $errFirst = $rmse($rgbaHalf(vio_read_render_target($out, -1, 0, ['raw' => true]), DW, DH), DW, DH);
    }
    $px = $rgbaHalf(vio_read_render_target($out, -1, 0, ['raw' => true]), DW, DH);
    $errFsr = $rmse($px, DW, DH);
    $after = $rgbaHalf(vio_read_render_target($rt, -1, 0, ['raw' => true]), $RW, $RH);
    $ref = $pattern(8.5 * DW / 32, 8.5 * DW / 32);
    if (abs($after[8 * $RW + 8][0] - $ref[0]) > 0.01) $fail[] = "draw after the dispatch did not land";

    if ($verbose) fprintf(STDERR, "%s: rmse bilinear %.4f, FSR frame 1 %.4f, frame %d %.4f, frame %d %.4f\n", $b, $errBilinear, $errFirst, $info['jitter_phases'], $errCycle, $N, $errFsr);
    if (!($errFsr < 0.6 * $errBilinear)) $fail[] = sprintf("FSR %.4f not below 0.6 x bilinear %.4f", $errFsr, $errBilinear);
    if (!($errFsr < 0.6 * $errFirst && $errFsr < 0.85 * $errCycle)) $fail[] = sprintf("no convergence: frame 1 %.4f, %d %.4f, %d %.4f", $errFirst, $info['jitter_phases'], $errCycle, $N, $errFsr);
    if (!($errFsr < 0.05)) $fail[] = sprintf("FSR error %.4f", $errFsr);

    // Contract: outside a frame the dispatch fails with a warning.
    error_clear_last();
    $r = @vio_upscaler_dispatch($ctx, $up, ['color' => $rt, 'motion' => [$rt, 1], 'output' => $out]);
    if ($r !== false || !error_get_last()) $fail[] = "dispatch outside a frame";
    try { vio_upscaler_dispatch($ctx, $up, ['color' => $rt, 'motion' => [$rt, 1], 'output' => $rt]); $fail[] = "non-storage output accepted"; }
    catch (ValueError) {}

    unset($up);
    if (vio_upscaler_info($ctx)['live'] !== 0) $fail[] = "live after unset";
    echo "$b: ", $fail ? "FAIL " . implode('; ', $fail) : "OK", "\n";
    vio_destroy($ctx);
}
echo "DONE\n";
?>
--EXPECTF--
d3d12: %r(OK|skip \(.*\))%r
vulkan: %r(OK|skip \(.*\))%r
DONE

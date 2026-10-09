--TEST--
Native upscaler DLSS (VIO_UPSCALER_DLSS, TEMPORAL-S4): NGX optimal render size, a synthetic pattern rendered at half resolution with vio's jitter convention converges towards the display-resolution reference (far below bilinear and the first frame - NGX shares vio's jitter sign, a flipped axis does not converge); DLAA (VIO_UPSCALE_NATIVE_AA) resolves at display size; clean under the D3D12 debug layer / Vulkan validation (D3D12 / Vulkan, needs a DLSS build, nvngx_dlss.dll and an RTX GPU)
--EXTENSIONS--
vio
--SKIPIF--
<?php
if (!extension_loaded('vio')) die('skip vio not loaded');
$any = false; $why = [];
foreach (['d3d12', 'vulkan'] as $b) {
    $c = @vio_create($b, ['width' => 16, 'height' => 16, 'headless' => true, 'headless_hardware' => true, 'vsync' => false]);
    if (!$c || vio_backend_name($c) !== $b) { $why[] = "$b unavailable"; continue; }
    if (vio_upscaler_supported($c, VIO_UPSCALER_DLSS)) $any = true; else $why[] = "$b: " . vio_upscaler_info($c, VIO_UPSCALER_DLSS)['reason'];
    vio_destroy($c);
}
if (!$any) die('skip DLSS not available (' . implode('; ', $why) . ')');
?>
--FILE--
<?php
/* The setup of 227 (FSR 3.1) with DLSS: display 64x64, Performance renders
 * 32x32 (NGX's optimal size). A pixel samples the scene at its centre - jitter
 * (vio's convention, the same jitter / motion contract for every provider). */
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
    return $px;
};
$rmse = function (array $px, int $w, int $h) use ($pattern): float {
    $s = 0.0; $n = 0;
    for ($y = 2; $y < $h - 2; $y++) for ($x = 2; $x < $w - 2; $x++) {
        $ref = $pattern($x + 0.5, $y + 0.5);
        $p = $px[$y * $w + $x];
        $s += ($p[0] - $ref[0]) ** 2 + ($p[1] - $ref[1]) ** 2; $n += 2;
    }
    return sqrt($s / $n);
};
$bilinear = function (array $src, int $sw, int $sh) {
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

/* $provider over $N frames at quality $q: errors after 1 frame, one cycle, $N frames, + bilinear. */
$run = function ($ctx, int $provider, int $q, array &$fail) use ($vs, $fs, $rgbaHalf, $rmse, $bilinear) {
    $up = vio_upscaler_create($ctx, ['provider' => $provider, 'display_width' => DW, 'display_height' => DH, 'quality' => $q, 'debug' => true]);
    if (!$up) { $fail[] = "create"; return null; }
    $info = vio_upscaler_info($ctx, $up);
    [$RW, $RH] = [$info['render_width'], $info['render_height']];
    $scale = DW / $RW;
    $rt = vio_render_target($ctx, ['width' => $RW, 'height' => $RH, 'attachments' => [VIO_FORMAT_RGBA16F, VIO_FORMAT_RG16F]]);
    $out = vio_render_target($ctx, ['width' => DW, 'height' => DH, 'attachments' => [VIO_FORMAT_RGBA16F], 'storage' => true]);
    $pipe = vio_pipeline($ctx, ['shader' => vio_shader($ctx, ['vertex' => $vs, 'fragment' => $fs]), 'depth_test' => true,
                                'cull_mode' => VIO_CULL_NONE, 'blend' => VIO_BLEND_NONE,
                                'attachments' => [VIO_FORMAT_RGBA16F, VIO_FORMAT_RG16F]]);
    $quad = vio_mesh($ctx, ['vertices' => [-1, -1, 0, 1, -1, 0, 1, 1, 0, -1, 1, 0], 'indices' => [0, 1, 2, 0, 2, 3], 'layout' => [VIO_FLOAT3]]);
    $render = function (array $jitter) use ($ctx, $rt, $pipe, $quad, $scale) {
        vio_bind_render_target($ctx, $rt);
        vio_clear($ctx, 0, 0, 0, 1);
        vio_bind_pipeline($ctx, $pipe);
        vio_set_uniform($ctx, 'u_p', [$jitter[0], $jitter[1], $scale, 0.0]);
        vio_draw($ctx, $quad);
    };
    vio_begin($ctx);
    $render([0.0, 0.0]);
    vio_unbind_render_target($ctx);
    vio_end($ctx);
    $errBilinear = $rmse($bilinear($rgbaHalf(vio_read_render_target($rt, -1, 0, ['raw' => true]), $RW, $RH), $RW, $RH), DW, DH);
    $P = $info['jitter_phases'];
    $N = $P * 4;
    $errFirst = $errCycle = null;
    for ($i = 0; $i < $N; $i++) {
        $j = vio_upscale_jitter($i, $P);
        vio_begin($ctx);
        $render($j);
        $ok = vio_upscaler_dispatch($ctx, $up, ['color' => [$rt, 0], 'depth' => [$rt, VIO_RT_DEPTH], 'motion' => [$rt, 1],
                                               'output' => $out, 'jitter' => $j, 'reset' => $i === 0,
                                               'frame_time_ms' => 16.7, 'near' => 0.1, 'far' => 100.0]);
        vio_unbind_render_target($ctx);
        vio_end($ctx);
        if (!$ok) { $fail[] = "dispatch $i"; return null; }
        if ($i === 0) $errFirst = $rmse($rgbaHalf(vio_read_render_target($out, -1, 0, ['raw' => true]), DW, DH), DW, DH);
        if ($i === $P - 1) $errCycle = $rmse($rgbaHalf(vio_read_render_target($out, -1, 0, ['raw' => true]), DW, DH), DW, DH);
    }
    $err = $rmse($rgbaHalf(vio_read_render_target($out, -1, 0, ['raw' => true]), DW, DH), DW, DH);
    unset($up);
    return ['render' => [$RW, $RH], 'phases' => $P, 'N' => $N, 'bilinear' => $errBilinear, 'first' => $errFirst, 'cycle' => $errCycle, 'final' => $err];
};

foreach (['d3d12', 'vulkan'] as $b) {
    /* 'debug' => true: D3D12 debug layer / Vulkan validation, messages on stderr. */
    $ctx = @vio_create($b, ['width' => DW, 'height' => DH, 'headless' => true, 'headless_hardware' => true, 'vsync' => false, 'debug' => true]);
    if ($ctx && vio_backend_name($ctx) !== $b) { vio_destroy($ctx); $ctx = null; }
    if (!$ctx) { echo "$b: skip (unavailable)\n"; continue; }
    if (!vio_upscaler_supported($ctx, VIO_UPSCALER_DLSS)) { echo "$b: skip (no DLSS)\n"; vio_destroy($ctx); continue; }
    $fail = [];
    $pi = vio_upscaler_info($ctx, VIO_UPSCALER_DLSS);
    if (!preg_match('/^\d+\.\d+\.\d+$/', $pi['version']) || $pi['library'] === '' || $pi['driver'] === '') $fail[] = "info " . json_encode($pi);
    if (vio_supports_feature($ctx, VIO_FEATURE_UPSCALER_NATIVE) !== true) $fail[] = "feature flag";
    $rs = vio_upscaler_render_size($ctx, VIO_UPSCALER_DLSS, VIO_UPSCALE_PERFORMANCE, DW, DH);
    $aa = vio_upscaler_render_size($ctx, VIO_UPSCALER_DLSS, VIO_UPSCALE_NATIVE_AA, DW, DH);
    if ($rs !== ['width' => 32, 'height' => 32] || $aa !== ['width' => DW, 'height' => DH]) $fail[] = "render_size " . json_encode([$rs, $aa]);

    $r = $run($ctx, VIO_UPSCALER_DLSS, VIO_UPSCALE_PERFORMANCE, $fail);
    if ($r) {
        if ($r['render'] !== [32, 32] || $r['phases'] !== 32) $fail[] = "render " . json_encode($r['render']) . " phases {$r['phases']}";
        if ($verbose) fprintf(STDERR, "%s DLSS Performance: rmse bilinear %.4f, frame 1 %.4f, frame %d %.4f, frame %d %.4f\n",
                              $b, $r['bilinear'], $r['first'], $r['phases'], $r['cycle'], $r['N'], $r['final']);
        if (!($r['final'] < 0.4 * $r['bilinear'])) $fail[] = sprintf("DLSS %.4f not below 0.4 x bilinear %.4f", $r['final'], $r['bilinear']);
        if (!($r['final'] < 0.4 * $r['first'] && $r['cycle'] < 0.4 * $r['first'])) $fail[] = sprintf("no convergence: frame 1 %.4f, %d %.4f, %d %.4f", $r['first'], $r['phases'], $r['cycle'], $r['N'], $r['final']);
        if (!($r['final'] < 0.03)) $fail[] = sprintf("DLSS error %.4f", $r['final']);
    }
    // DLAA: render = display size; already exact at display resolution, stays so.
    $a = $run($ctx, VIO_UPSCALER_DLSS, VIO_UPSCALE_NATIVE_AA, $fail);
    if ($a) {
        if ($a['render'] !== [DW, DH]) $fail[] = "DLAA render " . json_encode($a['render']);
        if ($verbose) fprintf(STDERR, "%s DLAA: rmse frame 1 %.4f, frame %d %.4f\n", $b, $a['first'], $a['N'], $a['final']);
        if (!($a['final'] < 0.03)) $fail[] = sprintf("DLAA error %.4f", $a['final']);
    }
    if ($verbose && vio_upscaler_supported($ctx, VIO_UPSCALER_FSR3)) {
        $f = $run($ctx, VIO_UPSCALER_FSR3, VIO_UPSCALE_PERFORMANCE, $fail);
        if ($f) fprintf(STDERR, "%s FSR 3.1 Performance: rmse bilinear %.4f, frame 1 %.4f, frame %d %.4f, frame %d %.4f\n",
                        $b, $f['bilinear'], $f['first'], $f['phases'], $f['cycle'], $f['N'], $f['final']);
    }
    if (vio_upscaler_info($ctx)['live'] !== 0) $fail[] = "live after unset";
    echo "$b: ", $fail ? "FAIL " . implode('; ', $fail) : "OK", "\n";
    vio_destroy($ctx);
}
echo "DONE\n";
?>
--EXPECTF--
%r(\[d3d12\][^\n]*\n)*%rd3d12: %r(OK|skip \(.*\))%r
vulkan: %r(OK|skip \(.*\))%r
DONE

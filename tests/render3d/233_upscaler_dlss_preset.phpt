--TEST--
Native upscaler DLSS model preset ('preset' => 'e' | 'f' | 'j' | 'k' | 'l' | 'm', TEMPORAL-S4): the chosen preset converges like NGX's default, vio_upscaler_info names it ('' = NGX's default per quality mode), an unknown letter and a preset on FSR 3.1 are refused with the reason (D3D12 / Vulkan, needs a DLSS build, nvngx_dlss.dll and an RTX GPU)
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
/* The cost of DLSS Super Resolution is the model's: NGX's defaults are the
 * transformer presets (K for DLAA / Quality / Balanced, M for Performance, L
 * for Ultra Performance), on Turing 2-6x the deprecated CNN presets E / F
 * (DLSS programming guide 310.6, execution times). The preset is the caller's
 * choice, the default stays NGX's. Setup of 230: display 64x64, Performance. */
const DW = 64, DH = 64;
$verbose = getenv('VIO_TEST_VERBOSE');
$h2f = function (int $h): float {
    $e = ($h >> 10) & 31; $m = $h & 1023;
    $v = $e === 0 ? $m / 1024 * 2 ** -14 : ($e === 31 ? INF : (1 + $m / 1024) * 2 ** ($e - 15));
    return ($h >> 15) & 1 ? -$v : $v;
};
$rmse = function (string $raw) use ($h2f): float {
    $v = array_values(unpack('v*', $raw));
    $s = 0.0; $n = 0;
    for ($y = 2; $y < DH - 2; $y++) for ($x = 2; $x < DW - 2; $x++) {
        $i = ($y * DW + $x) * 4;
        $rx = 0.5 + 0.5 * sin(2 * M_PI * ($x + 0.5) / 9.3) * cos(2 * M_PI * ($y + 0.5) / 11.1);
        $ry = 0.5 + 0.5 * sin(2 * M_PI * ($x + 0.5 + $y + 0.5) / 10.7);
        $s += ($h2f($v[$i]) - $rx) ** 2 + ($h2f($v[$i + 1]) - $ry) ** 2; $n += 2;
    }
    return sqrt($s / $n);
};
$vs = "#version 330 core\nlayout(location=0) in vec3 aPos;\nvoid main(){ gl_Position = vec4(aPos, 1.0); }";
$fs = "#version 330 core\nuniform vec4 u_p;\nlayout(location=0) out vec4 o_color;\nlayout(location=1) out vec4 o_motion;\n"
    . "void main(){ vec2 dp = (gl_FragCoord.xy - u_p.xy) * u_p.z;\n"
    . "  float r = 0.5 + 0.5 * sin(6.28318530718 * dp.x / 9.3) * cos(6.28318530718 * dp.y / 11.1);\n"
    . "  float g = 0.5 + 0.5 * sin(6.28318530718 * (dp.x + dp.y) / 10.7);\n"
    . "  o_color = vec4(r, g, 0.5, 1.0); o_motion = vec4(0.0); }";

/* Error after 2 jitter cycles with $opts (+ provider / sizes / quality). */
$run = function ($ctx, array $opts, array &$fail, string $label) use ($vs, $fs, $rmse) {
    $up = vio_upscaler_create($ctx, $opts + ['provider' => VIO_UPSCALER_DLSS, 'display_width' => DW, 'display_height' => DH,
                                             'quality' => VIO_UPSCALE_PERFORMANCE]);
    if (!$up) { $fail[] = "$label: create"; return null; }
    $info = vio_upscaler_info($ctx, $up);
    [$RW, $RH, $P] = [$info['render_width'], $info['render_height'], $info['jitter_phases']];
    $rt = vio_render_target($ctx, ['width' => $RW, 'height' => $RH, 'attachments' => [VIO_FORMAT_RGBA16F, VIO_FORMAT_RG16F]]);
    $out = vio_render_target($ctx, ['width' => DW, 'height' => DH, 'attachments' => [VIO_FORMAT_RGBA16F], 'storage' => true]);
    $pipe = vio_pipeline($ctx, ['shader' => vio_shader($ctx, ['vertex' => $vs, 'fragment' => $fs]), 'depth_test' => true,
                                'cull_mode' => VIO_CULL_NONE, 'blend' => VIO_BLEND_NONE, 'attachments' => [VIO_FORMAT_RGBA16F, VIO_FORMAT_RG16F]]);
    $quad = vio_mesh($ctx, ['vertices' => [-1, -1, 0, 1, -1, 0, 1, 1, 0, -1, 1, 0], 'indices' => [0, 1, 2, 0, 2, 3], 'layout' => [VIO_FLOAT3]]);
    for ($i = 0; $i < 2 * $P; $i++) {
        $j = vio_upscale_jitter($i, $P);
        vio_begin($ctx);
        vio_bind_render_target($ctx, $rt);
        vio_clear($ctx, 0, 0, 0, 1);
        vio_bind_pipeline($ctx, $pipe);
        vio_set_uniform($ctx, 'u_p', [$j[0], $j[1], DW / $RW, 0.0]);
        vio_draw($ctx, $quad);
        vio_unbind_render_target($ctx);
        $ok = vio_upscaler_dispatch($ctx, $up, ['color' => [$rt, 0], 'depth' => [$rt, VIO_RT_DEPTH], 'motion' => [$rt, 1],
                                               'output' => $out, 'jitter' => $j, 'reset' => $i === 0,
                                               'frame_time_ms' => 16.7, 'near' => 0.1, 'far' => 100.0]);
        vio_end($ctx);
        if (!$ok) { $fail[] = "$label: dispatch $i"; return null; }
    }
    return ['preset' => $info['preset'], 'error' => $rmse(vio_read_render_target($out, -1, 0, ['raw' => true]))];
};

foreach (['d3d12', 'vulkan'] as $b) {
    $ctx = @vio_create($b, ['width' => DW, 'height' => DH, 'headless' => true, 'headless_hardware' => true, 'vsync' => false]);
    if ($ctx && vio_backend_name($ctx) !== $b) { vio_destroy($ctx); $ctx = null; }
    if (!$ctx) { echo "$b: skip (unavailable)\n"; continue; }
    if (!vio_upscaler_supported($ctx, VIO_UPSCALER_DLSS)) { echo "$b: skip (no DLSS)\n"; vio_destroy($ctx); continue; }
    $fail = [];
    $err = [];
    foreach ([null, 'm', 'e', 'k', 'K'] as $preset) {
        $label = $preset ?? 'default';
        $r = $run($ctx, $preset === null ? [] : ['preset' => $preset], $fail, $label);
        if (!$r) continue;
        $err[$label] = $r['error'];
        $want = $preset === null ? '' : strtolower($preset);
        if ($r['preset'] !== $want) $fail[] = "$label: info preset '" . $r['preset'] . "'";
        if ($verbose) fprintf(STDERR, "%s DLSS preset %s: rmse %.4f\n", $b, $label, $r['error']);
        // Every model reconstructs better than bilinear (0.0741 for this pattern).
        if (!($r['error'] < 0.07)) $fail[] = sprintf("%s: error %.4f", $label, $r['error']);
    }
    if (count($err) === 5) {
        // E (CNN) and K (transformer) are different models: a different error
        // on this pattern shows the preset reaches NGX. 'K' is 'k'. NGX's
        // default (M for Performance in SDK 310.6) converges as in 230.
        if (!($err['default'] < 0.03)) $fail[] = "default: " . json_encode($err);
        if (abs($err['e'] - $err['k']) < 0.002) $fail[] = "the preset did not reach NGX: " . json_encode($err);
        if (abs($err['k'] - $err['K']) > 0.002) $fail[] = "'K' differs from 'k': " . json_encode($err);
    }
    // Refused: not one letter (ValueError); a letter DLSS does not have, a
    // reserved one (warning, false); FSR 3.1 has no presets.
    foreach (['kk', '', 7] as $bad) {
        try {
            vio_upscaler_create($ctx, ['provider' => VIO_UPSCALER_DLSS, 'display_width' => DW, 'display_height' => DH, 'preset' => $bad]);
            $fail[] = "preset " . json_encode($bad) . " accepted";
        } catch (ValueError $e) {
            if (!str_contains($e->getMessage(), "'preset'")) $fail[] = "ValueError: " . $e->getMessage();
        }
    }
    $warn = [];
    set_error_handler(function ($no, $str) use (&$warn) { $warn[] = $str; return true; });
    foreach (['x', 'g'] as $bad) {
        $warn = [];
        $u = vio_upscaler_create($ctx, ['provider' => VIO_UPSCALER_DLSS, 'display_width' => DW, 'display_height' => DH, 'preset' => $bad]);
        if ($u !== false || !$warn || !str_contains($warn[0], "no preset '$bad'")) $fail[] = "preset '$bad' not refused: " . json_encode($warn);
    }
    if (vio_upscaler_supported($ctx, VIO_UPSCALER_FSR3)) {
        $warn = [];
        $u = vio_upscaler_create($ctx, ['provider' => VIO_UPSCALER_FSR3, 'display_width' => DW, 'display_height' => DH, 'preset' => 'k']);
        if ($u !== false || !$warn || !str_contains($warn[0], 'preset')) $fail[] = "FSR 3.1 took a preset: " . json_encode($warn);
        $u = vio_upscaler_create($ctx, ['provider' => VIO_UPSCALER_FSR3, 'display_width' => DW, 'display_height' => DH]);
        if (!$u || vio_upscaler_info($ctx, $u)['preset'] !== '') $fail[] = "FSR 3.1 info preset";
        unset($u);
    }
    restore_error_handler();
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

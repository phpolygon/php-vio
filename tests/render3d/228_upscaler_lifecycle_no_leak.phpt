--TEST--
Native upscaler lifecycle (TEMPORAL-S3): 100 rounds of create / dispatch / destroy with changing display sizes, quality modes, resets and settings, destroyed outside and inside a frame, leave no live upscaler and no provider memory behind; vio_destroy sweeps a live one (D3D12 / Vulkan, needs the FidelityFX runtime)
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
$vs = "#version 330 core\nlayout(location=0) in vec3 aPos;\nvoid main(){ gl_Position = vec4(aPos, 1.0); }";
$fs = "#version 330 core\nuniform vec4 u_c;\nlayout(location=0) out vec4 o_color;\nlayout(location=1) out vec4 o_motion;\n"
    . "void main(){ o_color = u_c; o_motion = vec4(0.25, -0.5, 0.0, 0.0); }";
$sizes = [[64, 64], [96, 48], [40, 72], [128, 80]];
$qualities = [VIO_UPSCALE_NATIVE_AA, VIO_UPSCALE_QUALITY, VIO_UPSCALE_BALANCED, VIO_UPSCALE_PERFORMANCE, VIO_UPSCALE_ULTRA_PERFORMANCE];

foreach (['d3d12', 'vulkan'] as $b) {
    $ctx = @vio_create($b, ['width' => 32, 'height' => 32, 'headless' => true, 'headless_hardware' => true, 'vsync' => false]);
    if ($ctx && vio_backend_name($ctx) !== $b) { vio_destroy($ctx); $ctx = null; }
    if (!$ctx) { echo "$b: skip (unavailable)\n"; continue; }
    if (!vio_upscaler_supported($ctx)) { echo "$b: skip (no FSR 3.1)\n"; vio_destroy($ctx); continue; }
    $fail = [];
    $pipe = vio_pipeline($ctx, ['shader' => vio_shader($ctx, ['vertex' => $vs, 'fragment' => $fs]), 'depth_test' => true,
                                'cull_mode' => VIO_CULL_NONE, 'blend' => VIO_BLEND_NONE,
                                'attachments' => [VIO_FORMAT_RGBA16F, VIO_FORMAT_RG16F]]);
    $quad = vio_mesh($ctx, ['vertices' => [-1, -1, 0, 1, -1, 0, 1, 1, 0, -1, 1, 0], 'indices' => [0, 1, 2, 0, 2, 3], 'layout' => [VIO_FLOAT3]]);
    $outs = [];
    $ins = [];
    foreach ($sizes as $k => [$w, $h]) {
        $outs[$k] = vio_render_target($ctx, ['width' => $w, 'height' => $h, 'attachments' => [VIO_FORMAT_RGBA16F], 'storage' => true]);
        $ins[$k] = vio_render_target($ctx, ['width' => $w, 'height' => $h, 'attachments' => [VIO_FORMAT_RGBA16F, VIO_FORMAT_RG16F]]);
    }
    $base = vio_upscaler_info($ctx)['host_bytes'];
    $mem = [];
    for ($i = 0; $i < 100; $i++) {
        $k = $i % count($sizes);
        [$w, $h] = $sizes[$k];
        $up = vio_upscaler_create($ctx, ['display_width' => $w, 'display_height' => $h, 'quality' => $qualities[$i % 5],
                                         'dynamic_resolution' => $i % 3 === 0, 'hdr' => $i % 4 === 1, 'debug' => true]);
        if (!$up) { $fail[] = "create $i"; break; }
        $info = vio_upscaler_info($ctx, $up);
        $mem[$k][] = $info['gpu_memory'];
        if ($info['live'] !== 1 || $info['gpu_memory'] <= 0) { $fail[] = "round $i info " . json_encode($info); break; }
        $inside = $i % 2 === 1;
        for ($f = 0; $f < 3; $f++) {
            $j = vio_upscale_jitter($i * 3 + $f, $info['jitter_phases']);
            vio_begin($ctx);
            vio_bind_render_target($ctx, $ins[$k]);
            vio_clear($ctx, 0, 0, 0, 1);
            vio_bind_pipeline($ctx, $pipe);
            vio_set_uniform($ctx, 'u_c', [0.2 + 0.006 * $i, 0.5, 0.8, 1.0]);
            vio_draw($ctx, $quad);
            vio_unbind_render_target($ctx);
            // Dynamic resolution: a smaller render size every other frame.
            $rw = $info['render_width'] - ($i % 3 === 0 && $f === 1 ? 2 : 0);
            $ok = vio_upscaler_dispatch($ctx, $up, ['color' => $ins[$k], 'motion' => [$ins[$k], 1], 'output' => $outs[$k],
                                                   'render_width' => max(1, $rw), 'render_height' => $info['render_height'],
                                                   'jitter' => $j, 'reset' => $f === 0 || $i % 7 === 0,
                                                   'sharpness' => [0.0, 0.4, 1.0][$i % 3], 'frame_time_ms' => 8 + $i % 20,
                                                   'pre_exposure' => 1.0 + ($i % 2), 'mv_scale' => [1.0, 1.0]]);
            if (!$ok) { $fail[] = "dispatch $i/$f"; }
            if ($inside && $f === 2) vio_upscaler_destroy($up);   // inside the frame that used it
            vio_end($ctx);
        }
        if (!$inside) unset($up);
        else {
            if (vio_upscaler_info($ctx, $up)['valid'] !== false) $fail[] = "round $i still valid after destroy";
            error_clear_last();
            vio_begin($ctx);
            $r = @vio_upscaler_dispatch($ctx, $up, ['color' => $ins[$k], 'motion' => [$ins[$k], 1], 'output' => $outs[$k]]);
            vio_end($ctx);
            if ($r !== false || !error_get_last()) $fail[] = "dispatch on a destroyed upscaler";
            unset($up);
        }
        $live = vio_upscaler_info($ctx)['live'];
        if ($live !== 0) { $fail[] = "round $i: $live live"; break; }
        if ($fail) break;
    }
    $after = vio_upscaler_info($ctx)['host_bytes'];
    if ($after !== $base) $fail[] = "provider host memory $base -> $after bytes";
    // The output is still a target that can be drawn into and read.
    $px = vio_read_render_target($outs[0], -1, 0);
    if (strlen((string)$px) !== 64 * 64 * 4) $fail[] = "output readback";

    // A live upscaler is swept by vio_destroy (its object outlives the context).
    $keep = vio_upscaler_create($ctx, ['display_width' => 64, 'display_height' => 64]);
    echo "$b: ", $fail ? "FAIL " . implode('; ', $fail) : "OK", "\n";
    vio_destroy($ctx);
    $after = vio_upscaler_info($ctx, $keep);
    if ($after['valid'] !== false) echo "$b: FAIL upscaler valid after vio_destroy\n";
    unset($keep);
}
echo "DONE\n";
?>
--EXPECTF--
d3d12: %r(OK|skip \(.*\))%r
vulkan: %r(OK|skip \(.*\))%r
DONE

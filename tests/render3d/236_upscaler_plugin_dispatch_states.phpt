--TEST--
Native upscaler plugin end to end (include/vio_upscale_plugin.h, TEMPORAL-S4): the test plugin loaded as VIO_UPSCALER_DLSS reports itself (version, library, plugin path, driver), chooses the render size and may refuse a mode, gets a recording command list at creation (also inside a frame), logs through the host, counts its host memory, receives the native images and moves them to copy states and back on D3D12 (WARP and GPU) and Vulkan - its copy lands in the output, a refused dispatch and preset come back as warnings, its Vulkan extension is enabled, and the D3D12 debug layer / Vulkan validation stay silent (needs the test plugin built next to php_vio)
--EXTENSIONS--
vio
--SKIPIF--
<?php
if (!extension_loaded('vio')) die('skip vio not loaded');
$plugin = ini_get('extension_dir') . DIRECTORY_SEPARATOR . (PHP_OS_FAMILY === 'Windows' ? 'vio_test_upscaler.dll' : 'vio_test_upscaler.so');
if (!is_file($plugin)) die("skip test plugin not built ($plugin)");
?>
--FILE--
<?php
const DW = 64, DH = 48;
$plugin = ini_get('extension_dir') . DIRECTORY_SEPARATOR . (PHP_OS_FAMILY === 'Windows' ? 'vio_test_upscaler.dll' : 'vio_test_upscaler.so');
// Before the first context: Vulkan asks the provider for its extensions at device creation.
ini_set('vio.dlss_plugin_path', $plugin);
$same = fn(string $a, string $b): bool => strcasecmp(str_replace('/', '\\', $a), str_replace('/', '\\', $b)) === 0;

/* RGBA8 readback: how many pixels have colour $c (one byte tolerance). */
$count = function (string $px, array $c): int {
    $n = 0;
    for ($i = 0, $l = strlen($px); $i < $l; $i += 4) {
        if (abs(ord($px[$i]) - $c[0]) <= 1 && abs(ord($px[$i + 1]) - $c[1]) <= 1 && abs(ord($px[$i + 2]) - $c[2]) <= 1) $n++;
    }
    return $n;
};

$configs = ['d3d12 warp' => ['d3d12', []], 'd3d12 gpu' => ['d3d12', ['headless_hardware' => true]], 'vulkan' => ['vulkan', []]];
foreach ($configs as $label => [$b, $extra]) {
    $ctx = @vio_create($b, ['width' => DW, 'height' => DH, 'headless' => true, 'vsync' => false, 'debug' => true] + $extra);
    if ($ctx && vio_backend_name($ctx) !== $b) { vio_destroy($ctx); $ctx = null; }
    if (!$ctx) { echo "$label: skip (unavailable)\n"; continue; }
    $fail = [];

    // The provider as the plugin reports it.
    $pi = vio_upscaler_info($ctx, VIO_UPSCALER_DLSS);
    if ($pi['supported'] !== true) $fail[] = "unsupported: " . $pi['reason'];
    if ($pi['provider'] !== 'dlss' || $pi['version'] !== '0.1.0-test' || $pi['driver'] !== 'test-driver'
        || !$same($pi['library'], $plugin) || !$same($pi['plugin'], $plugin)) $fail[] = "info " . json_encode($pi);
    if (vio_supports_feature($ctx, VIO_FEATURE_UPSCALER_NATIVE) !== true) $fail[] = "feature flag";
    if ($b === 'vulkan' && strpos($pi['device'], 'VK_KHR_push_descriptor') === false) $fail[] = "device extensions '" . $pi['device'] . "'";

    // Its render sizes: 2x per axis, native AA at the display size, no Ultra Performance (false, no warning).
    error_clear_last();
    $rs = [vio_upscaler_render_size($ctx, VIO_UPSCALER_DLSS, VIO_UPSCALE_PERFORMANCE, DW, DH),
           vio_upscaler_render_size($ctx, VIO_UPSCALER_DLSS, VIO_UPSCALE_NATIVE_AA, DW, DH),
           vio_upscaler_render_size($ctx, VIO_UPSCALER_DLSS, VIO_UPSCALE_ULTRA_PERFORMANCE, DW, DH)];
    if ($rs !== [['width' => 32, 'height' => 24], ['width' => DW, 'height' => DH], false]) $fail[] = "render_size " . json_encode($rs);
    if (($e = error_get_last()) !== null) $fail[] = "render_size warned: " . $e['message'];

    $rt = vio_render_target($ctx, ['width' => 32, 'height' => 24, 'attachments' => [VIO_FORMAT_RGBA8, VIO_FORMAT_RG16F]]);
    $out = vio_render_target($ctx, ['width' => DW, 'height' => DH, 'attachments' => [VIO_FORMAT_RGBA8], 'storage' => true]);
    $base = vio_upscaler_info($ctx)['host_bytes'];

    // Created inside a frame: its own command list, the frame goes on. Preset 'l' makes the plugin log a warning.
    vio_begin($ctx);
    vio_bind_render_target($ctx, $out);
    vio_clear($ctx, 0, 0, 1, 1);   // output starts blue
    vio_unbind_render_target($ctx);
    error_clear_last();
    $up = @vio_upscaler_create($ctx, ['provider' => VIO_UPSCALER_DLSS, 'display_width' => DW, 'display_height' => DH,
                                      'quality' => VIO_UPSCALE_PERFORMANCE, 'preset' => 'l']);
    $e = error_get_last();
    vio_end($ctx);
    if (!$up) { echo "$label: FAIL create\n"; vio_destroy($ctx); continue; }
    if (!$e || strpos($e['message'], "test upscaler: preset 'l' logs a warning") === false) $fail[] = "plugin log " . json_encode($e);
    $ui = vio_upscaler_info($ctx, $up);
    if ([$ui['render_width'], $ui['render_height'], $ui['preset']] !== [32, 24, 'l']) $fail[] = "upscaler " . json_encode($ui);
    if (!(vio_upscaler_info($ctx)['host_bytes'] > $base)) $fail[] = "plugin memory not counted";

    // Frames: render a colour, dispatch (copy into the top-left 32x24), draw into the colour target again after it.
    $colours = [[255, 0, 0], [0, 255, 0], [255, 255, 0]];
    foreach ($colours as $i => $c) {
        vio_begin($ctx);
        vio_bind_render_target($ctx, $rt);
        vio_clear($ctx, $c[0] / 255, $c[1] / 255, $c[2] / 255, 1);
        $ok = vio_upscaler_dispatch($ctx, $up, ['color' => [$rt, 0], 'depth' => [$rt, VIO_RT_DEPTH], 'motion' => [$rt, 1],
                                               'output' => $out, 'jitter' => [0.25, -0.25], 'reset' => $i === 0]);
        vio_clear($ctx, 0, 0, 0, 1);   // the colour target is a render target again
        vio_unbind_render_target($ctx);
        vio_end($ctx);
        if (!$ok) { $fail[] = "dispatch $i"; break; }
        $px = vio_read_render_target($out, -1, 0);
        $hit = $count($px, $c);
        $blue = $count($px, [0, 0, 255]);
        if ($hit !== 32 * 24 || $blue !== DW * DH - 32 * 24) $fail[] = "frame $i: $hit copied, $blue blue";
    }
    // A smaller render size this frame: only 16x12 is copied.
    vio_begin($ctx);
    vio_bind_render_target($ctx, $rt);
    vio_clear($ctx, 1, 0, 1, 1);
    $ok = vio_upscaler_dispatch($ctx, $up, ['color' => [$rt, 0], 'motion' => [$rt, 1], 'output' => $out,
                                           'render_width' => 16, 'render_height' => 12]);
    vio_unbind_render_target($ctx);
    vio_end($ctx);
    $px = vio_read_render_target($out, -1, 0);
    if (!$ok || $count($px, [255, 0, 255]) !== 16 * 12 || $count($px, [255, 255, 0]) !== 32 * 24 - 16 * 12) $fail[] = "dynamic render size";
    if (vio_upscaler_info($ctx, $up)['gpu_memory'] !== 4) $fail[] = "query passthrough " . vio_upscaler_info($ctx, $up)['gpu_memory'];

    // The plugin's refusal comes back as the dispatch's warning; the frame stays usable.
    vio_begin($ctx);
    error_clear_last();
    $ok = @vio_upscaler_dispatch($ctx, $up, ['color' => [$rt, 0], 'motion' => [$rt, 1], 'output' => $out, 'jitter' => [3.0, 0.0]]);
    $e = error_get_last();
    vio_bind_render_target($ctx, $rt);
    vio_clear($ctx, 0, 0, 0, 1);
    vio_unbind_render_target($ctx);
    vio_end($ctx);
    if ($ok !== false || !$e || strpos($e['message'], 'test upscaler: jitter (3.0, 0.0) beyond one pixel') === false) $fail[] = "dispatch refusal " . json_encode($e);

    unset($up);
    if (vio_upscaler_info($ctx)['host_bytes'] !== $base) $fail[] = "host memory after destroy";
    if (vio_upscaler_info($ctx)['live'] !== 0) $fail[] = "live after unset";

    // Refused at creation: the plugin's reason in the warning.
    error_clear_last();
    $u = @vio_upscaler_create($ctx, ['provider' => VIO_UPSCALER_DLSS, 'display_width' => DW, 'display_height' => DH, 'preset' => 'x']);
    $e = error_get_last();
    if ($u !== false || !$e || strpos($e['message'], "no preset 'x'") === false) $fail[] = "preset refusal " . json_encode($e);

    // vio_destroy sweeps a live one.
    $keep = vio_upscaler_create($ctx, ['provider' => VIO_UPSCALER_DLSS, 'display_width' => DW, 'display_height' => DH]);
    echo "$label: ", $fail ? "FAIL " . implode('; ', $fail) : "OK", "\n";
    vio_destroy($ctx);
    if (vio_upscaler_info($ctx, $keep)['valid'] !== false) echo "$label: FAIL valid after vio_destroy\n";
    unset($keep);
}
echo "DONE\n";
?>
--EXPECTF--
%r(\[d3d12\][^\n]*\n)*%rd3d12 warp: %r(OK|skip \(unavailable\))%r
%r(\[d3d12\][^\n]*\n)*%rd3d12 gpu: %r(OK|skip \(unavailable\))%r
vulkan: %r(OK|skip \(unavailable\))%r
DONE

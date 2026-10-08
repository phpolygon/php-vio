--TEST--
vio_gpu_timestamp / vio_gpu_timings: named GPU sections of the last completed frame on every backend with timestamps - mark order, a heavy section outweighs a light one, repeated names add up, sections fit inside vio_gpu_frame_time, 32-mark limit, argument contract
--EXTENSIONS--
vio
--FILE--
<?php
/* OPEN-ITEMS-PLAN A19. */
$VS = "#version 450\nlayout(location=0) in vec2 aPos;\nvoid main(){ gl_Position = vec4(aPos, 0.0, 1.0); }";
$FS = "#version 450\nlayout(location=0) out vec4 o;\nuniform float u_n;\n"
    . "void main(){ float a = gl_FragCoord.x * 0.001; for (int i = 0; i < int(u_n); i++) a = sin(a + 0.37) * 1.01; o = vec4(a, 0.0, 0.0, 1.0); }";

function run_backend(string $name): string {
    global $VS, $FS;
    $ctx = @vio_create($name, ['width' => 128, 'height' => 128, 'headless' => true, 'vsync' => false]);
    if ($ctx && vio_backend_name($ctx) !== $name) { vio_destroy($ctx); $ctx = null; }
    if (!$ctx) return "skip (unavailable)";
    if (!vio_supports_feature($ctx, VIO_FEATURE_GPU_TIMESTAMP)) {
        $r = vio_gpu_timings($ctx) === false && vio_gpu_timestamp($ctx, 'x') === false;
        vio_destroy($ctx);
        return $r ? "skip (no GPU timestamps)" : "FAIL: no timestamps but the API answers";
    }
    $err = [];
    if (vio_gpu_timestamp($ctx, 'outside') !== false) $err[] = "mark outside a frame accepted";
    foreach (['', str_repeat('n', 48)] as $bad) {
        try { vio_gpu_timestamp($ctx, $bad); $err[] = "name of " . strlen($bad) . " bytes accepted"; } catch (ValueError $e) {}
    }
    if (vio_gpu_timings($ctx) !== []) $err[] = "timings before any frame: " . json_encode(vio_gpu_timings($ctx));

    $quad = vio_mesh($ctx, ['vertices' => [-1,-1, 1,-1, 1,1, -1,1], 'indices' => [0,1,2, 0,2,3], 'layout' => [VIO_FLOAT2]]);
    $tiny = vio_mesh($ctx, ['vertices' => [-1,-1, -0.98,-1, -1,-0.98], 'layout' => [VIO_FLOAT2]]);
    $pipe = vio_pipeline($ctx, ['shader' => vio_shader($ctx, ['vertex' => $VS, 'fragment' => $FS]), 'depth_test' => false, 'cull_mode' => VIO_CULL_NONE]);

    $limitOk = true;
    for ($f = 0; $f < 8; $f++) {
        vio_begin($ctx);
        vio_bind_pipeline($ctx, $pipe);
        vio_set_uniform($ctx, 'u_n', 400.0);
        for ($i = 0; $i < 12; $i++) vio_draw($ctx, $quad);
        vio_gpu_timestamp($ctx, 'heavy');
        vio_set_uniform($ctx, 'u_n', 1.0);
        vio_draw($ctx, $tiny);
        vio_gpu_timestamp($ctx, 'light');
        vio_gpu_timestamp($ctx, 'dup');
        vio_draw($ctx, $tiny);
        vio_gpu_timestamp($ctx, 'dup');
        if ($f === 7) {   // fill up to the limit; the 33rd mark is refused
            for ($k = 4; $k < 32; $k++) if (!vio_gpu_timestamp($ctx, "m$k")) $limitOk = false;
            if (vio_gpu_timestamp($ctx, 'over') !== false) $limitOk = false;
        }
        vio_end($ctx);
    }
    $t = vio_gpu_timings($ctx);
    $ft = vio_gpu_frame_time($ctx);
    if (!is_array($t) || array_slice(array_keys($t), 0, 3) !== ['heavy', 'light', 'dup']) $err[] = "keys " . json_encode($t);
    else {
        foreach ($t as $k => $v) if (!is_float($v) || $v < 0) $err[] = "$k = " . json_encode($v);
        if (!($t['heavy'] > $t['light'])) $err[] = sprintf("heavy %.4f ms not above light %.4f ms", $t['heavy'], $t['light']);
        if ($ft > 0 && array_sum($t) > $ft + 1e-3) $err[] = sprintf("sections %.4f ms exceed the frame %.4f ms", array_sum($t), $ft);
    }
    if (!$limitOk) $err[] = "32-mark limit not honoured";

    /* Frames without marks: once one completes, the result is empty. */
    for ($f = 0; $f < 8; $f++) { vio_begin($ctx); vio_bind_pipeline($ctx, $pipe); vio_draw($ctx, $tiny); vio_end($ctx); }
    if (vio_gpu_timings($ctx) !== []) $err[] = "unmarked frames: " . json_encode(vio_gpu_timings($ctx));
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

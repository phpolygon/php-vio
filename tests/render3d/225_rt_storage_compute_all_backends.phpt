--TEST--
Render targets as storage images (VIO_FEATURE_RENDER_TARGET_STORAGE): vio_render_target(['storage' => true]) attachments are drawn into, then read and written by a compute kernel through vio_render_target_texture() + vio_compute_bind_image() - in the frame (async, a later draw samples the result) and after it (sync) - and stay usable as render targets and for raw readback; backends without the flag refuse the option
--EXTENSIONS--
vio
--FILE--
<?php
/* TEMPORAL-UPSCALING S0: TAA history / FSR-style passes update a render target
 * in a compute kernel. Values depend on the column only (render-target rows are
 * flipped between GL and the rest) and are exact in their formats, so the raw
 * readback is compared bit for bit:
 *   draw     : hist = (0.25, 0.5, 0.75, 1.0), mask = blue
 *   kernel 1 : hist = hist * 4 + (x, 0, 0, 0)        -> (1 + x, 2, 3, 4); mask = red for x < 4
 *   draw     : sample mask onto the swapchain (same frame)
 *   kernel 2 : hist.g += 1 (sync, after the frame)   -> (1 + x, 3, 3, 4)
 *   draw     : right half (x >= 4) of the target again -> hist (0.5, 0.5, 0.5, 0.5), mask green */
$W = 8; $H = 4;
$half = function (float $f): int {   // exact for the small values used here
    if ($f == 0.0) return 0;
    $e = (int)floor(log(abs($f), 2));
    $m = (int)round((abs($f) / 2 ** $e - 1) * 1024);
    return ($f < 0 ? 0x8000 : 0) | (($e + 15) << 10) | $m;
};
$texel = fn(array $v) => pack('v*', ...array_map($half, $v));

foreach (['opengl', 'd3d11', 'd3d12', 'vulkan', 'metal'] as $b) {
    $ctx = @vio_create($b, ['width' => $W, 'height' => $H, 'headless' => true, 'vsync' => false]);
    if ($ctx && vio_backend_name($ctx) !== $b) { vio_destroy($ctx); $ctx = null; }
    if (!$ctx) { echo "$b: skip (unavailable)\n"; continue; }
    $formats = [VIO_FORMAT_RGBA16F, VIO_FORMAT_RGBA8];
    if (!vio_supports_feature($ctx, VIO_FEATURE_RENDER_TARGET_STORAGE)) {
        $r = @vio_render_target($ctx, ['width' => $W, 'height' => $H, 'attachments' => $formats, 'storage' => true]);
        echo "$b: ", $r === false ? "skip (no storage targets)" : "FAIL 'storage' accepted without VIO_FEATURE_RENDER_TARGET_STORAGE", "\n";
        vio_destroy($ctx);
        continue;
    }
    $fail = [];
    $vs = "#version 330 core\nlayout(location=0) in vec3 aPos;\nvoid main(){ gl_Position = vec4(aPos, 1.0); }";
    $fsMrt = "#version 330 core\nuniform vec4 u_h;\nuniform vec4 u_m;\nlayout(location=0) out vec4 o0;\nlayout(location=1) out vec4 o1;\nvoid main(){ o0 = u_h; o1 = u_m; }";
    $fsShow = "#version 330 core\nuniform sampler2D u_t;\nlayout(location=0) out vec4 o;\nvoid main(){ o = texelFetch(u_t, ivec2(int(gl_FragCoord.x), 0), 0); }";
    $cs1 = "#version 450\nlayout(local_size_x = 8, local_size_y = 4, local_size_z = 1) in;\n"
         . "layout(binding = 0, rgba16f) uniform image2D u_hist;\nlayout(binding = 1, rgba8) uniform image2D u_mask;\n"
         . "void main(){ ivec2 p = ivec2(gl_GlobalInvocationID.xy);\n"
         . "  imageStore(u_hist, p, imageLoad(u_hist, p) * 4.0 + vec4(float(p.x), 0.0, 0.0, 0.0));\n"
         . "  if (p.x < 4) imageStore(u_mask, p, vec4(1.0, 0.0, 0.0, 1.0)); }";
    $cs2 = "#version 450\nlayout(local_size_x = 8, local_size_y = 4, local_size_z = 1) in;\n"
         . "layout(binding = 0, rgba16f) uniform image2D u_hist;\n"
         . "void main(){ ivec2 p = ivec2(gl_GlobalInvocationID.xy); imageStore(u_hist, p, imageLoad(u_hist, p) + vec4(0.0, 1.0, 0.0, 0.0)); }";
    $pMrt = vio_pipeline($ctx, ['shader' => vio_shader($ctx, ['vertex' => $vs, 'fragment' => $fsMrt]), 'depth_test' => false,
                                'cull_mode' => VIO_CULL_NONE, 'blend' => VIO_BLEND_NONE, 'attachments' => $formats]);
    $pShow = vio_pipeline($ctx, ['shader' => vio_shader($ctx, ['vertex' => $vs, 'fragment' => $fsShow]), 'depth_test' => false,
                                 'cull_mode' => VIO_CULL_NONE, 'blend' => VIO_BLEND_NONE]);
    $c1 = vio_compute_pipeline($ctx, ['source' => $cs1]);
    $c2 = vio_compute_pipeline($ctx, ['source' => $cs2]);
    $quad = fn(float $x0) => vio_mesh($ctx, ['vertices' => [$x0, -1, 0, 1, -1, 0, 1, 1, 0, $x0, 1, 0], 'indices' => [0, 1, 2, 0, 2, 3], 'layout' => [VIO_FLOAT3]]);
    $full = $quad(-1.0);
    $right = $quad(0.0);
    $rt = vio_render_target($ctx, ['width' => $W, 'height' => $H, 'attachments' => $formats, 'storage' => true]);
    if (!$rt || !$pMrt || !$c1 || !$c2) { echo "$b: FAIL setup\n"; vio_destroy($ctx); continue; }
    $hist = vio_render_target_texture($rt, 0);
    $mask = vio_render_target_texture($rt, 1);
    vio_compute_bind_image($ctx, $c1, $hist, 0, VIO_COMPUTE_WRITE);
    vio_compute_bind_image($ctx, $c1, $mask, 1, VIO_COMPUTE_WRITE);
    vio_compute_bind_image($ctx, $c2, $hist, 0, VIO_COMPUTE_WRITE);

    // Frame 1: draw, kernel 1 recorded into the frame, a later draw samples the mask.
    vio_begin($ctx);
    vio_bind_render_target($ctx, $rt);
    vio_bind_pipeline($ctx, $pMrt);
    vio_set_uniform($ctx, 'u_h', [0.25, 0.5, 0.75, 1.0]);
    vio_set_uniform($ctx, 'u_m', [0.0, 0.0, 1.0, 1.0]);
    vio_draw($ctx, $full);
    vio_unbind_render_target($ctx);
    vio_compute_dispatch($ctx, $c1, 1, 1, 1, ['async' => true]);
    vio_clear($ctx, 0, 0, 0, 1);
    vio_bind_pipeline($ctx, $pShow);
    vio_set_uniform($ctx, 'u_t', 0);
    vio_bind_texture($ctx, $mask, 0);
    vio_draw($ctx, $full);
    vio_end($ctx);
    $p = vio_read_pixels($ctx);
    $px = fn(string $s, int $x) => [ord($s[$x * 4]), ord($s[$x * 4 + 1]), ord($s[$x * 4 + 2])];
    if ($px($p, 1) !== [255, 0, 0] || $px($p, 6) !== [0, 0, 255]) $fail[] = "sampled after the kernel " . json_encode([$px($p, 1), $px($p, 6)]);

    // Kernel 2 outside a frame (synchronous).
    vio_compute_dispatch($ctx, $c2, 1, 1, 1);
    $want = '';
    for ($x = 0; $x < $W; $x++) $want .= $texel([1.0 + $x, 3.0, 3.0, 4.0]);
    $raw = vio_read_render_target($rt, -1, 0, ['raw' => true]);
    if ($raw !== str_repeat($want, $H)) $fail[] = "hist after the kernels " . bin2hex(substr((string)$raw, 0, 16)) . "... want " . bin2hex(substr($want, 0, 16)) . "...";
    $m = vio_read_render_target($rt, -1, 1, ['raw' => true]);
    if (substr($m, 0, 4) !== "\xFF\x00\x00\xFF" || substr($m, 7 * 4, 4) !== "\x00\x00\xFF\xFF") $fail[] = "mask " . bin2hex(substr((string)$m, 0, 32));

    // Still a render target: draw the right half again (no clear).
    vio_begin($ctx);
    vio_bind_render_target($ctx, $rt);
    vio_bind_pipeline($ctx, $pMrt);
    vio_set_uniform($ctx, 'u_h', [0.5, 0.5, 0.5, 0.5]);
    vio_set_uniform($ctx, 'u_m', [0.0, 1.0, 0.0, 1.0]);
    vio_draw($ctx, $right);
    vio_unbind_render_target($ctx);
    vio_end($ctx);
    $want = '';
    for ($x = 0; $x < $W; $x++) $want .= $x < 4 ? $texel([1.0 + $x, 3.0, 3.0, 4.0]) : $texel([0.5, 0.5, 0.5, 0.5]);
    $raw = vio_read_render_target($rt, -1, 0, ['raw' => true]);
    if ($raw !== str_repeat($want, $H)) $fail[] = "hist after the second draw " . bin2hex(substr((string)$raw, 0, 64));
    $m = vio_read_render_target($rt, -1, 1, ['raw' => true]);
    if (substr($m, 0, 4) !== "\xFF\x00\x00\xFF" || substr($m, 7 * 4, 4) !== "\x00\xFF\x00\xFF") $fail[] = "mask after the second draw " . bin2hex(substr((string)$m, 0, 32));

    // Without the option the attachments are not storage images; MSAA storage targets are refused.
    $plain = vio_render_target($ctx, ['width' => $W, 'height' => $H]);
    $c3 = vio_compute_pipeline($ctx, ['source' => $cs2]);
    error_clear_last();
    @vio_compute_bind_image($ctx, $c3, vio_render_target_texture($plain, 0), 0, VIO_COMPUTE_WRITE);
    if (error_get_last() === null) $fail[] = "a plain target bound as storage image";
    if (@vio_render_target($ctx, ['width' => $W, 'height' => $H, 'storage' => true, 'samples' => 4]) !== false) $fail[] = "MSAA storage target accepted";

    unset($hist, $mask, $rt, $plain);
    echo "$b: ", $fail ? "FAIL " . implode('; ', $fail) : "OK", "\n";
    vio_destroy($ctx);
}
echo "DONE\n";
?>
--EXPECTF--
opengl: %r(OK|skip \(.*\))%r
d3d11: %r(OK|skip \(.*\))%r
d3d12: %r(OK|skip \(unavailable\))%r
vulkan: %r(OK|skip \(unavailable\))%r
metal: %r(OK|skip \(.*\))%r
DONE

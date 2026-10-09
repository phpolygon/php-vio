--TEST--
Vulkan validation stays clean for samplers a draw does not bind: a texture still bound from an earlier pass that is now an attachment of the open pass (a render target bound for drawing again, before and after a native upscaler dispatch) and an unbound shadow sampler (the depth dummy) - no layout mismatch, no feedback loop, the draw renders (Vulkan with 'debug' => true; the upscaler part where FSR 3.1 / DLSS runs)
--EXTENSIONS--
vio
--SKIPIF--
<?php
if (!extension_loaded('vio')) die('skip vio not loaded');
$c = @vio_create('vulkan', ['width' => 16, 'height' => 16, 'headless' => true, 'vsync' => false]);
if (!$c || vio_backend_name($c) !== 'vulkan') die('skip Vulkan unavailable');
vio_destroy($c);
// The test needs the validation layer: with 'debug' => true vio_create fails
// when VK_LAYER_KHRONOS_validation is not installed (e.g. lavapipe-only CI).
$c = @vio_create('vulkan', ['width' => 16, 'height' => 16, 'headless' => true, 'vsync' => false, 'debug' => true]);
if (!$c || vio_backend_name($c) !== 'vulkan') die('skip Vulkan validation layer (VK_LAYER_KHRONOS_validation) not installed');
if (!vio_supports_feature($c, VIO_FEATURE_3D_PIPELINE) || !vio_supports_feature($c, VIO_FEATURE_RENDER_TARGET)) die('skip no 3D pipeline / render targets');
vio_destroy($c);
?>
--FILE--
<?php
/* The engine's frame: the scene draws into a 2-attachment target, a post pass
 * samples attachment 1 (texture slot 0), the next frame draws the scene into
 * the target again with a shader that declares a sampler at slot 0 but binds
 * nothing (an untextured material). The slot still names attachment 1 - now a
 * colour attachment of the open pass. Sampling it is a feedback loop and the
 * descriptor's SHADER_READ_ONLY layout does not match COLOR_ATTACHMENT
 * (VUID-vkCmdDrawIndexed-imageLayout-00344). vio must not hand an attachment
 * of the open pass to a sampler; and the dummy behind an unbound shadow
 * sampler must be a valid depth image (DEPTH_STENCIL_READ_ONLY needs the
 * depth-attachment usage, VUID-VkImageMemoryBarrier2-oldLayout-01210). */
$msgs = [];
set_error_handler(function ($no, $str) use (&$msgs) { $msgs[] = $str; return true; });

$ctx = vio_create('vulkan', ['width' => 64, 'height' => 64, 'headless' => true, 'vsync' => false, 'debug' => true]);
$fail = [];

$vs = "#version 450\nlayout(location=0) in vec3 aPos;\nlayout(location=0) out vec2 v_uv;\n"
    . "void main(){ v_uv = aPos.xy * 0.5 + 0.5; gl_Position = vec4(aPos, 1.0); }";
// Scene: optional albedo texture at slot 0 (u_use = 0: not bound, not read).
$fsScene = "#version 450\nlayout(location=0) in vec2 v_uv;\nuniform sampler2D u_albedo;\nuniform vec4 u_color;\nuniform float u_use;\n"
         . "layout(location=0) out vec4 o0;\nlayout(location=1) out vec4 o1;\n"
         . "void main(){ vec4 c = u_color; if (u_use > 0.5) c *= texture(u_albedo, v_uv); o0 = c; o1 = vec4(0.25, 0.5, 0.75, 1.0); }";
// Post: samples its source at slot 0.
$fsPost = "#version 450\nlayout(location=0) in vec2 v_uv;\nuniform sampler2D u_src;\nlayout(location=0) out vec4 o;\n"
        . "void main(){ o = texture(u_src, v_uv); }";
// Unbound shadow sampler: the depth dummy (u_use = 0: not read).
$fsShadow = "#version 450\nlayout(location=0) in vec2 v_uv;\nuniform sampler2DShadow u_shadow;\nuniform float u_use;\nlayout(location=0) out vec4 o;\n"
          . "void main(){ float s = u_use > 0.5 ? texture(u_shadow, vec3(v_uv, 0.5)) : 1.0; o = vec4(0.0, s, 0.0, 1.0); }";

$quad = vio_mesh($ctx, ['vertices' => [-1, -1, 0, 1, -1, 0, 1, 1, 0, -1, 1, 0], 'indices' => [0, 1, 2, 0, 2, 3], 'layout' => [VIO_FLOAT3]]);
$rt = vio_render_target($ctx, ['width' => 32, 'height' => 32, 'attachments' => [VIO_FORMAT_RGBA8, VIO_FORMAT_RGBA8]]);
$scene = vio_pipeline($ctx, ['shader' => vio_shader($ctx, ['vertex' => $vs, 'fragment' => $fsScene]), 'depth_test' => false,
                             'cull_mode' => VIO_CULL_NONE, 'attachments' => [VIO_FORMAT_RGBA8, VIO_FORMAT_RGBA8]]);
$post = vio_pipeline($ctx, ['shader' => vio_shader($ctx, ['vertex' => $vs, 'fragment' => $fsPost]), 'depth_test' => false, 'cull_mode' => VIO_CULL_NONE]);
$shadow = vio_pipeline($ctx, ['shader' => vio_shader($ctx, ['vertex' => $vs, 'fragment' => $fsShadow]), 'depth_test' => false, 'cull_mode' => VIO_CULL_NONE]);
$att1 = vio_render_target_texture($rt, 1);

$drawScene = function (array $color) use ($ctx, $rt, $scene, $quad) {
    vio_bind_render_target($ctx, $rt);
    vio_clear($ctx, 0, 0, 0, 1);
    vio_bind_pipeline($ctx, $scene);
    vio_set_uniform($ctx, 'u_color', $color);
    vio_set_uniform($ctx, 'u_use', 0.0);
    vio_set_uniform($ctx, 'u_albedo', 0);
    vio_draw($ctx, $quad);
};
$drawPost = function ($tex) use ($ctx, $post, $quad) {
    vio_bind_pipeline($ctx, $post);
    vio_bind_texture($ctx, $tex, 0);
    vio_set_uniform($ctx, 'u_src', 0);
    vio_draw($ctx, $quad);
};
$px = function (string $p, int $w, int $x, int $y): array { $o = ($y * $w + $x) * 4; return [ord($p[$o]), ord($p[$o + 1]), ord($p[$o + 2])]; };
$near = function (array $a, array $b): bool { for ($k = 0; $k < 3; $k++) if (abs($a[$k] - $b[$k]) > 1) return false; return true; };

// 1. Stale slot = an attachment of the pass the next frame opens.
for ($f = 0; $f < 3; $f++) {
    vio_begin($ctx);
    $drawScene([0.2 + 0.2 * $f, 0.4, 0.6, 1.0]);
    vio_unbind_render_target($ctx);
    $drawPost($att1);
    vio_end($ctx);
}
$p = vio_read_render_target($rt, -1, 0);
if (!$near($px($p, 32, 16, 16), [153, 102, 153])) $fail[] = "scene colour " . json_encode($px($p, 32, 16, 16));
$p = vio_read_render_target($rt, -1, 1);
if (!$near($px($p, 32, 16, 16), [64, 128, 191])) $fail[] = "attachment 1 " . json_encode($px($p, 32, 16, 16));

// 2. The same within one frame: post samples attachment 1, then the scene pass opens again.
vio_begin($ctx);
$drawScene([1.0, 0.0, 0.0, 1.0]);
vio_unbind_render_target($ctx);
$drawPost($att1);
$drawScene([0.0, 0.0, 1.0, 1.0]);
vio_unbind_render_target($ctx);
vio_end($ctx);
$p = vio_read_render_target($rt, -1, 0);
if (!$near($px($p, 32, 16, 16), [0, 0, 255])) $fail[] = "second pass " . json_encode($px($p, 32, 16, 16));

// 3. Unbound shadow sampler.
vio_begin($ctx);
vio_bind_pipeline($ctx, $shadow);
vio_set_uniform($ctx, 'u_use', 0.0);
vio_set_uniform($ctx, 'u_shadow', 0);
vio_draw($ctx, $quad);
vio_end($ctx);

// 4. Around a native upscaler dispatch (the engine's path): scene into the
//    input target, unbind, dispatch, present samples the output; next frame
//    the scene draws into the input again.
$provider = 0;
foreach ([VIO_UPSCALER_FSR3, VIO_UPSCALER_DLSS] as $pv) {
    if (function_exists('vio_upscaler_supported') && @vio_upscaler_supported($ctx, $pv)) { $provider = $pv; break; }
}
$upscaled = 'n/a';
if ($provider) {
    $in = vio_render_target($ctx, ['width' => 32, 'height' => 32, 'attachments' => [VIO_FORMAT_RGBA16F, VIO_FORMAT_RG16F]]);
    $out = vio_render_target($ctx, ['width' => 64, 'height' => 64, 'attachments' => [VIO_FORMAT_RGBA16F], 'storage' => true]);
    $up = vio_upscaler_create($ctx, ['provider' => $provider, 'display_width' => 64, 'display_height' => 64,
                                     'render_width' => 32, 'render_height' => 32, 'quality' => VIO_UPSCALE_PERFORMANCE]);
    $inPipe = vio_pipeline($ctx, ['shader' => vio_shader($ctx, ['vertex' => $vs, 'fragment' => $fsScene]), 'depth_test' => true,
                                  'cull_mode' => VIO_CULL_NONE, 'attachments' => [VIO_FORMAT_RGBA16F, VIO_FORMAT_RG16F]]);
    $inColor = vio_render_target_texture($in, 0);
    $outTex = vio_render_target_texture($out);
    $ok = (bool)$up;
    for ($f = 0; $ok && $f < 4; $f++) {
        vio_begin($ctx);
        vio_bind_render_target($ctx, $in);
        vio_clear($ctx, 0, 0, 0, 1);
        vio_bind_pipeline($ctx, $inPipe);
        vio_set_uniform($ctx, 'u_color', [0.5, 0.25, 0.125, 1.0]);
        vio_set_uniform($ctx, 'u_use', 0.0);
        vio_set_uniform($ctx, 'u_albedo', 0);
        vio_draw($ctx, $quad);
        vio_unbind_render_target($ctx);
        $drawPost($inColor);   // the input colour, sampled before the dispatch
        $ok = vio_upscaler_dispatch($ctx, $up, ['color' => [$in, 0], 'depth' => [$in, VIO_RT_DEPTH], 'motion' => [$in, 1],
                                                'output' => $out, 'jitter' => [0.0, 0.0], 'reset' => $f === 0,
                                                'frame_time_ms' => 16.7, 'near' => 0.1, 'far' => 100.0]);
        $drawPost($outTex);    // present: the output
        vio_end($ctx);
    }
    if (!$ok) $fail[] = "upscaler dispatch";
    $upscaled = $ok ? 'ok' : 'failed';
    unset($up);
}

vio_destroy($ctx);
restore_error_handler();
foreach ($msgs as $m) $fail[] = "message: " . substr(strtok($m, "\n"), 0, 300);
echo $fail ? "FAIL\n" . implode("\n", $fail) . "\n" : "OK\n";
?>
--EXPECT--
OK

--TEST--
A chain of render targets, each sampling the previous one, with no unbind in between: binding the next target leaves the previous one readable on every backend (D3D12 kept its colour in RENDER_TARGET state - WARP 1.0.21 returned black / stale texels, vio_upscale's mid and history passes hit it)
--EXTENSIONS--
vio
--FILE--
<?php
/* Target 0 is cleared to a colour; target k (1..5) is drawn by a fullscreen
 * pass that samples target k-1 and adds 0.1 to red. Only one unbind, at the
 * end. Several frames, different start colours: a stale or black read shows
 * up as a wrong final value. */
$VS = "#version 450\nlayout(location=0) in vec2 aPos;\nlayout(location=0) out vec2 uv;\n"
    . "void main(){ uv = aPos * 0.5 + 0.5; gl_Position = vec4(aPos, 0.0, 1.0); }";
$FS = "#version 450\nlayout(location=0) in vec2 uv;\nlayout(location=0) out vec4 o;\nuniform sampler2D u_src;\n"
    . "void main(){ vec4 c = texture(u_src, uv); o = vec4(c.r + 0.1, c.g, c.b, 1.0); }";

function run_backend(string $name): string {
    global $VS, $FS;
    $ctx = @vio_create($name, ['width' => 16, 'height' => 16, 'headless' => true, 'vsync' => false]);
    if ($ctx && vio_backend_name($ctx) !== $name) { vio_destroy($ctx); $ctx = null; }
    if (!$ctx) return "skip (unavailable)";
    if (!vio_supports_feature($ctx, VIO_FEATURE_3D_PIPELINE) || !vio_supports_feature($ctx, VIO_FEATURE_RENDER_TARGET)) {
        vio_destroy($ctx); return "skip (no 3D pipeline)";
    }
    $quad = vio_mesh($ctx, ['vertices' => [-1,-1, 1,-1, 1,1, -1,1], 'indices' => [0,1,2, 0,2,3], 'layout' => [VIO_FLOAT2]]);
    $pipe = vio_pipeline($ctx, ['shader' => vio_shader($ctx, ['vertex' => $VS, 'fragment' => $FS]), 'depth_test' => false, 'cull_mode' => VIO_CULL_NONE]);
    $rts = [];
    for ($k = 0; $k < 6; $k++) $rts[] = vio_render_target($ctx, ['width' => 16, 'height' => 16]);
    $fail = [];
    foreach ([0.0, 0.2, 0.4, 0.1] as $f => $start) {
        vio_begin($ctx);
        vio_bind_render_target($ctx, $rts[0]);
        vio_viewport($ctx, 0, 0, 16, 16);
        vio_clear($ctx, $start, 0.5, 0.25, 1.0);
        for ($k = 1; $k < 6; $k++) {
            vio_bind_render_target($ctx, $rts[$k]);   // no unbind: the previous target stays the sampled source
            vio_viewport($ctx, 0, 0, 16, 16);
            vio_bind_pipeline($ctx, $pipe);
            vio_bind_texture($ctx, vio_render_target_texture($rts[$k - 1]), 0);
            vio_set_uniform($ctx, 'u_src', 0);
            vio_draw($ctx, $quad);
        }
        vio_unbind_render_target($ctx);
        vio_end($ctx);
        $px = vio_read_render_target($rts[5]);
        $want = (int)round(($start + 0.5) * 255);
        $r = ord($px[(8 * 16 + 8) * 4]);
        $g = ord($px[(8 * 16 + 8) * 4 + 1]);
        if (abs($r - $want) > 3 || abs($g - 128) > 2) $fail[] = sprintf("frame %d: red %d green %d, want %d / 128", $f, $r, $g, $want);
    }
    vio_destroy($ctx);
    return $fail ? "FAIL\n  " . implode("\n  ", $fail) : "OK";
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

--TEST--
Multiple render targets: three attachments of different formats written by one fragment shader, read back and re-sampled
--DESCRIPTION--
API-ROADMAP R1. vio_render_target(['attachments' => [RGBA8, RGBA16F, RG16F]]) is bound,
a fragment shader writes layout(location = 0..2) outputs, and every attachment is
checked through vio_read_render_target($rt, -1, $i) (format-specific readback ->
RGBA8) and, for attachment 1, by sampling vio_render_target_texture($rt, 1) in a
second pass onto the swapchain. vio_clear inside the frame must clear all three.
--EXTENSIONS--
vio
--SKIPIF--
<?php
require __DIR__ . '/../skipif_gl.inc';
?>
--FILE--
<?php
$backends = ['auto'];
foreach (['metal', 'opengl'] as $extra) {
    $probe = @vio_create($extra, ['width' => 4, 'height' => 4, 'headless' => true]);
    if ($probe) { vio_destroy($probe); $backends[] = $extra; }
}

$vs = "#version 330 core\nlayout(location=0) in vec3 aPos;\nlayout(location=1) in vec2 aUv;\nout vec2 vUv;\nvoid main(){ vUv = aUv; gl_Position = vec4(aPos, 1.0); }";
$fsMrt = <<<'GLSL'
#version 330 core
in vec2 vUv;
layout(location = 0) out vec4 o0;
layout(location = 1) out vec4 o1;
layout(location = 2) out vec4 o2;
void main() {
    o0 = vec4(1.0, 0.0, 0.0, 1.0);          // RGBA8
    o1 = vec4(0.0, 0.5, 0.0, 1.0);          // RGBA16F
    o2 = vec4(0.25, 1.0, 0.0, 0.0);         // RG16F: B/A are dropped by the format
}
GLSL;
$fsBlit = "#version 330 core\nin vec2 vUv;\nuniform sampler2D u_tex;\nlayout(location=0) out vec4 o;\nvoid main(){ o = vec4(texture(u_tex, vUv).rgb, 1.0); }";

$W = 8;
foreach ($backends as $be) {
    $ctx = vio_create($be, ['width' => $W, 'height' => $W, 'headless' => true, 'vsync' => false]);
    if (!$ctx) { echo "$be: create failed\n"; continue; }
    $name = vio_backend_name($ctx);
    if (!vio_supports_feature($ctx, VIO_FEATURE_3D_PIPELINE) || !vio_supports_feature($ctx, VIO_FEATURE_READ_PIXELS)
        || !vio_supports_feature($ctx, VIO_FEATURE_MRT)) {
        echo "$name: skipped\n";
        vio_destroy($ctx);
        continue;
    }
    $formats = [VIO_FORMAT_RGBA8, VIO_FORMAT_RGBA16F, VIO_FORMAT_RG16F];
    $rt = vio_render_target($ctx, ['width' => $W, 'height' => $W, 'attachments' => $formats]);
    if (!$rt) { echo "$name: render target failed\n"; vio_destroy($ctx); continue; }

    $fmt = $name === 'opengl' ? VIO_SHADER_GLSL_RAW : VIO_SHADER_GLSL;
    $pipeMrt = vio_pipeline($ctx, ['shader' => vio_shader($ctx, ['vertex' => $vs, 'fragment' => $fsMrt, 'format' => $fmt]),
                                   'depth_test' => false, 'cull_mode' => VIO_CULL_NONE, 'blend' => VIO_BLEND_NONE,
                                   'attachments' => $formats]);
    $pipeBlit = vio_pipeline($ctx, ['shader' => vio_shader($ctx, ['vertex' => $vs, 'fragment' => $fsBlit, 'format' => $fmt]),
                                    'depth_test' => false, 'cull_mode' => VIO_CULL_NONE, 'blend' => VIO_BLEND_NONE]);
    // Quad covering the left half only, so the right half keeps the clear colour.
    $half = vio_mesh($ctx, ['vertices' => [-1,-1,0, 0,0,  0,-1,0, 1,0,  0,1,0, 1,1,  -1,1,0, 0,1],
                            'indices' => [0,1,2,0,2,3], 'layout' => [VIO_FLOAT3, VIO_FLOAT2]]);
    $full = vio_mesh($ctx, ['vertices' => [-1,-1,0, 0,0,  1,-1,0, 1,0,  1,1,0, 1,1,  -1,1,0, 0,1],
                            'indices' => [0,1,2,0,2,3], 'layout' => [VIO_FLOAT3, VIO_FLOAT2]]);

    vio_begin($ctx);
    vio_clear($ctx, 0, 0, 0, 1);
    vio_bind_render_target($ctx, $rt);
    vio_clear($ctx, 0.2, 0.2, 0.2, 1);        // must reach all three attachments
    vio_viewport($ctx, 0, 0, $W, $W);
    vio_bind_pipeline($ctx, $pipeMrt);
    vio_draw($ctx, $half);
    vio_unbind_render_target($ctx);

    // Second pass: sample attachment 1 onto the swapchain.
    vio_viewport($ctx, 0, 0, $W, $W);
    vio_bind_pipeline($ctx, $pipeBlit);
    vio_bind_texture($ctx, vio_render_target_texture($rt, 1), 0);
    vio_set_uniform($ctx, 'u_tex', 0);
    vio_draw($ctx, $full);
    vio_end($ctx);

    $ok = true;
    $check = function (string $what, string $px, int $x, int $y, array $want) use ($W, &$ok, $name) {
        $o = ($y * $W + $x) * 4;
        $got = [ord($px[$o]), ord($px[$o + 1]), ord($px[$o + 2])];
        for ($c = 0; $c < 3; $c++) {
            if (abs($got[$c] - $want[$c]) > 3) {
                $ok = false;
                echo "$name: $what ($x,$y) = [{$got[0]},{$got[1]},{$got[2]}], expected [{$want[0]},{$want[1]},{$want[2]}]\n";
                break;
            }
        }
    };
    $a0 = vio_read_render_target($rt, -1, 0);
    $a1 = vio_read_render_target($rt, -1, 1);
    $a2 = vio_read_render_target($rt, -1, 2);
    $check('att0 drawn', $a0, 1, 4, [255, 0, 0]);   $check('att0 clear', $a0, 6, 4, [51, 51, 51]);
    $check('att1 drawn', $a1, 1, 4, [0, 128, 0]);   $check('att1 clear', $a1, 6, 4, [51, 51, 51]);
    $check('att2 drawn', $a2, 1, 4, [64, 255, 0]);  $check('att2 clear', $a2, 6, 4, [51, 51, 0]);
    $screen = vio_read_pixels($ctx);
    $check('blit att1', $screen, 1, 4, [0, 128, 0]); $check('blit att1 clear', $screen, 6, 4, [51, 51, 51]);
    // Default-argument compatibility: attachment 0 is what the 1-arg forms return.
    $check('att0 default', vio_read_render_target($rt), 1, 4, [255, 0, 0]);
    echo $name, ': ', $ok ? 'OK' : 'FAIL', "\n";
    vio_destroy($ctx);
}
?>
--EXPECTREGEX--
(\w+: (OK|skipped))(\n(\w+: (OK|skipped)))*

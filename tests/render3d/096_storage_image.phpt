--TEST--
Storage images: compute kernel writes an image2D (2D local_size), a fullscreen pass samples it
--DESCRIPTION--
R2 + R7 of API-ROADMAP.md. A texture created with 'storage' => true is bound to the
compute pipeline with vio_compute_bind_image() and filled by an 8x8-local-size kernel
(imageStore gradient: R = x, G = y, B = 0.5). The dispatch geometry is 2D, so the
backend must dispatch with the kernel's reflected local_size instead of the legacy
(64,1,1) contract. The result is then sampled by a normal fragment shader onto the
16x16 swapchain and read back — exercising the storage -> sampled hand-off.

Skips where compute / storage images are unavailable (macOS OpenGL 4.1, Vulkan).
--EXTENSIONS--
vio
--SKIPIF--
<?php
/* Any headless GPU context will do — the test probes the backends itself. */
if (!extension_loaded('vio')) die('skip vio not loaded');
$__ok = false;
foreach (['auto', 'metal', 'opengl'] as $__b) {
    $__c = @vio_create($__b, ['width' => 8, 'height' => 8, 'headless' => true]);
    if ($__c) { vio_destroy($__c); $__ok = true; break; }
}
if (!$__ok) die('skip no headless GPU context available');
?>
--FILE--
<?php
// 'auto' may resolve to a backend that cannot open a context on this host (e.g.
// Vulkan without an ICD on Linux CI): probe every candidate quietly and only keep
// the ones that come up.
$backends = [];
$seen = [];
foreach (['auto', 'metal', 'opengl'] as $candidate) {
    $probe = @vio_create($candidate, ['width' => 4, 'height' => 4, 'headless' => true]);
    if (!$probe) continue;
    // 'auto' may resolve to a name listed explicitly below ('opengl' on Linux):
    // run every real backend once.
    $resolved = vio_backend_name($probe);
    vio_destroy($probe);
    if (in_array($resolved, $seen, true)) continue;
    $seen[] = $resolved;
    $backends[] = $candidate;
}
if (!$backends) { echo "none: skipped\n"; }

$cs = <<<'GLSL'
#version 450
layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;
layout(binding = 0, rgba8) writeonly uniform image2D u_out;
void main() {
    ivec2 p  = ivec2(gl_GlobalInvocationID.xy);
    ivec2 sz = imageSize(u_out);
    if (p.x >= sz.x || p.y >= sz.y) return;
    imageStore(u_out, p, vec4(float(p.x) / float(sz.x - 1), float(p.y) / float(sz.y - 1), 0.5, 1.0));
}
GLSL;
$vs = "#version 330 core\nlayout(location=0) in vec3 aPos;\nlayout(location=1) in vec2 aUv;\nout vec2 vUv;\nvoid main(){ vUv = aUv; gl_Position = vec4(aPos, 1.0); }";
$fs = "#version 330 core\nin vec2 vUv;\nuniform sampler2D u_tex;\nlayout(location=0) out vec4 o;\nvoid main(){ o = texture(u_tex, vUv); }";

$W = 16;
foreach ($backends as $be) {
    $ctx = @vio_create($be, ['width' => $W, 'height' => $W, 'headless' => true, 'vsync' => false]);
    if (!$ctx) { echo "$be: skipped\n"; continue; }
    $name = vio_backend_name($ctx);
    if (!vio_supports_feature($ctx, VIO_FEATURE_COMPUTE) || !vio_supports_feature($ctx, VIO_FEATURE_STORAGE_IMAGE)
        || !vio_supports_feature($ctx, VIO_FEATURE_3D_PIPELINE) || !vio_supports_feature($ctx, VIO_FEATURE_READ_PIXELS)) {
        echo "$name: skipped\n";
        vio_destroy($ctx);
        continue;
    }

    $tex = vio_texture($ctx, ['width' => $W, 'height' => $W, 'storage' => true, 'filter' => VIO_FILTER_NEAREST]);
    if (!$tex) { echo "$name: storage texture failed\n"; vio_destroy($ctx); continue; }

    $cp = vio_compute_pipeline($ctx, ['source' => $cs]);
    if (!$cp) { echo "$name: compute pipeline failed\n"; vio_destroy($ctx); continue; }
    vio_compute_bind_image($ctx, $cp, $tex, 0, VIO_COMPUTE_WRITE);
    vio_compute_dispatch($ctx, $cp, (int)ceil($W / 8), (int)ceil($W / 8), 1);
    /* The D3D12 pixel check used to be disabled here ("GPU-written texture samples
     * stale"). That was the pending-bind table holding a raw pointer to a texture
     * temporary (see test 111), not a barrier problem — checked on every backend. */

    $fmt = $name === 'opengl' ? VIO_SHADER_GLSL_RAW : VIO_SHADER_GLSL;
    $pipe = vio_pipeline($ctx, ['shader' => vio_shader($ctx, ['vertex' => $vs, 'fragment' => $fs, 'format' => $fmt]),
                                'depth_test' => false, 'cull_mode' => VIO_CULL_NONE, 'blend' => VIO_BLEND_NONE]);
    // uv (0,0) at NDC bottom-left: texel row 0 lands on the bottom screen row on every backend.
    $quad = vio_mesh($ctx, ['vertices' => [-1,-1,0, 0,0,  1,-1,0, 1,0,  1,1,0, 1,1,  -1,1,0, 0,1],
                            'indices' => [0,1,2,0,2,3], 'layout' => [VIO_FLOAT3, VIO_FLOAT2]]);
    vio_begin($ctx);
    vio_clear($ctx, 0, 0, 0, 1);
    vio_bind_pipeline($ctx, $pipe);
    vio_bind_texture($ctx, $tex, 0);
    vio_set_uniform($ctx, 'u_tex', 0);
    vio_draw($ctx, $quad);
    vio_end($ctx);

    $px = vio_read_pixels($ctx);   // top-down RGBA
    // Headless swapchains are not 1:1 everywhere (Windows enforces a minimum
    // window size): map the 16x16 texel grid onto the real framebuffer.
    [$fw, $fh] = vio_framebuffer_size($ctx);
    $ok = true;
    $probe = function (int $tx, int $ty, int $r, int $g, int $b) use ($px, $W, $fw, $fh, &$ok, $name) {
        $x = (int)(($tx + 0.5) * $fw / $W);
        $y = (int)(($ty + 0.5) * $fh / $W);
        $o = ($y * $fw + $x) * 4;
        $got = [ord($px[$o]), ord($px[$o + 1]), ord($px[$o + 2])];
        if (abs($got[0] - $r) > 3 || abs($got[1] - $g) > 3 || abs($got[2] - $b) > 3) {
            $ok = false;
            echo "$name: pixel ($tx,$ty) = [{$got[0]},{$got[1]},{$got[2]}], expected [$r,$g,$b]\n";
        }
    };
    // screen row 15 (bottom) = texel row 0 => G = 0; screen row 0 (top) = texel row 15 => G = 255
    $probe(0,  15, 0,   0,   128);
    $probe(15, 15, 255, 0,   128);
    $probe(0,  0,  0,   255, 128);
    $probe(15, 0,  255, 255, 128);
    $probe(8,  8,  136, 119, 128);   // x=8 -> 8/15, screen y=8 -> texel row 7 -> 7/15
    echo $name, ': ', $ok ? 'OK' : 'FAIL', "\n";
    vio_destroy($ctx);
}
?>
--EXPECTREGEX--
(\w+: (OK|skipped))(\n(\w+: (OK|skipped)))*

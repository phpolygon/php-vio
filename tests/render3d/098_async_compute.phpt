--TEST--
Async compute: ['async' => true] records the dispatch into the frame, later draws see it, vio_compute_wait / readback fence it
--DESCRIPTION--
API-ROADMAP R7 (async half). Inside vio_begin/vio_end a kernel writes a storage
image and a storage buffer with ['async' => true]; a draw in the SAME frame samples
the image (in-order execution, no CPU sync), vio_end presents, then
vio_storage_buffer_read() must return the kernel's data (implicit wait) and the
swapchain pixels must show the sampled image. A second async dispatch mid-frame is
followed by an explicit vio_compute_wait() + readback before the frame ends. Outside
a frame, 'async' silently degrades to the synchronous path.
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
layout(local_size_x = 8, local_size_y = 8) in;
layout(binding = 0, rgba8) writeonly uniform image2D u_img;
layout(std430, binding = 1) writeonly buffer Out { float data[]; } u_out;
layout(std140, binding = 2) uniform Params { float scale; } u_p;
void main() {
    ivec2 p = ivec2(gl_GlobalInvocationID.xy);
    ivec2 sz = imageSize(u_img);
    if (p.x >= sz.x || p.y >= sz.y) return;
    imageStore(u_img, p, vec4(0.0, 1.0, 0.0, 1.0));           // solid green
    u_out.data[p.y * sz.x + p.x] = float(p.y * sz.x + p.x) * u_p.scale;
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
    $ok = true;
    $img = vio_texture($ctx, ['width' => $W, 'height' => $W, 'storage' => true, 'filter' => VIO_FILTER_NEAREST]);
    $buf = vio_storage_buffer($ctx, ['size' => $W * $W * 4, 'stride' => 4]);
    $cp  = vio_compute_pipeline($ctx, ['source' => $cs]);
    if (!$img || !$buf || !$cp) { echo "$name: setup failed\n"; vio_destroy($ctx); continue; }
    vio_compute_bind_image($ctx, $cp, $img, 0, VIO_COMPUTE_WRITE);
    vio_compute_bind_buffer($ctx, $cp, $buf, 1, VIO_COMPUTE_WRITE);

    $fmt = $name === 'opengl' ? VIO_SHADER_GLSL_RAW : VIO_SHADER_GLSL;
    $pipe = vio_pipeline($ctx, ['shader' => vio_shader($ctx, ['vertex' => $vs, 'fragment' => $fs, 'format' => $fmt]),
                                'depth_test' => false, 'cull_mode' => VIO_CULL_NONE, 'blend' => VIO_BLEND_NONE]);
    $quad = vio_mesh($ctx, ['vertices' => [-1,-1,0, 0,0,  1,-1,0, 1,0,  1,1,0, 1,1,  -1,1,0, 0,1],
                            'indices' => [0,1,2,0,2,3], 'layout' => [VIO_FLOAT3, VIO_FLOAT2]]);

    // --- Frame 1: async dispatch, draw sampling the image, readback after vio_end.
    vio_begin($ctx);
    vio_clear($ctx, 1, 0, 0, 1);                       // red: a failed sample would stay red
    vio_compute_set_uniforms($ctx, $cp, pack('f', 2.0));
    vio_compute_dispatch($ctx, $cp, (int)ceil($W / 8), (int)ceil($W / 8), 1, ['async' => true]);
    vio_bind_pipeline($ctx, $pipe);
    vio_bind_texture($ctx, $img, 0);
    vio_set_uniform($ctx, 'u_tex', 0);
    vio_draw($ctx, $quad);
    vio_end($ctx);

    $px = vio_read_pixels($ctx);
    [$fw, $fh] = vio_framebuffer_size($ctx);       /* swapchain may exceed 16x16 (Windows minimum size) */
    $o = ((int)($fh / 2) * $fw + (int)($fw / 2)) * 4;
    if (ord($px[$o]) > 3 || ord($px[$o + 1]) < 250) { $ok = false; echo "$name: frame-1 sample = [", ord($px[$o]), ",", ord($px[$o+1]), "]\n"; }
    $data = vio_storage_buffer_read($ctx, $buf);      // implicit wait on the async dispatch
    $vals = array_values(unpack('f*', $data));
    if (count($vals) !== $W * $W || abs($vals[5] - 10.0) > 1e-4 || abs($vals[$W * $W - 1] - 2.0 * ($W * $W - 1)) > 1e-3) {
        $ok = false; echo "$name: frame-1 buffer = ", json_encode(array_slice($vals, 0, 6)), "\n";
    }

    // --- Frame 2: async dispatch with a new scale, explicit wait + readback mid-frame.
    vio_begin($ctx);
    vio_clear($ctx, 0, 0, 0, 1);
    vio_compute_set_uniforms($ctx, $cp, pack('f', 3.0));
    vio_compute_dispatch($ctx, $cp, (int)ceil($W / 8), (int)ceil($W / 8), 1, ['async' => true]);
    vio_compute_wait($ctx);
    $vals = array_values(unpack('f*', vio_storage_buffer_read($ctx, $buf)));
    if (abs($vals[5] - 15.0) > 1e-4) { $ok = false; echo "$name: frame-2 buffer[5] = {$vals[5]}\n"; }
    vio_bind_pipeline($ctx, $pipe);                    // the frame must still be drawable after the wait
    vio_bind_texture($ctx, $img, 0);
    vio_set_uniform($ctx, 'u_tex', 0);
    vio_draw($ctx, $quad);
    vio_end($ctx);
    $px = vio_read_pixels($ctx);
    if (ord($px[$o + 1]) < 250) { $ok = false; echo "$name: frame-2 sample G = ", ord($px[$o + 1]), "\n"; }

    // --- Outside a frame: 'async' degrades to synchronous.
    vio_compute_set_uniforms($ctx, $cp, pack('f', 4.0));
    vio_compute_dispatch($ctx, $cp, (int)ceil($W / 8), (int)ceil($W / 8), 1, ['async' => true]);
    $vals = array_values(unpack('f*', vio_storage_buffer_read($ctx, $buf)));
    if (abs($vals[5] - 20.0) > 1e-4) { $ok = false; echo "$name: sync fallback buffer[5] = {$vals[5]}\n"; }

    echo $name, ': ', $ok ? 'OK' : 'FAIL', "\n";
    vio_destroy($ctx);
}
?>
--EXPECTREGEX--
(\w+: (OK|skipped))(\n(\w+: (OK|skipped)))*

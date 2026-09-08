--TEST--
Texture/cubemap binds resolve against the shader bound at DRAW time (bind before the sampler uniform, or before the pipeline switch)
--DESCRIPTION--
OpenGL binds textures to numbered units and only reads the sampler uniform at draw
time, so engines routinely `vio_bind_texture(unit)` first and `vio_set_uniform
('u_tex', unit)` afterwards — sometimes while a different pipeline is still bound.
Metal used to translate the unit into a [[texture(n)]] index at bind time, using
whatever mapping (and whatever pipeline) happened to be current; the sample then
read from the wrong or an empty slot. The unit -> register resolution must happen
when the draw is recorded.

Three orders are checked, each must sample the intended texture on every backend
with a 3D pipeline: (a) bind, then set sampler uniforms; (b) bind while an
unrelated pipeline is bound, then switch; (c) the reference order.
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
foreach (['auto', 'metal', 'opengl'] as $candidate) {
    $probe = @vio_create($candidate, ['width' => 4, 'height' => 4, 'headless' => true]);
    if ($probe) { vio_destroy($probe); $backends[] = $candidate; }
}
if (!$backends) { echo "none: skipped\n"; }

$vs = "#version 330 core\nlayout(location=0) in vec3 aPos;\nvoid main(){ gl_Position = vec4(aPos, 1.0); }";
// Two 2D samplers + a cube, deliberately declared in an order that differs from
// the GL units the caller picks (unit 5 -> first sampler, unit 2 -> second, unit 9 -> cube).
$fs = <<<'GLSL'
#version 330 core
uniform sampler2D u_a;
uniform sampler2D u_b;
uniform samplerCube u_env;
layout(location=0) out vec4 o;
void main() {
    vec3 a = texture(u_a, vec2(0.5)).rgb;
    vec3 b = texture(u_b, vec2(0.5)).rgb;
    vec3 e = textureLod(u_env, vec3(0.0, 1.0, 0.0), 0.0).rgb;
    o = vec4(a.r, b.g, e.b, 1.0);
}
GLSL;
$fsOther = "#version 330 core\nuniform sampler2D u_x;\nlayout(location=0) out vec4 o;\nvoid main(){ o = texture(u_x, vec2(0.5)); }";

$solid = fn(int $r, int $g, int $b) => str_repeat(pack('C4', $r, $g, $b, 255), 16);
$face = fn(int $r, int $g, int $b) => array_merge(...array_fill(0, 16, [$r, $g, $b, 255]));

foreach ($backends as $be) {
    $ctx = @vio_create($be, ['width' => 8, 'height' => 8, 'headless' => true, 'vsync' => false]);
    if (!$ctx) { echo "$be: skipped\n"; continue; }
    $name = vio_backend_name($ctx);
    if (!vio_supports_feature($ctx, VIO_FEATURE_3D_PIPELINE) || !vio_supports_feature($ctx, VIO_FEATURE_READ_PIXELS)
        || !vio_supports_feature($ctx, VIO_FEATURE_CUBEMAP)) {
        echo "$name: skipped\n";
        vio_destroy($ctx);
        continue;
    }
    $fmt = $name === 'opengl' ? VIO_SHADER_GLSL_RAW : VIO_SHADER_GLSL;
    $pipe  = vio_pipeline($ctx, ['shader' => vio_shader($ctx, ['vertex' => $vs, 'fragment' => $fs, 'format' => $fmt]), 'depth_test' => false, 'cull_mode' => VIO_CULL_NONE]);
    $other = vio_pipeline($ctx, ['shader' => vio_shader($ctx, ['vertex' => $vs, 'fragment' => $fsOther, 'format' => $fmt]), 'depth_test' => false, 'cull_mode' => VIO_CULL_NONE]);
    $quad = vio_mesh($ctx, ['vertices' => [-1,-1,0, 1,-1,0, 1,1,0, -1,1,0], 'indices' => [0,1,2,0,2,3], 'layout' => [VIO_FLOAT3]]);
    $texA = vio_texture($ctx, ['data' => $solid(200, 0, 0), 'width' => 4, 'height' => 4]);
    $texB = vio_texture($ctx, ['data' => $solid(0, 100, 0), 'width' => 4, 'height' => 4]);
    $faces = [];
    for ($i = 0; $i < 6; $i++) { $faces[] = $i === 2 ? $face(0, 0, 50) : $face(255, 255, 255); }
    $cube = vio_cubemap($ctx, ['pixels' => $faces, 'width' => 4, 'height' => 4]);

    $bind = function () use ($ctx, $texA, $texB, $cube) {
        vio_bind_texture($ctx, $texA, 5);
        vio_bind_texture($ctx, $texB, 2);
        vio_bind_cubemap($ctx, $cube, 9);
    };
    $set = function () use ($ctx) {
        vio_set_uniform($ctx, 'u_a', 5);
        vio_set_uniform($ctx, 'u_b', 2);
        vio_set_uniform($ctx, 'u_env', 9);
    };

    $orders = [
        'bind-then-set' => function () use ($ctx, $pipe, $bind, $set) { vio_bind_pipeline($ctx, $pipe); $bind(); $set(); },
        'bind-under-other-pipeline' => function () use ($ctx, $pipe, $other, $bind, $set) { vio_bind_pipeline($ctx, $other); $bind(); vio_bind_pipeline($ctx, $pipe); $set(); },
        'set-then-bind' => function () use ($ctx, $pipe, $bind, $set) { vio_bind_pipeline($ctx, $pipe); $set(); $bind(); },
    ];
    foreach ($orders as $label => $prepare) {
        vio_begin($ctx);
        vio_clear($ctx, 0, 0, 0, 1);
        $prepare();
        vio_draw($ctx, $quad);
        vio_end($ctx);
        $px = vio_read_pixels($ctx);
        $o = (4 * 8 + 4) * 4;
        printf("%s %s: [%d,%d,%d]\n", $name, $label, ord($px[$o]), ord($px[$o + 1]), ord($px[$o + 2]));
    }
    vio_destroy($ctx);
}
?>
--EXPECTREGEX--
(\w+ [a-z-]+: \[200,100,50\]|\w+: skipped)(\n(\w+ [a-z-]+: \[200,100,50\]|\w+: skipped))*

--TEST--
Default-block layout: struct arrays with an std140 stride larger than their packed size (SpotLight-style) keep every following uniform at its reflected offset
--DESCRIPTION--
Regression for the Metal path: SPIRV-Cross emits `spvPaddedArrayElement<T, stride>`
for a dynamically indexed struct array whose std140 stride (64) exceeds the packed
MSL struct size (52), and used to ALSO keep the `_mN_pad` member it had computed for
the unpadded layout — shifting every member after the array by 48 bytes. Uniforms
written at their SPIR-V offsets (u_albedo, u_roughness, ...) were then read from the
wrong place. The array must be indexed with a runtime value, otherwise the compiler
never takes the padded-element path.

Runs on every backend that offers the 3D pipeline + readback; expected values are
exact on all of them.
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

$vs = "#version 330 core\nlayout(location=0) in vec3 aPos;\nvoid main(){ gl_Position = vec4(aPos, 1.0); }";
$fs = <<<'GLSL'
#version 330 core
struct SpotLight {
    vec3 position;
    float range;
    vec3 direction;
    float angle;
    vec3 color;
    float intensity;
    float penumbra;
};
uniform SpotLight u_spot_lights[4];
uniform int u_spot_light_count;
uniform vec3 u_albedo;
uniform float u_roughness;
uniform float u_alpha;
layout(location=0) out vec4 o;
void main() {
    vec3 acc = vec3(0.0);
    int n = min(u_spot_light_count, 4);
    for (int i = 0; i < n; i++) {          // runtime index => padded array element in MSL
        acc += u_spot_lights[i].color * u_spot_lights[i].intensity * u_spot_lights[i].penumbra;
    }
    o = vec4(u_albedo.r, u_roughness, u_alpha * acc.b, 1.0);
}
GLSL;

foreach ($backends as $be) {
    $ctx = vio_create($be, ['width' => 8, 'height' => 8, 'headless' => true, 'vsync' => false]);
    if (!$ctx) { echo "$be: create failed\n"; continue; }
    $name = vio_backend_name($ctx);
    if (!vio_supports_feature($ctx, VIO_FEATURE_3D_PIPELINE) || !vio_supports_feature($ctx, VIO_FEATURE_READ_PIXELS)) {
        echo "$name: skipped\n";
        vio_destroy($ctx);
        continue;
    }
    $fmt = $name === 'opengl' ? VIO_SHADER_GLSL_RAW : VIO_SHADER_GLSL;
    $sh = vio_shader($ctx, ['vertex' => $vs, 'fragment' => $fs, 'format' => $fmt]);
    $pipe = vio_pipeline($ctx, ['shader' => $sh, 'depth_test' => false, 'cull_mode' => VIO_CULL_NONE, 'blend' => VIO_BLEND_NONE]);
    $quad = vio_mesh($ctx, ['vertices' => [-1,-1,0, 1,-1,0, 1,1,0, -1,1,0], 'indices' => [0,1,2,0,2,3], 'layout' => [VIO_FLOAT3]]);

    vio_begin($ctx);
    vio_clear($ctx, 0, 0, 0, 1);
    vio_bind_pipeline($ctx, $pipe);
    vio_set_uniforms($ctx, [
        'u_spot_light_count' => 2,
        'u_spot_lights[0].color' => [0.0, 0.0, 0.5], 'u_spot_lights[0].intensity' => 1.0, 'u_spot_lights[0].penumbra' => 1.0,
        'u_spot_lights[1].color' => [0.0, 0.0, 0.5], 'u_spot_lights[1].intensity' => 0.5, 'u_spot_lights[1].penumbra' => 1.0,
        'u_albedo' => [0.2, 0.9, 0.9],
        'u_roughness' => 0.6,
        'u_alpha' => 1.0,
    ]);
    vio_draw($ctx, $quad);
    vio_end($ctx);

    $px = vio_read_pixels($ctx);
    $o = (4 * 8 + 4) * 4;
    // r = 0.2 -> 51, g = 0.6 -> 153, b = 1.0 * (0.5*1 + 0.5*0.5) = 0.75 -> 191
    printf("%s: [%d,%d,%d]\n", $name, ord($px[$o]), ord($px[$o + 1]), ord($px[$o + 2]));
    vio_destroy($ctx);
}
?>
--EXPECTREGEX--
(\w+: (\[51,153,191\]|skipped))(\n(\w+: (\[51,153,191\]|skipped)))*

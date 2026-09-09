--TEST--
OpenGL: vio_set_uniform() reaches UBO-block members, default-block uniforms and uniform arrays of SPIR-V-path (#version 450) shaders
--SKIPIF--
<?php
if (!extension_loaded('vio')) die('skip vio not loaded');
require __DIR__ . '/../skipif_gl.inc';
?>
--FILE--
<?php
// Regression: SPIRV-Cross emits uniform blocks (and glslang's default uniform
// block) as ONE struct-typed uniform — `uniform Matrices _19;` — so the GL
// names are `_19.uProjection`. glGetUniformLocation("uProjection") returned -1
// and every vio_set_uniform() of every non-raw shader was silently dropped on
// OpenGL (D3D/Metal resolve through the reflected cbuffer offsets instead).
// The backend now falls back to a `<struct>.name` / `<struct>.name[0]` match.

$ctx = vio_create('opengl', ['width' => 16, 'height' => 16, 'headless' => true, 'vsync' => false]);
if (!$ctx) die('skip cannot create OpenGL context');

$I = [1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1];
$quad = vio_mesh($ctx, ['vertices' => [-1,-1,0, 0,0,  3,-1,0, 2,0,  -1,3,0, 0,2], 'layout' => [VIO_FLOAT3, VIO_FLOAT2]]);
$green = vio_texture($ctx, ['data' => pack('C4', 0, 255, 0, 255), 'width' => 1, 'height' => 1]);

$fs = "#version 450\nlayout(location=0) in vec2 vUv;\nuniform sampler2D u_tex;\nuniform vec4 u_tint;\n"
    . "layout(location=0) out vec4 o;\nvoid main(){ o = texture(u_tex, vUv) * u_tint; }";

$cases = [
    // A: uniform block members
    'ubo block' => "#version 450\nlayout(location=0) in vec3 aPos;\nlayout(location=1) in vec2 aUv;\n"
        . "layout(std140, binding=0) uniform Matrices { mat4 uProjection; mat4 uView; mat4 uModel; };\n"
        . "layout(location=0) out vec2 vUv;\nvoid main(){ gl_Position = uProjection * uView * uModel * vec4(aPos, 1.0); vUv = aUv; }",
    // B: loose uniforms (gl_DefaultUniformBlock)
    'default block' => "#version 450\nlayout(location=0) in vec3 aPos;\nlayout(location=1) in vec2 aUv;\n"
        . "uniform mat4 uProjection; uniform mat4 uView; uniform mat4 uModel;\n"
        . "layout(location=0) out vec2 vUv;\nvoid main(){ gl_Position = uProjection * uView * uModel * vec4(aPos, 1.0); vUv = aUv; }",
    // C: a mat4 array member set in one call (`name` -> `<struct>.name[0]`)
    'array member' => "#version 450\nlayout(location=0) in vec3 aPos;\nlayout(location=1) in vec2 aUv;\n"
        . "layout(std140, binding=0) uniform Bones { mat4 uBones[3]; };\n"
        . "layout(location=0) out vec2 vUv;\nvoid main(){ gl_Position = uBones[0] * uBones[1] * uBones[2] * vec4(aPos, 1.0); vUv = aUv; }",
];

function centre(string $px): string { $o = (8 * 16 + 8) * 4; return sprintf('[%d,%d,%d]', ord($px[$o]), ord($px[$o + 1]), ord($px[$o + 2])); }

foreach ($cases as $label => $vs) {
    $sh = vio_shader($ctx, ['vertex' => $vs, 'fragment' => $fs]);
    $pipe = $sh ? vio_pipeline($ctx, ['shader' => $sh, 'depth_test' => false]) : false;
    if (!$pipe) { echo "$label: shader FAIL\n"; continue; }
    vio_begin($ctx);
    vio_clear($ctx, 0, 0, 0, 1);
    vio_viewport($ctx, 0, 0, 16, 16);
    vio_bind_pipeline($ctx, $pipe);
    if ($label === 'array member') {
        // Elements are set individually (`name[i]`, as on D3D/Metal); GL only
        // reports the array as `<struct>.uBones[0]`, so the index must survive.
        vio_set_uniform($ctx, 'uBones[0]', $I);
        vio_set_uniform($ctx, 'uBones[1]', $I);
        vio_set_uniform($ctx, 'uBones[2]', $I);
    } else {
        vio_set_uniform($ctx, 'uProjection', $I);
        vio_set_uniform($ctx, 'uView', $I);
        vio_set_uniform($ctx, 'uModel', $I);
    }
    // Texture unit 1 selected through the sampler uniform, GL-style.
    vio_bind_texture($ctx, $green, 1);
    vio_set_uniform($ctx, 'u_tex', 1);
    vio_set_uniform($ctx, 'u_tint', [1.0, 1.0, 1.0, 1.0]);
    vio_draw($ctx, $quad);
    $px = vio_read_pixels($ctx);
    vio_end($ctx);
    echo "$label: ", centre($px), "\n";
    unset($sh, $pipe);
}

// Tint through the fragment default block, halved -> [0,128,0]
$sh = vio_shader($ctx, ['vertex' => $cases['ubo block'], 'fragment' => $fs]);
$pipe = vio_pipeline($ctx, ['shader' => $sh, 'depth_test' => false]);
vio_begin($ctx);
vio_clear($ctx, 0, 0, 0, 1);
vio_viewport($ctx, 0, 0, 16, 16);
vio_bind_pipeline($ctx, $pipe);
vio_set_uniform($ctx, 'uProjection', $I); vio_set_uniform($ctx, 'uView', $I); vio_set_uniform($ctx, 'uModel', $I);
vio_bind_texture($ctx, $green, 0);
vio_set_uniform($ctx, 'u_tex', 0);
vio_set_uniform($ctx, 'u_tint', [1.0, 0.5, 1.0, 1.0]);
vio_draw($ctx, $quad);
$px = vio_read_pixels($ctx);
vio_end($ctx);
$c = centre($px);
echo "fragment tint: ", ($c === '[0,127,0]' || $c === '[0,128,0]') ? 'OK' : $c, "\n";

unset($sh, $pipe, $quad, $green);
vio_destroy($ctx);
?>
--EXPECT--
ubo block: [0,255,0]
default block: [0,255,0]
array member: [0,255,0]
fragment tint: OK

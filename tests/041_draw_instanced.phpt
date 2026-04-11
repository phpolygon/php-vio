--TEST--
vio_draw_instanced renders multiple instances of a mesh
--EXTENSIONS--
vio
--FILE--
<?php
$ctx = vio_create("opengl", ["width" => 32, "height" => 32, "headless" => true]);
if (!$ctx) { echo "SKIP\n"; exit; }

$mesh = vio_mesh($ctx, [
    'vertices' => [
        -0.5, -0.5, 0.0,
         0.5, -0.5, 0.0,
         0.0,  0.5, 0.0,
    ],
    'layout' => [VIO_FLOAT3],
]);
var_dump($mesh instanceof VioMesh);

$vs = <<<'GLSL'
#version 410 core
layout(location = 0) in vec3 aPos;
layout(location = 3) in mat4 aModel;
void main() {
    gl_Position = aModel * vec4(aPos, 1.0);
}
GLSL;

$fs = <<<'GLSL'
#version 410 core
layout(location = 0) out vec4 FragColor;
void main() {
    FragColor = vec4(1.0);
}
GLSL;

$shader = vio_shader($ctx, ['vertex' => $vs, 'fragment' => $fs, 'format' => VIO_SHADER_GLSL_RAW]);
$pipeline = vio_pipeline($ctx, ['shader' => $shader]);

// 2 identity matrices (16 floats each)
$identity = [
    1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1,
    1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1,
];

vio_begin($ctx);
vio_clear($ctx, 0.0, 0.0, 0.0, 1.0);
vio_bind_pipeline($ctx, $pipeline);
vio_draw_instanced($ctx, $mesh, $identity, 2);
echo "draw_instanced OK\n";

$pixels = vio_read_pixels($ctx);
$has_white = false;
for ($i = 0; $i < strlen($pixels); $i += 4) {
    if (ord($pixels[$i]) > 200) { $has_white = true; break; }
}
var_dump($has_white);

vio_end($ctx);
vio_destroy($ctx);
echo "OK\n";
?>
--EXPECT--
bool(true)
draw_instanced OK
bool(true)
OK

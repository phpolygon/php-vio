<?php
$ctx = vio_create("opengl", ["width" => 32, "height" => 32, "headless" => true]);
if (!$ctx) { echo "SKIP\n"; exit; }

// Create a triangle mesh (config array with vertices inside)
$mesh = vio_mesh($ctx, [
    'vertices' => [
        -1.0, -1.0, 0.0,
         1.0, -1.0, 0.0,
         0.0,  1.0, 0.0,
    ],
    'layout' => [VIO_FLOAT3],
    'topology' => VIO_TRIANGLES,
]);
var_dump($mesh instanceof VioMesh);

// Create shader + pipeline
$vs = <<<'GLSL'
#version 410 core
layout(location = 0) in vec3 aPos;
void main() {
    gl_Position = vec4(aPos, 1.0);
}
GLSL;

$fs = <<<'GLSL'
#version 410 core
layout(location = 0) out vec4 FragColor;
void main() {
    FragColor = vec4(1.0, 1.0, 1.0, 1.0);
}
GLSL;

$shader = vio_shader($ctx, ['vertex' => $vs, 'fragment' => $fs]);
$pipeline = vio_pipeline($ctx, ['shader' => $shader]);

vio_clear($ctx, 0.0, 0.0, 0.0, 1.0);
vio_begin($ctx);
vio_bind_pipeline($ctx, $pipeline);
vio_draw($ctx, $mesh);

// Verify that something was drawn (not all black)
$pixels = vio_read_pixels($ctx);
$has_white = false;
for ($i = 0; $i < strlen($pixels); $i += 4) {
    if (ord($pixels[$i]) > 200) {
        $has_white = true;
        break;
    }
}
var_dump($has_white); // triangle should produce some white pixels

vio_end($ctx);
vio_destroy($ctx);
echo "OK\n";
?>
